// SPDX-License-Identifier: GPL-2.0-only
/*
 * dc30_video.c - V4L2/vb2 video capture device: raw YUYV or MJPEG
 *
 * One fixed native format per TV standard (YUYV, interlaced,
 * 768x576 PAL / 640x480 NTSC - see zr36057.c) - no scaling/decimation yet.
 *
 * Capture runs the ZR36057 VFE in continuous mode into one driver-owned
 * DMA "bounce" buffer. On every vsync (ZR36057_ISR_GIRQ1, DC30's vsync_int)
 * the field that has just finished is copied into the vb2 buffer being
 * filled. That field's rows are not rewritten until the field after next,
 * so the copy has a full field period (20ms) of slack. SnapShot/FrameGrab
 * single grabs (the GPL driver's method) lose one field per frame, see
 * zr36057.c.
 *
 * The ZR36057 has no readable field-parity status, so the driver works out
 * which field has just finished from a sentinel word in the last dword of
 * each field's last line: the VFE overwrites it only when it writes that
 * field. The same check shows the actual field sequence, so a repeated
 * field (a field jump on a bad source) is detected rather than paired
 * blindly.
 *
 * MJPEG is the second format: the card's ZR36050 compresses, and the field
 * interrupt collects finished frames from the ZR36057's code buffer table
 * (dc30_jpeg.c) instead of copying fields - see "MJPEG frames" below.
 *
 * A second node, "dc30-meta", delivers one metadata record per completed
 * frame (dc30_meta.h): field timestamps, the audio position at each field
 * interrupt and the stream's error counters, with the same sequence
 * number as the frame.
 */

#include <linux/i2c.h>
#include <linux/pci.h>
#include <linux/ratelimit.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
#include <linux/unaligned.h>
#else
#include <asm/unaligned.h>
#endif
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-vmalloc.h>

#include "dc30.h"
#include "dc30_alsa.h"
#include "dc30_jpeg.h"
#include "dc30_meta.h"
#include "dc30_power.h"
#include "dc30_video.h"
#include "zr36057.h"

/* Unlikely as real pixel data; if it ever is, that field is counted in
 * no_field and dropped - nothing worse.
 */
#define DC30_FIELD_SENTINEL	0xdc30f1e1

/* Fields completed after VidEn is set are ignored for this many vsyncs,
 * since the first one may have started mid-field.
 */
#define DC30_SETTLE_VSYNCS	1

/* MJPEG: without a frame for this many fields the codec is restarted -
 * longer at stream start and after a restart that brought nothing yet
 * (decoder locking, or no signal at all).
 */
#define DC30_JPEG_WATCHDOG_FIELDS	12
#define DC30_JPEG_START_FIELDS		50

/* Field interrupts remembered for MJPEG, whose frames are ready a field
 * or so after their fields: time and audio position of each.
 */
#define DC30_FIELD_LOG		32

/* Fields between the interrupt that ends a frame's second field and the
 * first interrupt that sees the frame done. A frame is ready 0.2 ms after
 * that interrupt (board, debugfs jpeg_test), so the next one sees it: 1.
 * A frame that only got done while the handler ran (dc30_jpeg_vsync())
 * counts as seen by the next interrupt too. The field mapping comes from
 * the first two frames in a row seen two interrupts apart, and the
 * pairing check relies on it from there on. As a guard, the stream checks
 * it over its first DC30_JPEG_CALIB_FRAMES frames and moves it to the
 * delay seen most.
 */
static unsigned int dc30_jpeg_field_delay = 1;
module_param_named(jpeg_field_delay, dc30_jpeg_field_delay, uint, 0644);
MODULE_PARM_DESC(jpeg_field_delay, "MJPEG: field interrupts between a frame's last field and its completion (default 1)");

/* Diagnosis of frames the codec skipped (dc30_jpeg_skipped()): read the
 * ZR36016's line count at every field. Four more GuestBus accesses per
 * field interrupt, so off by default.
 */
static bool dc30_jpeg_nol;
module_param_named(jpeg_nol, dc30_jpeg_nol, bool, 0644);
MODULE_PARM_DESC(jpeg_nol, "MJPEG: log the ZR36016 line count per field, for skipped frames (default off)");

#define DC30_JPEG_CALIB_FRAMES	25
#define DC30_JPEG_DELAYS	4	/* histogram buckets: 0, 1, 2, 3+ */

struct dc30_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
};

struct dc30_field_log {
	u64 vsync;		/* vid->vsyncs of this interrupt */
	u64 ns;
	struct dc30_audio_mark mark;
	s8 parity;		/* of the field it ended, -1 unknown */
	u8 isr;			/* JPEGRepIRQ, GIRQ0 seen (DC30_LOG_*) */
	s16 nol;		/* ZR36016 lines, -1 not read (jpeg_nol) */
};

#define DC30_LOG_REP		(1 << 0)
#define DC30_LOG_GIRQ0		(1 << 1)

struct dc30_video {
	struct dc30_dev *dc30;
	struct v4l2_device v4l2_dev;
	struct v4l2_ctrl_handler hdl;	/* collects the decoder's controls */
	struct video_device vdev;
	struct vb2_queue queue;
	struct mutex lock;	/* serializes ioctls / vb2 queue ops */

	/* Continuous-mode DMA target, sized for the largest standard. */
	void *bounce;
	dma_addr_t bounce_dma;
	size_t bounce_size;

	spinlock_t qlock;	/* protects everything below */
	struct list_head pending;
	struct vb2_v4l2_buffer *active;	/* frame being assembled */
	bool have_first;		/* active holds the first field */
	s64 held_v;			/* MJPEG: its field interrupt */
	unsigned int held_parity;	/* MJPEG: and parity */
	unsigned int held_len;		/* MJPEG: its JPEG's length */
	bool raw_pending;		/* raw: active also holds a
					 * tentative second field */
	struct dc30_meta_field raw_second;	/* and its metadata */
	u64 parity_fixes;		/* misread field flags corrected */
	u64 single_fields;		/* lone raw fields sent as a frame */
	unsigned int first_parity;	/* 0 = top first (PAL), 1 = bottom */
	bool streaming;

	/* Per-stream statistics, reset in start_streaming and readable via
	 * debugfs (dc30_video_stats_show()). Protected by qlock.
	 *
	 * vsyncs counts every GIRQ1 (one per field), and a completed frame's
	 * V4L2 sequence number is derived from it (vsyncs / 2, counted from
	 * the first frame so the stream starts at 0) rather than
	 * from the number of completed buffers - so a frame the hardware
	 * never grabbed shows up as a gap in the sequence instead of being
	 * silently skipped.
	 */
	u64 vsyncs;
	u64 first_vsync_ns;
	u64 last_vsync_ns;
	u64 frames_done;
	u64 frames_dropped;	/* sum of gaps between consecutive sequences */
	u64 fields[2];		/* completed top / bottom fields */
	u64 no_field;		/* vsyncs with no completed field */
	u64 both_fields;	/* vsyncs seeing two fields: IRQ >1 field late */
	u64 field_order;	/* repeated or orphaned field (field jump) */
	u64 no_buffer;		/* first fields dropped, no vb2 buffer queued */
	u64 overflows;		/* VSSFGR VidOvf seen */
	s64 last_sequence;	/* -1 before the first completed frame */
	u32 seq_base;		/* vsyncs / 2 at the first frame: seq starts at 0 */

	/* Metadata node. Its buffers are handed out from the field
	 * interrupt, so meta_pending and meta_streaming are under qlock too.
	 */
	struct video_device meta_vdev;
	struct vb2_queue meta_queue;
	struct mutex meta_lock;		/* serializes the meta node's ioctls */
	struct list_head meta_pending;
	bool meta_streaming;
	u64 meta_no_buffer;		/* frames without a queued meta buffer */
	struct dc30_meta_field first_field;	/* of the frame in progress */
	struct dc30_audio_mark last_mark;	/* latest valid audio mark */

	/* MJPEG (dc30_jpeg.c) instead of raw YUYV. The per-stream state
	 * below rate_ctrl is under qlock.
	 */
	bool jpeg;
	struct v4l2_ctrl *rate_ctrl;	/* data rate, bits/s */
	bool pair_by_flag;		/* DC30_CID_PAIR_BY_FLAG */
	unsigned int jpeg_kbps;		/* of the running stream */
	struct work_struct jpeg_restart_work;
	bool jpeg_paused;		/* codec restart in progress */
	bool jpeg_restart_pending;
	bool jpeg_have_prev;		/* settling: a frame counter seen */
	u8 jpeg_prev_fcnt;
	s64 jpeg_prev_seen;		/* and the interrupt that saw it */
	bool jpeg_based;		/* frame counter mapped to sequence */
	bool jpeg_calibrated;		/* field mapping checked */
	unsigned int jpeg_delays[DC30_JPEG_DELAYS];
	s64 jpeg_vbase;			/* vsync that ended field 0 of seq 0 */
	s64 jpeg_fcnt;			/* ZR36057 frame counter, unwrapped */
	s64 jpeg_fcnt_off;		/* codec frame = jpeg_fcnt + offset */
	s64 jpeg_last_codec;		/* codec frame number of the last */
	bool jpeg_crossed;		/* last frame paired across codec
					 * frames */
	s64 jpeg_seq_off;		/* frame slots added by field jumps */
	u64 jpeg_last_frame_vsync;
	u64 jpeg_frames_since;		/* frames since start/restart */
	u64 jpeg_start_vsync;		/* vsyncs at start/restart */
	unsigned int jpeg_rep_since;	/* fields with JPEGRepIRQ since */
	bool jpeg_done_early;		/* next frame done at handler entry */
	struct ratelimit_state jpeg_dump_rs;	/* codec state at stalls */
	u64 jpeg_bytes;
	u64 jpeg_restarts;
	u64 jpeg_skipped;		/* codec frames lost: F_CNT gaps */
	u64 jpeg_skips;			/* gaps in F_CNT */
	struct ratelimit_state jpeg_skip_rs;	/* their log lines */
	/* jpeg_nol: ZR36016 line counts read, failed reads, counts other
	 * than the window height, smallest and largest.
	 */
	u64 nol_fields, nol_failed, nol_off;
	unsigned int nol_min, nol_max;
	u64 jpeg_bad;			/* empty or out-of-order frames */
	u64 jpeg_settle;		/* frames dropped while settling */
	u64 jpeg_oversize;		/* frames too large for vb2 */
	unsigned int jpeg_max_len;
	u64 jpeg_shifts;		/* field mapping corrections */
	u64 jpeg_crossed_frames;	/* frames paired across codec frames */
	u64 jpeg_pair_switches;		/* changes into or out of that */
	u64 jpeg_single_fields;		/* lone fields sent as a frame */
	s64 jpeg_delay_min, jpeg_delay_max;
	struct dc30_field_log fieldlog[DC30_FIELD_LOG];

	v4l2_std_id std;
	unsigned int input;
	struct dc30_vfe_geometry geo;
	unsigned int bytesperline;
	unsigned int sizeimage;
};

struct dc30_input {
	unsigned int muxsel;	/* vpx3220's own input index, see dc30_vpx3220.c */
	const char *name;
};

/* Order and muxsel values from the DC30 card table in the GPL zoran
 * driver's zoran_card.c (.input[]).
 */
static const struct dc30_input dc30_inputs[] = {
	{ 1, "Composite" },
	{ 2, "S-Video" },
	{ 0, "Internal" },
};

static inline struct v4l2_subdev *dc30_decoder_sd(struct dc30_dev *dc30)
{
	return i2c_get_clientdata(dc30->decoder);
}

/* The one mode of a format and standard. MJPEG: one JPEG per field, both
 * in one buffer, the first field first (V4L2_FIELD_SEQ_*).
 */
static void dc30_pix_format(v4l2_std_id std, bool jpeg,
			    struct v4l2_pix_format *pix)
{
	struct dc30_vfe_geometry geo;

	dc30_vfe_geometry_for_std(std, &geo);
	pix->width = geo.width;
	pix->height = geo.height;
	if (jpeg) {
		pix->pixelformat = V4L2_PIX_FMT_MJPEG;
		pix->field = (std & V4L2_STD_NTSC) ? V4L2_FIELD_SEQ_BT :
						     V4L2_FIELD_SEQ_TB;
		pix->bytesperline = 0;
		pix->sizeimage = DC30_JPEG_MAX_FRAME;
	} else {
		pix->pixelformat = V4L2_PIX_FMT_YUYV;
		pix->field = V4L2_FIELD_INTERLACED;
		pix->bytesperline = geo.width * 2;
		pix->sizeimage = pix->bytesperline * geo.height;
	}
	/* Both carry the decoder's BT.601 levels - the ZR36016 does no
	 * conversion from YCbCr to YCbCr, so the JPEGs are limited range too.
	 */
	pix->colorspace = V4L2_COLORSPACE_SMPTE170M;
	pix->ycbcr_enc = V4L2_YCBCR_ENC_601;
	pix->quantization = V4L2_QUANTIZATION_LIM_RANGE;
	pix->xfer_func = V4L2_XFER_FUNC_709;
}

static void dc30_fill_pix_format(struct dc30_video *vid,
				  struct v4l2_pix_format *pix)
{
	dc30_pix_format(vid->std, vid->jpeg, pix);
}

/* Buffer layout for the selected format and standard. */
static void dc30_update_format(struct dc30_video *vid)
{
	struct v4l2_pix_format pix;

	dc30_pix_format(vid->std, vid->jpeg, &pix);
	dc30_vfe_geometry_for_std(vid->std, &vid->geo);
	vid->bytesperline = pix.bytesperline;
	vid->sizeimage = pix.sizeimage;
}

/* ---- vb2 queue ops ---- */

static u32 *dc30_field_sentinel(struct dc30_video *vid, unsigned int parity)
{
	unsigned int row = vid->geo.height - 2 + parity;

	return vid->bounce + row * vid->bytesperline + vid->bytesperline - 4;
}

static void dc30_arm_sentinels(struct dc30_video *vid)
{
	WRITE_ONCE(*dc30_field_sentinel(vid, 0), DC30_FIELD_SENTINEL);
	WRITE_ONCE(*dc30_field_sentinel(vid, 1), DC30_FIELD_SENTINEL);
}

static int dc30_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
			     unsigned int *nplanes, unsigned int sizes[],
			     struct device *alloc_devs[])
{
	struct dc30_video *vid = vb2_get_drv_priv(vq);

	if (*nplanes)
		return sizes[0] < vid->sizeimage ? -EINVAL : 0;

	*nplanes = 1;
	sizes[0] = vid->sizeimage;
	return 0;
}

static int dc30_buf_prepare(struct vb2_buffer *vb)
{
	struct dc30_video *vid = vb2_get_drv_priv(vb->vb2_queue);

	if (vb2_plane_size(vb, 0) < vid->sizeimage)
		return -EINVAL;
	vb2_set_plane_payload(vb, 0, vid->sizeimage);
	return 0;
}

static void dc30_buf_queue(struct vb2_buffer *vb)
{
	struct dc30_video *vid = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct dc30_buffer *buf = container_of(vbuf, struct dc30_buffer, vb);
	unsigned long flags;

	spin_lock_irqsave(&vid->qlock, flags);
	list_add_tail(&buf->list, &vid->pending);
	spin_unlock_irqrestore(&vid->qlock, flags);
}

/* All buffers back to vb2: ERROR on stop, QUEUED on a failed start. */
static void dc30_return_buffers(struct dc30_video *vid,
				enum vb2_buffer_state state)
{
	struct dc30_buffer *buf, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&vid->qlock, flags);
	if (vid->active) {
		vb2_buffer_done(&vid->active->vb2_buf, state);
		vid->active = NULL;
	}
	list_for_each_entry_safe(buf, tmp, &vid->pending, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, state);
	}
	spin_unlock_irqrestore(&vid->qlock, flags);
}

/* MJPEG: code buffers and the codec, at the rate the control asks for. */
static int dc30_jpeg_stream_start(struct dc30_video *vid)
{
	struct dc30_dev *dc30 = vid->dc30;
	int err;

	vid->jpeg_kbps = vid->rate_ctrl->val / 8000;
	err = dc30_jpeg_alloc(dc30);
	if (err)
		return err;
	err = dc30_jpeg_start(dc30, vid->std, vid->jpeg_kbps);
	if (err) {
		dc30_jpeg_free(dc30);
		return err;
	}
	v4l2_ctrl_grab(vid->rate_ctrl, true);
	return 0;
}

static void dc30_jpeg_stream_stop(struct dc30_video *vid)
{
	struct dc30_dev *dc30 = vid->dc30;

	/* A restart in progress finishes first; streaming is off, so
	 * none is queued anew.
	 */
	cancel_work_sync(&vid->jpeg_restart_work);
	dc30_jpeg_stop(dc30);
	dc30_jpeg_free(dc30);
	v4l2_ctrl_grab(vid->rate_ctrl, false);
}

static int dc30_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct dc30_video *vid = vb2_get_drv_priv(vq);
	struct dc30_dev *dc30 = vid->dc30;
	unsigned long flags;
	int err;

	/* Decoder locks onto its analog input independently of this (it
	 * is powered up while the node is open), but never drives a pixel
	 * clock/sync toward the ZR36057 until its output stage is
	 * explicitly enabled - see dc30_vpx3220.c. The JPEG chips run on its
	 * pixel clock, so it streams before they are set up.
	 */
	if (test_and_set_bit(0, &dc30->capture_busy)) {
		dc30_return_buffers(vid, VB2_BUF_STATE_QUEUED);
		return -EBUSY;
	}
	dc30_board_capture(dc30, true);
	err = v4l2_subdev_call(dc30_decoder_sd(dc30), video, s_stream, 1);
	if (err && err != -ENOIOCTLCMD)
		goto err_bus;
	dc30_decoder_settle(dc30, vid->std);

	spin_lock_irqsave(&vid->qlock, flags);
	vid->vsyncs = 0;
	vid->first_vsync_ns = 0;
	vid->last_vsync_ns = 0;
	vid->frames_done = 0;
	vid->frames_dropped = 0;
	memset(vid->fields, 0, sizeof(vid->fields));
	vid->no_field = 0;
	vid->both_fields = 0;
	vid->field_order = 0;
	vid->no_buffer = 0;
	vid->overflows = 0;
	vid->last_sequence = -1;
	vid->have_first = false;
	vid->raw_pending = false;
	vid->parity_fixes = 0;
	vid->single_fields = 0;
	/* V4L2_FIELD_INTERLACED: bottom field first for NTSC, top otherwise. */
	vid->first_parity = (vid->std & V4L2_STD_NTSC) ? 1 : 0;
	vid->jpeg_paused = false;
	vid->jpeg_restart_pending = false;
	vid->jpeg_have_prev = false;
	vid->jpeg_based = false;
	vid->jpeg_last_codec = -1;
	vid->jpeg_crossed = false;
	vid->jpeg_seq_off = 0;
	vid->jpeg_settle = 0;
	vid->jpeg_oversize = 0;
	vid->jpeg_max_len = 0;
	vid->jpeg_shifts = 0;
	vid->jpeg_crossed_frames = 0;
	vid->jpeg_pair_switches = 0;
	vid->jpeg_single_fields = 0;
	vid->jpeg_last_frame_vsync = 0;
	vid->jpeg_frames_since = 0;
	vid->jpeg_start_vsync = 0;
	vid->jpeg_rep_since = 0;
	vid->jpeg_bytes = 0;
	vid->jpeg_restarts = 0;
	vid->jpeg_skipped = 0;
	vid->jpeg_skips = 0;
	vid->nol_fields = 0;
	vid->nol_failed = 0;
	vid->nol_off = 0;
	vid->nol_min = UINT_MAX;
	vid->nol_max = 0;
	vid->jpeg_bad = 0;
	vid->jpeg_delay_min = S64_MAX;
	vid->jpeg_delay_max = S64_MIN;
	memset(vid->fieldlog, 0, sizeof(vid->fieldlog));
	spin_unlock_irqrestore(&vid->qlock, flags);

	if (vid->jpeg) {
		err = dc30_jpeg_stream_start(vid);
		if (err)
			goto err_stream;
	} else {
		dc30_board_capture_raw(dc30);
		dc30_vfe_set_geometry(dc30, vid->std, &vid->geo);
		dc30_arm_sentinels(vid);
		dc30_vfe_start_continuous(dc30, vid->bounce_dma,
					  vid->bytesperline);
		dc30_vfe_enable(dc30, true);
	}

	/* The decoder's sync outputs may have run before this stream (audio
	 * in video lock mode, dc30_vpx3220.c), so a vsync from before it can be
	 * pending: drop it, or it fires at once as a field that never
	 * completed.
	 */
	dc30_write(dc30, ZR36057_ISR, ZR36057_ISR_GIRQ1);
	dc30_write(dc30, ZR36057_ICR, dc30_read(dc30, ZR36057_ICR) |
		   ZR36057_ICR_GIRQ1 | ZR36057_ICR_INTPINEN);

	spin_lock_irqsave(&vid->qlock, flags);
	vid->streaming = true;
	spin_unlock_irqrestore(&vid->qlock, flags);

	return 0;

err_stream:
	v4l2_subdev_call(dc30_decoder_sd(dc30), video, s_stream, 0);
err_bus:
	dc30_board_capture(dc30, false);
	clear_bit(0, &dc30->capture_busy);
	dc30_return_buffers(vid, VB2_BUF_STATE_QUEUED);
	return err;
}

static void dc30_stop_streaming(struct vb2_queue *vq)
{
	struct dc30_video *vid = vb2_get_drv_priv(vq);
	struct dc30_dev *dc30 = vid->dc30;
	unsigned long flags;

	dc30_write(dc30, ZR36057_ICR, dc30_read(dc30, ZR36057_ICR) &
		   ~(ZR36057_ICR_GIRQ1 | ZR36057_ICR_INTPINEN));
	/* From here on the field interrupt leaves the capture alone (it
	 * checks under qlock), so the code buffers can go.
	 */
	spin_lock_irqsave(&vid->qlock, flags);
	vid->streaming = false;
	spin_unlock_irqrestore(&vid->qlock, flags);

	if (vid->jpeg)
		dc30_jpeg_stream_stop(vid);
	else
		dc30_vfe_enable(dc30, false);
	v4l2_subdev_call(dc30_decoder_sd(dc30), video, s_stream, 0);
	dc30_board_capture(dc30, false);
	clear_bit(0, &dc30->capture_busy);

	dc30_return_buffers(vid, VB2_BUF_STATE_ERROR);
}

/* ---- metadata node vb2 queue ---- */

static int dc30_meta_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
				 unsigned int *nplanes, unsigned int sizes[],
				 struct device *alloc_devs[])
{
	if (*nplanes)
		return sizes[0] < sizeof(struct dc30_meta) ? -EINVAL : 0;

	*nplanes = 1;
	sizes[0] = sizeof(struct dc30_meta);
	return 0;
}

static int dc30_meta_buf_prepare(struct vb2_buffer *vb)
{
	if (vb2_plane_size(vb, 0) < sizeof(struct dc30_meta))
		return -EINVAL;
	vb2_set_plane_payload(vb, 0, sizeof(struct dc30_meta));
	return 0;
}

static void dc30_meta_buf_queue(struct vb2_buffer *vb)
{
	struct dc30_video *vid = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct dc30_buffer *buf = container_of(vbuf, struct dc30_buffer, vb);
	unsigned long flags;

	spin_lock_irqsave(&vid->qlock, flags);
	list_add_tail(&buf->list, &vid->meta_pending);
	spin_unlock_irqrestore(&vid->qlock, flags);
}

static int dc30_meta_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct dc30_video *vid = vb2_get_drv_priv(vq);
	unsigned long flags;

	spin_lock_irqsave(&vid->qlock, flags);
	vid->meta_no_buffer = 0;
	vid->meta_streaming = true;
	spin_unlock_irqrestore(&vid->qlock, flags);
	return 0;
}

static void dc30_meta_stop_streaming(struct vb2_queue *vq)
{
	struct dc30_video *vid = vb2_get_drv_priv(vq);
	struct dc30_buffer *buf, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&vid->qlock, flags);
	vid->meta_streaming = false;
	list_for_each_entry_safe(buf, tmp, &vid->meta_pending, list) {
		list_del(&buf->list);
		vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_ERROR);
	}
	spin_unlock_irqrestore(&vid->qlock, flags);
}

/* From 6.13 on vb2 releases and takes q->lock itself around a wait when
 * these callbacks are missing, and later kernels dropped them.
 */
static const struct vb2_ops dc30_meta_vb2_ops = {
	.queue_setup = dc30_meta_queue_setup,
	.buf_prepare = dc30_meta_buf_prepare,
	.buf_queue = dc30_meta_buf_queue,
	.start_streaming = dc30_meta_start_streaming,
	.stop_streaming = dc30_meta_stop_streaming,
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 13, 0)
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
#endif
};

static const struct vb2_ops dc30_vb2_ops = {
	.queue_setup = dc30_queue_setup,
	.buf_prepare = dc30_buf_prepare,
	.buf_queue = dc30_buf_queue,
	.start_streaming = dc30_start_streaming,
	.stop_streaming = dc30_stop_streaming,
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 13, 0)
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
#endif
};

static void dc30_meta_field_set(struct dc30_meta_field *f,
				unsigned int parity, u64 now,
				const struct dc30_audio_mark *mark)
{
	memset(f, 0, sizeof(*f));
	f->vsync_ns = now;
	f->flags = parity ? DC30_META_FIELD_BOTTOM : 0;
	if (mark->valid) {
		f->audio_pos = mark->pos;
		f->audio_epoch = mark->epoch;
		f->flags |= DC30_META_FIELD_AUDIO;
	}
}

static u32 dc30_meta_count(u64 v)
{
	return min_t(u64, v, U32_MAX);
}

/* Hand out the metadata record for a completed frame. Caller holds
 * vid->qlock.
 */
static void dc30_meta_done(struct dc30_video *vid, u32 seq, u64 now,
			   const struct dc30_meta_field *second)
{
	struct dc30_buffer *buf;
	struct dc30_meta *m;

	if (!vid->meta_streaming)
		return;
	if (list_empty(&vid->meta_pending)) {
		vid->meta_no_buffer++;
		return;
	}
	buf = list_first_entry(&vid->meta_pending, struct dc30_buffer, list);
	list_del(&buf->list);

	m = vb2_plane_vaddr(&buf->vb.vb2_buf, 0);
	memset(m, 0, sizeof(*m));
	m->version = DC30_META_VERSION;
	m->size = sizeof(*m);
	m->sequence = seq;
	m->field[0] = vid->first_field;
	m->field[1] = *second;
	if (vid->last_mark.valid) {
		m->audio_rate = vid->last_mark.rate;
		m->audio_frame_bytes = DC30_ALSA_FRAME_BYTES;
		m->audio_clock = vid->last_mark.videolock ?
				 DC30_META_CLOCK_VIDEOLOCK :
				 DC30_META_CLOCK_CRYSTAL;
	}
	m->frames_dropped = dc30_meta_count(vid->frames_dropped);
	m->no_field = dc30_meta_count(vid->no_field);
	m->late_irq = dc30_meta_count(vid->both_fields);
	m->field_order = dc30_meta_count(vid->field_order);
	m->fifo_overflows = dc30_meta_count(vid->overflows);
	m->codec_restarts = dc30_meta_count(vid->jpeg_restarts);
	m->pairing_switches = dc30_meta_count(vid->jpeg_pair_switches);
	m->codec_skipped = dc30_meta_count(vid->jpeg_skipped);
	m->frames_rejected = dc30_meta_count(vid->no_buffer +
					     vid->jpeg_oversize +
					     vid->jpeg_bad);

	buf->vb.vb2_buf.timestamp = now;
	buf->vb.sequence = seq;
	buf->vb.field = V4L2_FIELD_NONE;
	vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
}

/* Raw fields. The ZR36057 writes each field into the bounce buffer's rows
 * of the parity the VPX flags, and the sentinels tell which one it was.
 * With the flag following the input (dc30_vpx3220 field_follow) the VPX
 * sees a field jump at once, but misreads single fields on a VCR's
 * signal (on the board ~20 in 90 s, T B T T T B instead of
 * T B T B T B). A misread shows as two repeats in a row, a jump as one:
 * a repeated field is taken as the other parity until the next one
 * decides. Without input the VPX flags bottom fields only; they pair up
 * the same way. Caller holds vid->qlock for all of these.
 */

/* Bounce rows of parity 'src' into the frame's rows of parity 'dst'. */
static void dc30_copy_rows(struct dc30_video *vid, unsigned int src,
			   unsigned int dst)
{
	void *d = vb2_plane_vaddr(&vid->active->vb2_buf, 0);
	unsigned int bpl = vid->bytesperline;
	unsigned int y;

	for (y = 0; y + 1 < vid->geo.height; y += 2)
		memcpy(d + (y + dst) * bpl, vid->bounce + (y + src) * bpl, bpl);
}

/* Rows of parity 'src' of frame 'from' into the rows of 'dst' of 'to'. */
static void dc30_move_rows(struct dc30_video *vid,
			   struct vb2_v4l2_buffer *from, unsigned int src,
			   struct vb2_v4l2_buffer *to, unsigned int dst)
{
	void *s = vb2_plane_vaddr(&from->vb2_buf, 0);
	void *d = vb2_plane_vaddr(&to->vb2_buf, 0);
	unsigned int bpl = vid->bytesperline;
	unsigned int y;

	for (y = 0; y + 1 < vid->geo.height; y += 2)
		memcpy(d + (y + dst) * bpl, s + (y + src) * bpl, bpl);
}

/* The frame in vid->active is complete, its second field ended by
 * interrupt 'v' - or, for a lone field (its rows doubled), 'lone' ended
 * by 'v': into the frame slot the field falls in, rounded up as for
 * MJPEG (dc30_jpeg_frame_out()), and only if that one is still free.
 * False: not sent, the buffer stays in vid->active.
 */
static bool dc30_raw_out(struct dc30_video *vid, s64 v, bool lone,
			 const struct dc30_meta_field *second)
{
	struct vb2_v4l2_buffer *vbuf = vid->active;
	unsigned int f = vid->first_parity;
	s64 seq;

	if (lone) {
		seq = (v + 1) / 2 - vid->seq_base;
		if (vid->last_sequence < 0 || seq <= vid->last_sequence)
			return false;
		dc30_move_rows(vid, vbuf, f, vbuf, !f);
		vid->single_fields++;
	} else {
		if (vid->last_sequence < 0)
			vid->seq_base = v / 2;
		seq = v / 2 - vid->seq_base;
		if (seq <= vid->last_sequence)
			seq = vid->last_sequence + 1;
	}
	if (vid->last_sequence >= 0 && seq > vid->last_sequence + 1)
		vid->frames_dropped += seq - vid->last_sequence - 1;
	vid->last_sequence = seq;
	vid->frames_done++;
	vid->fields[0]++;
	if (!lone)
		vid->fields[1]++;

	vbuf->vb2_buf.timestamp = second->vsync_ns;
	vbuf->sequence = seq;
	vbuf->field = V4L2_FIELD_INTERLACED;
	vb2_buffer_done(&vbuf->vb2_buf, VB2_BUF_STATE_DONE);
	vid->active = NULL;
	vid->have_first = false;

	dc30_meta_done(vid, seq, second->vsync_ns, second);
	return true;
}

/* The held first field stays alone. Caller holds vid->qlock. */
static void dc30_raw_lone(struct dc30_video *vid)
{
	struct dc30_meta_field second = vid->first_field;

	vid->field_order++;
	vid->have_first = false;
	dc30_raw_out(vid, vid->held_v, true, &second);
}

/* Start a frame with field 'v' (flagged 'flag') as its first field, from
 * the bounce rows of 'flag'.
 */
static void dc30_raw_first(struct dc30_video *vid, unsigned int flag, s64 v,
			   u64 now, const struct dc30_audio_mark *mark)
{
	if (vid->have_first)
		dc30_raw_lone(vid);
	if (!vid->active) {
		struct dc30_buffer *buf;

		if (list_empty(&vid->pending)) {
			vid->no_buffer++;
			return;
		}
		buf = list_first_entry(&vid->pending, struct dc30_buffer, list);
		list_del(&buf->list);
		vid->active = &buf->vb;
	}
	dc30_copy_rows(vid, flag, vid->first_parity);
	dc30_meta_field_set(&vid->first_field, vid->first_parity, now, mark);
	vid->have_first = true;
	vid->held_v = v;
	vid->held_parity = flag;
}

static void dc30_field_done(struct dc30_video *vid, unsigned int flag,
			    u64 now, const struct dc30_audio_mark *mark)
{
	unsigned int f = vid->first_parity;
	struct dc30_meta_field second;
	s64 v = vid->vsyncs;

	dc30_meta_field_set(&second, !f, now, mark);

	/* The frame holds a first field and, from the repeat right after
	 * it, a tentative second one. A third in a row: that one was
	 * misread, the frame is right. Otherwise it was a jump: the first
	 * field stays alone, the tentative one starts the frame.
	 */
	if (vid->raw_pending) {
		vid->raw_pending = false;
		if (flag == f) {
			vid->parity_fixes++;
			dc30_raw_out(vid, v - 1, false, &vid->raw_second);
		} else {
			struct vb2_v4l2_buffer *held = vid->active;
			struct dc30_buffer *buf;

			/* The tentative field into a buffer of its own, so
			 * that the first one can go out alone.
			 */
			if (!list_empty(&vid->pending)) {
				buf = list_first_entry(&vid->pending,
						       struct dc30_buffer, list);
				list_del(&buf->list);
				dc30_move_rows(vid, held, !f, &buf->vb, f);
				dc30_raw_lone(vid);
				if (vid->active) {	/* slot taken */
					list_add(&buf->list, &vid->pending);
					dc30_move_rows(vid, &buf->vb, f,
						       held, f);
				} else {
					vid->active = &buf->vb;
				}
			} else {
				vid->field_order++;
				dc30_move_rows(vid, held, !f, held, f);
			}
			vid->first_field = vid->raw_second;
			vid->first_field.flags &= ~DC30_META_FIELD_BOTTOM;
			if (f)
				vid->first_field.flags |= DC30_META_FIELD_BOTTOM;
			dc30_copy_rows(vid, flag, !f);
			dc30_raw_out(vid, v, false, &second);
			return;
		}
	}

	if (vid->have_first && vid->held_v == v - 1) {
		if (flag != f) {
			/* The second field; after a tentative first field
			 * (two second ones in a row) that one was misread.
			 */
			if (vid->held_parity != f)
				vid->parity_fixes++;
			dc30_copy_rows(vid, flag, !f);
			dc30_raw_out(vid, v, false, &second);
			return;
		}
		if (vid->held_parity == f) {
			/* A first field again: tentatively the second. */
			dc30_copy_rows(vid, flag, !f);
			vid->raw_second = second;
			vid->raw_pending = true;
			return;
		}
	}

	/* A first field, or a second one without its first right before
	 * it - misread, or a jump: tentatively the first.
	 */
	dc30_raw_first(vid, flag, v, now, mark);
}

/* ---- MJPEG frames ----
 *
 * A frame's status appears in the code buffer table a little after its
 * second field (the ZR36050 finishes the last strips first). The frame
 * counter in the status numbers the frames the ZR36057 started, dropped
 * ones included, so it gives the sequence; the field interrupts that
 * ended the frame's two fields are found from where the first frame
 * showed up (jpeg_vbase) plus two per frame. Their times and audio
 * positions come from the field log. After a codec restart the counter
 * starts over, so the mapping is set up again from the field count.
 */

/* Metadata of the field that interrupt 'v' ended, if still in the log. */
static bool dc30_jpeg_field(struct dc30_video *vid, s64 v, unsigned int parity,
			    struct dc30_meta_field *f, u64 *ns)
{
	const struct dc30_field_log *l;

	memset(f, 0, sizeof(*f));
	if (v < 1 || v > (s64)vid->vsyncs ||
	    vid->vsyncs - v >= DC30_FIELD_LOG)
		return false;
	l = &vid->fieldlog[v % DC30_FIELD_LOG];
	if (l->vsync != v)
		return false;
	dc30_meta_field_set(f, parity, l->ns, &l->mark);
	if (ns)
		*ns = l->ns;
	return true;
}

/* Count where the frames show up; after DC30_JPEG_CALIB_FRAMES move the
 * mapping so that the delay seen most becomes jpeg_field_delay. Caller
 * holds vid->qlock.
 */
static void dc30_jpeg_calibrate(struct dc30_video *vid, s64 delay,
				s64 *first_v)
{
	unsigned int i, n = 0, mode = 0;
	s64 shift;

	vid->jpeg_delays[clamp_t(s64, delay, 0, DC30_JPEG_DELAYS - 1)]++;
	for (i = 0; i < DC30_JPEG_DELAYS; i++) {
		n += vid->jpeg_delays[i];
		if (vid->jpeg_delays[i] > vid->jpeg_delays[mode])
			mode = i;
	}
	if (n < DC30_JPEG_CALIB_FRAMES)
		return;

	shift = (s64)mode - dc30_jpeg_field_delay;
	if (shift) {
		vid->jpeg_vbase += shift;
		*first_v += shift;
		vid->jpeg_shifts++;
	}
	vid->jpeg_calibrated = true;
}

/* Parity of the field that interrupt 'v' ended, if still in the log;
 * -1 if not or unknown.
 */
static int dc30_jpeg_log_parity(struct dc30_video *vid, s64 v)
{
	const struct dc30_field_log *l;

	if (v < 1 || v > (s64)vid->vsyncs ||
	    vid->vsyncs - v >= DC30_FIELD_LOG)
		return -1;
	l = &vid->fieldlog[v % DC30_FIELD_LOG];
	return l->vsync == v ? l->parity : -1;
}

/* The codec pairs fields by count, the first field it sees after its GO
 * with the next one. Two fields of the same parity in a row (the decoder
 * settling after a start, a cut on an edited tape, a field jump on a worn
 * one) leave it pairing each second field with the next first one, until
 * the next such jump: spatially and temporally the wrong way round.
 * dc30.sys leaves it at that (sections with the wrong field order in the
 * AVI). Restarting the codec, as earlier versions of this driver did, cost
 * 8-9 frames at every cut of an edited tape. Each frame holds one complete
 * JPEG per field, so instead the fields go out one by one by their parity
 * from the field log: a first field is held in a buffer and completed by a
 * second field from the next field interrupt, whether that one is in the
 * same codec frame or the next. The one field a jump leaves without a
 * partner becomes a frame of its own, so no field from the tape is lost.
 */

/* Length of the JPEG at d (through its EOI), 0 if there is none. Marker
 * segments up to SOS, then entropy-coded data, where the ZR36050 stuffs
 * every 0xFF with 0x00 and writes no RST markers (DRI off): the first
 * other marker there is EOI. Looks at 8 bytes per step for an 0xFF, a
 * field is ~120 KB and this runs in the field interrupt.
 */
static unsigned int dc30_jpeg_end(const u8 *d, unsigned int len)
{
	const u64 ones = 0x0101010101010101ULL;
	unsigned int i = 2;
	u8 marker;

	if (len < 4 || d[0] != 0xff || d[1] != 0xd8)
		return 0;
	do {
		if (i + 4 > len || d[i] != 0xff)
			return 0;
		marker = d[i + 1];
		i += 2 + get_unaligned_be16(d + i + 2);
	} while (marker != 0xda);

	while (i + 1 < len) {
		if (i + 8 < len) {
			u64 w = ~get_unaligned((const u64 *)(d + i));

			if (!((w - ones) & ~w & (ones << 7))) {
				i += 8;
				continue;
			}
		}
		if (d[i] == 0xff && d[i + 1] != 0x00 && d[i + 1] != 0xff) {
			if (d[i + 1] == 0xd9)
				return i + 2;
			return 0;
		}
		i++;
	}
	return 0;
}

/* A frame is complete in vid->active: 'len' bytes, its first field ended
 * by interrupt 'v0', its second by 'v1' (the same for a single field).
 * The sequence number is the frame slot the first field falls in:
 * fields on the codec's grid (jpeg_vbase + 2n) start slot n, the ones in
 * between round up. A jump puts one field more into a stretch than whole
 * frames take; jpeg_seq_off then moves the rest of the stream one slot on.
 * Caller holds vid->qlock.
 */
static void dc30_jpeg_frame_out(struct dc30_video *vid, unsigned int len,
				s64 v0, s64 v1, bool crossed, u64 now)
{
	struct vb2_v4l2_buffer *vbuf = vid->active;
	struct dc30_meta_field second;
	u64 ts = now;
	s64 seq;

	seq = ((v0 - vid->jpeg_vbase + 1) >> 1) + vid->jpeg_seq_off;
	if (seq <= vid->last_sequence) {
		vid->jpeg_seq_off += vid->last_sequence + 1 - seq;
		seq = vid->last_sequence + 1;
	}

	vid->active = NULL;
	vid->have_first = false;
	vb2_set_plane_payload(&vbuf->vb2_buf, 0, len);
	dc30_jpeg_field(vid, v1, v0 == v1 ? vid->held_parity :
					    !vid->first_parity, &second, &ts);
	if (v0 == v1) {
		vid->fields[vid->held_parity]++;
	} else {
		vid->fields[vid->first_parity]++;
		vid->fields[!vid->first_parity]++;
	}

	if (v0 != v1 && crossed != vid->jpeg_crossed) {
		vid->jpeg_crossed = crossed;
		vid->jpeg_pair_switches++;
	}
	if (crossed)
		vid->jpeg_crossed_frames++;

	if (vid->last_sequence >= 0 && seq > vid->last_sequence + 1)
		vid->frames_dropped += seq - vid->last_sequence - 1;
	vid->last_sequence = seq;
	vid->frames_done++;
	vid->jpeg_bytes += len;

	vbuf->vb2_buf.timestamp = ts;
	vbuf->sequence = seq;
	vbuf->field = (vid->std & V4L2_STD_NTSC) ? V4L2_FIELD_SEQ_BT :
						   V4L2_FIELD_SEQ_TB;
	vb2_buffer_done(&vbuf->vb2_buf, VB2_BUF_STATE_DONE);

	dc30_meta_done(vid, seq, ts, &second);
}

/* The held first field never got its second one. It goes out as a frame
 * of its own (its JPEG twice: the picture at half the vertical
 * resolution) into the frame slot it falls in, if that slot is still
 * free - a lone field never moves the stream on, so a run of them (tape
 * noise, picture search) cannot stretch it. Caller holds vid->qlock.
 */
static void dc30_jpeg_flush_held(struct dc30_video *vid, u64 now)
{
	struct vb2_v4l2_buffer *vbuf = vid->active;
	struct dc30_buffer *buf;
	s64 seq;
	u8 *p;

	if (!vbuf)
		return;
	if (vid->have_first && vid->last_sequence >= 0)
		vid->field_order++;

	seq = ((vid->held_v - vid->jpeg_vbase + 1) >> 1) + vid->jpeg_seq_off;
	if (vid->have_first && seq > vid->last_sequence &&
	    2 * vid->held_len <= vb2_plane_size(&vbuf->vb2_buf, 0)) {
		p = vb2_plane_vaddr(&vbuf->vb2_buf, 0);
		memcpy(p + vid->held_len, p, vid->held_len);
		vid->jpeg_single_fields++;
		dc30_jpeg_frame_out(vid, 2 * vid->held_len, vid->held_v,
				    vid->held_v, vid->jpeg_crossed, now);
		return;
	}

	buf = container_of(vbuf, struct dc30_buffer, vb);
	list_add(&buf->list, &vid->pending);
	vid->active = NULL;
	vid->have_first = false;
}

/* A field's JPEG into a new buffer, to be completed by the next field.
 * Caller holds vid->qlock.
 */
static void dc30_jpeg_hold(struct dc30_video *vid, const u8 *d,
			   unsigned int len, s64 v, unsigned int parity,
			   u64 now)
{
	struct dc30_buffer *buf;

	dc30_jpeg_flush_held(vid, now);
	if (list_empty(&vid->pending)) {
		vid->no_buffer++;
		return;
	}
	buf = list_first_entry(&vid->pending, struct dc30_buffer, list);
	list_del(&buf->list);
	vid->active = &buf->vb;
	memcpy(vb2_plane_vaddr(&buf->vb.vb2_buf, 0), d, len);
	dc30_jpeg_field(vid, v, parity, &vid->first_field, NULL);
	vid->have_first = true;
	vid->held_v = v;
	vid->held_parity = parity;
	vid->held_len = len;
}

/* One field's JPEG, ended by interrupt 'v', in codec frame 'codec'.
 * Caller holds vid->qlock.
 */
static void dc30_jpeg_half(struct dc30_video *vid, const u8 *d,
			   unsigned int len, s64 v, unsigned int parity,
			   s64 codec, u64 now)
{
	/* A second field needs the first field right before it; without
	 * one it stands alone, as the first field of its own frame.
	 */
	if (parity == vid->first_parity || !vid->have_first ||
	    vid->held_v != v - 1) {
		dc30_jpeg_hold(vid, d, len, v, parity, now);
		if (parity != vid->first_parity)
			dc30_jpeg_flush_held(vid, now);
		return;
	}
	if (vid->held_len + len >
	    vb2_plane_size(&vid->active->vb2_buf, 0)) {
		vid->jpeg_oversize++;
		dc30_jpeg_flush_held(vid, now);
		return;
	}
	memcpy(vb2_plane_vaddr(&vid->active->vb2_buf, 0) + vid->held_len,
	       d, len);
	/* Field 0 of its codec frame: the first came from the one before. */
	dc30_jpeg_frame_out(vid, vid->held_len + len, v - 1, v,
			    v == vid->jpeg_vbase + 2 * codec, now);
}

/* A gap in the ZR36057 frame counter: frames the codec did not finish
 * (dc30.sys counts them as lost too, Sync_SetFrames). A failed frame
 * gets no status at all, so the gap is all
 * there is. The log line has the field flags since the frame before -
 * whether irregular fields come with it. Caller holds vid->qlock.
 */
static void dc30_jpeg_skipped(struct dc30_video *vid, unsigned int n,
			      s64 seen)
{
	char flags[DC30_FIELD_LOG + 1], isr[DC30_FIELD_LOG + 1];
	char nol[DC30_FIELD_LOG * 4 + 1];
	static const char isr_c[] = ".RIX";
	const struct dc30_field_log *l;
	s64 v, from = max_t(s64, vid->jpeg_prev_seen - 7,
			    seen - DC30_FIELD_LOG + 1);
	unsigned int i = 0, n_nol = 0;
	int p;

	vid->jpeg_skipped += n;
	vid->jpeg_skips++;
	if (!__ratelimit(&vid->jpeg_skip_rs))
		return;
	nol[0] = '\0';
	for (v = from; v <= seen; v++) {
		p = dc30_jpeg_log_parity(vid, v);
		flags[i] = p < 0 ? '?' : p ? 'B' : 'T';
		l = &vid->fieldlog[v % DC30_FIELD_LOG];
		isr[i] = l->vsync == v ? isr_c[l->isr & 3] : '?';
		if (l->vsync == v && l->nol >= 0)
			n_nol += scnprintf(nol + n_nol, sizeof(nol) - n_nol,
					   " %d", l->nol);
		i++;
	}
	flags[i] = '\0';
	isr[i] = '\0';
	dev_info(&vid->dc30->pdev->dev,
		 "MJPEG: codec skipped %u frame(s) after frame %lld, %lld fields since the last one, fields %s, irq %s (from %lld fields before the last frame)%s%s\n",
		 n, vid->jpeg_last_codec, seen - vid->jpeg_prev_seen, flags,
		 isr, vid->jpeg_prev_seen - from, n_nol ? ", NOL" : "", nol);
}

/* 'seen' is the field interrupt that counts as the first to see the
 * frame done (dc30_jpeg_vsync()). Caller holds vid->qlock.
 */
static void dc30_jpeg_frame_done(struct dc30_video *vid,
				 const struct dc30_jpeg_frame *f, u64 now,
				 s64 seen)
{
	struct dc30_buffer *buf;
	unsigned int len0, start1, len1;
	s64 seq, first_v, delay;
	int p0, p1;

	vid->jpeg_last_frame_vsync = vid->vsyncs;
	vid->jpeg_frames_since++;

	if (!vid->jpeg_based) {
		/* Settling: the first frame of a start is coded with the
		 * starting scale factor, and the bit rate control then loses
		 * a few (board: frame counter 0, then 4). Take frames only
		 * from two in a row on - by the frame counter, and seen two
		 * field interrupts apart: the field mapping comes from this
		 * frame, and the pairing relies on it from here on.
		 */
		if (!vid->jpeg_have_prev ||
		    (u8)(f->fcnt - vid->jpeg_prev_fcnt) != 1 ||
		    seen - vid->jpeg_prev_seen != 2) {
			vid->jpeg_have_prev = true;
			vid->jpeg_prev_fcnt = f->fcnt;
			vid->jpeg_prev_seen = seen;
			vid->jpeg_settle++;
			return;
		}

		/* The interrupt that ended field 0 of this frame. */
		first_v = seen - dc30_jpeg_field_delay - 1;
		if (vid->jpeg_last_codec < 0) {
			vid->jpeg_vbase = first_v;
			seq = 0;
		} else {
			/* After a restart: the codec frames go on where the
			 * fields have got to, the field mapping comes from
			 * this frame. The codec may now pair the other way
			 * round, and rounded onto the old grid the pairing
			 * read the wrong fields (seen on the board; the
			 * calibration used to hide that after 25 frames).
			 */
			seq = DIV_ROUND_CLOSEST(first_v - vid->jpeg_vbase, 2);
			vid->jpeg_vbase = first_v - 2 * seq;
		}
		vid->jpeg_fcnt = f->fcnt;
		vid->jpeg_fcnt_off = seq - f->fcnt;
		vid->jpeg_based = true;
		vid->jpeg_calibrated = false;
		memset(vid->jpeg_delays, 0, sizeof(vid->jpeg_delays));
	} else {
		u8 step = f->fcnt - (u8)vid->jpeg_fcnt;

		if (step > 1)
			dc30_jpeg_skipped(vid, step - 1, seen);
		vid->jpeg_fcnt += step;
		seq = vid->jpeg_fcnt + vid->jpeg_fcnt_off;
	}
	vid->jpeg_prev_seen = seen;

	vid->jpeg_max_len = max(vid->jpeg_max_len, f->len);
	if (f->len > DC30_JPEG_MAX_FRAME) {
		vid->jpeg_oversize++;
		return;
	}
	if (!f->len || seq <= vid->jpeg_last_codec) {
		vid->jpeg_bad++;
		return;
	}
	vid->jpeg_last_codec = seq;

	/* Should stay at jpeg_field_delay; more is interrupt latency, less
	 * would mean the mapping is off.
	 */
	first_v = vid->jpeg_vbase + 2 * seq;
	delay = seen - (first_v + 1);
	if (!vid->jpeg_calibrated) {
		dc30_jpeg_calibrate(vid, delay, &first_v);
	} else {
		vid->jpeg_delay_min = min(vid->jpeg_delay_min, delay);
		vid->jpeg_delay_max = max(vid->jpeg_delay_max, delay);
	}

	/* Parity not in the log (handler more than DC30_FIELD_LOG fields
	 * late): as the codec paired.
	 */
	p0 = dc30_jpeg_log_parity(vid, first_v);
	p1 = dc30_jpeg_log_parity(vid, first_v + 1);
	if (p0 < 0)
		p0 = p1 < 0 ? vid->first_parity : !p1;
	if (p1 < 0)
		p1 = !p0;

	/* Paired right, or userspace pairs the fields itself: the frame as
	 * it is, no need to split it.
	 */
	if (!READ_ONCE(vid->pair_by_flag) ||
	    (p0 == vid->first_parity && p1 != vid->first_parity)) {
		dc30_jpeg_flush_held(vid, now);
		if (list_empty(&vid->pending)) {
			vid->no_buffer++;
			return;
		}
		buf = list_first_entry(&vid->pending, struct dc30_buffer, list);
		list_del(&buf->list);
		vid->active = &buf->vb;
		memcpy(vb2_plane_vaddr(&buf->vb.vb2_buf, 0), f->data, f->len);
		dc30_jpeg_field(vid, first_v, p0, &vid->first_field, NULL);
		dc30_jpeg_frame_out(vid, f->len, first_v, first_v + 1, false,
				    now);
		return;
	}

	len0 = dc30_jpeg_end(f->data, f->len);
	start1 = len0;
	len1 = len0 ? dc30_jpeg_end(f->data + start1, f->len - start1) : 0;
	if (!len1) {
		vid->jpeg_bad++;
		dc30_jpeg_flush_held(vid, now);
		return;
	}
	dc30_jpeg_half(vid, f->data, len0, first_v, p0, seq, now);
	dc30_jpeg_half(vid, f->data + start1, len1, first_v + 1, p1, seq, now);
}

/* MJPEG: the field log's flag of the field before this interrupt's, now
 * that the next one is known. A misread flag shows as two repeats in a
 * row, a field jump as one (see the raw fields above).
 */
static void dc30_jpeg_fix_parity(struct dc30_video *vid)
{
	s64 n = vid->vsyncs;
	struct dc30_field_log *a, *b, *c;

	if (n < 3)
		return;
	a = &vid->fieldlog[(n - 2) % DC30_FIELD_LOG];
	b = &vid->fieldlog[(n - 1) % DC30_FIELD_LOG];
	c = &vid->fieldlog[n % DC30_FIELD_LOG];
	if (a->vsync != n - 2 || b->vsync != n - 1 ||
	    a->parity < 0 || b->parity < 0 || c->parity < 0)
		return;
	if (b->parity == a->parity && c->parity == b->parity) {
		b->parity = !a->parity;
		vid->parity_fixes++;
	}
}

/* Field interrupt, MJPEG stream. Caller holds vid->qlock. */
static void dc30_jpeg_vsync(struct dc30_video *vid, u64 now)
{
	struct dc30_dev *dc30 = vid->dc30;
	struct dc30_jpeg_frame f;
	u64 limit;

	if (vid->jpeg_paused)
		return;

	if (READ_ONCE(dc30->irq_isr) & ZR36057_ISR_JPEGREPIRQ)
		vid->jpeg_rep_since++;
	/* A frame is done ~0.2 ms after the interrupt that ends its second
	 * field, and this handler looks only after the audio counter, whose
	 * PostOffice reads wait up to ~90 us each for a FIFO block of the
	 * audio drain. So a frame was seen at the next interrupt or already
	 * at this one, in long runs depending on how drain and fields lined
	 * up, and the calibration settled a field off in about half of the
	 * starts with audio (seen on the board). Only a frame done when this
	 * interrupt came in is taken, one per interrupt: one done later
	 * waits for the next interrupt, which also has the flag of the field
	 * after its second one (dc30_jpeg_fix_parity()).
	 */
	if (vid->jpeg_done_early && dc30_jpeg_next(dc30, &f)) {
		dc30_jpeg_frame_done(vid, &f, now, vid->vsyncs);
		dc30_jpeg_release(dc30);
	}

	limit = vid->jpeg_frames_since ? DC30_JPEG_WATCHDOG_FIELDS :
					 DC30_JPEG_START_FIELDS;
	if (!vid->jpeg_restart_pending &&
	    vid->vsyncs - vid->jpeg_last_frame_vsync > limit) {
		vid->jpeg_restart_pending = true;
		queue_work(system_unbound_wq, &vid->jpeg_restart_work);
	}
}

/* The ZR36050 can stop without a word (the GPL driver's experience too):
 * set it up and start it again.
 * The stream goes on; the lost frames show as a gap in the sequence.
 * Runs on the unbound workqueue: the start polls up to two fields for
 * the field change.
 */
static void dc30_jpeg_restart_work(struct work_struct *work)
{
	struct dc30_video *vid = container_of(work, struct dc30_video,
					      jpeg_restart_work);
	struct dc30_dev *dc30 = vid->dc30;
	unsigned int rep, fields;
	unsigned long flags;
	char state[320];
	int err;

	spin_lock_irqsave(&vid->qlock, flags);
	if (!vid->streaming || !vid->jpeg) {
		spin_unlock_irqrestore(&vid->qlock, flags);
		return;
	}
	vid->jpeg_paused = true;
	rep = vid->jpeg_rep_since;
	fields = vid->vsyncs - vid->jpeg_start_vsync;
	spin_unlock_irqrestore(&vid->qlock, flags);

	/* Where it got stuck, before the restart resets everything. */
	if (__ratelimit(&vid->jpeg_dump_rs)) {
		dc30_jpeg_dump(dc30, state, sizeof(state));
		dev_info(&dc30->pdev->dev,
			 "MJPEG stall: JPEGRepIRQ in %u of %u fields, %s\n",
			 rep, fields, state);
	}

	dc30_jpeg_stop(dc30);
	err = dc30_jpeg_start(dc30, vid->std, vid->jpeg_kbps);

	spin_lock_irqsave(&vid->qlock, flags);
	vid->jpeg_restarts++;
	vid->jpeg_have_prev = false;
	vid->jpeg_based = false;
	vid->jpeg_frames_since = 0;
	vid->jpeg_last_frame_vsync = vid->vsyncs;
	vid->jpeg_start_vsync = vid->vsyncs;
	vid->jpeg_rep_since = 0;
	vid->jpeg_paused = false;
	vid->jpeg_restart_pending = false;
	spin_unlock_irqrestore(&vid->qlock, flags);

	dev_warn_ratelimited(&dc30->pdev->dev,
			     "JPEG codec stalled, restarted%s (%d)\n",
			     err ? " - failed" : "", err);
}

void dc30_video_vsync_early(struct dc30_dev *dc30)
{
	struct dc30_video *vid = dc30->video;
	unsigned long flags;

	if (!vid)
		return;
	spin_lock_irqsave(&vid->qlock, flags);
	vid->jpeg_done_early = vid->streaming && vid->jpeg &&
			       !vid->jpeg_paused && dc30_jpeg_peek(dc30);
	spin_unlock_irqrestore(&vid->qlock, flags);
}

void dc30_video_vsync(struct dc30_dev *dc30,
		      const struct dc30_audio_mark *mark)
{
	struct dc30_video *vid = dc30->video;
	unsigned long flags;
	u64 now = ktime_get_ns();
	bool top, bottom;
	int parity = -1, nol = -1;
	bool nol_read = false;
	u32 isr = READ_ONCE(dc30->irq_isr);

	if (!vid)
		return;
	/* MJPEG: the pairing check needs each field's parity. Not while
	 * the codec restarts: nothing may delay its GO sequence
	 * (dc30_jpeg_start_at_field()). Outside the lock, it takes the
	 * PostOffice one.
	 */
	if (READ_ONCE(vid->jpeg) && READ_ONCE(vid->streaming) &&
	    !READ_ONCE(vid->jpeg_paused)) {
		parity = dc30_jpeg_ended_field(dc30);
		nol_read = READ_ONCE(dc30_jpeg_nol);
		if (nol_read)
			nol = dc30_jpeg_field_lines(dc30);
	}

	spin_lock_irqsave(&vid->qlock, flags);

	if (mark->valid)
		vid->last_mark = *mark;

	if (!vid->streaming)
		goto out;

	if (!vid->vsyncs)
		vid->first_vsync_ns = now;
	vid->last_vsync_ns = now;
	vid->vsyncs++;

	if (vid->jpeg) {
		struct dc30_field_log *l;

		l = &vid->fieldlog[vid->vsyncs % DC30_FIELD_LOG];
		l->vsync = vid->vsyncs;
		l->ns = now;
		l->mark = *mark;
		l->parity = parity;
		l->isr = (isr & ZR36057_ISR_JPEGREPIRQ ? DC30_LOG_REP : 0) |
			 (isr & ZR36057_ISR_GIRQ0 ? DC30_LOG_GIRQ0 : 0);
		l->nol = nol;
		if (nol_read) {
			if (nol < 0) {
				vid->nol_failed++;
			} else {
				vid->nol_fields++;
				vid->nol_min = min_t(unsigned int,
						     vid->nol_min, nol);
				vid->nol_max = max_t(unsigned int,
						     vid->nol_max, nol);
				if (nol != ALIGN(vid->geo.height / 2, 8))
					vid->nol_off++;
			}
		}
		dc30_jpeg_fix_parity(vid);
		dc30_jpeg_vsync(vid, now);
		goto out;
	}

	if (dc30_vfe_check_overflow(dc30))
		vid->overflows++;

	if (vid->vsyncs <= DC30_SETTLE_VSYNCS) {
		dc30_arm_sentinels(vid);
		goto out;
	}

	top = READ_ONCE(*dc30_field_sentinel(vid, 0)) != DC30_FIELD_SENTINEL;
	bottom = READ_ONCE(*dc30_field_sentinel(vid, 1)) != DC30_FIELD_SENTINEL;

	if (top && bottom) {
		/* This IRQ came more than a field late: which field is the
		 * newer one is unknown, so drop the frame in progress.
		 */
		vid->both_fields++;
		vid->have_first = false;
		vid->raw_pending = false;
		dc30_arm_sentinels(vid);
	} else if (top || bottom) {
		unsigned int parity = bottom ? 1 : 0;

		dc30_field_done(vid, parity, now, mark);
		/* Only after the copy - the sentinel is real pixel data. */
		WRITE_ONCE(*dc30_field_sentinel(vid, parity),
			   DC30_FIELD_SENTINEL);
	} else {
		vid->no_field++;
	}

out:
	spin_unlock_irqrestore(&vid->qlock, flags);
}

const char *dc30_video_input_name(struct dc30_dev *dc30)
{
	struct dc30_video *vid = dc30->video;

	return vid ? dc30_inputs[READ_ONCE(vid->input)].name : "none";
}

void dc30_video_stats_show(struct seq_file *m, struct dc30_dev *dc30)
{
	struct dc30_video *vid = dc30->video;
	u64 vsyncs, span_ns, done, dropped, top, bottom, no_field, both;
	u64 order, parity_fixes, single_fields, no_buffer, overflows;
	u64 meta_no_buffer;
	u64 jpeg_bytes, jpeg_restarts, jpeg_bad, jpeg_settle, jpeg_shifts;
	u64 jpeg_skipped, jpeg_skips;
	u64 nol_fields, nol_failed, nol_off;
	unsigned int nol_min, nol_max;
	u64 jpeg_oversize, jpeg_crossed, jpeg_switches, jpeg_single;
	unsigned int jpeg_max_len;
	s64 delay_min, delay_max;
	unsigned long flags;
	bool streaming, jpeg;

	if (!vid) {
		seq_puts(m, "video not registered\n");
		return;
	}

	spin_lock_irqsave(&vid->qlock, flags);
	streaming = vid->streaming;
	vsyncs = vid->vsyncs;
	span_ns = vid->last_vsync_ns - vid->first_vsync_ns;
	done = vid->frames_done;
	dropped = vid->frames_dropped;
	top = vid->fields[0];
	bottom = vid->fields[1];
	no_field = vid->no_field;
	both = vid->both_fields;
	order = vid->field_order;
	parity_fixes = vid->parity_fixes;
	single_fields = vid->single_fields;
	no_buffer = vid->no_buffer;
	overflows = vid->overflows;
	meta_no_buffer = vid->meta_no_buffer;
	jpeg = vid->jpeg;
	jpeg_bytes = vid->jpeg_bytes;
	jpeg_restarts = vid->jpeg_restarts;
	jpeg_skipped = vid->jpeg_skipped;
	jpeg_skips = vid->jpeg_skips;
	nol_fields = vid->nol_fields;
	nol_failed = vid->nol_failed;
	nol_off = vid->nol_off;
	nol_min = vid->nol_min;
	nol_max = vid->nol_max;
	jpeg_bad = vid->jpeg_bad;
	jpeg_settle = vid->jpeg_settle;
	jpeg_oversize = vid->jpeg_oversize;
	jpeg_max_len = vid->jpeg_max_len;
	jpeg_shifts = vid->jpeg_shifts;
	jpeg_crossed = vid->jpeg_crossed_frames;
	jpeg_switches = vid->jpeg_pair_switches;
	jpeg_single = vid->jpeg_single_fields;
	delay_min = vid->jpeg_delay_min;
	delay_max = vid->jpeg_delay_max;
	spin_unlock_irqrestore(&vid->qlock, flags);

	seq_printf(m, "streaming      %s\n", streaming ? "yes" : "no");
	seq_printf(m, "vsyncs         %llu\n", vsyncs);
	if (vsyncs > 1 && span_ns)
		seq_printf(m, "vsync rate     %llu.%02llu Hz\n",
			   div64_u64((vsyncs - 1) * NSEC_PER_SEC, span_ns),
			   div64_u64((vsyncs - 1) * NSEC_PER_SEC * 100, span_ns) % 100);
	seq_printf(m, "fields         %llu top, %llu bottom\n", top, bottom);
	seq_printf(m, "frames done    %llu\n", done);
	seq_printf(m, "frames dropped %llu\n", dropped);
	seq_printf(m, "no field       %llu\n", no_field);
	seq_printf(m, "late irq       %llu\n", both);
	seq_printf(m, "field order    %llu\n", order);
	seq_printf(m, "parity fixes   %llu\n", parity_fixes);
	if (!jpeg)
		seq_printf(m, "single fields  %llu\n", single_fields);
	seq_printf(m, "no buffer      %llu\n", no_buffer);
	seq_printf(m, "fifo overflows %llu\n", overflows);
	seq_printf(m, "meta no buffer %llu\n", meta_no_buffer);
	if (!jpeg)
		return;
	seq_printf(m, "format         MJPEG\n");
	if (done)
		seq_printf(m, "bytes/frame    %llu\n", div64_u64(jpeg_bytes, done));
	seq_printf(m, "codec restarts %llu\n", jpeg_restarts);
	seq_printf(m, "codec skipped  %llu frames in %llu gaps\n",
		   jpeg_skipped, jpeg_skips);
	if (nol_fields || nol_failed)
		seq_printf(m, "016 lines      %u..%u, %llu of %llu fields not %u, %llu reads failed\n",
			   nol_fields ? nol_min : 0, nol_max, nol_off,
			   nol_fields, ALIGN(vid->geo.height / 2, 8),
			   nol_failed);
	seq_printf(m, "pairing        %llu switches, %llu frames across codec frames, %llu single fields\n",
		   jpeg_switches, jpeg_crossed, jpeg_single);
	seq_printf(m, "largest frame  %u bytes\n", jpeg_max_len);
	seq_printf(m, "oversize       %llu (> %u bytes, dropped)\n",
		   jpeg_oversize, DC30_JPEG_MAX_FRAME);
	seq_printf(m, "bad frames     %llu\n", jpeg_bad);
	seq_printf(m, "settling       %llu frames skipped\n", jpeg_settle);
	seq_printf(m, "mapping fixes  %llu\n", jpeg_shifts);
	if (delay_min <= delay_max)
		seq_printf(m, "ready after    %lld..%lld field irqs (expected %u)\n",
			   delay_min, delay_max, dc30_jpeg_field_delay);
}

/* ---- ioctls ---- */

/* The data rate is read at stream start, and grabbed while streaming.
 * The pairing takes effect with the next frame.
 */
static int dc30_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct dc30_video *vid = container_of(ctrl->handler, struct dc30_video,
					      hdl);

	if (ctrl->id == DC30_CID_PAIR_BY_FLAG)
		WRITE_ONCE(vid->pair_by_flag, ctrl->val);
	return 0;
}

static const struct v4l2_ctrl_ops dc30_ctrl_ops = {
	.s_ctrl = dc30_s_ctrl,
};

static const struct v4l2_ctrl_config dc30_pair_ctrl = {
	.ops = &dc30_ctrl_ops,
	.id = DC30_CID_PAIR_BY_FLAG,
	.name = "Pair Fields by Flag",
	.type = V4L2_CTRL_TYPE_BOOLEAN,
	.min = 0,
	.max = 1,
	.step = 1,
	.def = 1,
};

static int dc30_querycap(struct file *file, void *priv,
			  struct v4l2_capability *cap)
{
	struct dc30_video *vid = video_drvdata(file);

	strscpy(cap->driver, "dc30", sizeof(cap->driver));
	strscpy(cap->card, "miro DC30", sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "PCI:%s",
		 pci_name(vid->dc30->pdev));
	return 0;
}

static int dc30_enum_fmt_vid_cap(struct file *file, void *priv,
				  struct v4l2_fmtdesc *f)
{
	static const u32 formats[] = { V4L2_PIX_FMT_YUYV, V4L2_PIX_FMT_MJPEG };

	if (f->index >= ARRAY_SIZE(formats))
		return -EINVAL;
	f->pixelformat = formats[f->index];
	return 0;
}

static int dc30_g_fmt_vid_cap(struct file *file, void *priv,
			       struct v4l2_format *f)
{
	dc30_fill_pix_format(video_drvdata(file), &f->fmt.pix);
	return 0;
}

/* One mode per format and standard: TRY/S_FMT take the pixel format
 * (anything but MJPEG means YUYV) and report the rest back.
 */
static int dc30_try_fmt_vid_cap(struct file *file, void *priv,
				 struct v4l2_format *f)
{
	struct dc30_video *vid = video_drvdata(file);

	dc30_pix_format(vid->std,
			f->fmt.pix.pixelformat == V4L2_PIX_FMT_MJPEG,
			&f->fmt.pix);
	return 0;
}

static int dc30_s_fmt_vid_cap(struct file *file, void *priv,
			       struct v4l2_format *f)
{
	struct dc30_video *vid = video_drvdata(file);

	if (vb2_is_busy(&vid->queue))
		return -EBUSY;
	vid->jpeg = f->fmt.pix.pixelformat == V4L2_PIX_FMT_MJPEG;
	dc30_update_format(vid);
	dc30_fill_pix_format(vid, &f->fmt.pix);
	return 0;
}

static int dc30_g_std(struct file *file, void *priv, v4l2_std_id *std)
{
	*std = ((struct dc30_video *)video_drvdata(file))->std;
	return 0;
}

static int dc30_s_std(struct file *file, void *priv, v4l2_std_id std)
{
	struct dc30_video *vid = video_drvdata(file);
	struct dc30_dev *dc30 = vid->dc30;
	int err;

	/* Re-setting the current standard must succeed even while buffers
	 * are allocated (v4l2-compliance testCanSetSameTimings).
	 */
	if (std == vid->std)
		return 0;
	if (vb2_is_busy(&vid->queue))
		return -EBUSY;

	err = v4l2_subdev_call(dc30_decoder_sd(dc30), video, s_std, std);
	if (err)
		return err;
	dc30_decoder_changed(dc30);

	vid->std = std;
	dc30_update_format(vid);
	dc30_vfe_set_geometry(dc30, std, &vid->geo);
	return 0;
}

static int dc30_enum_input(struct file *file, void *priv,
			    struct v4l2_input *inp)
{
	struct dc30_video *vid = video_drvdata(file);

	if (inp->index >= ARRAY_SIZE(dc30_inputs))
		return -EINVAL;
	strscpy(inp->name, dc30_inputs[inp->index].name, sizeof(inp->name));
	inp->type = V4L2_INPUT_TYPE_CAMERA;
	inp->std = V4L2_STD_PAL | V4L2_STD_NTSC | V4L2_STD_SECAM;
	inp->capabilities = V4L2_IN_CAP_STD;

	/* The decoder only reports lock for the input it is switched to. */
	if (inp->index == vid->input)
		v4l2_subdev_call(dc30_decoder_sd(vid->dc30), video,
				 g_input_status, &inp->status);
	return 0;
}

static int dc30_g_input(struct file *file, void *priv, unsigned int *i)
{
	*i = ((struct dc30_video *)video_drvdata(file))->input;
	return 0;
}

static int dc30_s_input(struct file *file, void *priv, unsigned int i)
{
	struct dc30_video *vid = video_drvdata(file);
	struct dc30_dev *dc30 = vid->dc30;
	int err;

	if (i >= ARRAY_SIZE(dc30_inputs))
		return -EINVAL;

	err = v4l2_subdev_call(dc30_decoder_sd(dc30), video, s_routing,
				dc30_inputs[i].muxsel, 0, 0);
	if (err)
		return err;
	if (i != vid->input)
		dc30_decoder_changed(dc30);

	vid->input = i;
	return 0;
}

static const struct v4l2_ioctl_ops dc30_ioctl_ops = {
	.vidioc_querycap = dc30_querycap,
	.vidioc_enum_fmt_vid_cap = dc30_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap = dc30_g_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap = dc30_s_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap = dc30_try_fmt_vid_cap,
	.vidioc_g_std = dc30_g_std,
	.vidioc_s_std = dc30_s_std,
	.vidioc_enum_input = dc30_enum_input,
	.vidioc_g_input = dc30_g_input,
	.vidioc_s_input = dc30_s_input,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	.vidioc_log_status = v4l2_ctrl_log_status,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

/* ---- metadata node ioctls ---- */

static int dc30_enum_fmt_meta_cap(struct file *file, void *priv,
				  struct v4l2_fmtdesc *f)
{
	if (f->index != 0)
		return -EINVAL;
	f->pixelformat = DC30_META_FMT;
	/* Not a format the V4L2 core knows: it warns without a description. */
	strscpy(f->description, "DC30 A/V sync metadata", sizeof(f->description));
	return 0;
}

/* A capture node needs an input; the records follow the video node's. */
static int dc30_meta_enum_input(struct file *file, void *priv,
				struct v4l2_input *inp)
{
	if (inp->index != 0)
		return -EINVAL;
	strscpy(inp->name, "Metadata", sizeof(inp->name));
	inp->type = V4L2_INPUT_TYPE_CAMERA;
	return 0;
}

static int dc30_meta_g_input(struct file *file, void *priv, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int dc30_meta_s_input(struct file *file, void *priv, unsigned int i)
{
	return i ? -EINVAL : 0;
}

/* One fixed format; G/TRY/S_FMT all report it. */
static int dc30_fmt_meta_cap(struct file *file, void *priv,
			     struct v4l2_format *f)
{
	f->fmt.meta.dataformat = DC30_META_FMT;
	f->fmt.meta.buffersize = sizeof(struct dc30_meta);
	return 0;
}

static const struct v4l2_ioctl_ops dc30_meta_ioctl_ops = {
	.vidioc_querycap = dc30_querycap,
	.vidioc_enum_fmt_meta_cap = dc30_enum_fmt_meta_cap,
	.vidioc_g_fmt_meta_cap = dc30_fmt_meta_cap,
	.vidioc_s_fmt_meta_cap = dc30_fmt_meta_cap,
	.vidioc_try_fmt_meta_cap = dc30_fmt_meta_cap,
	.vidioc_enum_input = dc30_meta_enum_input,
	.vidioc_g_input = dc30_meta_g_input,
	.vidioc_s_input = dc30_meta_s_input,
	.vidioc_reqbufs = vb2_ioctl_reqbufs,
	.vidioc_querybuf = vb2_ioctl_querybuf,
	.vidioc_qbuf = vb2_ioctl_qbuf,
	.vidioc_dqbuf = vb2_ioctl_dqbuf,
	.vidioc_expbuf = vb2_ioctl_expbuf,
	.vidioc_create_bufs = vb2_ioctl_create_bufs,
	.vidioc_prepare_buf = vb2_ioctl_prepare_buf,
	.vidioc_streamon = vb2_ioctl_streamon,
	.vidioc_streamoff = vb2_ioctl_streamoff,
	/* The decoder's controls show up here too (v4l2_dev's handler). */
	.vidioc_log_status = v4l2_ctrl_log_status,
	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,
};

/* Both nodes: the board is awake and the decoder powered up while one is
 * open - so the input status is right at once and a stream starts
 * without waiting for the decoder to lock.
 */
static int dc30_open(struct file *file)
{
	struct dc30_video *vid = video_drvdata(file);
	int err;

	err = dc30_pm_get(vid->dc30);
	if (err)
		return err;
	err = dc30_decoder_get(vid->dc30, false);
	if (err)
		goto err_pm;
	err = v4l2_fh_open(file);
	if (err)
		goto err_decoder;
	return 0;

err_decoder:
	dc30_decoder_put(vid->dc30, false);
err_pm:
	dc30_pm_put(vid->dc30);
	return err;
}

static int dc30_release(struct file *file)
{
	struct dc30_video *vid = video_drvdata(file);
	int err;

	/* Stops a stream this file owns, before the decoder goes down. */
	err = vb2_fop_release(file);
	dc30_decoder_put(vid->dc30, false);
	dc30_pm_put(vid->dc30);
	return err;
}

static const struct v4l2_file_operations dc30_fops = {
	.owner = THIS_MODULE,
	.open = dc30_open,
	.release = dc30_release,
	.unlocked_ioctl = video_ioctl2,
	.read = vb2_fop_read,
	.mmap = vb2_fop_mmap,
	.poll = vb2_fop_poll,
};

static int dc30_meta_register(struct dc30_video *vid)
{
	struct vb2_queue *q = &vid->meta_queue;
	int err;

	q->type = V4L2_BUF_TYPE_META_CAPTURE;
	q->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF | VB2_READ;
	q->dev = &vid->dc30->pdev->dev;
	q->drv_priv = vid;
	q->buf_struct_size = sizeof(struct dc30_buffer);
	q->ops = &dc30_meta_vb2_ops;
	q->mem_ops = &vb2_vmalloc_memops;
	q->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	q->lock = &vid->meta_lock;

	err = vb2_queue_init(q);
	if (err)
		return err;

	vid->meta_vdev.fops = &dc30_fops;
	vid->meta_vdev.ioctl_ops = &dc30_meta_ioctl_ops;
	vid->meta_vdev.release = video_device_release_empty;
	vid->meta_vdev.v4l2_dev = &vid->v4l2_dev;
	vid->meta_vdev.queue = q;
	vid->meta_vdev.lock = &vid->meta_lock;
	vid->meta_vdev.device_caps = V4L2_CAP_META_CAPTURE |
				     V4L2_CAP_STREAMING | V4L2_CAP_READWRITE;
	strscpy(vid->meta_vdev.name, "dc30-meta", sizeof(vid->meta_vdev.name));
	video_set_drvdata(&vid->meta_vdev, vid);

	err = video_register_device(&vid->meta_vdev, VFL_TYPE_VIDEO, -1);
	if (err)
		vb2_queue_release(q);
	return err;
}

int dc30_video_register(struct dc30_dev *dc30)
{
	struct dc30_video *vid;
	struct v4l2_subdev *sd;
	int err;

	if (!dc30->decoder)
		return 0;

	sd = dc30_decoder_sd(dc30);
	if (!sd)
		return 0; /* i2c_client exists but vpx3220 never bound to it */

	vid = kzalloc(sizeof(*vid), GFP_KERNEL);
	if (!vid)
		return -ENOMEM;

	vid->dc30 = dc30;
	mutex_init(&vid->lock);
	mutex_init(&vid->meta_lock);
	spin_lock_init(&vid->qlock);
	INIT_LIST_HEAD(&vid->pending);
	INIT_LIST_HEAD(&vid->meta_pending);
	vid->std = V4L2_STD_PAL;
	vid->input = 0; /* Composite, matches vpx3220_probe()'s default */
	dc30_update_format(vid);
	INIT_WORK(&vid->jpeg_restart_work, dc30_jpeg_restart_work);
	ratelimit_state_init(&vid->jpeg_dump_rs, 60 * HZ, 5);
	ratelimit_state_init(&vid->jpeg_skip_rs, 60 * HZ, 10);

	/* PAL/SECAM is the largest geometry (see zr36057.c). */
	vid->bounce_size = vid->sizeimage;
	vid->bounce = dma_alloc_coherent(&dc30->pdev->dev, vid->bounce_size,
					 &vid->bounce_dma, GFP_KERNEL);
	if (!vid->bounce) {
		err = -ENOMEM;
		goto err_free;
	}

	err = v4l2_device_register(&dc30->pdev->dev, &vid->v4l2_dev);
	if (err)
		goto err_free_bounce;

	/* Registering the subdev merges its controls (brightness, contrast,
	 * saturation, hue - dc30_vpx3220.c) into this handler, which the video
	 * node then exposes.
	 */
	v4l2_ctrl_handler_init(&vid->hdl, 5);
	/* MJPEG data rate, bits/s (the ZR36050 meets it field by field). */
	vid->rate_ctrl = v4l2_ctrl_new_std(&vid->hdl, &dc30_ctrl_ops,
					   V4L2_CID_MPEG_VIDEO_BITRATE,
					   DC30_JPEG_RATE_MIN * 8000,
					   DC30_JPEG_RATE_MAX * 8000, 8000,
					   DC30_JPEG_RATE_DEF * 8000);
	vid->pair_by_flag = true;
	v4l2_ctrl_new_custom(&vid->hdl, &dc30_pair_ctrl, NULL);
	err = vid->hdl.error;
	if (err)
		goto err_free_hdl;
	vid->v4l2_dev.ctrl_handler = &vid->hdl;

	err = v4l2_device_register_subdev(&vid->v4l2_dev, sd);
	if (err)
		goto err_free_hdl;

	vid->queue.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	vid->queue.io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF | VB2_READ;
	vid->queue.dev = &dc30->pdev->dev;
	vid->queue.drv_priv = vid;
	vid->queue.buf_struct_size = sizeof(struct dc30_buffer);
	vid->queue.ops = &dc30_vb2_ops;
	vid->queue.mem_ops = &vb2_vmalloc_memops;
	vid->queue.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	vid->queue.min_queued_buffers = 2;
	vid->queue.lock = &vid->lock;

	err = vb2_queue_init(&vid->queue);
	if (err)
		goto err_unreg_subdev;

	vid->vdev.fops = &dc30_fops;
	vid->vdev.ioctl_ops = &dc30_ioctl_ops;
	vid->vdev.release = video_device_release_empty;
	vid->vdev.v4l2_dev = &vid->v4l2_dev;
	vid->vdev.queue = &vid->queue;
	vid->vdev.lock = &vid->lock;
	vid->vdev.device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING |
				 V4L2_CAP_READWRITE;
	vid->vdev.tvnorms = V4L2_STD_PAL | V4L2_STD_NTSC | V4L2_STD_SECAM;
	strscpy(vid->vdev.name, "dc30", sizeof(vid->vdev.name));
	video_set_drvdata(&vid->vdev, vid);

	err = video_register_device(&vid->vdev, VFL_TYPE_VIDEO, -1);
	if (err)
		goto err_release_queue;

	err = dc30_meta_register(vid);
	if (err)
		goto err_unreg_vdev;

	/* Safe to free vid synchronously in dc30_video_unregister(): the
	 * fops' THIS_MODULE owner pins dc30.ko while any fd is open, so
	 * rmmod (which drives dc30_remove() -> dc30_video_unregister())
	 * cannot run while a userspace reference still exists.
	 */
	dc30->video = vid;
	dev_info(&dc30->pdev->dev, "registered %s, metadata %s\n",
		 video_device_node_name(&vid->vdev),
		 video_device_node_name(&vid->meta_vdev));
	return 0;

err_unreg_vdev:
	video_unregister_device(&vid->vdev);
err_release_queue:
	vb2_queue_release(&vid->queue);
err_unreg_subdev:
	v4l2_device_unregister_subdev(sd);
err_free_hdl:
	v4l2_ctrl_handler_free(&vid->hdl);
	v4l2_device_unregister(&vid->v4l2_dev);
err_free_bounce:
	dma_free_coherent(&dc30->pdev->dev, vid->bounce_size, vid->bounce,
			  vid->bounce_dma);
err_free:
	kfree(vid);
	return err;
}

void dc30_video_unregister(struct dc30_dev *dc30)
{
	struct dc30_video *vid = dc30->video;

	if (!vid)
		return;

	dc30->video = NULL;
	video_unregister_device(&vid->meta_vdev);
	vb2_queue_release(&vid->meta_queue);
	video_unregister_device(&vid->vdev);
	vb2_queue_release(&vid->queue);
	v4l2_device_unregister(&vid->v4l2_dev);
	v4l2_ctrl_handler_free(&vid->hdl);
	dma_free_coherent(&dc30->pdev->dev, vid->bounce_size, vid->bounce,
			  vid->bounce_dma);
	kfree(vid);
}

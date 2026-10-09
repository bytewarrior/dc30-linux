// SPDX-License-Identifier: GPL-2.0-only
/*
 * dc30_jpeg.c - motion JPEG compression on the DC30
 *
 * Pixel path: VPX3220 -> ZR36016 (raster to 8x8 blocks) -> ZR36050 (JPEG)
 * -> ZR36057 code DMA -> memory. Every frame lands as two complete
 * baseline JPEGs, one per field (AVI1 style, frame mode), in one of four
 * code buffers. The ZR36057 writes a status word into the buffer's entry
 * of the code buffer table ("stat_com") when a frame is done: bit 0 set,
 * length and a frame counter. It sends no interrupt for that here - as
 * dc30.sys, the driver looks at the table on every field interrupt.
 *
 * The code buffers are driver-owned coherent memory, one fragment each;
 * frames are copied out to vb2 (a few hundred KB per frame, cheap next to
 * the raw path's 885 KB).
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/pci.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <media/v4l2-subdev.h>

#include "dc30.h"
#include "dc30_alsa.h"
#include "dc30_jpeg.h"
#include "dc30_power.h"
#include "zr36016.h"
#include "zr36050.h"
#include "zr36057.h"

#define DC30_JPEG_BUFS		4	/* entries in the code buffer table */

/* The fragment length is written in bytes, as dc30.sys does. The
 * datasheet counts it in doublewords (the GPL driver writes dwords * 2),
 * and the board follows the datasheet: with 256 KB written, it completed
 * frames larger than that. The ZR36057 may thus fill twice the written
 * length, so every buffer is that large. Frames over DC30_JPEG_MAX_FRAME
 * do not fit the vb2 buffer and are dropped as oversize.
 */
#define DC30_JPEG_BUF_ALLOC	(2 * DC30_JPEG_MAX_FRAME)

#define DC30_STAT_COM_DONE	BIT(0)

/* Where the active field starts, counted from the ZR36016's HIN/VIN, i.e.
 * inside the VPX3220's window 1: 304 lines per PAL field from line 7
 * (dc30_vpx3220.c). NAY + PAY must fit in there - with 17 + 288 the
 * ZR36016 waited for a line that never came and no field completed
 * on the board). The raw path's VFE starts at line 17 of the same
 * window, it seems to count one line more.
 */
/* Pixel 1, as the raw path's VFE (HStart is odd there): 4:2:2 chroma
 * alternates Cb/Cr per pixel, and starting at pixel 0 swapped the two
 * (on the board: yellow came out cyan).
 */
static unsigned int dc30_jpeg_nax = 1;
module_param_named(jpeg_nax, dc30_jpeg_nax, uint, 0644);
MODULE_PARM_DESC(jpeg_nax, "JPEG: first pixel of a line, from HIN; odd, or Cb/Cr swap (default 1)");

static unsigned int dc30_jpeg_nay = 16;
module_param_named(jpeg_nay, dc30_jpeg_nay, uint, 0644);
MODULE_PARM_DESC(jpeg_nay, "JPEG: first line of a field, from VIN (default 16)");

static unsigned int dc30_jpeg_test_rate = DC30_JPEG_RATE_DEF;
module_param_named(jpeg_test_rate, dc30_jpeg_test_rate, uint, 0644);
MODULE_PARM_DESC(jpeg_test_rate, "JPEG data rate of the debugfs jpeg_test, kB/s (default 6000)");

struct dc30_jpeg {
	struct dc30_dev *dc30;

	void *buf[DC30_JPEG_BUFS];
	dma_addr_t buf_dma[DC30_JPEG_BUFS];

	/* stat_com[4] followed by one fragment table entry per buffer. */
	__le32 *table;
	dma_addr_t table_dma;

	unsigned int next;	/* stat_com entry the next frame goes to */
	bool running;
	unsigned int go_us;	/* GO sequence of the last start */

	/* Last debugfs test, for the "jpeg" file. */
	struct mutex test_lock;
	void *test_frame;
	unsigned int test_len;
	char test_log[1024];
};

#define DC30_JPEG_TABLE_BYTES	(DC30_JPEG_BUFS * 4 + DC30_JPEG_BUFS * 8)

static dma_addr_t dc30_frag_dma(struct dc30_jpeg *jp, unsigned int i)
{
	return jp->table_dma + DC30_JPEG_BUFS * 4 + i * 8;
}

static __le32 *dc30_frag(struct dc30_jpeg *jp, unsigned int i)
{
	return jp->table + DC30_JPEG_BUFS + i * 2;
}

/* Hand entry 'i' (back) to the ZR36057: a command pointing at the
 * buffer's fragment table, STAT_BIT clear.
 */
static void dc30_stat_com_arm(struct dc30_jpeg *jp, unsigned int i)
{
	wmb();	/* done with the buffer before the ZR36057 may refill it */
	WRITE_ONCE(jp->table[i], cpu_to_le32((u32)dc30_frag_dma(jp, i)));
}

int dc30_jpeg_alloc(struct dc30_dev *dc30)
{
	struct dc30_jpeg *jp = dc30->jpeg;
	struct device *dev = &dc30->pdev->dev;
	unsigned int i;

	jp->table = dma_alloc_coherent(dev, DC30_JPEG_TABLE_BYTES,
				       &jp->table_dma, GFP_KERNEL);
	if (!jp->table)
		return -ENOMEM;

	for (i = 0; i < DC30_JPEG_BUFS; i++) {
		jp->buf[i] = dma_alloc_coherent(dev, DC30_JPEG_BUF_ALLOC,
						&jp->buf_dma[i], GFP_KERNEL);
		if (!jp->buf[i]) {
			dc30_jpeg_free(dc30);
			return -ENOMEM;
		}
		*dc30_frag(jp, i) = cpu_to_le32((u32)jp->buf_dma[i]);
		*(dc30_frag(jp, i) + 1) = cpu_to_le32(DC30_JPEG_MAX_FRAME | 1);
	}
	return 0;
}

void dc30_jpeg_free(struct dc30_dev *dc30)
{
	struct dc30_jpeg *jp = dc30->jpeg;
	struct device *dev = &dc30->pdev->dev;
	unsigned int i;

	for (i = 0; i < DC30_JPEG_BUFS; i++) {
		if (jp->buf[i])
			dma_free_coherent(dev, DC30_JPEG_BUF_ALLOC, jp->buf[i],
					  jp->buf_dma[i]);
		jp->buf[i] = NULL;
	}
	if (jp->table)
		dma_free_coherent(dev, DC30_JPEG_TABLE_BYTES, jp->table,
				  jp->table_dma);
	jp->table = NULL;
}

/* Code volume per field: the data rate spread over the fields of a
 * second. With bit rate control the ZR36050 meets it field by field (it
 * rescales its tables after every field), so the rate is the setting and
 * the quality follows the picture content, as with dc30.sys's data rate
 * (default 63.5 KB per field there). Two fields plus the overshoot of
 * the rate control must fit a code buffer.
 */
#define DC30_JPEG_FIELD_BYTES_MAX	(128 * 1024 - 4096)

static unsigned int dc30_jpeg_field_bytes(v4l2_std_id std, unsigned int kbps)
{
	unsigned int fields = (std & V4L2_STD_525_60) ? 60 : 50;

	kbps = clamp(kbps, DC30_JPEG_RATE_MIN, DC30_JPEG_RATE_MAX);
	return min(kbps * 1000 / fields, DC30_JPEG_FIELD_BYTES_MAX);
}

static void dc30_jpeg_params(v4l2_std_id std, unsigned int kbps,
			     struct zr36050_params *p,
			     struct zr36016_window *win)
{
	struct dc30_vfe_geometry geo;

	dc30_vfe_geometry_for_std(std, &geo);
	p->width = geo.width;
	p->field_height = geo.height / 2;
	p->field_bytes = dc30_jpeg_field_bytes(std, kbps);
	p->two_fields = true;

	win->nax = dc30_jpeg_nax;
	win->pax = ALIGN(geo.width, 16);
	win->nay = dc30_jpeg_nay;
	win->pay = ALIGN(geo.height / 2, 8);
}

/* Field indicator: guest 7, register 0 (dc30.sys _I22_GetFI). On the
 * board, bit 0 is the field identity and changes about 130us after
 * the field interrupt, bit 1 (with bit 2) is high during those 130us;
 * bit 0 = 0 marks the top field (the first JPEG of a frame from such a
 * field weaves as the top field, 10 of 10 starts).
 */
#define DC30_JPEG_FI_GUEST	7
#define DC30_JPEG_FI		BIT(0)
#define DC30_JPEG_FI_VSYNC	BIT(1)
#define DC30_JPEG_FIELD_WAIT_MS	100

static unsigned int dc30_jpeg_top_fi;
module_param_named(jpeg_top_fi, dc30_jpeg_top_fi, uint, 0644);
MODULE_PARM_DESC(jpeg_top_fi, "MJPEG: field indicator level (guest 7 bit 0) of the top field (default 0)");

int dc30_jpeg_ended_field(struct dc30_dev *dc30)
{
	u8 val;

	if (dc30_guest_read(dc30, DC30_JPEG_FI_GUEST, 0, &val))
		return -1;
	/* Past the vsync pulse the indicator already shows the new field. */
	if (!(val & DC30_JPEG_FI_VSYNC))
		val ^= DC30_JPEG_FI;
	return (val & DC30_JPEG_FI) == (dc30_jpeg_top_fi & 1) ? 0 : 1;
}

int dc30_jpeg_field_lines(struct dc30_dev *dc30)
{
	unsigned int nol;

	return zr36016_read_nol(dc30, &nol) ? -1 : nol;
}

/* Field interrupt fallback: the next field of either parity. */
static void dc30_jpeg_wait_any_field(struct dc30_dev *dc30, u64 end)
{
	if (dc30_read(dc30, ZR36057_ICR) & ZR36057_ICR_GIRQ1)
		return;
	dc30_write(dc30, ZR36057_ISR, ZR36057_ISR_GIRQ1);
	while (!(dc30_read(dc30, ZR36057_ISR) & ZR36057_ISR_GIRQ1)) {
		if (ktime_get_ns() > end)
			return;
		udelay(10);
	}
	dc30_write(dc30, ZR36057_ISR, ZR36057_ISR_GIRQ1);
}

/* dc30.sys _I22_StartCapture: JPEG process out of reset, ZR36050 GO,
 * Go_en (from here on every PostOffice access drops it), ZR36016 GO.
 * Returns how long it took.
 */
static int dc30_jpeg_go(struct dc30_dev *dc30, u64 *ns)
{
	u64 t0 = ktime_get_ns();
	int err;

	dc30_jpeg_hw_start(dc30);
	err = zr36050_go(dc30);
	if (!err) {
		dc30_jpeg_hw_go(dc30);
		err = zr36016_go(dc30);
	}
	*ns = ktime_get_ns() - t0;
	return err;
}

/* A GO sequence slower than this has stalled the codec (see below). */
#define DC30_JPEG_GO_WARN_US	100

/* dc30.sys _WaitBeginField(dev, 0) and _I22_StartCapture: wait for the
 * field indicator to leave the top field level and come back, then run
 * the GO sequence at the beginning of that top field. That alone does not
 * fix the field pairing (board: still random), the stream pairs the
 * fields itself (dc30_video.c).
 *
 * The GO sequence has to follow the field change closely. With the audio
 * capture running it took 0.2-0.45 ms instead of 30 us - every PostOffice
 * access waited for a FIFO block of the audio drain, ~90 us each - and
 * then the ZR36050 never started: status 00/00, ACV 0, no frame until the
 * watchdog restarted it, often over and over (on the board: 246 of
 * 249 starts slower than 0.2 ms stalled, none of 56 faster than 0.15 ms).
 * dc30.sys runs the whole start in StartIo at DISPATCH_LEVEL, where its
 * audio DPC cannot come in between. Here the audio drain is held off for
 * the wait (a field or two - the ASIC ring lasts ~160 ms), and the last
 * field indicator read and the GO sequence run with interrupts off.
 */
static int dc30_jpeg_start_at_field(struct dc30_dev *dc30, u64 *go_ns)
{
	u64 end = ktime_get_ns() + DC30_JPEG_FIELD_WAIT_MS * NSEC_PER_MSEC;
	bool start = dc30_jpeg_top_fi & 1;
	bool seen_other = false;
	unsigned long flags;
	int err;
	u8 val;

	dc30_alsa_hold(dc30, true);
	for (;;) {
		local_irq_save(flags);
		if (dc30_guest_read(dc30, DC30_JPEG_FI_GUEST, 0, &val)) {
			local_irq_restore(flags);
			break;
		}
		if (!!(val & DC30_JPEG_FI) != start) {
			seen_other = true;
		} else if (seen_other) {
			err = dc30_jpeg_go(dc30, go_ns);
			local_irq_restore(flags);
			goto out;
		}
		local_irq_restore(flags);
		if (ktime_get_ns() > end)
			break;
	}

	/* No change of the field indicator: no input signal, with the
	 * VPX's field flag following the input (dc30_vpx3220 field_follow).
	 * The stream pairs the fields itself (dc30_video.c).
	 */
	dev_info_once(&dc30->pdev->dev,
		      "MJPEG: field indicator on guest 7 did not change (no signal?), codec started without field sync\n");
	/* Two fields at most, the drain is still held. */
	dc30_jpeg_wait_any_field(dc30, ktime_get_ns() + 40 * NSEC_PER_MSEC);
	local_irq_save(flags);
	err = dc30_jpeg_go(dc30, go_ns);
	local_irq_restore(flags);
out:
	dc30_alsa_hold(dc30, false);
	return err;
}

/* dc30.sys _DC30StartIO for capture: _I22_SetupCapture, _Z016_SetupCapture,
 * _CoderCfg, code buffer table, _WaitBeginField, _I22_StartCapture.
 */
int dc30_jpeg_start(struct dc30_dev *dc30, v4l2_std_id std,
		    unsigned int kbps)
{
	struct dc30_jpeg *jp = dc30->jpeg;
	struct dc30_vfe_geometry geo;
	struct zr36050_params p;
	struct zr36016_window win;
	unsigned int i;
	u64 go_ns;
	int err;

	dc30_jpeg_params(std, kbps, &p, &win);

	/* VFE polarity and field bits as for raw capture; VidEn stays off. */
	dc30_vfe_geometry_for_std(std, &geo);
	dc30_vfe_set_geometry(dc30, std, &geo);

	for (i = 0; i < DC30_JPEG_BUFS; i++)
		dc30_stat_com_arm(jp, i);
	jp->next = 0;
	dc30_jpeg_hw_setup(dc30, jp->table_dma, p.two_fields);

	/* ZR36050 first: its reset pulse (GPIO1) may reach the ZR36016 too,
	 * which lost its set-up when it came first (seen on the board).
	 */
	err = zr36050_configure(dc30, &p);
	if (!err)
		err = zr36016_setup_capture(dc30, &win);
	if (err)
		goto fail;

	err = dc30_jpeg_start_at_field(dc30, &go_ns);
	jp->go_us = div_u64(go_ns, NSEC_PER_USEC);
	if (err)
		goto fail;
	if (jp->go_us > DC30_JPEG_GO_WARN_US)
		dev_warn_ratelimited(&dc30->pdev->dev,
				     "MJPEG: GO sequence took %u us, the codec may not start\n",
				     jp->go_us);
	jp->running = true;
	return 0;

fail:
	dev_err(&dc30->pdev->dev, "JPEG start failed: %d\n", err);
	dc30_jpeg_hw_stop(dc30);
	dc30_jpeg_reset(dc30);
	return err;
}

void dc30_jpeg_dump(struct dc30_dev *dc30, char *buf, size_t len)
{
	struct dc30_jpeg *jp = dc30->jpeg;
	u8 st0 = 0, st1 = 0, gostop = 0, b;
	unsigned int sf = 0, nol = 0, i;
	u32 acv = 0;
	int err;

	/* STATUS_1 clears on reading - only right before a restart. */
	err = zr36050_read(dc30, ZR36050_STATUS_0, &st0);
	if (!err)
		err = zr36050_read(dc30, ZR36050_STATUS_1, &st1);
	for (i = 0; i < 2 && !err; i++) {
		err = zr36050_read(dc30, ZR36050_SF + i, &b);
		sf = sf << 8 | b;
	}
	for (i = 0; i < 4 && !err; i++) {
		err = zr36050_read(dc30, ZR36050_ACV + i, &b);
		acv = acv << 8 | b;
	}
	if (!err)
		err = zr36016_read_gostop(dc30, &gostop);
	if (!err)
		err = zr36016_read_nol(dc30, &nol);

	scnprintf(buf, len,
		  "GO sequence %u us, JPC %08x JMC %08x ISR %08x MCTCR %08x, stat_com %08x %08x %08x %08x next %u, 050 status %02x/%02x SF 0x%x ACV %u, 016 go %02x NOL %u (%d)",
		  jp->go_us, dc30_read(dc30, ZR36057_JPC),
		  dc30_read(dc30, ZR36057_JMC), dc30_read(dc30, ZR36057_ISR),
		  dc30_read(dc30, ZR36057_MCTCR),
		  le32_to_cpu(READ_ONCE(jp->table[0])),
		  le32_to_cpu(READ_ONCE(jp->table[1])),
		  le32_to_cpu(READ_ONCE(jp->table[2])),
		  le32_to_cpu(READ_ONCE(jp->table[3])), jp->next,
		  st0, st1, sf, acv, gostop, nol, err);
}

/* dc30.sys _I22_Stop. The ZR36050 goes back to stand-by with the video
 * bus (dc30_board_capture(dc30, false)).
 */
void dc30_jpeg_stop(struct dc30_dev *dc30)
{
	struct dc30_jpeg *jp = dc30->jpeg;

	if (!jp->running)
		return;
	dc30_jpeg_hw_stop(dc30);
	zr36016_stop(dc30);
	dc30_jpeg_reset(dc30);
	jp->running = false;
}

bool dc30_jpeg_peek(struct dc30_dev *dc30)
{
	struct dc30_jpeg *jp = dc30->jpeg;

	return le32_to_cpu(READ_ONCE(jp->table[jp->next])) & DC30_STAT_COM_DONE;
}

bool dc30_jpeg_next(struct dc30_dev *dc30, struct dc30_jpeg_frame *f)
{
	struct dc30_jpeg *jp = dc30->jpeg;
	u32 st;

	st = le32_to_cpu(READ_ONCE(jp->table[jp->next]));
	if (!(st & DC30_STAT_COM_DONE))
		return false;
	rmb();	/* status before the data it covers */

	f->data = jp->buf[jp->next];
	f->len = (st >> 1) & 0x3fffff;
	f->fcnt = st >> 24;
	/* Past the declared buffer length the ZR36057 should not have
	 * written at all; it still lies within the allocation.
	 */
	f->len = min_t(unsigned int, f->len, DC30_JPEG_BUF_ALLOC);
	return true;
}

void dc30_jpeg_release(struct dc30_dev *dc30)
{
	struct dc30_jpeg *jp = dc30->jpeg;

	dc30_stat_com_arm(jp, jp->next);
	jp->next = (jp->next + 1) % DC30_JPEG_BUFS;
}

/* ---- debugfs ---- */

/* Decoder on and streaming, video bus in capture set-up. */
static int dc30_jpeg_bus_on(struct dc30_dev *dc30)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(dc30->decoder);
	int err;

	err = dc30_decoder_get(dc30, false);
	if (err)
		return err;
	dc30_board_capture(dc30, true);
	err = v4l2_subdev_call(sd, video, s_stream, 1);
	if (err && err != -ENOIOCTLCMD) {
		dc30_board_capture(dc30, false);
		dc30_decoder_put(dc30, false);
		return err;
	}
	dc30_decoder_settle(dc30, V4L2_STD_PAL);
	return 0;
}

static void dc30_jpeg_bus_off(struct dc30_dev *dc30)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(dc30->decoder);

	v4l2_subdev_call(sd, video, s_stream, 0);
	dc30_board_capture(dc30, false);
	dc30_decoder_put(dc30, false);
}

static int dc30_jpeg_show(struct seq_file *m, void *data)
{
	struct dc30_dev *dc30 = m->private;
	struct dc30_jpeg *jp = dc30->jpeg;
	struct zr36050_params p;
	struct zr36016_window win;
	int err;

	mutex_lock(&jp->test_lock);
	if (jp->test_log[0])
		seq_printf(m, "last jpeg_test:\n%s\n", jp->test_log);
	mutex_unlock(&jp->test_lock);

	if (test_and_set_bit(0, &dc30->capture_busy)) {
		seq_puts(m, "capture running, codec registers not read\n");
		return 0;
	}
	err = dc30_pm_get(dc30);
	if (err)
		goto out_busy;

	/* Set both chips up as for a stream and read everything back. They
	 * run on the decoder's pixel clock: without its output enabled the
	 * ZR36050 does not answer (seen on the board).
	 */
	dc30_jpeg_params(V4L2_STD_PAL, dc30_jpeg_test_rate, &p, &win);
	err = dc30_jpeg_bus_on(dc30);
	if (err) {
		seq_printf(m, "decoder start failed: %d\n", err);
		goto out_pm;
	}
	err = zr36050_configure(dc30, &p);
	if (err)
		seq_printf(m, "050 set-up failed: %d\n", err);
	err = zr36016_setup_capture(dc30, &win);
	if (err)
		seq_printf(m, "016 set-up failed: %d\n", err);
	zr36016_show(m, dc30);
	zr36050_show(m, dc30, &p);
	seq_printf(m, "050 target   %u bytes per field (%u kB/s)\n",
		   p.field_bytes, dc30_jpeg_test_rate);
	dc30_jpeg_bus_off(dc30);
out_pm:
	dc30_pm_put(dc30);
out_busy:
	clear_bit(0, &dc30->capture_busy);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(dc30_jpeg);

#define DC30_JPEG_TEST_FRAMES	8
#define DC30_JPEG_TEST_MS	1000

/* Compress a few frames with the decoder's current input and keep the
 * last one. Needs a PAL signal.
 */
static int dc30_jpeg_test_run(struct dc30_dev *dc30)
{
	struct dc30_jpeg *jp = dc30->jpeg;
	unsigned long start, timeout;
	struct dc30_jpeg_frame f;
	struct zr36016_window win;
	unsigned int frames = 0, nol, vsyncs = 0, ready_us;
	u64 vsync_ns;
	size_t pos = 0, len = sizeof(jp->test_log);
	u8 mode;
	int err;

	jp->test_len = 0;
	jp->test_log[0] = 0;

	err = dc30_jpeg_alloc(dc30);
	if (err)
		return err;
	err = dc30_jpeg_bus_on(dc30);
	if (err)
		goto out_free;
	err = dc30_jpeg_start(dc30, V4L2_STD_PAL, dc30_jpeg_test_rate);
	if (err)
		goto out_bus;

	pos += scnprintf(jp->test_log + pos, len - pos,
			 "data rate %u kB/s, %u bytes per field\n",
			 dc30_jpeg_test_rate,
			 dc30_jpeg_field_bytes(V4L2_STD_PAL, dc30_jpeg_test_rate));
	if (!zr36016_read_window(dc30, &mode, &win))
		pos += scnprintf(jp->test_log + pos, len - pos,
				 "016 mode 0x%02x, NAX %u PAX %u NAY %u PAY %u\n",
				 mode, win.nax, win.pax, win.nay, win.pay);

	/* Where in the field a frame gets ready: GIRQ1 (masked here, the
	 * status bit still sets) polled next to the code buffer table. Tells
	 * which field interrupt first sees a frame - the A/V assignment of
	 * the stream depends on it.
	 */
	dc30_write(dc30, ZR36057_ISR, ZR36057_ISR_GIRQ1);
	vsync_ns = 0;
	start = jiffies;
	timeout = start + msecs_to_jiffies(DC30_JPEG_TEST_MS);
	while (frames < DC30_JPEG_TEST_FRAMES && time_before(jiffies, timeout)) {
		if (dc30_read(dc30, ZR36057_ISR) & ZR36057_ISR_GIRQ1) {
			dc30_write(dc30, ZR36057_ISR, ZR36057_ISR_GIRQ1);
			vsync_ns = ktime_get_ns();
			vsyncs++;
		}
		if (!dc30_jpeg_next(dc30, &f)) {
			usleep_range(100, 200);
			continue;
		}
		ready_us = vsync_ns ? div_u64(ktime_get_ns() - vsync_ns, 1000) : 0;
		frames++;
		if (f.len && f.len <= DC30_JPEG_MAX_FRAME) {
			memcpy(jp->test_frame, f.data, f.len);
			jp->test_len = f.len;
		}
		pos += scnprintf(jp->test_log + pos, len - pos,
				 "frame %u: buffer %u, fcnt %u, %u bytes, ready %u.%u ms after field irq %u\n",
				 frames, jp->next, f.fcnt, f.len, ready_us / 1000,
				 ready_us % 1000 / 100, vsyncs);
		dc30_jpeg_release(dc30);
	}
	pos += scnprintf(jp->test_log + pos, len - pos,
			 "%u frames in %u ms, JPC 0x%08x JMC 0x%08x\n", frames,
			 jiffies_to_msecs(jiffies - start),
			 dc30_read(dc30, ZR36057_JPC),
			 dc30_read(dc30, ZR36057_JMC));
	if (!zr36016_read_nol(dc30, &nol))
		scnprintf(jp->test_log + pos, len - pos,
			  "016 NOL %u lines in the last field\n", nol);
	dc30_jpeg_stop(dc30);
	if (!frames)
		err = -ETIMEDOUT;
out_bus:
	dc30_jpeg_bus_off(dc30);
out_free:
	dc30_jpeg_free(dc30);
	return err;
}

static int dc30_jpeg_test_open(struct inode *inode, struct file *file)
{
	struct dc30_dev *dc30 = inode->i_private;
	struct dc30_jpeg *jp = dc30->jpeg;
	int err;

	if (!dc30->decoder)
		return -ENODEV;
	if (test_and_set_bit(0, &dc30->capture_busy))
		return -EBUSY;
	err = dc30_pm_get(dc30);
	if (err)
		goto out_busy;

	mutex_lock(&jp->test_lock);
	err = dc30_jpeg_test_run(dc30);
	if (err)
		dev_warn(&dc30->pdev->dev, "jpeg_test: %d\n%s", err,
			 jp->test_log);
	else
		dev_info(&dc30->pdev->dev, "jpeg_test:\n%s", jp->test_log);
	mutex_unlock(&jp->test_lock);

	dc30_pm_put(dc30);
out_busy:
	clear_bit(0, &dc30->capture_busy);
	return err;
}

static ssize_t dc30_jpeg_test_read(struct file *file, char __user *ubuf,
				   size_t count, loff_t *ppos)
{
	struct dc30_dev *dc30 = file_inode(file)->i_private;
	struct dc30_jpeg *jp = dc30->jpeg;
	ssize_t ret;

	mutex_lock(&jp->test_lock);
	ret = simple_read_from_buffer(ubuf, count, ppos, jp->test_frame,
				      jp->test_len);
	mutex_unlock(&jp->test_lock);
	return ret;
}

static const struct file_operations dc30_jpeg_test_fops = {
	.owner = THIS_MODULE,
	.open = dc30_jpeg_test_open,
	.read = dc30_jpeg_test_read,
	.llseek = default_llseek,
};

void dc30_jpeg_debugfs_init(struct dc30_dev *dc30, struct dentry *dir)
{
	debugfs_create_file("jpeg", 0400, dir, dc30, &dc30_jpeg_fops);
	debugfs_create_file("jpeg_test", 0400, dir, dc30,
			    &dc30_jpeg_test_fops);
}

int dc30_jpeg_init(struct dc30_dev *dc30)
{
	struct dc30_jpeg *jp;

	jp = devm_kzalloc(&dc30->pdev->dev, sizeof(*jp), GFP_KERNEL);
	if (!jp)
		return -ENOMEM;
	jp->test_frame = vmalloc(DC30_JPEG_MAX_FRAME);
	if (!jp->test_frame)
		return -ENOMEM;
	jp->dc30 = dc30;
	mutex_init(&jp->test_lock);
	dc30->jpeg = jp;
	return 0;
}

void dc30_jpeg_exit(struct dc30_dev *dc30)
{
	if (dc30->jpeg)
		vfree(dc30->jpeg->test_frame);
}

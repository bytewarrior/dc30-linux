/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * dc30_meta.h - per-frame metadata of the dc30 driver's metadata capture
 * device ("dc30-meta", V4L2_BUF_TYPE_META_CAPTURE, format DC3M)
 *
 * Shared between the driver and userspace. One record per captured video
 * frame, with the same v4l2_buffer.sequence and timestamp as that frame on
 * the video node. Both nodes stream independently; a recorder pairs them
 * by sequence number.
 *
 * Audio position: at every video field interrupt the driver reads the
 * audio ASIC's sample counter. audio_pos is the byte offset in the ALSA
 * capture stream (hw:DC30, as delivered) that the hardware had reached at
 * that moment; audio_pos / audio_frame_bytes is the audio frame index.
 * Counted from the start of the ALSA stream whose number is audio_epoch
 * (it changes on every ALSA start, e.g. after an xrun). Single values
 * jitter by a few audio frames (interrupt latency); fit over many frames.
 */

#ifndef DC30_META_H
#define DC30_META_H

#include <linux/types.h>

#ifndef v4l2_fourcc
#define v4l2_fourcc(a, b, c, d) \
	((__u32)(a) | ((__u32)(b) << 8) | ((__u32)(c) << 16) | ((__u32)(d) << 24))
#endif

#define DC30_META_FMT		v4l2_fourcc('D', 'C', '3', 'M')
#define DC30_META_VERSION	1

/* Video node control, boolean, default 1: MJPEG fields paired by the
 * decoder's field flag (the driver splits and re-pairs the codec's
 * frames where the flag says so). 0: the codec's frames as they come -
 * for userspace that pairs the fields itself (dc30-capture, "pair fields
 * by the picture"), so every frame's fields are the two field interrupts
 * of its sequence number.
 */
#define DC30_CID_PAIR_BY_FLAG	(0x00980900 + 0x10f0)	/* V4L2_CID_USER_BASE + */

/* dc30_meta_field.flags */
#define DC30_META_FIELD_AUDIO	(1 << 0)	/* audio_pos/audio_epoch valid */
#define DC30_META_FIELD_BOTTOM	(1 << 1)	/* bottom field (else top) */

/* dc30_meta.audio_clock */
#define DC30_META_CLOCK_CRYSTAL		0
#define DC30_META_CLOCK_VIDEOLOCK	1	/* AD1843 locked to VACT */

struct dc30_meta_field {
	__u64 vsync_ns;		/* CLOCK_MONOTONIC, interrupt that completed it */
	__s64 audio_pos;	/* bytes into the ALSA stream, see above */
	__u32 audio_epoch;
	__u32 flags;		/* DC30_META_FIELD_* */
};

struct dc30_meta {
	__u32 version;		/* DC30_META_VERSION */
	__u32 size;		/* sizeof(struct dc30_meta) */
	__u32 sequence;		/* v4l2_buffer.sequence of the video frame */
	__u32 flags;		/* none defined yet */

	/* The frame's two fields in capture order: [0] came first. */
	struct dc30_meta_field field[2];

	/* Audio stream parameters, valid when a field has ..._AUDIO. */
	__u32 audio_rate;	/* nominal rate in Hz */
	__u16 audio_frame_bytes;	/* bytes per audio frame (4: S16 stereo) */
	__u16 audio_clock;	/* DC30_META_CLOCK_* */

	/* Video stream counters since STREAMON, as of this frame (see the
	 * driver's debugfs stats for their meaning).
	 */
	__u32 frames_dropped;
	__u32 no_field;
	__u32 late_irq;
	__u32 field_order;
	__u32 fifo_overflows;
	/* MJPEG: restarts of a stalled JPEG codec. Zero in raw capture
	 * (was reserved, so version 1 readers see 0).
	 */
	__u32 codec_restarts;
	/* MJPEG: changes between frames as the codec paired them and frames
	 * paired across its frames (a field jump, e.g. a cut on an edited
	 * tape, turns the codec's pairing round). Each one leaves a gap in
	 * the sequence. Zero in raw capture (was reserved).
	 */
	__u32 pairing_switches;
	/* MJPEG: frames the codec did not finish (gaps in the ZR36057 frame
	 * counter, no other sign of them). Zero in raw capture (was
	 * reserved).
	 */
	__u32 codec_skipped;
	/* Frames the driver dropped: no buffer queued, in MJPEG also too
	 * large, empty or out of order (debugfs stats: no buffer, oversize,
	 * bad frames). Was reserved.
	 */
	__u32 frames_rejected;
	__u32 reserved[1];
};

#endif /* DC30_META_H */

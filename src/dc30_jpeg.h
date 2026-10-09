/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * dc30_jpeg.h - motion JPEG compression: ZR36057 code buffers plus the
 * ZR36050/ZR36016 set-up (dc30_jpeg.c)
 */

#ifndef DC30_JPEG_H
#define DC30_JPEG_H

#include <linux/types.h>
#include <linux/videodev2.h>

struct dc30_dev;
struct dentry;
struct seq_file;

/* Largest compressed frame (both fields) handed to userspace. The bit
 * rate control overshoots its target on detailed pictures: at 6000 kB/s
 * (240 KB per frame) 35 of 7500 frames were over 256 KB, and on a worn
 * tape one of 123000 reached 425 KB.
 */
#define DC30_JPEG_MAX_FRAME	(512 * 1024)

/* Data rate in kB/s (10^3 bytes). The maximum fills the code buffers at
 * PAL; NTSC's 60 fields get less per field at the same rate.
 */
#define DC30_JPEG_RATE_MIN	1000U
#define DC30_JPEG_RATE_MAX	6300U
#define DC30_JPEG_RATE_DEF	6000U

struct dc30_jpeg_frame {
	const void *data;
	unsigned int len;	/* bytes, both fields; can exceed
				 * DC30_JPEG_MAX_FRAME (then drop it) */
	unsigned int fcnt;	/* ZR36057 frame counter, modulo 256 */
};

int dc30_jpeg_init(struct dc30_dev *dc30);
void dc30_jpeg_exit(struct dc30_dev *dc30);

/* Code buffers, allocated for the duration of a stream. Sleeps. */
int dc30_jpeg_alloc(struct dc30_dev *dc30);
void dc30_jpeg_free(struct dc30_dev *dc30);

/* Start compressing: the caller holds the board awake, the decoder
 * streams and dc30_board_capture(dc30, true) has run. Sleeps.
 */
int dc30_jpeg_start(struct dc30_dev *dc30, v4l2_std_id std,
		    unsigned int kbps);
void dc30_jpeg_stop(struct dc30_dev *dc30);

/* One line on the state of ZR36057, ZR36050 and ZR36016 for the log, when
 * the codec stalled. Clears ZR36050 status bits: only before a restart.
 */
void dc30_jpeg_dump(struct dc30_dev *dc30, char *buf, size_t len);

/* The next finished frame, in order, if there is one. Any context. The
 * data stays valid until dc30_jpeg_release() hands the code buffer back
 * to the ZR36057.
 */
bool dc30_jpeg_next(struct dc30_dev *dc30, struct dc30_jpeg_frame *f);

/* Whether dc30_jpeg_next() would find a frame, without taking it. */
bool dc30_jpeg_peek(struct dc30_dev *dc30);
void dc30_jpeg_release(struct dc30_dev *dc30);

/* At the field interrupt: parity of the field that just ended (0 top,
 * 1 bottom) from the board's field indicator, or -1. Reads the GuestBus.
 */
int dc30_jpeg_ended_field(struct dc30_dev *dc30);

/* Lines the ZR36016 processed in the last field (NOL), or -1. Four
 * GuestBus accesses; not while a restart runs.
 */
int dc30_jpeg_field_lines(struct dc30_dev *dc30);

void dc30_jpeg_debugfs_init(struct dc30_dev *dc30, struct dentry *dir);

#endif /* DC30_JPEG_H */

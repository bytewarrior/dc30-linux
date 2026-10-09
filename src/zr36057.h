/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * zr36057.h - ZR36057 helpers: Video Front End (raw, unscaled continuous
 * capture, dc30_video.c), PostOffice access to the guests, and the JPEG
 * code DMA registers (dc30_jpeg.c).
 */

#ifndef DC30_ZR36057_H
#define DC30_ZR36057_H

#include <linux/types.h>
#include <linux/videodev2.h>

struct dc30_dev;
struct seq_file;

/* Native, unscaled capture geometry for a given TV standard - fixed square-
 * pixel width/height the VPX3220 decoder actually outputs on this board
 * (see the DC30 card table in the GPL zoran driver's zoran_card.c:
 * f50sqpixel_dc10/f60sqpixel_dc10).
 * No horizontal/vertical decimation is supported yet - one fixed format
 * per standard keeps zr36057_set_geometry() a direct, faithful port of the
 * proven register math instead of the general scaler case.
 */
struct dc30_vfe_geometry {
	u16 width;		/* active pixels per line */
	u16 height;		/* full interlaced frame height (2 fields) */
};

void dc30_vfe_geometry_for_std(v4l2_std_id std, struct dc30_vfe_geometry *geo);

/* Program VFEHCR/VFEVCR/VFESPFR/VDCR for the given standard and geometry.
 * Does not touch VidEn - call dc30_vfe_enable() separately.
 */
void dc30_vfe_set_geometry(struct dc30_dev *dc30, v4l2_std_id std,
			    const struct dc30_vfe_geometry *geo);

void dc30_vfe_enable(struct dc30_dev *dc30, bool on);

/* Continuous capture (SnapShot = 0): the VFE writes every field, top
 * field lines to even rows and bottom field lines to odd rows of the
 * buffer at 'dma_addr' (weaved, 'bytesperline' stride). VDTR/VDBR/
 * DispStride are "vid" parameters, so call this only while VidEn = 0.
 */
void dc30_vfe_start_continuous(struct dc30_dev *dc30, dma_addr_t dma_addr,
			       unsigned int bytesperline);

/* Returns true (and clears the flag) if the video FIFO overflowed since
 * the last call.
 */
bool dc30_vfe_check_overflow(struct dc30_dev *dc30);

/* PostOffice (GuestBus) byte access to guest 'guest' (0-7), register
 * 'reg' (0-7). Return 0, or -ETIMEDOUT if the guest never answered
 * (POPen stuck or POTime set). Safe from any context.
 */
int dc30_guest_read(struct dc30_dev *dc30, unsigned int guest,
		    unsigned int reg, u8 *val);
int dc30_guest_write(struct dc30_dev *dc30, unsigned int guest,
		     unsigned int reg, u8 val);

/* Read 'len' bytes in a row from one guest register (a FIFO port) with
 * dc30.sys's streaming trick: one PostOffice read request, then per byte
 * wait for POPen, take the data byte and re-arm the same read by writing
 * POR's low byte. Keep 'len' small (dc30.sys: <= 128), interrupts are
 * off meanwhile.
 */
int dc30_guest_read_stream(struct dc30_dev *dc30, unsigned int guest,
			   unsigned int reg, u8 *buf, unsigned int len);

/* Automatic pause before each streamed byte (po_stream_delay_ns = -1).
 * Init once at probe; start at every capture start, then tune after
 * every pass of the reader (the audio drain thread, the only caller).
 */
void dc30_po_tune_init(struct dc30_dev *dc30);
void dc30_po_tune_start(struct dc30_dev *dc30);
void dc30_po_tune(struct dc30_dev *dc30);
void dc30_po_tune_show(struct seq_file *m, struct dc30_dev *dc30);

/* JPEG compression, code DMA side. 'stat_com' is the bus address of the
 * four-entry code buffer table. Order: setup, codec set-up, start (JPEG
 * process out of reset), ZR36050 GO, go (Go_en on - from here on every
 * PostOffice access drops it around itself), ZR36016 GO. Stop undoes
 * everything on the ZR36057 side.
 */
void dc30_jpeg_hw_setup(struct dc30_dev *dc30, dma_addr_t stat_com,
			bool two_fields);
void dc30_jpeg_hw_start(struct dc30_dev *dc30);
void dc30_jpeg_hw_go(struct dc30_dev *dc30);
void dc30_jpeg_hw_stop(struct dc30_dev *dc30);

#endif /* DC30_ZR36057_H */

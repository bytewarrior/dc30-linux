/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * zr36016.h - ZR36016 colour space / raster-to-block converter
 * (doc/zr36016.pdf) between the video bus and the ZR36050.
 */

#ifndef DC30_ZR36016_H
#define DC30_ZR36016_H

#include <linux/types.h>

struct dc30_dev;
struct seq_file;

/* Direct registers (guest 2) */
#define ZR36016_GOSTOP		0
#define ZR36016_GOSTOP_GO		(1 << 0)
#define ZR36016_MODE		1
#define ZR36016_MODE_CMPR		(1 << 7)
#define ZR36016_MODE_DSPY_422		(2 << 5)
#define ZR36016_MODE_YUV422		0x11	/* 4:2:2 YCbCr in and out */
#define ZR36016_ADDR		2
#define ZR36016_DATA		3

/* Indirect registers */
#define ZR36016_SETUP1		0x00
#define ZR36016_SETUP1_CNTI		(1 << 0)	/* sequential */
#define ZR36016_SETUP2		0x01
#define ZR36016_SETUP2_CCIR		(1 << 2)
#define ZR36016_NAX		0x02	/* 2 bytes each, low byte first */
#define ZR36016_PAX		0x04
#define ZR36016_NAY		0x06
#define ZR36016_PAY		0x08
#define ZR36016_NOL		0x0a	/* read only */

/* Active window of one field, as counted from HIN/VIN. */
struct zr36016_window {
	unsigned int nax, pax;	/* pixel offset and count */
	unsigned int nay, pay;	/* line offset and count */
};

/* Sequential 4:2:2 compression over 'win' (dc30.sys _Z016_SetupCapture),
 * GO/STOP left at stop.
 */
int zr36016_setup_capture(struct dc30_dev *dc30,
			  const struct zr36016_window *win);

int zr36016_go(struct dc30_dev *dc30);
int zr36016_stop(struct dc30_dev *dc30);

/* GO/STOP register: GO bit and the chip version. */
int zr36016_read_gostop(struct dc30_dev *dc30, u8 *val);

/* Lines the last field actually had (compression). */
int zr36016_read_nol(struct dc30_dev *dc30, unsigned int *lines);

/* Read back the mode and window, e.g. to check a set-up took effect. */
int zr36016_read_window(struct dc30_dev *dc30, u8 *mode,
			struct zr36016_window *win);

void zr36016_show(struct seq_file *m, struct dc30_dev *dc30);

#endif /* DC30_ZR36016_H */

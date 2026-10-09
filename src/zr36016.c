// SPDX-License-Identifier: GPL-2.0-only
/*
 * zr36016.c - ZR36016 on the DC30 (GuestBus guest 2): turns the 4:2:2
 * raster from the VPX3220 into 8x8 blocks for the ZR36050. Values as
 * dc30.sys _Z016_SetupCapture/setupMode/setWindow.
 */

#include <linux/seq_file.h>

#include "dc30.h"
#include "zr36016.h"
#include "zr36057.h"

#define DC30_GUEST_ZR36016	2

static int zr36016_write(struct dc30_dev *dc30, unsigned int reg, u8 val)
{
	return dc30_guest_write(dc30, DC30_GUEST_ZR36016, reg, val);
}

/* dc30.sys _Z016_write: address pointer, then data, for every byte. */
static int zr36016_write_ind(struct dc30_dev *dc30, unsigned int idx, u8 val)
{
	int err;

	err = zr36016_write(dc30, ZR36016_ADDR, idx);
	if (!err)
		err = zr36016_write(dc30, ZR36016_DATA, val);
	return err;
}

static int zr36016_write_ind16(struct dc30_dev *dc30, unsigned int idx,
			       unsigned int val)
{
	int err;

	err = zr36016_write_ind(dc30, idx, val & 0xff);
	if (!err)
		err = zr36016_write_ind(dc30, idx + 1, val >> 8);
	return err;
}

static int zr36016_read_ind(struct dc30_dev *dc30, unsigned int idx, u8 *val)
{
	int err;

	err = zr36016_write(dc30, ZR36016_ADDR, idx);
	if (!err)
		err = dc30_guest_read(dc30, DC30_GUEST_ZR36016, ZR36016_DATA,
				      val);
	return err;
}

int zr36016_setup_capture(struct dc30_dev *dc30,
			  const struct zr36016_window *win)
{
	int err;

	err = zr36016_write(dc30, ZR36016_GOSTOP, 0);
	if (!err)
		err = zr36016_write(dc30, ZR36016_MODE,
				    ZR36016_MODE_CMPR | ZR36016_MODE_DSPY_422 |
				    ZR36016_MODE_YUV422);
	if (!err)
		err = zr36016_write_ind(dc30, ZR36016_SETUP1,
					ZR36016_SETUP1_CNTI);
	if (!err)
		err = zr36016_write_ind(dc30, ZR36016_SETUP2,
					ZR36016_SETUP2_CCIR);
	if (!err)
		err = zr36016_write_ind16(dc30, ZR36016_NAX, win->nax);
	if (!err)
		err = zr36016_write_ind16(dc30, ZR36016_PAX, win->pax);
	if (!err)
		err = zr36016_write_ind16(dc30, ZR36016_NAY, win->nay);
	if (!err)
		err = zr36016_write_ind16(dc30, ZR36016_PAY, win->pay);
	return err;
}

int zr36016_go(struct dc30_dev *dc30)
{
	return zr36016_write(dc30, ZR36016_GOSTOP, ZR36016_GOSTOP_GO);
}

int zr36016_stop(struct dc30_dev *dc30)
{
	return zr36016_write(dc30, ZR36016_GOSTOP, 0);
}

static int zr36016_read_ind16(struct dc30_dev *dc30, unsigned int idx,
			      unsigned int *val)
{
	u8 lo, hi;
	int err;

	err = zr36016_read_ind(dc30, idx, &lo);
	if (!err)
		err = zr36016_read_ind(dc30, idx + 1, &hi);
	*val = err ? 0 : hi << 8 | lo;
	return err;
}

int zr36016_read_gostop(struct dc30_dev *dc30, u8 *val)
{
	return dc30_guest_read(dc30, DC30_GUEST_ZR36016, ZR36016_GOSTOP, val);
}

int zr36016_read_nol(struct dc30_dev *dc30, unsigned int *lines)
{
	return zr36016_read_ind16(dc30, ZR36016_NOL, lines);
}

int zr36016_read_window(struct dc30_dev *dc30, u8 *mode,
			struct zr36016_window *win)
{
	int err;

	err = dc30_guest_read(dc30, DC30_GUEST_ZR36016, ZR36016_MODE, mode);
	if (!err)
		err = zr36016_read_ind16(dc30, ZR36016_NAX, &win->nax);
	if (!err)
		err = zr36016_read_ind16(dc30, ZR36016_PAX, &win->pax);
	if (!err)
		err = zr36016_read_ind16(dc30, ZR36016_NAY, &win->nay);
	if (!err)
		err = zr36016_read_ind16(dc30, ZR36016_PAY, &win->pay);
	return err;
}

void zr36016_show(struct seq_file *m, struct dc30_dev *dc30)
{
	static const char * const names[] = {
		"SETUP1", "SETUP2", "NAX", NULL, "PAX", NULL, "NAY", NULL,
		"PAY", NULL, "NOL", NULL,
	};
	u8 gostop, mode, lo, hi = 0;
	unsigned int i;
	int err;

	err = dc30_guest_read(dc30, DC30_GUEST_ZR36016, ZR36016_GOSTOP,
			      &gostop);
	if (!err)
		err = dc30_guest_read(dc30, DC30_GUEST_ZR36016, ZR36016_MODE,
				      &mode);
	if (err) {
		seq_printf(m, "016 read failed: %d\n", err);
		return;
	}
	seq_printf(m, "016 GO/STOP     = 0x%02x (version %u)\n", gostop,
		   gostop >> 4);
	seq_printf(m, "016 MODE        = 0x%02x\n", mode);

	for (i = 0; i < ARRAY_SIZE(names); i++) {
		if (!names[i])
			continue;
		err = zr36016_read_ind(dc30, i, &lo);
		if (!err && i >= ZR36016_NAX)
			err = zr36016_read_ind(dc30, i + 1, &hi);
		if (err) {
			seq_printf(m, "016 read failed: %d\n", err);
			return;
		}
		if (i >= ZR36016_NAX)
			seq_printf(m, "016 %-6s 0x%02x = %u\n", names[i], i,
				   hi << 8 | lo);
		else
			seq_printf(m, "016 %-6s 0x%02x = 0x%02x\n", names[i], i,
				   lo);
	}
}

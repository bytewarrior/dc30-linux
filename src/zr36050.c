// SPDX-License-Identifier: GPL-2.0-only
/*
 * zr36050.c - ZR36050 JPEG processor on the DC30: register access over
 * the ZR36057 PostOffice and set-up for motion JPEG compression.
 *
 * The sequence and all values follow dc30.sys (_CoderCfg, _MakeMarker*,
 * _CalcScaleFactor, _CalcDataVolume), checked against its machine code.
 * The quantization and Huffman tables are
 * the JPEG standard's example tables (ITU T.81 Annex K); dc30.sys and the
 * GPL zoran driver's zr36050.c carry the same bytes.
 *
 * Access: the ZR36050 has a 10-bit internal address space but only two
 * address lines on the GuestBus. Guest 1 register 0 is an external latch
 * for address bits 9-2, guest 0 registers 0-3 are the chip itself with
 * address bits 1-0.
 */

#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/seq_file.h>
#include <linux/slab.h>

#include "dc30.h"
#include "dc30_power.h"
#include "zr36050.h"
#include "zr36057.h"

#define DC30_GUEST_ZR36050	0
#define DC30_GUEST_ZR36050_LATCH	1

/* dc30.sys _CoderWaitReady: up to 50ms for END. */
#define ZR36050_END_TIMEOUT_MS	50

static const u8 zr36050_qt_luma[64] = {
	16, 11, 12, 14, 12, 10, 16, 14, 13, 14, 18, 17, 16, 19, 24, 40,
	26, 24, 22, 22, 24, 49, 35, 37, 29, 40, 58, 51, 61, 60, 57, 51,
	56, 55, 64, 72, 92, 78, 64, 68, 87, 69, 55, 56, 80, 109, 81, 87,
	95, 98, 103, 104, 103, 62, 77, 113, 121, 112, 100, 120, 92, 101, 103, 99,
};

static const u8 zr36050_qt_chroma[64] = {
	17, 18, 18, 24, 21, 24, 47, 26, 26, 47, 99, 66, 56, 66, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
	99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
};

/* DHT segment body: DC luma, DC chroma, AC luma, AC chroma. */
static const u8 zr36050_dht[416] = {
	0x00,
	0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
	0x01,
	0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
	0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
	0x10,
	0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03,
	0x05, 0x05, 0x04, 0x04, 0x00, 0x00, 0x01, 0x7d,
	0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12,
	0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
	0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
	0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
	0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16,
	0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
	0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
	0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
	0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
	0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
	0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
	0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
	0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
	0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
	0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
	0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
	0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4,
	0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
	0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
	0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
	0xf9, 0xfa,
	0x11,
	0x00, 0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04,
	0x07, 0x05, 0x04, 0x04, 0x00, 0x01, 0x02, 0x77,
	0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21,
	0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
	0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
	0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
	0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34,
	0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
	0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38,
	0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
	0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
	0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
	0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
	0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
	0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96,
	0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
	0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
	0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
	0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2,
	0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
	0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
	0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
	0xf9, 0xfa,
};

/* YCbCr 4:2:2: Y 2x1 with table 0, Cb and Cr 1x1 with table 1. */
#define ZR36050_NCOMP		3
static const u8 zr36050_sampling[ZR36050_NCOMP] = { 0x21, 0x11, 0x11 };
static const u8 zr36050_qtable[ZR36050_NCOMP] = { 0, 1, 1 };
static const u8 zr36050_htable[ZR36050_NCOMP] = { 0x00, 0x11, 0x11 };
#define ZR36050_MCU_WIDTH	16

/* Marker segments this driver puts into every field. dc30.sys uses
 * APP | COM | DQT; DHT is added so every field decodes without the
 * implicit AVI MJPEG tables (~420 bytes).
 */
#define ZR36050_MARKERS		(ZR36050_ME_APP | ZR36050_ME_COM | \
				 ZR36050_ME_DQT | ZR36050_ME_DHT)

/* COM "CS=ITU601": the JPEGs carry the decoder's BT.601 limited range,
 * not JFIF's full range, and FFmpeg's MJPEG decoder takes this comment
 * as the sign for it (without it: yuvj422p, and the picture looks flat).
 * dc30.sys writes "DC30" and version bytes here.
 */
static const u8 zr36050_com[] = "CS=ITU601";

/* One marker segment in the internal memory: FF xx, length, body. */
struct zr36050_seg {
	unsigned int addr;
	u8 marker;
	const u8 *body;
	unsigned int len;	/* body bytes, without marker and length */
};

/* Address latch cache for a run of writes (one configure call). */
struct zr36050_io {
	struct dc30_dev *dc30;
	int latch;
	int err;
};

static void zr36050_io_write(struct zr36050_io *io, unsigned int addr, u8 val)
{
	if (io->err)
		return;
	if (io->latch != (int)(addr >> 2)) {
		io->err = dc30_guest_write(io->dc30, DC30_GUEST_ZR36050_LATCH,
					   0, addr >> 2);
		if (io->err)
			return;
		io->latch = addr >> 2;
	}
	io->err = dc30_guest_write(io->dc30, DC30_GUEST_ZR36050, addr & 3,
				   val);
}

static void zr36050_io_write_be(struct zr36050_io *io, unsigned int addr,
				u32 val, unsigned int bytes)
{
	while (bytes--)
		zr36050_io_write(io, addr++, val >> (8 * bytes));
}

static void zr36050_io_seg(struct zr36050_io *io,
			   const struct zr36050_seg *seg)
{
	unsigned int addr = seg->addr, i;

	zr36050_io_write(io, addr++, 0xff);
	zr36050_io_write(io, addr++, seg->marker);
	zr36050_io_write_be(io, addr, seg->len + 2, 2);
	addr += 2;
	for (i = 0; i < seg->len; i++)
		zr36050_io_write(io, addr++, seg->body[i]);
}

int zr36050_write(struct dc30_dev *dc30, unsigned int addr, u8 val)
{
	struct zr36050_io io = { .dc30 = dc30, .latch = -1 };

	zr36050_io_write(&io, addr, val);
	return io.err;
}

int zr36050_read(struct dc30_dev *dc30, unsigned int addr, u8 *val)
{
	int err;

	err = dc30_guest_write(dc30, DC30_GUEST_ZR36050_LATCH, 0, addr >> 2);
	if (!err)
		err = dc30_guest_read(dc30, DC30_GUEST_ZR36050, addr & 3, val);
	return err;
}

int zr36050_go(struct dc30_dev *dc30)
{
	return zr36050_write(dc30, ZR36050_GO, 1);
}

/* Starting scale factor. With bit rate control the ZR36050 computes a new
 * one after every field (NSF, written back to the SF register) so that
 * the code volume meets TCV - the value here only matters for the first
 * field, whatever quality it stands for (on the board: identical
 * output for starting values 0x6d to 0x800).
 *
 * The chip quantizes with table * SF / 2048 (its DCT output carries a
 * factor of 8), so 0x800 is the standard tables themselves, coarse
 * enough for the first field to fit any volume target. dc30.sys starts
 * at 1/40 to 1/3 of that.
 */
#define ZR36050_SF_START	0x800

/* dc30.sys _CoderWaitReady: poll STATUS_1.END, then a read of address 0. */
static int zr36050_wait_end(struct dc30_dev *dc30)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(ZR36050_END_TIMEOUT_MS);
	u8 st, dummy;
	int err;

	for (;;) {
		err = zr36050_read(dc30, ZR36050_STATUS_1, &st);
		if (err)
			return err;
		if (st & ZR36050_ST1_END)
			break;
		if (time_after(jiffies, timeout))
			return -ETIMEDOUT;
		usleep_range(100, 200);
	}
	return zr36050_read(dc30, ZR36050_GO, &dummy);
}

struct zr36050_markers {
	u8 sof[6 + 3 * ZR36050_NCOMP];
	u8 sos[1 + 2 * ZR36050_NCOMP + 3];
	u8 dri[2];
	u8 dqt[2 * 65];
	u8 app[8];
	struct zr36050_seg segs[7];
	unsigned int header_bytes;	/* bytes the markers add to a field */
};

static void zr36050_build_markers(const struct zr36050_params *p,
				  struct zr36050_markers *mk)
{
	unsigned int width = ALIGN(p->width, ZR36050_MCU_WIDTH);
	unsigned int height = ALIGN(p->field_height, 8);
	unsigned int i;
	u8 *b;

	/* SOF0: precision, lines, samples per line, components. */
	b = mk->sof;
	*b++ = 8;
	*b++ = height >> 8;
	*b++ = height;
	*b++ = width >> 8;
	*b++ = width;
	*b++ = ZR36050_NCOMP;
	for (i = 0; i < ZR36050_NCOMP; i++) {
		*b++ = i;
		*b++ = zr36050_sampling[i];
		*b++ = zr36050_qtable[i];
	}

	/* SOS: components with their DC/AC tables, Ss 0, Se 63, Ah/Al 0. */
	b = mk->sos;
	*b++ = ZR36050_NCOMP;
	for (i = 0; i < ZR36050_NCOMP; i++) {
		*b++ = i;
		*b++ = zr36050_htable[i];
	}
	*b++ = 0;
	*b++ = 63;
	*b++ = 0;

	/* DRI: one MCU row, as dc30.sys - not enabled in MARKERS_EN. */
	mk->dri[0] = (width / ZR36050_MCU_WIDTH) >> 8;
	mk->dri[1] = width / ZR36050_MCU_WIDTH;

	mk->dqt[0] = 0;
	memcpy(&mk->dqt[1], zr36050_qt_luma, 64);
	mk->dqt[65] = 1;
	memcpy(&mk->dqt[66], zr36050_qt_chroma, 64);

	/* APP0 "AVI1": field polarity 1 when a buffer holds both fields,
	 * as dc30.sys _MakeAPPString.
	 */
	memset(mk->app, 0, sizeof(mk->app));
	memcpy(mk->app, "AVI1", 4);
	mk->app[4] = p->two_fields ? 1 : 0;

	mk->segs[0] = (struct zr36050_seg){ ZR36050_SOF_ADDR, 0xc0, mk->sof,
					    sizeof(mk->sof) };
	mk->segs[1] = (struct zr36050_seg){ ZR36050_SOS_ADDR, 0xda, mk->sos,
					    sizeof(mk->sos) };
	mk->segs[2] = (struct zr36050_seg){ ZR36050_DRI_ADDR, 0xdd, mk->dri,
					    sizeof(mk->dri) };
	mk->segs[3] = (struct zr36050_seg){ ZR36050_DQT_ADDR, 0xdb, mk->dqt,
					    sizeof(mk->dqt) };
	mk->segs[4] = (struct zr36050_seg){ ZR36050_DHT_ADDR, 0xc4,
					    zr36050_dht, sizeof(zr36050_dht) };
	mk->segs[5] = (struct zr36050_seg){ ZR36050_APP_ADDR, 0xe0, mk->app,
					    sizeof(mk->app) };
	/* Without the string's NUL. */
	mk->segs[6] = (struct zr36050_seg){ ZR36050_COM_ADDR, 0xfe, zr36050_com,
					    sizeof(zr36050_com) - 1 };

	/* SOI + EOI, SOF and SOS always, plus the enabled optional ones. */
	mk->header_bytes = 4 + (4 + sizeof(mk->sof)) + (4 + sizeof(mk->sos)) +
			   (4 + sizeof(mk->dqt)) + (4 + sizeof(zr36050_dht)) +
			   (4 + sizeof(mk->app)) +
			   (4 + sizeof(zr36050_com) - 1);
}

int zr36050_configure(struct dc30_dev *dc30, const struct zr36050_params *p)
{
	struct zr36050_io io = { .dc30 = dc30, .latch = -1 };
	struct zr36050_markers *mk;
	unsigned int sf, i;
	u32 tcv_net, tcv_data, t;
	u8 dummy;
	int err;

	mk = kzalloc(sizeof(*mk), GFP_KERNEL);
	if (!mk)
		return -ENOMEM;
	zr36050_build_markers(p, mk);

	dc30_jpeg_reset(dc30);

	/* Table load pass. */
	zr36050_io_write(&io, ZR36050_HARDWARE, ZR36050_HW_MSTR);
	zr36050_io_write(&io, ZR36050_MODE, ZR36050_MODE_COMP |
					    ZR36050_MODE_TLM);
	zr36050_io_write(&io, ZR36050_OPTIONS, 0);
	zr36050_io_write(&io, ZR36050_MBCV, 0xf0);
	zr36050_io_write(&io, ZR36050_INT_REQ_0, 0);
	/* TCVOVF and DATOVF must be set after every reset (datasheet). */
	zr36050_io_write(&io, ZR36050_INT_REQ_1, ZR36050_ST1_TCVOVF |
						ZR36050_ST1_DATOVF);
	sf = ZR36050_SF_START;
	zr36050_io_write_be(&io, ZR36050_SF, sf, 2);
	zr36050_io_write_be(&io, ZR36050_AF, 0xffffff, 3);
	for (i = 0; i < ARRAY_SIZE(mk->segs); i++)
		zr36050_io_seg(&io, &mk->segs[i]);
	zr36050_io_write(&io, ZR36050_MARKERS_EN, ZR36050_ME_DHTI);
	zr36050_io_write(&io, ZR36050_GO, 1);
	err = io.err;
	if (!err)
		err = zr36050_wait_end(dc30);
	if (err)
		goto out;

	/* Compression pass with bit rate control. */
	io.latch = -1;
	zr36050_io_write(&io, ZR36050_MODE, ZR36050_MODE_COMP |
					    ZR36050_MODE_PASS2 |
					    ZR36050_MODE_BRC);
	zr36050_io_write(&io, ZR36050_MARKERS_EN, ZR36050_MARKERS);

	/* Target volumes in bits (dc30.sys _CoderCfg/_CalcDataVolume). */
	tcv_net = (p->field_bytes - mk->header_bytes) * 8;
	t = tcv_net - (tcv_net >> 7);
	tcv_data = t - ((t * 5) >> 6);
	zr36050_io_write_be(&io, ZR36050_TCV_NET, tcv_net, 4);
	zr36050_io_write_be(&io, ZR36050_TCV_DATA, tcv_data, 4);
	for (i = ZR36050_ACV; i < ZR36050_ACV_TRUN + 4; i++)
		zr36050_io_write(&io, i, 0);
	err = io.err;
	if (!err)
		err = zr36050_read(dc30, ZR36050_GO, &dummy);
out:
	kfree(mk);
	return err;
}

void zr36050_show(struct seq_file *m, struct dc30_dev *dc30,
		  const struct zr36050_params *p)
{
	static const struct { unsigned int addr, len; const char *name; } regs[] = {
		{ ZR36050_HARDWARE,   1, "HARDWARE" },
		{ ZR36050_MODE,       1, "MODE" },
		{ ZR36050_OPTIONS,    1, "OPTIONS" },
		{ ZR36050_MBCV,       1, "MBCV" },
		{ ZR36050_MARKERS_EN, 1, "MARKERS_EN" },
		{ ZR36050_INT_REQ_0,  1, "INT_REQ_0" },
		{ ZR36050_INT_REQ_1,  1, "INT_REQ_1" },
		{ ZR36050_TCV_NET,    4, "TCV_NET" },
		{ ZR36050_TCV_DATA,   4, "TCV_DATA" },
		{ ZR36050_SF,         2, "SF" },
		{ ZR36050_AF,         3, "AF" },
		{ ZR36050_ACV,        4, "ACV" },
		{ ZR36050_ACT,        4, "ACT" },
		{ ZR36050_ACV_TRUN,   4, "ACV_TRUN" },
		{ ZR36050_STATUS_0,   1, "STATUS_0" },
		{ ZR36050_STATUS_1,   1, "STATUS_1" },
	};
	struct zr36050_markers *mk;
	unsigned int i, j, bytes = 0, bad = 0;
	u32 val;
	u8 b;
	int err = 0;

	for (i = 0; i < ARRAY_SIZE(regs) && !err; i++) {
		val = 0;
		for (j = 0; j < regs[i].len && !err; j++) {
			err = zr36050_read(dc30, regs[i].addr + j, &b);
			val = val << 8 | b;
		}
		if (!err)
			seq_printf(m, "050 %-10s 0x%03x = 0x%0*x\n", regs[i].name,
				   regs[i].addr, 2 * regs[i].len, val);
	}
	if (err) {
		seq_printf(m, "050 read failed: %d\n", err);
		return;
	}

	/* Read the marker segments back against what configure wrote. */
	mk = kzalloc(sizeof(*mk), GFP_KERNEL);
	if (!mk)
		return;
	zr36050_build_markers(p, mk);
	for (i = 0; i < ARRAY_SIZE(mk->segs) && !err; i++) {
		const struct zr36050_seg *s = &mk->segs[i];

		for (j = 0; j < s->len + 4 && !err; j++) {
			u8 want = j == 0 ? 0xff : j == 1 ? s->marker :
				  j == 2 ? (s->len + 2) >> 8 :
				  j == 3 ? (s->len + 2) & 0xff : s->body[j - 4];

			err = zr36050_read(dc30, s->addr + j, &b);
			bytes++;
			if (!err && b != want && !bad++)
				seq_printf(m, "050 marker mismatch at 0x%03x: 0x%02x, want 0x%02x\n",
					   s->addr + j, b, want);
		}
	}
	if (err)
		seq_printf(m, "050 marker read failed: %d\n", err);
	else
		seq_printf(m, "050 markers  %u bytes, %u wrong\n", bytes, bad);
	seq_printf(m, "050 header   %u bytes per field\n", mk->header_bytes);
	kfree(mk);
}

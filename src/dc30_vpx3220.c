// SPDX-License-Identifier: GPL-2.0-only
/*
 * dc30_vpx3220.c - V4L2 subdev driver for the Micronas/ITT VPX3220A video decoder
 *
 * Based on vpx3220.c, Copyright (C) 2001 Laurent Pinchart
 * Copyright (C) 2026 bytewarrior
 *
 * Ported to the current i2c_driver/v4l2_subdev API from vpx3220.c of the
 * historical GPL zoran driver 0.9.4 (Laurent Pinchart, 2001), its real,
 * working chip driver - used here as the porting basis rather than
 * writing register handling from scratch. Cross-checked against the
 * VPX3220A datasheet and the register use of dc30.sys.
 *
 * The module is dc30_vpx3220, not vpx3220: mainline has its own vpx3220
 * module (drivers/media/i2c), which lacks what dc30 needs here - the
 * dc30.sys timing mode, power handling and the sync-output switch for the
 * audio video lock.
 *
 * One deliberate correction versus the ported source: the "no signal /
 * detected norm" status readout (FP register 0xf3) used the old driver's
 * mask 0x18 for the norm field, which - per the official datasheet - is
 * actually a 3-bit field at bits[4:2] (mask 0x1c, shift 2). The old mask
 * left several of its own switch-case values unreachable, which is itself
 * evidence for the datasheet's bit layout being the correct one. Since the
 * exact norm-code-to-standard mapping under the corrected shift is not yet
 * empirically confirmed against real hardware, this driver only exposes
 * the datasheet-confirmed "no signal" bit (bit 5) via g_input_status and
 * does not guess a norm mapping from the field.
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/swab.h>
#include <linux/version.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>

#include "dc30_vpx3220.h"

/* ---- direct I2C registers ---- */
#define VPX3220_REG_MFG_ID		0x00
#define VPX3220_REG_PN_LOW		0x01
#define VPX3220_REG_PN_HIGH		0x02
#define VPX3220_REG_SOURCE		0x33
/* Luma and chroma ADC stand-by (bits 6/7 of 0x33), what dc30.sys
 * _VPX_DecoderDeinit sets. The rest of the chip keeps running and keeps
 * its registers, so leaving stand-by needs no re-initialization.
 */
#define VPX3220_SOURCE_STANDBY		0xc0
#define VPX3220_REG_BRIGHTNESS		0xe6
#define VPX3220_REG_CONTRAST		0xe7
#define VPX3220_REG_FPRD		0x26	/* FP read-address pointer */
#define VPX3220_REG_FPWR		0x27	/* FP write-address pointer */
#define VPX3220_REG_FPDAT		0x28	/* FP data (read or write) */
#define VPX3220_REG_FPSTAT		0x29	/* FP handshake status */

/* Direct register 0xf2 - port A/B (PIXCLK, HREF, VREF, ...) output driver
 * enable. Distinct address space from the FP-indirect 0xf2 below (same
 * register number, different chip resource). Without this, the chip
 * locks onto its
 * analog input fine internally but never drives a pixel clock/sync toward
 * the ZR36057, so the VFE never sees a vsync at all.
 */
#define VPX3220_REG_OUTPUT_EN		0xf2
#define VPX3220_OUTPUT_EN_VAL		0x1b
/* Sync outputs only (HREF, VREF, ... and LLC), video ports high-impedance.
 * HREF feeds the AD1843's SYNC1 pin, so audio in video lock mode needs
 * this while no video streams (it ran at 39.5kHz instead of 44.1kHz with
 * HREF off). dc30.sys keeps it on whenever the
 * card is idle (_VPX_DecoderEnable: 0x58); this driver only while an
 * audio capture in video lock mode asks for it (VPX3220_IOCTL_SYNC_OUT),
 * all outputs are off otherwise (0x00) - they drive a 13.5MHz bus.
 */
#define VPX3220_OUTPUT_SYNC_VAL		0x18

/* ---- FP (indirect) registers ---- */
#define VPX3220_FP_HUE			0x1c
#define VPX3220_FP_SATURATION		0xa0
#define VPX3220_FP_CABLE_FMT		0xf2
#define VPX3220_FP_STATUS		0xf3
#define VPX3220_FP_CABLE_LATCH		0x0010	/* in VPX3220_FP_CABLE_FMT */
#define VPX3220_FP_VSTD			0xe7	/* vertical standard */
#define VPX3220_FP_PLL_GAIN		0x4b
#define VPX3220_FP_C1			0xc1	/* not in the datasheet */
#define VPX3220_FP_50			0x50	/* chip variant, not in the
						 * datasheet */

#define VPX3220_MFG_ID			0xec

#define VPX3220_FP_STATUS_NOSIGNAL	(1 << 5)

#define VPX3220_FP_STATUS_BUSY		0x04
#define VPX3220_FP_TIMEOUT_COUNT	100

#define VPX3220_CONTRAST_NOISE_SHAPE	0xc0	/* fixed mode from init_common */

/* Every latch of the cable format/standard (FP 0xf2 bit 4) loads the
 * firmware's values for the standard, among them FP 0xe7 = 0x26f: vertical
 * standard lock on, 311 lines. dc30.sys waits for the latch and then, on
 * chips with FP 0x50 = 0x400 or 0x410 (ours: 0x400), turns the lock off
 * and sets FP 0xc1 and the PLL gain (_VPX_FixAfterStandardLatch).
 * With the lock on, the VPX pulled its
 * field raster over 3-5 frames after a cut on an edited tape, the fields
 * came out swapped (top lines from the bottom field) meanwhile, then it
 * jumped (seen on the board).
 */
static bool vpx3220_fix_latch = true;
module_param_named(fix_latch, vpx3220_fix_latch, bool, 0644);
MODULE_PARM_DESC(fix_latch, "After each standard latch set FP 0xe7 (vertical lock off), 0xc1 and 0x4b as dc30.sys (default on; takes effect at the next input/standard change)");

struct vpx3220 {
	struct v4l2_subdev sd;
	struct v4l2_ctrl_handler hdl;
	v4l2_std_id std;
	unsigned int input;

	/* Power and output state (out_lock), see vpx3220_apply_outputs().
	 * Starts powered down: dc30 powers the decoder up while someone
	 * uses it.
	 */
	struct mutex out_lock;
	bool powered;
	bool streaming;
	bool sync_out;

	/* Bring-up register access through debugfs, see vpx3220_dbg_write(). */
	struct dentry *dbg;
	bool dbg_fp;
	u8 dbg_addr;
};

static inline struct vpx3220 *to_vpx3220(struct v4l2_subdev *sd)
{
	return container_of(sd, struct vpx3220, sd);
}

/* ---- low-level register access ---- */

static int vpx3220_write(struct i2c_client *client, u8 reg, u8 val)
{
	return i2c_smbus_write_byte_data(client, reg, val);
}

static int vpx3220_read(struct i2c_client *client, u8 reg)
{
	return i2c_smbus_read_byte_data(client, reg);
}

static int vpx3220_write_block(struct i2c_client *client, const u8 *data,
				unsigned int len)
{
	int ret;

	while (len >= 2) {
		ret = vpx3220_write(client, data[0], data[1]);
		if (ret < 0)
			return ret;
		data += 2;
		len -= 2;
	}

	return 0;
}

static int vpx3220_fp_status(struct i2c_client *client)
{
	unsigned int i;

	for (i = 0; i < VPX3220_FP_TIMEOUT_COUNT; i++) {
		int status = vpx3220_read(client, VPX3220_REG_FPSTAT);

		if (status < 0)
			return status;
		if (!(status & VPX3220_FP_STATUS_BUSY))
			return 0;
		udelay(10);
	}

	return -ETIMEDOUT;
}

static int vpx3220_fp_write(struct i2c_client *client, u8 fpaddr, u16 data)
{
	int ret;

	ret = i2c_smbus_write_word_data(client, VPX3220_REG_FPWR,
					 swab16(fpaddr));
	if (ret < 0)
		return ret;

	ret = vpx3220_fp_status(client);
	if (ret < 0)
		return ret;

	return i2c_smbus_write_word_data(client, VPX3220_REG_FPDAT,
					  swab16(data));
}

static int vpx3220_fp_read(struct i2c_client *client, u8 fpaddr)
{
	int ret, data;

	ret = i2c_smbus_write_word_data(client, VPX3220_REG_FPRD,
					 swab16(fpaddr));
	if (ret < 0)
		return ret;

	ret = vpx3220_fp_status(client);
	if (ret < 0)
		return ret;

	data = i2c_smbus_read_word_data(client, VPX3220_REG_FPDAT);
	if (data < 0)
		return data;

	return swab16(data);
}

static int vpx3220_write_fp_block(struct i2c_client *client, const u16 *data,
				   unsigned int len)
{
	int ret;

	while (len >= 2) {
		ret = vpx3220_fp_write(client, data[0], data[1]);
		if (ret < 0)
			return ret;
		data += 2;
		len -= 2;
	}

	return 0;
}

/* ---- init/standard register tables, transcribed from the GPL driver ---- */

static const u8 vpx3220_init_common[] = {
	0xf2, 0x00,		/* Disable all outputs */
	0x33, 0x0d,		/* Luma: VIN2, Chroma: CIN (clamp off) */
	0xd8, 0x80,		/* HREF/VREF active high, VREF pulse = 2 */
	0x20, 0x03,		/* IF compensation 0dB/oct */
	0xe0, 0xff,		/* Open up all comparators */
	0xe1, 0x00,
	0xe2, 0x7f,
	0xe3, 0x80,
	0xe4, 0x7f,
	0xe5, 0x80,
	0xe6, 0x00,		/* Brightness set to 0 */
	0xe7, 0xe0,		/* Contrast 1.0, noise shaping mode */
	0xe8, 0xf8,		/* YUV422, CbCr binary offset */
	0xea, 0x18,		/* LLC2 connected, FIFO reset with VACTintern */
	0xf0, 0x8a,		/* Half full level 10, bus shuffler */
	0xf1, 0x18,		/* Single clock, sync mode */
	0xf8, 0x12,		/* Port A drive strength */
	0xf9, 0x24,		/* Port B drive strength */
};

static const u16 vpx3220_init_fp[] = {
	0x59, 0,
	0xa0, 2070,		/* ACC reference / default saturation */
	0xa3, 0,
	0xa4, 0,
	0xa8, 30,
	0xb2, 768,
	0xbe, 27,
	0x58, 0,
	0x26, 0,
	0x4b, 0x298,		/* PLL gain */
};

/* FP 0xf0 (CMDWD): 13.5 MHz transport, sync timing mode Open, transport
 * rate, timing mode and both windows latched, odd/even toggling always -
 * the state dc30.sys leaves it in (_VPX_DecoderInit: 0, transport rate
 * 1, _SetTimingMode(0); later writes keep those bits).
 * - Open: HREF and VREF track the input and keep running through
 *   drop-outs. The GPL driver's PAL/SECAM value 0x177 was Forced, which
 *   suppresses the video ports and switches to free-running syncs when
 *   the input leaves the timing tolerances (datasheet 3.2.2).
 * - Bit 8 (VPX3220_CMDWD_ODDEVEN, vpx3220_field_follow): clear, the field
 *   flag toggles every field, as in dc30.sys. It then misses a field jump
 *   in the input: after a cut on an edited tape the fields came out
 *   swapped (top lines from the bottom field) for 3-5 frames until the
 *   flag caught up (seen on the board). Set, it follows the input's
 *   odd/even and catches the jump at the cut - but on the same tape it
 *   misread the odd/even again and again in some scenes, flipping for
 *   5-20 frames at a time (3700 switches in 82 min, half of the frames
 *   between them swapped, 74 s of silence in the sound from the extra
 *   fields), so it stays off. Without input it delivers bottom fields
 *   only; the driver pairs them up.
 */
#define VPX3220_CMDWD_STD		0x073
#define VPX3220_CMDWD_ODDEVEN		0x100

static bool vpx3220_field_follow;
module_param_named(field_follow, vpx3220_field_follow, bool, 0644);
MODULE_PARM_DESC(field_follow, "Field flag follows the input's odd/even instead of toggling every field as dc30.sys (default off); takes effect at the next standard change");

static const u16 vpx3220_init_ntsc[] = {
	0x1c, 0x000,		/* NTSC tint angle */
	0x88, 17,
	0x89, 240,
	0x8a, 240,
	0x8b, 0,
	0x8c, 640,
	0x8d, 640,
	0x8f, 0xc00,		/* Disable window 2 */
	0xf0, VPX3220_CMDWD_STD,
	0xf2, 0x013,		/* NTSC-M, composite input */
	0xe7, 0x1e1,		/* Vertical standard lock @ 240 lines */
};

static const u16 vpx3220_init_pal[] = {
	0x88, 23 - 16,
	0x89, 288 + 16,
	0x8a, 288 + 16,
	0x8b, 16,
	0x8c, 768,
	0x8d, 784,
	0x8f, 0xc00,
	0xf0, VPX3220_CMDWD_STD,
	0xf2, 0x3d1,		/* PAL B,G,H,I, composite input */
	0xe7, 0x261,
};

static const u16 vpx3220_init_secam[] = {
	0x88, 23 - 16,
	0x89, 288 + 16,
	0x8a, 288 + 16,
	0x8b, 16,
	0x8c, 768,
	0x8d, 784,
	0x8f, 0xc00,
	0xf0, VPX3220_CMDWD_STD,
	0xf2, 0x3d5,		/* SECAM, composite input */
	0xe7, 0x261,
};

/* input[n] -> { direct reg 0x33 value, FP 0xf2 S-Video bit } */
static const struct {
	u8 reg33;
	bool svideo;
} vpx3220_inputs[] = {
	[0] = { 0x0c, false },	/* internal / tuner */
	[1] = { 0x0d, false },	/* composite */
	[2] = { 0x0e, true },	/* S-Video */
};

/* ---- v4l2_subdev video ops ---- */

static int vpx3220_set_routing(struct v4l2_subdev *sd, u32 input, u32 output,
				u32 config);

static int vpx3220_set_std(struct v4l2_subdev *sd, v4l2_std_id std)
{
	struct vpx3220 *dec = to_vpx3220(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	const u16 *table;
	u16 buf[32];
	unsigned int i, len;
	int ret;

	if (std & V4L2_STD_NTSC) {
		table = vpx3220_init_ntsc;
		len = ARRAY_SIZE(vpx3220_init_ntsc);
	} else if (std & V4L2_STD_SECAM) {
		table = vpx3220_init_secam;
		len = ARRAY_SIZE(vpx3220_init_secam);
	} else if (std & V4L2_STD_PAL) {
		table = vpx3220_init_pal;
		len = ARRAY_SIZE(vpx3220_init_pal);
	} else {
		return -EINVAL;
	}

	if (WARN_ON(len > ARRAY_SIZE(buf)))
		return -EINVAL;
	memcpy(buf, table, len * sizeof(*buf));
	for (i = 0; i < len; i += 2)
		if (buf[i] == 0xf0 && vpx3220_field_follow)
			buf[i + 1] |= VPX3220_CMDWD_ODDEVEN;

	ret = vpx3220_write_fp_block(client, buf, len);
	if (ret < 0)
		return ret;

	dec->std = std;

	/* The standard tables rewrite FP 0xf2 with the S-Video bit (0x20)
	 * cleared, so re-apply the current input's cable format. Otherwise a
	 * standard change after selecting S-Video leaves the chroma input
	 * unused: the color killer engages and Cb/Cr come out as a constant
	 * 128. The GPL driver has the same trap, but it always set the norm
	 * before the input.
	 */
	return vpx3220_set_routing(sd, dec->input, 0, 0);
}

/* dc30.sys _VPX_DecoderSetStandard: up to 100 reads for the latch bit to
 * clear, then _VPX_FixAfterStandardLatch.
 */
static int vpx3220_after_latch(struct vpx3220 *dec, struct i2c_client *client)
{
	bool lines525 = dec->std & V4L2_STD_525_60;
	unsigned int i;
	int data;

	if (!vpx3220_fix_latch)
		return 0;

	for (i = 0; i < 100; i++) {
		data = vpx3220_fp_read(client, VPX3220_FP_CABLE_FMT);
		if (data < 0)
			return data;
		if (!(data & VPX3220_FP_CABLE_LATCH))
			break;
	}
	if (i == 100) {
		dev_warn(&client->dev, "standard latch still pending\n");
		return 0;
	}

	data = vpx3220_fp_read(client, VPX3220_FP_50);
	if (data < 0)
		return data;
	if (data != 0x400 && data != 0x410)
		return 0;

	return vpx3220_fp_write(client, VPX3220_FP_C1,
				lines525 ? 0x56 : 0x62) ?:
	       vpx3220_fp_write(client, VPX3220_FP_VSTD,
				lines525 ? 0x20a : 0x26e) ?:
	       vpx3220_fp_write(client, VPX3220_FP_PLL_GAIN, 0x29c);
}

static int vpx3220_set_routing(struct v4l2_subdev *sd, u32 input, u32 output,
				u32 config)
{
	struct vpx3220 *dec = to_vpx3220(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int data, ret;

	if (input >= ARRAY_SIZE(vpx3220_inputs))
		return -EINVAL;

	mutex_lock(&dec->out_lock);
	ret = vpx3220_write(client, VPX3220_REG_SOURCE,
			     vpx3220_inputs[input].reg33 |
			     (dec->powered ? 0 : VPX3220_SOURCE_STANDBY));
	mutex_unlock(&dec->out_lock);
	if (ret < 0)
		return ret;

	data = vpx3220_fp_read(client, VPX3220_FP_CABLE_FMT);
	if (data < 0)
		return data;

	data &= ~0x0020;
	if (vpx3220_inputs[input].svideo)
		data |= 0x0020;

	/* Bit 0x0010 latches the new cable format into the chip. */
	ret = vpx3220_fp_write(client, VPX3220_FP_CABLE_FMT,
			       data | VPX3220_FP_CABLE_LATCH);
	if (ret < 0)
		return ret;

	udelay(10);
	dec->input = input;
	return vpx3220_after_latch(dec, client);
}

static int vpx3220_g_input_status(struct v4l2_subdev *sd, u32 *status)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int fp_status;

	*status = 0;

	fp_status = vpx3220_fp_read(client, VPX3220_FP_STATUS);
	if (fp_status < 0)
		return fp_status;

	if (fp_status & VPX3220_FP_STATUS_NOSIGNAL)
		*status |= V4L2_IN_ST_NO_SIGNAL;

	return 0;
}

/* Output enable from the power/stream state. Caller holds out_lock. */
static int vpx3220_apply_outputs(struct vpx3220 *dec)
{
	struct i2c_client *client = v4l2_get_subdevdata(&dec->sd);
	u8 val = 0;

	if (dec->powered && dec->streaming)
		val = VPX3220_OUTPUT_EN_VAL;
	else if (dec->powered && dec->sync_out)
		val = VPX3220_OUTPUT_SYNC_VAL;

	return vpx3220_write(client, VPX3220_REG_OUTPUT_EN, val);
}

static int vpx3220_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct vpx3220 *dec = to_vpx3220(sd);
	int ret;

	mutex_lock(&dec->out_lock);
	dec->streaming = enable;
	ret = vpx3220_apply_outputs(dec);
	mutex_unlock(&dec->out_lock);
	return ret;
}

/* Power saving as dc30.sys _VPX_DecoderDeinit: both ADCs to stand-by,
 * and all outputs off. On again, the decoder needs a few fields to lock
 * onto the input.
 */
static int vpx3220_s_power(struct v4l2_subdev *sd, int on)
{
	struct vpx3220 *dec = to_vpx3220(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;

	mutex_lock(&dec->out_lock);
	dec->powered = on;
	ret = vpx3220_write(client, VPX3220_REG_SOURCE,
			    vpx3220_inputs[dec->input].reg33 |
			    (on ? 0 : VPX3220_SOURCE_STANDBY));
	if (ret >= 0)
		ret = vpx3220_apply_outputs(dec);
	mutex_unlock(&dec->out_lock);
	return ret < 0 ? ret : 0;
}

static long vpx3220_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
	struct vpx3220 *dec = to_vpx3220(sd);
	int ret;

	if (cmd != VPX3220_IOCTL_SYNC_OUT)
		return -ENOIOCTLCMD;

	mutex_lock(&dec->out_lock);
	dec->sync_out = *(bool *)arg;
	ret = vpx3220_apply_outputs(dec);
	mutex_unlock(&dec->out_lock);
	return ret < 0 ? ret : 0;
}

static int vpx3220_hw_init(struct vpx3220 *dec);

/* Full register set-up again, keeping the current standard, input,
 * controls and power state - for when the chip may have lost its
 * registers (the card was powered down).
 */
static int vpx3220_init(struct v4l2_subdev *sd, u32 val)
{
	struct vpx3220 *dec = to_vpx3220(sd);
	int ret;

	ret = vpx3220_hw_init(dec);
	if (ret < 0)
		return ret;
	return v4l2_ctrl_handler_setup(&dec->hdl);
}

static const struct v4l2_subdev_core_ops vpx3220_core_ops = {
	.init = vpx3220_init,
	.s_power = vpx3220_s_power,
	.ioctl = vpx3220_ioctl,
};

static const struct v4l2_subdev_video_ops vpx3220_video_ops = {
	.s_std = vpx3220_set_std,
	.s_routing = vpx3220_set_routing,
	.g_input_status = vpx3220_g_input_status,
	.s_stream = vpx3220_s_stream,
};

static const struct v4l2_subdev_ops vpx3220_ops = {
	.core = &vpx3220_core_ops,
	.video = &vpx3220_video_ops,
};

/* ---- controls ---- */

static int vpx3220_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct vpx3220 *dec = container_of(ctrl->handler, struct vpx3220, hdl);
	struct i2c_client *client = v4l2_get_subdevdata(&dec->sd);

	switch (ctrl->id) {
	case V4L2_CID_BRIGHTNESS:
		return vpx3220_write(client, VPX3220_REG_BRIGHTNESS,
				      (s8)ctrl->val);
	case V4L2_CID_CONTRAST:
		return vpx3220_write(client, VPX3220_REG_CONTRAST,
				      (ctrl->val & 0x3f) |
				      VPX3220_CONTRAST_NOISE_SHAPE);
	case V4L2_CID_SATURATION:
		return vpx3220_fp_write(client, VPX3220_FP_SATURATION,
					 ctrl->val);
	case V4L2_CID_HUE:
		return vpx3220_fp_write(client, VPX3220_FP_HUE,
					 ctrl->val & 0xfff);
	}

	return -EINVAL;
}

static const struct v4l2_ctrl_ops vpx3220_ctrl_ops = {
	.s_ctrl = vpx3220_s_ctrl,
};

/* ---- debugfs register access (bring-up) ----
 *
 * /sys/kernel/debug/vpx3220-<bus>-<addr>/reg:
 *   echo "d8 3c"      > reg    write direct register 0xd8
 *   echo "fp f0 177"  > reg    write FP register 0xf0
 *   echo "fp f0"      > reg    select FP register 0xf0 for reading
 *   cat reg                    read the selected register
 * All numbers hex. Not serialized against the V4L2 ops - for experiments
 * while nothing else touches the decoder.
 */

static ssize_t vpx3220_dbg_write(struct file *file, const char __user *ubuf,
				 size_t count, loff_t *ppos)
{
	struct i2c_client *client = file->private_data;
	struct vpx3220 *dec = to_vpx3220(i2c_get_clientdata(client));
	unsigned int addr, val;
	char buf[32];
	bool fp;
	int n, ret = 0;

	if (count >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;
	buf[count] = '\0';

	fp = !strncmp(buf, "fp ", 3);
	n = sscanf(buf + (fp ? 3 : 0), "%x %x", &addr, &val);
	if (n < 1 || addr > 0xff || (n == 2 && val > (fp ? 0xfff : 0xff)))
		return -EINVAL;

	dec->dbg_fp = fp;
	dec->dbg_addr = addr;
	if (n == 2)
		ret = fp ? vpx3220_fp_write(client, addr, val)
			 : vpx3220_write(client, addr, val);
	return ret < 0 ? ret : count;
}

static ssize_t vpx3220_dbg_read(struct file *file, char __user *ubuf,
				size_t count, loff_t *ppos)
{
	struct i2c_client *client = file->private_data;
	struct vpx3220 *dec = to_vpx3220(i2c_get_clientdata(client));
	char buf[32];
	int val, len;

	if (*ppos)
		return 0;
	val = dec->dbg_fp ? vpx3220_fp_read(client, dec->dbg_addr)
			  : vpx3220_read(client, dec->dbg_addr);
	if (val < 0)
		return val;
	len = scnprintf(buf, sizeof(buf), "%s%02x = 0x%03x\n",
			dec->dbg_fp ? "fp " : "", dec->dbg_addr, val);
	return simple_read_from_buffer(ubuf, count, ppos, buf, len);
}

static const struct file_operations vpx3220_dbg_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = vpx3220_dbg_read,
	.write = vpx3220_dbg_write,
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
	.llseek = no_llseek,	/* the default from 6.12 on, and gone */
#endif
};

static void vpx3220_debugfs_init(struct i2c_client *client,
				 struct vpx3220 *dec)
{
	char name[32];

	snprintf(name, sizeof(name), "vpx3220-%d-%04x",
		 i2c_adapter_id(client->adapter), client->addr);
	dec->dbg = debugfs_create_dir(name, NULL);
	debugfs_create_file("reg", 0600, dec->dbg, client, &vpx3220_dbg_fops);
}

/* ---- initialization ---- */

/* Everything but the controls (their handler writes those): common
 * tables, standard, input, then the power/output state - all outputs off
 * and the ADCs in stand-by until dc30 powers the decoder up.
 */
static int vpx3220_hw_init(struct vpx3220 *dec)
{
	struct i2c_client *client = v4l2_get_subdevdata(&dec->sd);
	int ret;

	ret = vpx3220_write_block(client, vpx3220_init_common,
				   ARRAY_SIZE(vpx3220_init_common));
	if (ret < 0)
		return ret;

	ret = vpx3220_write_fp_block(client, vpx3220_init_fp,
				      ARRAY_SIZE(vpx3220_init_fp));
	if (ret < 0)
		return ret;

	/* set_std() also re-applies the input (set_routing()). */
	ret = vpx3220_set_std(&dec->sd, dec->std);
	if (ret < 0)
		return ret;

	mutex_lock(&dec->out_lock);
	ret = vpx3220_apply_outputs(dec);
	mutex_unlock(&dec->out_lock);
	return ret < 0 ? ret : 0;
}

/* ---- i2c_driver / probe ---- */

static int vpx3220_probe(struct i2c_client *client)
{
	struct vpx3220 *dec;
	const char *name;
	int id, pn_lo, pn_hi, pn, ret;

	id = vpx3220_read(client, VPX3220_REG_MFG_ID);
	if (id < 0) {
		dev_err(&client->dev,
			"no response at 0x%02x (%d) - wrong address or I2C bus not up\n",
			client->addr, id);
		return id;
	}
	if (id != VPX3220_MFG_ID) {
		dev_err(&client->dev,
			"unexpected manufacturer ID 0x%02x (want 0x%02x)\n",
			id, VPX3220_MFG_ID);
		return -ENODEV;
	}

	pn_lo = vpx3220_read(client, VPX3220_REG_PN_LOW);
	pn_hi = vpx3220_read(client, VPX3220_REG_PN_HIGH);
	if (pn_lo < 0)
		return pn_lo;
	if (pn_hi < 0)
		return pn_hi;
	pn = (pn_hi << 8) | pn_lo;

	switch (pn) {
	case 0x4680:
		name = "vpx3220a";
		break;
	case 0x4260:
		name = "vpx3216b";
		break;
	case 0x4280:
		name = "vpx3214c";
		break;
	default:
		dev_err(&client->dev, "unknown part number 0x%04x\n", pn);
		return -ENODEV;
	}

	dec = devm_kzalloc(&client->dev, sizeof(*dec), GFP_KERNEL);
	if (!dec)
		return -ENOMEM;

	v4l2_i2c_subdev_init(&dec->sd, client, &vpx3220_ops);
	mutex_init(&dec->out_lock);
	dec->std = V4L2_STD_PAL;
	dec->input = 1;		/* default: composite */

	v4l2_ctrl_handler_init(&dec->hdl, 4);
	v4l2_ctrl_new_std(&dec->hdl, &vpx3220_ctrl_ops,
			   V4L2_CID_BRIGHTNESS, -128, 127, 1, 0);
	v4l2_ctrl_new_std(&dec->hdl, &vpx3220_ctrl_ops,
			   V4L2_CID_CONTRAST, 0, 0x3f, 1, 32);
	v4l2_ctrl_new_std(&dec->hdl, &vpx3220_ctrl_ops,
			   V4L2_CID_SATURATION, 0, 4095, 1, 2070);
	v4l2_ctrl_new_std(&dec->hdl, &vpx3220_ctrl_ops,
			   V4L2_CID_HUE, -2048, 2047, 1, 0);
	if (dec->hdl.error) {
		ret = dec->hdl.error;
		goto err_free_ctrl;
	}
	dec->sd.ctrl_handler = &dec->hdl;

	ret = vpx3220_hw_init(dec);
	if (ret < 0)
		goto err_free_ctrl;

	vpx3220_debugfs_init(client, dec);

	dev_info(&client->dev,
		 "%s decoder found (mfg 0x%02x, pn 0x%04x) at 0x%02x\n",
		 name, id, pn, client->addr);

	return 0;

err_free_ctrl:
	v4l2_ctrl_handler_free(&dec->hdl);
	return ret;
}

static void vpx3220_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct vpx3220 *dec = to_vpx3220(sd);

	debugfs_remove_recursive(dec->dbg);
	v4l2_device_unregister_subdev(sd);
	v4l2_ctrl_handler_free(&dec->hdl);
}

static const struct i2c_device_id vpx3220_id[] = {
	{ "dc30-vpx3220" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, vpx3220_id);

static struct i2c_driver vpx3220_driver = {
	.driver = {
		.name = "dc30-vpx3220",
	},
	.probe = vpx3220_probe,
	.remove = vpx3220_remove,
	.id_table = vpx3220_id,
};
module_i2c_driver(vpx3220_driver);

MODULE_DESCRIPTION("Micronas/ITT VPX3220A video decoder driver");
MODULE_AUTHOR("bytewarrior");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0.0");

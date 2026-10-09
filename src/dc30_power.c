// SPDX-License-Identifier: GPL-2.0-only
/*
 * dc30_power.c - board set-up and power management
 *
 * The reset sequence in dc30_reset() is based on zr36057_restart() in
 * zoran_device.c of the GPL zoran driver, Copyright (C) 2000 Serguei
 * Miridonov, maintained by Ronald Bultje and Laurent Pinchart.
 * Copyright (C) 2026 bytewarrior
 *
 * The card is kept in its lowest power state whenever nobody uses it,
 * right from probe on. What is off when:
 *
 *   ADV7176 encoder   always: all four DACs and "lower power" mode, as
 *                     dc30.sys _ADV_EncoderDeinit (no video output yet)
 *   ZR36050 JPEG      always but while MJPEG streams: reset, then
 *                     stand-by (GPIO2 low) - held in reset it draws its
 *                     full current (raw capture: raw_jpeg_standby)
 *   VPX3220 decoder   ADCs in stand-by and all outputs off while no V4L2
 *                     node is open (dc30_decoder_get/put())
 *   AD1843 codec      converters powered down while no PCM is open and
 *                     the analog loop-through is off (dc30_audio.c)
 *   ZR36057           runtime PM: when nothing holds the device for
 *                     idle_delay_ms, the board goes to dc30.sys's
 *                     _I22_PowerDownBoard state - soft reset asserted,
 *                     all GPIO pins inputs (module parameter
 *                     idle_board_powerdown)
 *
 * dc30.sys itself only powers down this far when the driver unloads;
 * while merely closed it keeps decoder, encoder and the analog
 * loop-throughs of picture and sound running (_DC30DispatchOpenClose).
 *
 * The ZR36057 has no PCI power management capability, so runtime PM
 * keeps it in D0; the soft reset is what saves power there. Coming back
 * takes a full re-initialization of the ZR36057 and the decoder
 * (dc30_pm_runtime_resume()).
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/seq_file.h>
#include <media/v4l2-subdev.h>

#include "dc30.h"
#include "dc30_audio.h"
#include "dc30_power.h"
#include "dc30_vpx3220.h"
#include "zr36057.h"

static unsigned int dc30_idle_delay_ms = 2000;
module_param_named(idle_delay_ms, dc30_idle_delay_ms, uint, 0444);
MODULE_PARM_DESC(idle_delay_ms, "Put the board to sleep this long after the last user is gone (default 2000; later changes via power/autosuspend_delay_ms in sysfs)");

static bool dc30_raw_jpeg_standby = true;
module_param_named(raw_jpeg_standby, dc30_raw_jpeg_standby, bool, 0644);
MODULE_PARM_DESC(raw_jpeg_standby, "Raw (YUYV) capture: ZR36050 in stand-by instead of clocked and held in reset (default 1; takes effect at the next stream start)");

static bool dc30_idle_board_powerdown = true;
module_param_named(idle_board_powerdown, dc30_idle_board_powerdown, bool, 0644);
MODULE_PARM_DESC(idle_board_powerdown, "While idle, hold the ZR36057 in soft reset with all GPIOs as inputs, as dc30.sys _I22_PowerDownBoard (default 1; takes effect at the next suspend)");

/* ADV7176 video encoder, 7-bit address 0x2a (dc30.sys probes 8-bit 0x54
 * first, the I2C scan on the board found it there).
 */
#define DC30_ADV7176_I2C_ADDR	0x2a
#define ADV7176_REG_MR1		0x01
#define ADV7176_MR1_DACS_OFF	0x78	/* MR13-MR16: all four DACs down */
#define ADV7176_REG_MR2		0x0d
#define ADV7176_MR2_LOW_POWER	0xc0	/* MR27 lower power, MR26 */
#define ADV7176_NUM_REGS	0x13

/* DC30 GPIO assignment (ZR36057_GPPGCR1[31:24], bit n = 1 << (24 + n)),
 * confirmed by both the GPL zoran driver's DC30 table (zoran_card.c) and
 * the _I22_ModifyGPIO call arguments in dc30.sys.
 */
#define DC30_GPIO_JPEG_RESET	1	/* ZR36050 reset, active low */
#define DC30_GPIO_050CLK	2	/* ZR36050 clock / "JPEG sleep", 1 = running */
#define DC30_GPIO_VID_DIR	3	/* video bus direction, 1 = decoder -> ZR36057 */
#define DC30_GPIO_CLK_SEL1	4
#define DC30_GPIO_CLK_SEL2	5
#define DC30_GPIO_VID_EN	7	/* video bus sync signals, active low */

/* dc30.sys _I22_PowerDownBoard: SoftReset (bit 24) cleared = reset
 * asserted, GenPurDir 0xff = all GPIOs inputs.
 */
#define DC30_SPGPPCR_POWERDOWN	0xff

static struct v4l2_subdev *dc30_decoder_sd(struct dc30_dev *dc30)
{
	return dc30->decoder ? i2c_get_clientdata(dc30->decoder) : NULL;
}

/* ---- ZR36057 GPIOs and reset ---- */

static void dc30_gpio(struct dc30_dev *dc30, unsigned int bit, bool val)
{
	u32 reg = dc30_read(dc30, ZR36057_GPPGCR1);

	if (val)
		reg |= 1 << (24 + bit);
	else
		reg &= ~(1 << (24 + bit));
	dc30_write(dc30, ZR36057_GPPGCR1, reg);
	udelay(1);
}

static bool dc30_gpio_get(struct dc30_dev *dc30, unsigned int bit)
{
	return dc30_read(dc30, ZR36057_GPPGCR1) & (1 << (24 + bit));
}

static void dc30_reset(struct dc30_dev *dc30)
{
	/* GuestBus/GPIO bring-up sequence, replicated from the proven GPL
	 * zoran driver's zr36057_restart() (zoran_device.c). The
	 * bit-banged I2C bus in dc30_i2c.c rides on
	 * this same GuestBus block via ZR36057_I2CBR - without this reset/
	 * timing setup, I2CBR is writable but the bus never actually comes
	 * up, and every transaction fails silently at the first byte.
	 */
	dc30_write(dc30, ZR36057_SPGPPCR, 0);
	mdelay(1);
	dc30_write(dc30, ZR36057_SPGPPCR,
		   dc30_read(dc30, ZR36057_SPGPPCR) | ZR36057_SPGPPCR_SOFTRESET);
	mdelay(1);

	/* Assert P_Reset on the JPEG codec (deasserted by an MJPEG start). */
	dc30_write(dc30, ZR36057_JPC, 0);
	/* GPIO direction (all output), soft reset still asserted. */
	dc30_write(dc30, ZR36057_SPGPPCR, ZR36057_SPGPPCR_SOFTRESET);
	/* GPIO pin config + GuestBus timing. */
	dc30_write(dc30, ZR36057_GPPGCR1, (0x81 << 24) | 0x8888);
}

/* Capture-side video bus setup, in the exact order dc30.sys's
 * _I22_OverlayEnable/_I22_SetupCapture use: stop the 050 clock, select
 * the pixel clock, restart the clock and let it settle (Set050Clock(1)
 * waits 1ms - the GPL driver's "toggle JPEG codec sleep to sync PLL"),
 * then turn the video bus toward the ZR36057 and enable the sync signals.
 *
 * VID_DIR was missing here before (left at 0 from dc30_reset()'s 0x81
 * GPIO pattern, i.e. bus pointed at the encoder) - both the GPL driver
 * (set_videobus_dir(zr, 0) with gpio_pol = 1) and dc30.sys
 * (ModifyGPIO(mask 0, value 8)) drive GPIO3 high for capture.
 *
 * The ZR36050 is held in reset (GPIO1 low) meanwhile - asserted only
 * once its clock runs, which its reset input needs. MJPEG releases it
 * (zr36050_configure()), raw capture puts it into stand-by
 * (dc30_board_capture_raw()).
 */
static void dc30_video_bus_capture(struct dc30_dev *dc30)
{
	dc30_gpio(dc30, DC30_GPIO_050CLK, 0);
	dc30_gpio(dc30, DC30_GPIO_CLK_SEL1, 0);
	dc30_gpio(dc30, DC30_GPIO_CLK_SEL2, 1);
	dc30_gpio(dc30, DC30_GPIO_050CLK, 1);
	mdelay(1);
	dc30_gpio(dc30, DC30_GPIO_JPEG_RESET, 0);
	dc30_gpio(dc30, DC30_GPIO_VID_DIR, 1);
	dc30_gpio(dc30, DC30_GPIO_VID_EN, 0);
}

/* ZR36050 into stand-by (doc/zr36050.pdf, STDBY/CLKEN): only from the
 * Idle state, i.e. right after a reset, and the reset needs the clock
 * running. Held in reset instead, it draws its full operating current
 * (320mA typ.) - stand-by is 5mA, 15mA with the clock still enabled.
 * The GPL driver keeps it asleep the same way outside JPEG work
 * (jpeg_codec_sleep()).
 */
static void dc30_jpeg_standby(struct dc30_dev *dc30)
{
	if (!dc30_gpio_get(dc30, DC30_GPIO_050CLK)) {
		dc30_gpio(dc30, DC30_GPIO_050CLK, 1);
		mdelay(1);	/* PLL: 5000 CLK_IN cycles */
	}
	dc30_gpio(dc30, DC30_GPIO_JPEG_RESET, 0);
	udelay(2);		/* >= 4 CLK_IN cycles */
	dc30_gpio(dc30, DC30_GPIO_JPEG_RESET, 1);
	udelay(2);
	dc30_gpio(dc30, DC30_GPIO_050CLK, 0);
}

/* dc30.sys _I22_ResetCodec: reset asserted for two GPIO writes, then
 * released - needs the clock running. Leaves the ZR36050 idle, ready for
 * its set-up.
 */
void dc30_jpeg_reset(struct dc30_dev *dc30)
{
	dc30_gpio(dc30, DC30_GPIO_JPEG_RESET, 0);
	dc30_gpio(dc30, DC30_GPIO_JPEG_RESET, 0);
	udelay(2);		/* >= 4 CLK_IN cycles */
	dc30_gpio(dc30, DC30_GPIO_JPEG_RESET, 1);
	udelay(2);		/* 4 CLK_IN cycles before the first access */
}

/* Video bus while nothing streams: JPEG asleep. The sync signals stay
 * enabled (VID_EN): where the AD1843's SYNC1 is tapped relative to that
 * gate is not known, and audio in video lock mode needs it without video.
 */
static void dc30_video_bus_idle(struct dc30_dev *dc30)
{
	dc30_jpeg_standby(dc30);
}

/* Raw capture does not use the ZR36050: stand-by instead of its full
 * current in reset (dc30_video_bus_capture()), as the GPL driver keeps
 * it asleep outside JPEG work. The MJPEG path brings it back itself:
 * dc30_board_capture() restarts the clock, zr36050_configure() resets.
 */
void dc30_board_capture_raw(struct dc30_dev *dc30)
{
	if (dc30_raw_jpeg_standby)
		dc30_jpeg_standby(dc30);
}

void dc30_board_capture(struct dc30_dev *dc30, bool on)
{
	if (on)
		dc30_video_bus_capture(dc30);
	else
		dc30_video_bus_idle(dc30);
}

/* ZR36057 out of reset and everything on it set up again that does not
 * belong to a stream. Leaves the ZR36050 clock running, so the I2C chips
 * can be set up before dc30_board_idle().
 */
static void dc30_board_wake(struct dc30_dev *dc30)
{
	unsigned long flags;

	dc30_reset(dc30);
	/* JPEG code FIFO threshold 20, the datasheet's recommended value
	 * (the soft reset clears it).
	 */
	dc30_write(dc30, ZR36057_JCFT, 20);
	dc30_video_bus_capture(dc30);

	/* I2C lines released (idle high), and the shadow to match. */
	spin_lock_irqsave(&dc30->reg_lock, flags);
	dc30->i2c_bits = ZR36057_I2CBR_SDA | ZR36057_I2CBR_SCL;
	dc30_write(dc30, ZR36057_I2CBR, dc30->i2c_bits);
	spin_unlock_irqrestore(&dc30->reg_lock, flags);

	dc30_write(dc30, ZR36057_ICR, 0);
	dc30_write(dc30, ZR36057_ISR, dc30_read(dc30, ZR36057_ISR));
	dc30->board_off = false;
}

static int dc30_encoder_write(struct dc30_dev *dc30, u8 reg, u8 val)
{
	union i2c_smbus_data data = { .byte = val };

	return i2c_smbus_xfer(&dc30->i2c_adap, DC30_ADV7176_I2C_ADDR, 0,
			      I2C_SMBUS_WRITE, reg, I2C_SMBUS_BYTE_DATA, &data);
}

/* As dc30.sys _ADV_EncoderDeinit: DACs off, then lower power mode. The
 * DACs alone are 140mA typ.
 */
static void dc30_encoder_powerdown(struct dc30_dev *dc30)
{
	int err;

	err = dc30_encoder_write(dc30, ADV7176_REG_MR1, ADV7176_MR1_DACS_OFF);
	if (!err)
		err = dc30_encoder_write(dc30, ADV7176_REG_MR2,
					 ADV7176_MR2_LOW_POWER);
	if (err)
		dev_warn_once(&dc30->pdev->dev,
			      "ADV7176 encoder not answering (%d), not powered down\n",
			      err);
}

/* Everything that is idle while the board is awake: encoder down,
 * ZR36050 in stand-by. Needs the ZR36050 clock running
 * (dc30_board_wake()), the encoder may take its clock from there.
 */
static void dc30_board_idle(struct dc30_dev *dc30)
{
	dc30_encoder_powerdown(dc30);
	dc30_video_bus_idle(dc30);
}

void dc30_board_init(struct dc30_dev *dc30)
{
	dc30_board_wake(dc30);
}

/* ---- decoder references ---- */

int dc30_decoder_get(struct dc30_dev *dc30, bool sync)
{
	struct v4l2_subdev *sd = dc30_decoder_sd(dc30);
	bool on = true;
	int err = 0;

	if (!sd)
		return 0;

	mutex_lock(&dc30->power_lock);
	if (!dc30->decoder_users) {
		err = v4l2_subdev_call(sd, core, s_power, 1);
		if (err)
			goto out;
		dc30->decoder_on_ns = ktime_get_ns();
		dc30_decoder_changed(dc30);
	}
	dc30->decoder_users++;

	if (sync && !dc30->decoder_sync_users) {
		err = v4l2_subdev_call(sd, core, ioctl, VPX3220_IOCTL_SYNC_OUT,
				       &on);
		if (err) {
			if (!--dc30->decoder_users)
				v4l2_subdev_call(sd, core, s_power, 0);
			goto out;
		}
	}
	if (sync)
		dc30->decoder_sync_users++;
out:
	mutex_unlock(&dc30->power_lock);
	return err;
}

void dc30_decoder_put(struct dc30_dev *dc30, bool sync)
{
	struct v4l2_subdev *sd = dc30_decoder_sd(dc30);
	bool off = false;

	if (!sd)
		return;

	mutex_lock(&dc30->power_lock);
	if (sync && !--dc30->decoder_sync_users)
		v4l2_subdev_call(sd, core, ioctl, VPX3220_IOCTL_SYNC_OUT, &off);
	if (!--dc30->decoder_users)
		v4l2_subdev_call(sd, core, s_power, 0);
	mutex_unlock(&dc30->power_lock);
}

/* After power-up, or a change of input or standard, the VPX3220 first
 * runs free at the nominal period, then pulls its vertical raster onto
 * the input: at the ZR36057 a few field interrupts come 0.5-2 ms late
 * (20.5-22.0 ms for PAL), then steadily at 20.0 +- 0.2 ms. On the board,
 * 40 cold starts: 0-5 free-running fields, pulling from 6-113
 * ms after power-up for 1-10 fields, steady 190-440 ms after power-up.
 * An MJPEG start inside that time paired the fields wrongly in about 1 of
 * 8 cold starts, and raw capture rolled for a few frames. The VPX has no
 * status bit for this - its "no video" bit (FP 0xf3) comes from the
 * standard recognition, which does not run with the standard set by
 * hand, and read 0 all along - so the raster itself is watched. The
 * free-running start looks just as steady: 8 fields in a row at the
 * nominal period (as many as the datasheet gives for its field lock at
 * most, Forced mode), and not before 250 ms after the change, twice the
 * latest start of the pulling seen. Without a signal the VPX runs free,
 * steadily: done after 250 ms too.
 */
#define DC30_DECODER_FRESH_MS		1000	/* only this long after a change */
#define DC30_DECODER_SETTLE_MIN_MS	250
#define DC30_DECODER_SETTLE_MAX_MS	800
#define DC30_DECODER_STEADY_FIELDS	8
#define DC30_DECODER_FIELD_TOL_US	300
#define DC30_DECODER_TRACE_FIELDS	24

void dc30_decoder_changed(struct dc30_dev *dc30)
{
	WRITE_ONCE(dc30->decoder_change_ns, ktime_get_ns());
}

void dc30_decoder_settle(struct dc30_dev *dc30, v4l2_std_id std)
{
	u64 change = READ_ONCE(dc30->decoder_change_ns);
	unsigned int period_us = (std & V4L2_STD_525_60) ? 16683 : 20000;
	u64 floor = change + DC30_DECODER_SETTLE_MIN_MS * NSEC_PER_MSEC;
	unsigned int steady = 0;
	u64 now = ktime_get_ns(), end, last = 0;
	char trace[DC30_DECODER_TRACE_FIELDS * 5 + 1];
	int tpos = 0, tn = 0;

	if (!dc30->decoder ||
	    now - change >= DC30_DECODER_FRESH_MS * NSEC_PER_MSEC)
		return;

	end = now + DC30_DECODER_SETTLE_MAX_MS * NSEC_PER_MSEC;
	dc30_write(dc30, ZR36057_ISR, ZR36057_ISR_GIRQ1);
	while (steady < DC30_DECODER_STEADY_FIELDS || now < floor) {
		now = ktime_get_ns();
		if (now > end)
			break;
		if (!(dc30_read(dc30, ZR36057_ISR) & ZR36057_ISR_GIRQ1)) {
			usleep_range(100, 150);
			continue;
		}
		dc30_write(dc30, ZR36057_ISR, ZR36057_ISR_GIRQ1);
		/* Field intervals in 0.1 ms, for the debug log. */
		if (tn++ < DC30_DECODER_TRACE_FIELDS)
			tpos += scnprintf(trace + tpos, sizeof(trace) - tpos,
					  " %llu", div_u64(now - (last ?: change),
							   100000));
		if (last &&
		    abs((s64)div_u64(now - last, NSEC_PER_USEC) - period_us) <=
		    DC30_DECODER_FIELD_TOL_US)
			steady++;
		else
			steady = 0;
		last = now;
		/* The next field change is a period away at the earliest. */
		usleep_range(period_us - 2000, period_us - 1500);
	}
	dc30->decoder_settle_ms = div_u64(ktime_get_ns() - change,
					  NSEC_PER_MSEC);
	trace[tpos] = 0;
	dev_dbg(&dc30->pdev->dev,
		"decoder steady after %u ms (%u steady fields), field intervals from the change (0.1 ms):%s\n",
		dc30->decoder_settle_ms, steady, trace);
}

int dc30_decoder_signal(struct dc30_dev *dc30)
{
	struct v4l2_subdev *sd = dc30_decoder_sd(dc30);
	u32 status;
	int err;

	if (!sd)
		return -ENODEV;
	err = v4l2_subdev_call(sd, video, g_input_status, &status);
	if (err)
		return err;
	return !(status & V4L2_IN_ST_NO_SIGNAL);
}

/* ---- runtime PM ---- */

int dc30_pm_get(struct dc30_dev *dc30)
{
	return pm_runtime_resume_and_get(&dc30->pdev->dev);
}

void dc30_pm_put(struct dc30_dev *dc30)
{
	pm_runtime_mark_last_busy(&dc30->pdev->dev);
	pm_runtime_put_autosuspend(&dc30->pdev->dev);
}

/* dc30.sys _I22_Deinit -> _I22_PowerDownBoard, if enabled. */
static void dc30_board_sleep(struct dc30_dev *dc30)
{
	if (!dc30_idle_board_powerdown)
		return;
	dc30_write(dc30, ZR36057_ICR, 0);
	dc30_write(dc30, ZR36057_SPGPPCR, DC30_SPGPPCR_POWERDOWN);
	dc30->board_off = true;
}

void dc30_board_shutdown(struct dc30_dev *dc30)
{
	dc30_board_sleep(dc30);
}

static int dc30_pm_runtime_suspend(struct device *dev)
{
	struct dc30_dev *dc30 = dev_get_drvdata(dev);

	/* Nobody holds a reference, so the decoder and the codec are
	 * already down and no stream runs.
	 */
	dc30_board_sleep(dc30);
	dc30->suspends++;
	return 0;
}

static int dc30_pm_runtime_resume(struct device *dev)
{
	struct dc30_dev *dc30 = dev_get_drvdata(dev);
	struct v4l2_subdev *sd = dc30_decoder_sd(dc30);
	ktime_t t0 = ktime_get();
	int err;

	if (dc30->board_off) {
		dc30_board_wake(dc30);
		dc30_audio_board_resume(dc30);
		/* The decoder keeps its registers across the ZR36057 reset
		 * as far as known, but its reset line and clock select are
		 * not documented - set it up again, it is cheap.
		 */
		if (sd) {
			err = v4l2_subdev_call(sd, core, init, 0);
			if (err)
				dev_warn(dev, "decoder re-init failed: %d\n",
					 err);
		}
		dc30_board_idle(dc30);
	}

	dc30->resumes++;
	dc30->last_resume_us = ktime_us_delta(ktime_get(), t0);
	return 0;
}

/* System sleep: the card loses power in S3 and comes back with every
 * chip reset, so the next runtime resume has to set it all up again
 * (board_off). Streams open across a system sleep are not restored.
 */
static int dc30_pm_suspend(struct device *dev)
{
	return pm_runtime_force_suspend(dev);
}

static int dc30_pm_resume(struct device *dev)
{
	struct dc30_dev *dc30 = dev_get_drvdata(dev);

	dc30->board_off = true;
	return pm_runtime_force_resume(dev);
}

const struct dev_pm_ops dc30_pm_ops = {
	SYSTEM_SLEEP_PM_OPS(dc30_pm_suspend, dc30_pm_resume)
	RUNTIME_PM_OPS(dc30_pm_runtime_suspend, dc30_pm_runtime_resume, NULL)
};

void dc30_pm_enable(struct dc30_dev *dc30)
{
	struct device *dev = &dc30->pdev->dev;

	dc30_board_idle(dc30);

	/* The PCI core holds a reference over probe and forbids runtime PM
	 * for PCI devices by default; allow it (writing "on" to
	 * power/control in sysfs keeps the card awake) and drop the probe
	 * reference.
	 */
	pm_runtime_set_autosuspend_delay(dev, dc30_idle_delay_ms);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_allow(dev);
	pm_runtime_put_autosuspend(dev);
}

void dc30_pm_disable(struct dc30_dev *dc30)
{
	struct device *dev = &dc30->pdev->dev;

	/* The PCI core has resumed the device for remove(). Undo
	 * dc30_pm_enable(): forbid (takes a reference) and the probe
	 * reference, which the core drops again after remove().
	 */
	pm_runtime_forbid(dev);
	pm_runtime_dont_use_autosuspend(dev);
	pm_runtime_get_noresume(dev);
}

/* ---- debugfs ---- */

void dc30_power_show(struct seq_file *m, struct dc30_dev *dc30)
{
	struct device *dev = &dc30->pdev->dev;
	bool suspended = pm_runtime_suspended(dev);
	u32 gpio;

	seq_printf(m, "board          %s\n",
		   !suspended ? "awake" :
		   dc30->board_off ? "asleep, ZR36057 in soft reset (PowerDownBoard)"
				   : "asleep, ZR36057 not reset");
	seq_printf(m, "idle powerdown %s\n",
		   dc30_idle_board_powerdown ? "on" : "off");
	seq_printf(m, "suspends       %lu\n", dc30->suspends);
	seq_printf(m, "resumes        %lu, last took %llu us\n",
		   dc30->resumes, dc30->last_resume_us);

	mutex_lock(&dc30->power_lock);
	seq_printf(m, "decoder        %s, %u users (%u with sync outputs)\n",
		   dc30->decoder_users ? "on" : "stand-by",
		   dc30->decoder_users, dc30->decoder_sync_users);
	if (dc30->decoder_users)
		seq_printf(m, "decoder on for %llu ms\n",
			   div_u64(ktime_get_ns() - dc30->decoder_on_ns,
				   NSEC_PER_MSEC));
	seq_printf(m, "decoder steady %u ms after power-up or input/standard change (last stream start that waited)\n",
		   dc30->decoder_settle_ms);
	mutex_unlock(&dc30->power_lock);

	dc30_audio_power_show(m, dc30);

	/* Pin levels are only meaningful while the ZR36057 drives them. */
	if (suspended && dc30->board_off) {
		seq_puts(m, "jpeg codec     GPIOs are inputs (board pull-ups/downs)\n");
		return;
	}
	gpio = dc30_read(dc30, ZR36057_GPPGCR1) >> 24;
	seq_printf(m, "jpeg codec     %s (GPIO 0x%02x)\n",
		   !(gpio & BIT(DC30_GPIO_050CLK)) ? "stand-by" :
		   !(gpio & BIT(DC30_GPIO_JPEG_RESET)) ? "clock on, held in reset"
						       : "running", gpio);
}

void dc30_encoder_regs_show(struct seq_file *m, struct dc30_dev *dc30)
{
	unsigned int reg;

	for (reg = 0; reg < ADV7176_NUM_REGS; reg++) {
		union i2c_smbus_data data;
		int err;

		err = i2c_smbus_xfer(&dc30->i2c_adap, DC30_ADV7176_I2C_ADDR, 0,
				     I2C_SMBUS_READ, reg, I2C_SMBUS_BYTE_DATA,
				     &data);
		if (err)
			seq_printf(m, "0x%02x  error %d\n", reg, err);
		else
			seq_printf(m, "0x%02x  0x%02x%s\n", reg, data.byte,
				   reg == ADV7176_REG_MR1 ? "  MR1 (0x78 = DACs off)" :
				   reg == ADV7176_REG_MR2 ? "  MR2 (0x80 = lower power)" : "");
	}
}

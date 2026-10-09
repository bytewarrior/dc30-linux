// SPDX-License-Identifier: GPL-2.0-only
/*
 * dc30_audio.c - audio ASIC (GuestBus guest 4) and AD1843 codec access
 *
 * There is no audio DMA on this board: samples, codec registers and the
 * hardware sample counter all go through a small audio ASIC on the ZR36057
 * GuestBus (guest 4), reached with PostOffice PIO accesses. Register usage
 * as found in dc30.sys:
 *
 *   guest 4 reg 0     sample FIFO
 *   guest 4 reg 1     ASIC control (0x20 = trigger); bit 0x80 read at init
 *                     selects the board variant (see dc30_audio_init())
 *   guest 4 reg 2     AD1843 access: bits 4:0 codec register, 0x40 busy /
 *                     start, 0x80 read
 *   guest 4 reg 3/4   AD1843 data word, high / low byte
 *   guest 4 reg 5/6   hardware sample counter, low / high byte
 *
 * Codec reads only work once the AD1843 runs 16-slot TDM frames: after
 * reset it uses 32 slots with a second control word in slot 16, and the
 * answer to our slot 0 read request goes out in slot 17, which the ASIC
 * doesn't sample - every read returned register 0. dc30.sys writes FRS
 * (register 26) before anything else, which is what dc30_audio_init()
 * does too.
 */

#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/ktime.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/seq_file.h>
#include <linux/vmalloc.h>

#include "dc30.h"
#include "dc30_audio.h"
#include "dc30_power.h"
#include "zr36057.h"

#define ASIC_REG_CTRL		1
#define ASIC_REG_CODEC		2
#define ASIC_REG_DATA_HI	3
#define ASIC_REG_DATA_LO	4

#define ASIC_REG_FIFO		0
#define ASIC_REG_COUNT_LO	5
#define ASIC_REG_COUNT_HI	6

#define ASIC_CTRL_VARIANT	0x80
#define ASIC_CTRL_RUN		0x20
#define ASIC_CTRL_16BIT		0x10
#define ASIC_CTRL_STEREO	0x08
#define ASIC_CTRL_PLAYBACK	0x04

#define ASIC_RING_SIZE		DC30_AUDIO_RING_SIZE
#define ASIC_READ_CHUNK		DC30_AUDIO_READ_CHUNK
#define ASIC_READ_MIN		512	/* dc30.sys Sync_AudioCapture */

#define ASIC_CODEC_READ		0x80
#define ASIC_CODEC_BUSY		0x40
#define ASIC_CODEC_ADDR_MASK	0x1f

/* dc30.sys _AudioInit: guest 4 Tdur/Trec 4/4 PCI clocks (nibble 0x5),
 * guests 5-7 at the slowest 15/15 (0xf). Nibble bits 3:2 = Tdur, 1:0 =
 * Trec, each 00b = 3, 01b = 4, 10b = 12, 11b = 15 PCI clocks.
 */
#define DC30_GCR2_GUESTS5_7	0xf0
/* ... and _AudioAsicSet widens guest 4 to Tdur 12 around each write. */
#define DC30_GCR2_GUEST4_MASK	0xf
#define DC30_GCR2_GUEST4_WRITE	0x9
#define DC30_GCR2_GUEST4_DEFAULT 0x5

#define AD1843_NUM_REGS		32	/* 0-28 defined, 29-31 reserved */
#define AD1843_WRITE_TRIES	5

/* Register 26, serial interface: SCF (16.384MHz SCLK), FRS (16 slots per
 * frame), FRST, ADTLK - the value dc30.sys _AudioHWInit writes first.
 */
#define AD1843_REG_SERIAL	26
#define AD1843_SERIAL_INIT	0x00f0

#define AD1843_REG_STATUS	0
#define AD1843_STATUS_INIT	0x8000	/* clocks not yet settled */
#define AD1843_STATUS_PDNO	0x4000	/* converters powered down */

/* Register 28, fundamental settings: PDNI (converter power down) and
 * ACEN (autocalibrate when leaving power down); clock generators 1-3 and
 * CLKOUT stay off. dc30.sys _AudioHWInit writes 0x4000 and waits 500ms.
 * Powering down, dc30.sys _AudioDeinit writes the reset default 0xc400,
 * which leaves CLKOUT on; nothing on the board needs it (capture runs
 * with it off), so it goes off here as well.
 */
#define AD1843_REG_FUNDAMENTAL	28
#define AD1843_FUND_PDNI	0x8000
#define AD1843_FUND_ACEN	0x4000
/* Leaving power down takes ~470ms (474ms with autocalibration). */
#define AD1843_POWERUP_MS	500

/* Analog path. On the DC30 the audio inputs are wired to the AD1843's Mic
 * and Aux 2 inputs (dc30.sys _AudioSelectSource offers only ADC sources 1
 * and 3), not to Line. Which socket is which is still to be confirmed.
 */
#define AD1843_REG_ADC_INPUT	2
#define AD1843_ADC_SRC_MIC	0x2020	/* LSS/RSS: dc30.sys default */
#define AD1843_REG_MIX_AUX2	5
#define AD1843_REG_MIX_MIC	7
#define AD1843_MIX_MUTE		0x8080	/* L/R mute in regs 3-8 */
#define AD1843_MIX_DEFAULT	0x8888	/* reset default: muted, 0dB */
#define AD1843_REG_POWER	27
#define AD1843_POWER_AAMEN	0x0010	/* analog input to analog mix */
/* Reset default of register 27: analog channels and headphone driver
 * enabled, the rest down. Mixer registers 4-8 only hold a value while
 * AAMEN is set as well.
 */
#define AD1843_POWER_IDLE	0x00c0

/* Registers the mixer controls own (dc30->ad1843_user[]). */
#define AD1843_USER_REGS	(BIT(AD1843_REG_ADC_INPUT) | \
				 BIT(AD1843_REG_MIX_AUX2) | \
				 BIT(AD1843_REG_MIX_MIC))

/* ADC overrange bits (sticky, cleared by any write). */
#define AD1843_REG_ADC_STATUS	1

/* Capture setup, dc30.sys _SetAudioMode(0, 16, 2) / _AudioSetup /
 * _StartHW for 16-bit stereo.
 */
#define AD1843_POWER_CAPTURE	0x0093	/* ANAEN, AAMEN, ADREN, ADLEN */
#define AD1843_SERIAL_CAPTURE	0x00f5	/* + ADC L/R 16-bit linear PCM */
#define AD1843_FUND_CAPTURE	0x5800	/* ACEN, clock generators 1 and 2 */
#define AD1843_FUND_XCTL0	0x0100	/* pulsed high first, as dc30.sys */
#define AD1843_REG_RATE_SRC	15
#define AD1843_RATE_SRC_CG1	0x0505	/* conversion clocks from generator 1 */
#define AD1843_RATE_SRC_CG2	0x0a0a	/* conversion clocks from generator 2 */
#define AD1843_REG_CG1_MODE	16
#define AD1843_REG_CG1_RATE	17
#define AD1843_REG_CG2_MODE	19
/* Clock generator mode: CxREF | CxVID (video lock to the line rate on the
 * generator's SYNC pin: SYNC1 for generator 1, SYNC2 for generator 2),
 * CxM7 = PAL, base 44.1kHz, divisor 1. 0x00ff is the crystal referenced
 * reset default (rate from register 17).
 */
#define AD1843_CG_REF		0x8000
#define AD1843_CG_VIDEOLOCK_PAL	0xc0a0
#define AD1843_CG_CRYSTAL	0x00ff

/* Video lock, as dc30.sys always captures: the sample clock comes from
 * the line rate of the decoder's input (SYNC2, see audio_sync_input), so
 * audio and video run in step in hardware - 882.0 frames per PAL field
 * on the board. Needs a signal on the selected V4L2 input, even for audio
 * alone, and allows 44.1kHz only; the mode word is PAL's. 0 = crystal,
 * 8-48kHz, independent of the video.
 */
static bool dc30_audio_videolock = true;
module_param_named(audio_videolock, dc30_audio_videolock, bool, 0644);
MODULE_PARM_DESC(audio_videolock, "Lock the audio sample clock to the video line rate, as dc30.sys (default 1; 0 = crystal, any rate, not in step with the video)");

/* Experiments: replaces the clock generator 1/2 mode word (registers 16
 * and 19) at the next capture start, e.g. 0xe0a0 = video lock with
 * infinite PLL loop gain (C1PLLG). 0 = use the default above.
 */
static unsigned int dc30_audio_cg_mode;
module_param_named(audio_cg_mode, dc30_audio_cg_mode, uint, 0644);
MODULE_PARM_DESC(audio_cg_mode, "Override the AD1843 clock generator mode word, for experiments (0 = default)");

/* Which generator, and so which SYNC pin, clocks the ADCs in video lock.
 * dc30.sys puts both generators into video lock and captures from
 * generator 2 (_StartHW: register 15 = 0x0a0a unless playing back, which
 * uses generator 1). SYNC1 carries the VPX's VACT, which pauses every
 * field, and the DPLL then runs 0.2-0.4% fast. Taken over at the next
 * capture start; crystal mode always uses generator 1.
 */
static unsigned int dc30_audio_sync_input = 2;
module_param_named(audio_sync_input, dc30_audio_sync_input, uint, 0644);
MODULE_PARM_DESC(audio_sync_input, "SYNC pin for video lock: 2 = clock generator 2, as dc30.sys captures (default); 1 = generator 1 (VPX VACT)");

/* Experiment (CPU cost of the FIFO reads): guest 4 timing for
 * reads and non-widened writes. A shorter GuestBus cycle might allow a
 * shorter pause before each FIFO byte (dc30_po_tune() follows on its own).
 * Taken over at the next capture start.
 */
static unsigned int dc30_audio_guest4_timing = DC30_GCR2_GUEST4_DEFAULT;
module_param_named(audio_guest4_timing, dc30_audio_guest4_timing, uint, 0644);
MODULE_PARM_DESC(audio_guest4_timing, "GuestBus timing nibble of the audio ASIC (Tdur<<2 | Trec, 0 = 3/3 PCI clocks; default 5 = 4/4 as dc30.sys), for experiments");

#define DC30_AUDIO_RATE		44100
#define DC30_AUDIO_TEST_SECS	3
#define AD1843_POWERUP_EXTRA_MS	500
#define AD1843_BUSY_TIMEOUT_US	10000

/* Reset defaults from doc/ad1843.pdf, for the debugfs dump. */
static const u16 ad1843_defaults[AD1843_NUM_REGS] = {
	0xc001, 0x0000, 0x0000, 0x8888, 0x8888, 0x8888, 0x8888, 0x8888,
	0x8868, 0x8888, 0x8888, 0x0000, 0x0000, 0x8080, 0x8080, 0x0000,
	0x00ff, 0xbb80, 0x0000, 0x00ff, 0xbb80, 0x0000, 0x00ff, 0xbb80,
	0x0000, 0x0000, 0x0000, 0x00c0, 0xc400, 0x0000, 0x0000, 0x0000,
};

static int dc30_asic_read(struct dc30_dev *dc30, unsigned int reg, u8 *val)
{
	return dc30_guest_read(dc30, DC30_GUEST_ID_AUDIO, reg, val);
}

static u32 dc30_guest4_timing(void)
{
	return READ_ONCE(dc30_audio_guest4_timing) & DC30_GCR2_GUEST4_MASK;
}

static void dc30_gcr2_guest4(struct dc30_dev *dc30, u32 nibble)
{
	u32 reg = dc30_read(dc30, ZR36057_GCR2);

	dc30_write(dc30, ZR36057_GCR2,
		   (reg & ~DC30_GCR2_GUEST4_MASK) | nibble);
}

/* All guest timings the audio side uses, guest 4 per audio_guest4_timing. */
static void dc30_gcr2_audio(struct dc30_dev *dc30)
{
	dc30_write(dc30, ZR36057_GCR2,
		   DC30_GCR2_GUESTS5_7 | dc30_guest4_timing());
}

static int dc30_asic_write(struct dc30_dev *dc30, unsigned int reg, u8 val)
{
	int err;

	if (!dc30->audio_slow_writes)
		return dc30_guest_write(dc30, DC30_GUEST_ID_AUDIO, reg, val);

	dc30_gcr2_guest4(dc30, DC30_GCR2_GUEST4_WRITE);
	err = dc30_guest_write(dc30, DC30_GUEST_ID_AUDIO, reg, val);
	dc30_gcr2_guest4(dc30, dc30_guest4_timing());
	return err;
}

/* Bring-up trace of one codec register access (debugfs dump). */
struct dc30_ad1843_trace {
	u8 st_before;		/* reg 2 before the command */
	u8 st_first;		/* first reg 2 poll after the command */
	unsigned int polls;	/* reg 2 reads until busy was clear */
};

/* Caller holds ad1843_lock. */
static int dc30_ad1843_wait_idle(struct dc30_dev *dc30, u8 *first,
				 unsigned int *polls)
{
	unsigned int us;
	u8 st;
	int err;

	for (us = 0; us < AD1843_BUSY_TIMEOUT_US; us += 2) {
		err = dc30_asic_read(dc30, ASIC_REG_CODEC, &st);
		if (err)
			return err;
		if (!us && first)
			*first = st;
		if (!(st & ASIC_CODEC_BUSY)) {
			if (polls)
				*polls = us / 2 + 1;
			return 0;
		}
		udelay(2);
	}
	return -ETIMEDOUT;
}

static int dc30_ad1843_read_traced(struct dc30_dev *dc30, unsigned int idx,
				   u16 *val, struct dc30_ad1843_trace *tr)
{
	u8 hi, lo;
	int err;

	*val = 0;
	if (!dc30->audio_present)
		return -ENODEV;

	mutex_lock(&dc30->ad1843_lock);
	err = dc30_ad1843_wait_idle(dc30, tr ? &tr->st_before : NULL, NULL);
	if (!err)
		err = dc30_asic_write(dc30, ASIC_REG_CODEC,
				      (idx & ASIC_CODEC_ADDR_MASK) |
				      ASIC_CODEC_READ | ASIC_CODEC_BUSY);
	if (!err)
		err = dc30_ad1843_wait_idle(dc30, tr ? &tr->st_first : NULL,
					    tr ? &tr->polls : NULL);
	if (!err)
		err = dc30_asic_read(dc30, ASIC_REG_DATA_HI, &hi);
	if (!err)
		err = dc30_asic_read(dc30, ASIC_REG_DATA_LO, &lo);
	mutex_unlock(&dc30->ad1843_lock);

	if (!err)
		*val = hi << 8 | lo;
	return err;
}

int dc30_ad1843_read(struct dc30_dev *dc30, unsigned int idx, u16 *val)
{
	return dc30_ad1843_read_traced(dc30, idx, val, NULL);
}

/* As dc30.sys _AudioSetReg: write, read back, retry a few times. Unlike
 * dc30.sys, a value that never reads back is reported, not ignored.
 */
int dc30_ad1843_update(struct dc30_dev *dc30, unsigned int idx, u16 mask,
		       u16 val)
{
	u16 old;
	int err;

	err = dc30_ad1843_read(dc30, idx, &old);
	if (err)
		return err;
	return dc30_ad1843_write(dc30, idx, (old & ~mask) | (val & mask));
}

int dc30_ad1843_write_noverify(struct dc30_dev *dc30, unsigned int idx,
			       u16 val)
{
	int err;

	if (!dc30->audio_present)
		return -ENODEV;

	mutex_lock(&dc30->ad1843_lock);
	err = dc30_ad1843_wait_idle(dc30, NULL, NULL);
	if (!err)
		err = dc30_asic_write(dc30, ASIC_REG_DATA_HI, val >> 8);
	if (!err)
		err = dc30_asic_write(dc30, ASIC_REG_DATA_LO, val & 0xff);
	if (!err)
		err = dc30_asic_write(dc30, ASIC_REG_CODEC,
				      (idx & ASIC_CODEC_ADDR_MASK) |
				      ASIC_CODEC_BUSY);
	mutex_unlock(&dc30->ad1843_lock);
	return err;
}

int dc30_ad1843_write(struct dc30_dev *dc30, unsigned int idx, u16 val)
{
	unsigned int try;
	u16 rb = 0;
	int err;

	if (!dc30->audio_present)
		return -ENODEV;

	for (try = 0; try < AD1843_WRITE_TRIES; try++) {
		err = dc30_ad1843_write_noverify(dc30, idx, val);
		if (err)
			return err;

		err = dc30_ad1843_read(dc30, idx, &rb);
		if (err)
			return err;
		if (rb == val)
			return 0;
	}

	dev_warn(&dc30->pdev->dev,
		 "AD1843 reg %u: wrote 0x%04x, reads back 0x%04x\n", idx, val, rb);
	return -EIO;
}

/* Scratch test from dc30.sys _AudioInit: the ASIC's data registers must
 * read back what was written.
 */
static int dc30_asic_probe(struct dc30_dev *dc30)
{
	u8 hi, lo;
	int err;

	err = dc30_asic_write(dc30, ASIC_REG_DATA_HI, 0x55);
	if (!err)
		err = dc30_asic_write(dc30, ASIC_REG_DATA_HI, 0x55);
	if (!err)
		err = dc30_asic_write(dc30, ASIC_REG_DATA_LO, 0xaa);
	if (!err)
		err = dc30_asic_write(dc30, ASIC_REG_DATA_LO, 0xaa);
	if (!err)
		err = dc30_asic_read(dc30, ASIC_REG_DATA_HI, &hi);
	if (!err)
		err = dc30_asic_read(dc30, ASIC_REG_DATA_LO, &lo);
	if (err)
		return err;

	if (hi != 0x55 || lo != 0xaa) {
		dev_warn(&dc30->pdev->dev,
			 "audio ASIC scratch test failed: 0x%02x 0x%02x (expected 0x55 0xaa)\n",
			 hi, lo);
		return -ENODEV;
	}
	return 0;
}

static void dc30_codec_power_down(struct dc30_dev *dc30);

int dc30_audio_init(struct dc30_dev *dc30)
{
	u16 status;
	u8 ctrl;
	int err;

	mutex_init(&dc30->ad1843_lock);
	mutex_init(&dc30->codec_lock);
	dc30->audio_present = false;

	/* Mixer settings the codec gets at each power-up: ADC source Mic at
	 * 0dB as dc30.sys, both analog loop-throughs muted. dc30.sys turns
	 * the Mic loop-through on (_AudioAnalogLoopthrough(1, 0)); here that
	 * would keep the converters powered all the time, so it is left to
	 * the "External/Internal Playback Switch" controls.
	 */
	dc30->ad1843_user[AD1843_REG_ADC_INPUT] = AD1843_ADC_SRC_MIC;
	dc30->ad1843_user[AD1843_REG_MIX_AUX2] = AD1843_MIX_DEFAULT;
	dc30->ad1843_user[AD1843_REG_MIX_MIC] = AD1843_MIX_DEFAULT;

	/* Order and delays as dc30.sys _AudioInit. The guest 5 read and the
	 * 500ms after it are taken over as-is; what they do on the board
	 * is not known (an ASIC or codec reset is the likely guess).
	 */
	dc30_gcr2_audio(dc30);
	dc30_guest_read(dc30, DC30_GUEST_ID_AUDIO_INIT, 0, &ctrl);
	msleep(500);

	dc30->audio_slow_writes = true;
	err = dc30_asic_probe(dc30);
	if (err) {
		dev_warn(&dc30->pdev->dev, "audio ASIC not found (%d)\n", err);
		return err;
	}

	/* dc30.sys keeps the slow guest 4 writes only on boards that set
	 * bit 7 of the control register.
	 */
	err = dc30_asic_read(dc30, ASIC_REG_CTRL, &ctrl);
	if (err)
		return err;
	dc30->audio_slow_writes = ctrl & ASIC_CTRL_VARIANT;
	dc30->audio_present = true;

	err = dc30_ad1843_read(dc30, 0, &status);
	if (err) {
		dev_warn(&dc30->pdev->dev,
			 "audio ASIC found (ctrl 0x%02x), AD1843 not responding (%d)\n",
			 ctrl, err);
		return err;
	}

	dev_info(&dc30->pdev->dev,
		 "audio ASIC found (ctrl 0x%02x), AD1843 status 0x%04x (rev %u%s%s)\n",
		 ctrl, status, status & 0xf,
		 status & 0x8000 ? ", clocks not settled" : "",
		 status & 0x4000 ? ", converters powered down" : "");

	/* 16-slot frames, so that register reads return the right register
	 * (see the top of this file).
	 */
	err = dc30_ad1843_write(dc30, AD1843_REG_SERIAL, AD1843_SERIAL_INIT);
	if (err) {
		dev_warn(&dc30->pdev->dev,
			 "AD1843: switching to 16-slot frames failed (%d)\n", err);
		return err;
	}
	dev_info(&dc30->pdev->dev, "AD1843 serial interface set to 16-slot frames\n");

	/* Converters stay down until a PCM is opened or a loop-through is
	 * switched on (dc30_audio_codec_get()). After power-on they are
	 * down already; after a module reload they may not be.
	 */
	mutex_lock(&dc30->codec_lock);
	dc30_codec_power_down(dc30);
	mutex_unlock(&dc30->codec_lock);
	return 0;
}

void dc30_audio_board_resume(struct dc30_dev *dc30)
{
	/* The ZR36057 soft reset cleared the guest timings. */
	dc30_gcr2_audio(dc30);
}

/* ---- codec power ----
 *
 * Powered down (PDNI) the AD1843 clears nearly all its registers to their
 * defaults and ignores writes to them, the mixer registers included. So
 * the mixer settings live in dc30->ad1843_user[] and go to the codec
 * whenever it powers up. Powering up takes ~470ms (datasheet: to exit
 * power down), plus 4ms autocalibration.
 */

static bool dc30_codec_loop_wanted(struct dc30_dev *dc30)
{
	return (~dc30->ad1843_user[AD1843_REG_MIX_AUX2] & AD1843_MIX_MUTE) ||
	       (~dc30->ad1843_user[AD1843_REG_MIX_MIC] & AD1843_MIX_MUTE);
}

/* Write the mixer settings to the powered codec. The analog mix (AAMEN)
 * is powered only while a loop-through is on - or while a capture runs,
 * whose register 27 value (dc30.sys _StartHW) includes it. Caller holds
 * codec_lock.
 */
static int dc30_codec_apply(struct dc30_dev *dc30)
{
	bool loop = dc30_codec_loop_wanted(dc30);
	bool capturing = test_bit(0, &dc30->audio_busy);
	u16 power;
	int err;

	err = dc30_ad1843_write(dc30, AD1843_REG_ADC_INPUT,
				dc30->ad1843_user[AD1843_REG_ADC_INPUT]);
	if (err)
		return err;

	if (loop || !capturing) {
		err = dc30_ad1843_update(dc30, AD1843_REG_POWER,
					 AD1843_POWER_AAMEN,
					 loop ? AD1843_POWER_AAMEN : 0);
		if (err)
			return err;
	}
	err = dc30_ad1843_read(dc30, AD1843_REG_POWER, &power);
	if (err || !(power & AD1843_POWER_AAMEN))
		return err;

	err = dc30_ad1843_write(dc30, AD1843_REG_MIX_AUX2,
				dc30->ad1843_user[AD1843_REG_MIX_AUX2]);
	if (!err)
		err = dc30_ad1843_write(dc30, AD1843_REG_MIX_MIC,
					dc30->ad1843_user[AD1843_REG_MIX_MIC]);
	return err;
}

/* Leave converter power down (with autocalibration) and wait for the
 * PDNO flag to clear. dc30.sys just sleeps 500ms; this also checks.
 * Caller holds codec_lock.
 */
static int dc30_ad1843_power_up(struct dc30_dev *dc30)
{
	unsigned int ms;
	u16 status;
	int err;

	err = dc30_ad1843_write(dc30, AD1843_REG_FUNDAMENTAL, AD1843_FUND_ACEN);
	if (err) {
		dev_warn(&dc30->pdev->dev, "AD1843: power-up write failed (%d)\n",
			 err);
		return err;
	}

	msleep(AD1843_POWERUP_MS);
	for (ms = 0; ; ms += 20) {
		err = dc30_ad1843_read(dc30, AD1843_REG_STATUS, &status);
		if (err)
			return err;
		if (!(status & (AD1843_STATUS_INIT | AD1843_STATUS_PDNO)))
			break;
		if (ms >= AD1843_POWERUP_EXTRA_MS) {
			dev_warn(&dc30->pdev->dev,
				 "AD1843: still powered down after %ums (status 0x%04x)\n",
				 AD1843_POWERUP_MS + ms, status);
			return -ETIMEDOUT;
		}
		msleep(20);
	}

	dc30->codec_powerup_ms = AD1843_POWERUP_MS + ms;
	dev_dbg(&dc30->pdev->dev,
		"AD1843 converters powered up and calibrated (status 0x%04x, %ums)\n",
		status, AD1843_POWERUP_MS + ms);
	return 0;
}

/* Caller holds codec_lock. */
static int dc30_codec_power_up(struct dc30_dev *dc30)
{
	int err;

	/* 16-slot frames again, in case the codec was reset meanwhile
	 * (the board went through a power-down).
	 */
	err = dc30_ad1843_write(dc30, AD1843_REG_SERIAL, AD1843_SERIAL_INIT);
	if (!err)
		err = dc30_ad1843_power_up(dc30);
	if (!err)
		err = dc30_ad1843_write(dc30, AD1843_REG_POWER,
					AD1843_POWER_IDLE);
	if (!err)
		err = dc30_codec_apply(dc30);
	if (err) {
		dc30_codec_power_down(dc30);
		return err;
	}
	dc30->codec_on = true;
	dc30->codec_powerups++;
	return 0;
}

/* Everything off the datasheet's power management table lists: all
 * channels in register 27, then PDNI with all clock generators and
 * CLKOUT off - 176mA of the 200mA the codec draws running. What is left
 * is the serial interface and the crystal oscillator, which only the
 * PWRDWN pin stops (not reachable here). Caller holds codec_lock.
 */
static void dc30_codec_power_down(struct dc30_dev *dc30)
{
	u16 status = AD1843_STATUS_PDNO;

	dc30->codec_on = false;

	/* Register 27 can't be written while powered down (PDNO). */
	dc30_ad1843_read(dc30, AD1843_REG_STATUS, &status);
	if (!(status & AD1843_STATUS_PDNO))
		dc30_ad1843_write(dc30, AD1843_REG_POWER, 0);
	dc30_ad1843_write(dc30, AD1843_REG_FUNDAMENTAL,
			  AD1843_FUND_PDNI | AD1843_FUND_ACEN);
}

/* Caller holds codec_lock. */
static int __dc30_codec_get(struct dc30_dev *dc30)
{
	int err;

	if (!dc30->codec_users) {
		err = dc30_codec_power_up(dc30);
		if (err)
			return err;
	}
	dc30->codec_users++;
	return 0;
}

static void __dc30_codec_put(struct dc30_dev *dc30)
{
	if (!--dc30->codec_users)
		dc30_codec_power_down(dc30);
}

int dc30_audio_codec_get(struct dc30_dev *dc30)
{
	int err;

	if (!dc30->audio_present)
		return -ENODEV;

	err = dc30_pm_get(dc30);
	if (err)
		return err;
	mutex_lock(&dc30->codec_lock);
	err = __dc30_codec_get(dc30);
	mutex_unlock(&dc30->codec_lock);
	if (err)
		dc30_pm_put(dc30);
	return err;
}

void dc30_audio_codec_put(struct dc30_dev *dc30)
{
	mutex_lock(&dc30->codec_lock);
	__dc30_codec_put(dc30);
	mutex_unlock(&dc30->codec_lock);
	dc30_pm_put(dc30);
}

u16 dc30_audio_mixer_read(struct dc30_dev *dc30, unsigned int idx)
{
	u16 val;

	mutex_lock(&dc30->codec_lock);
	val = dc30->ad1843_user[idx];
	mutex_unlock(&dc30->codec_lock);
	return val;
}

int dc30_audio_mixer_update(struct dc30_dev *dc30, unsigned int idx,
			    u16 mask, u16 val)
{
	bool loop;
	u16 old;
	int err = 0;

	if (idx >= ARRAY_SIZE(dc30->ad1843_user) ||
	    !(AD1843_USER_REGS & BIT(idx)))
		return -EINVAL;

	mutex_lock(&dc30->codec_lock);
	old = dc30->ad1843_user[idx];
	dc30->ad1843_user[idx] = (old & ~mask) | (val & mask);
	if (dc30->ad1843_user[idx] == old)
		goto out;

	/* A loop-through needs the analog part powered with no PCM open,
	 * so it holds a reference of its own.
	 */
	loop = dc30_codec_loop_wanted(dc30);
	if (loop && !dc30->codec_loop) {
		err = dc30_pm_get(dc30);
		if (!err) {
			/* Powers up with the new settings, or applies them. */
			if (dc30->codec_users)
				err = dc30_codec_apply(dc30);
			if (!err)
				err = __dc30_codec_get(dc30);
			if (err)
				dc30_pm_put(dc30);
		}
		if (!err)
			dc30->codec_loop = true;
	} else if (!loop && dc30->codec_loop) {
		dc30->codec_loop = false;
		if (dc30->codec_users > 1)
			err = dc30_codec_apply(dc30);
		__dc30_codec_put(dc30);
		dc30_pm_put(dc30);
	} else if (dc30->codec_on) {
		err = dc30_codec_apply(dc30);
	}
	if (err)
		dc30->ad1843_user[idx] = old;
out:
	mutex_unlock(&dc30->codec_lock);
	return err ? err : dc30->ad1843_user[idx] != old;
}

int dc30_audio_overrange(struct dc30_dev *dc30, u16 *reg)
{
	int err = 0;

	*reg = 0;
	mutex_lock(&dc30->codec_lock);
	if (dc30->codec_on) {
		err = dc30_ad1843_read(dc30, AD1843_REG_ADC_STATUS, reg);
		if (!err)
			err = dc30_ad1843_write_noverify(dc30,
							 AD1843_REG_ADC_STATUS,
							 0);
	}
	mutex_unlock(&dc30->codec_lock);
	return err;
}

void dc30_audio_power_show(struct seq_file *m, struct dc30_dev *dc30)
{
	if (!dc30->audio_present) {
		seq_puts(m, "audio codec    not present\n");
		return;
	}
	mutex_lock(&dc30->codec_lock);
	seq_printf(m, "audio codec    %s, %u users%s\n",
		   dc30->codec_on ? "on" : "powered down",
		   dc30->codec_users,
		   dc30->codec_loop ? " (one is the analog loop-through)" : "");
	seq_printf(m, "codec power-ups %lu, last took %u ms\n",
		   dc30->codec_powerups, dc30->codec_powerup_ms);
	mutex_unlock(&dc30->codec_lock);
}

void dc30_audio_exit(struct dc30_dev *dc30)
{
	if (!dc30->audio_present)
		return;

	/* The ALSA card is gone, so only a loop-through can still hold the
	 * codec. PDNI is also the datasheet's way to power down with the
	 * least output click: the DAC and VREF outputs decay slowly.
	 */
	mutex_lock(&dc30->codec_lock);
	if (dc30->codec_loop) {
		dc30->codec_loop = false;
		dc30->codec_users--;
		pm_runtime_put_noidle(&dc30->pdev->dev);
	}
	/* Already down otherwise - and then register 28 doesn't read back
	 * (it returns the status word), which only made for a warning.
	 */
	if (dc30->codec_on)
		dc30_codec_power_down(dc30);
	mutex_unlock(&dc30->codec_lock);
}

void dc30_ad1843_regs_show(struct seq_file *m, struct dc30_dev *dc30)
{
	unsigned int i;
	u16 val;
	int err;

	if (!dc30->audio_present) {
		seq_puts(m, "audio ASIC not present\n");
		return;
	}

	/* The r2 columns are a bring-up trace of the ASIC's codec access
	 * register: before the command, first poll after it, polls needed.
	 */
	seq_puts(m, "reg  value  default    r2 before/after/polls\n");
	for (i = 0; i < AD1843_NUM_REGS; i++) {
		struct dc30_ad1843_trace tr = {};

		err = dc30_ad1843_read_traced(dc30, i, &val, &tr);
		if (err) {
			seq_printf(m, "%3u  error %d\n", i, err);
			continue;
		}
		seq_printf(m, "%3u  0x%04x  0x%04x%s   0x%02x 0x%02x %u\n", i,
			   val, ad1843_defaults[i],
			   val != ad1843_defaults[i] ? " *" : "  ",
			   tr.st_before, tr.st_first, tr.polls);
	}
}

/* ---- capture ---- */

/* Consistent 16-bit read of the ASIC's write counter, as dc30.sys
 * _AudioGetCCounter: high, low, high, low; if the high bytes differ, the
 * low byte may have wrapped in between, so take the second pair.
 */
int dc30_audio_counter(struct dc30_dev *dc30, u16 *count)
{
	u8 h1, l1, h2, l2;
	int err;

	err = dc30_asic_read(dc30, ASIC_REG_COUNT_HI, &h1);
	if (!err)
		err = dc30_asic_read(dc30, ASIC_REG_COUNT_LO, &l1);
	if (!err)
		err = dc30_asic_read(dc30, ASIC_REG_COUNT_HI, &h2);
	if (!err)
		err = dc30_asic_read(dc30, ASIC_REG_COUNT_LO, &l2);
	if (err)
		return err;

	*count = h1 == h2 ? h1 << 8 | l1 : h2 << 8 | l2;
	return 0;
}

static int dc30_asic_set_ctrl(struct dc30_dev *dc30, u8 ctrl)
{
	unsigned int try;
	u8 rb = 0;
	int err;

	/* dc30.sys _StartHW rewrites until it reads back (unbounded). Bit 7
	 * is the read-only board variant flag and always reads as set here.
	 */
	for (try = 0; try < 100; try++) {
		err = dc30_asic_write(dc30, ASIC_REG_CTRL, ctrl);
		if (!err)
			err = dc30_asic_read(dc30, ASIC_REG_CTRL, &rb);
		if (err)
			return err;
		if ((rb & ~ASIC_CTRL_VARIANT) == ctrl)
			return 0;
	}
	dev_warn(&dc30->pdev->dev, "audio ASIC ctrl: wrote 0x%02x, reads 0x%02x\n",
		 ctrl, rb);
	return -EIO;
}

bool dc30_audio_videolock_enabled(void)
{
	return dc30_audio_videolock;
}

u16 dc30_audio_cg_mode_word(void)
{
	if (dc30_audio_cg_mode)
		return dc30_audio_cg_mode;
	return dc30_audio_videolock ? AD1843_CG_VIDEOLOCK_PAL
				    : AD1843_CG_CRYSTAL;
}

u16 dc30_audio_rate_src_word(void)
{
	/* Generator 2 only when SYNC referenced: with the crystal its rate
	 * would come from register 20, which isn't set.
	 */
	if ((dc30_audio_cg_mode_word() & AD1843_CG_REF) &&
	    dc30_audio_sync_input == 2)
		return AD1843_RATE_SRC_CG2;
	return AD1843_RATE_SRC_CG1;
}

int dc30_audio_capture_start(struct dc30_dev *dc30, unsigned int rate)
{
	u16 cg = dc30_audio_cg_mode_word();
	u8 ctrl = ASIC_CTRL_16BIT | ASIC_CTRL_STEREO;
	int err;

	if (!dc30->audio_present)
		return -ENODEV;

	mutex_lock(&dc30->codec_lock);
	if (!dc30->codec_on) {
		mutex_unlock(&dc30->codec_lock);
		return -EIO;
	}
	/* ALSA and the debugfs test must not run the ASIC at the same time. */
	if (test_and_set_bit(0, &dc30->audio_busy)) {
		mutex_unlock(&dc30->codec_lock);
		return -EBUSY;
	}

	dc30_gcr2_audio(dc30);
	err = dc30_ad1843_write(dc30, AD1843_REG_POWER, AD1843_POWER_CAPTURE);
	if (!err)
		err = dc30_ad1843_write(dc30, AD1843_REG_SERIAL,
					AD1843_SERIAL_CAPTURE);
	if (!err)
		err = dc30_asic_set_ctrl(dc30, ctrl);
	if (!err)
		err = dc30_ad1843_write(dc30, AD1843_REG_FUNDAMENTAL,
					AD1843_FUND_CAPTURE | AD1843_FUND_XCTL0);
	if (!err)
		err = dc30_ad1843_write(dc30, AD1843_REG_FUNDAMENTAL,
					AD1843_FUND_CAPTURE);
	if (!err)
		err = dc30_ad1843_write(dc30, AD1843_REG_RATE_SRC,
					dc30_audio_rate_src_word());
	if (!err)
		err = dc30_ad1843_write(dc30, AD1843_REG_CG1_RATE, rate);
	if (!err)
		err = dc30_ad1843_write(dc30, AD1843_REG_CG1_MODE, cg);
	if (!err)
		err = dc30_ad1843_write(dc30, AD1843_REG_CG2_MODE, cg);
	/* dc30.sys also mutes DAC1 (register 9 = 0x8080) here, but with DAC1
	 * powered down (register 27) that register is held at its muted
	 * default and can't be written.
	 */
	if (!err)
		err = dc30_asic_set_ctrl(dc30, ctrl | ASIC_CTRL_RUN);
	if (err)
		clear_bit(0, &dc30->audio_busy);
	mutex_unlock(&dc30->codec_lock);
	return err;
}

void dc30_audio_capture_stop(struct dc30_dev *dc30)
{
	mutex_lock(&dc30->codec_lock);
	dc30_asic_set_ctrl(dc30, ASIC_CTRL_16BIT | ASIC_CTRL_STEREO);
	clear_bit(0, &dc30->audio_busy);
	mutex_unlock(&dc30->codec_lock);
}

int dc30_audio_fifo_read(struct dc30_dev *dc30, u8 *buf, unsigned int len)
{
	return dc30_guest_read_stream(dc30, DC30_GUEST_ID_AUDIO, ASIC_REG_FIFO,
				      buf, len);
}

struct dc30_audio_test {
	u8 *buf;
	size_t len;
};

/* Record DC30_AUDIO_TEST_SECS of 16-bit stereo into a buffer, draining
 * the ASIC ring the way dc30.sys Sync_AudioCapture does. Raw FIFO byte
 * order, nothing swapped or skipped - that is to be worked out from the
 * data.
 */
static int dc30_audio_test_record(struct dc30_dev *dc30,
				  struct dc30_audio_test *t)
{
	size_t want = DC30_AUDIO_RATE * 4 * DC30_AUDIO_TEST_SECS;
	unsigned int max_fill = 0, drains = 0, idle = 0;
	u16 start, count, pos;
	ktime_t t0, t_end, deadline;
	s64 read_ns = 0;
	int err;

	t->buf = vzalloc(want);
	if (!t->buf)
		return -ENOMEM;
	t->len = 0;

	err = dc30_audio_capture_start(dc30, DC30_AUDIO_RATE);
	if (err)
		goto out;
	err = dc30_audio_counter(dc30, &start);
	if (err)
		goto out_stop;

	pos = start;
	t0 = ktime_get();
	deadline = ktime_add_ms(t0, DC30_AUDIO_TEST_SECS * 1000 + 2000);

	while (t->len < want) {
		unsigned int fill, n;

		if (ktime_after(ktime_get(), deadline)) {
			err = -ETIMEDOUT;
			break;
		}
		err = dc30_audio_counter(dc30, &count);
		if (err)
			break;
		fill = (u16)(count - pos) % ASIC_RING_SIZE;
		max_fill = max(max_fill, fill);
		if (fill < ASIC_READ_MIN) {
			idle++;
			usleep_range(2000, 3000);
			continue;
		}

		n = min_t(size_t, fill, want - t->len) & ~7U;
		drains++;
		while (n) {
			unsigned int chunk = min(n, (unsigned int)ASIC_READ_CHUNK);

			ktime_t r0 = ktime_get();

			err = dc30_audio_fifo_read(dc30, t->buf + t->len,
						   chunk);
			read_ns += ktime_to_ns(ktime_sub(ktime_get(), r0));
			if (err)
				goto out_stop;
			t->len += chunk;
			n -= chunk;
			pos = (pos + chunk) % ASIC_RING_SIZE;
		}
	}

out_stop:
	/* Bytes the hardware produced = read + still in the ring. */
	t_end = ktime_get();
	if (!err && !dc30_audio_counter(dc30, &count)) {
		u64 produced = t->len + (u16)(count - pos) % ASIC_RING_SIZE;
		s64 us = ktime_us_delta(t_end, t0);

		dev_info(&dc30->pdev->dev,
			 "audio test: measured rate %llu Hz over %lld ms\n",
			 div64_u64(produced * 1000000ULL / 4, us), us / 1000);
	}
	dc30_audio_capture_stop(dc30);
	dev_info(&dc30->pdev->dev,
		 "audio test: %zu bytes in %lld ms, counter start 0x%04x, max fill %u of %u, %u drains, %u idle polls, %lld ns/byte, err %d\n",
		 t->len, ktime_ms_delta(t_end, t0), start, max_fill,
		 ASIC_RING_SIZE, drains, idle,
		 t->len ? div64_s64(read_ns, t->len) : 0, err);
out:
	return err;
}

static int dc30_audio_test_open(struct inode *inode, struct file *file)
{
	struct dc30_dev *dc30 = inode->i_private;
	struct dc30_audio_test *t;
	int err;

	if (!dc30->audio_present)
		return -ENODEV;

	t = kzalloc(sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;

	err = dc30_audio_codec_get(dc30);
	if (err) {
		kfree(t);
		return err;
	}
	err = dc30_audio_test_record(dc30, t);
	dc30_audio_codec_put(dc30);
	if (err && !t->len) {
		vfree(t->buf);
		kfree(t);
		return err;
	}
	file->private_data = t;
	return 0;
}

static ssize_t dc30_audio_test_read(struct file *file, char __user *ubuf,
				    size_t count, loff_t *ppos)
{
	struct dc30_audio_test *t = file->private_data;

	return simple_read_from_buffer(ubuf, count, ppos, t->buf, t->len);
}

static int dc30_audio_test_release(struct inode *inode, struct file *file)
{
	struct dc30_audio_test *t = file->private_data;

	vfree(t->buf);
	kfree(t);
	return 0;
}

static const struct file_operations dc30_audio_test_fops = {
	.owner = THIS_MODULE,
	.open = dc30_audio_test_open,
	.read = dc30_audio_test_read,
	.release = dc30_audio_test_release,
	.llseek = default_llseek,
};

/* Guest 4 read check for audio_guest4_timing experiments: writes changing
 * patterns to the two data registers of the ASIC and reads them back at
 * the current timing. Registers 3/4 only hold data for the next AD1843
 * command, so this is harmless while no capture runs.
 */
#define DC30_GUEST4_TEST_ROUNDS	5000

static int dc30_guest4_test_show(struct seq_file *m, void *data)
{
	struct dc30_dev *dc30 = m->private;
	unsigned int i, errors = 0, first_bad = 0;
	u8 hi, lo, want_hi = 0, want_lo = 0;
	u64 read_ns = 0;
	ktime_t t;
	int err;

	if (!dc30->audio_present)
		return -ENODEV;
	err = dc30_pm_get(dc30);
	if (err)
		return err;

	/* codec_lock keeps mixer writes (which use registers 3/4) out. */
	mutex_lock(&dc30->codec_lock);
	if (test_bit(0, &dc30->audio_busy)) {
		err = -EBUSY;
		goto out;
	}
	dc30_gcr2_audio(dc30);
	for (i = 0; i < DC30_GUEST4_TEST_ROUNDS; i++) {
		want_hi = i * 0x9d + 0x55;
		want_lo = ~(i * 0x3b);
		err = dc30_asic_write(dc30, ASIC_REG_DATA_HI, want_hi);
		if (!err)
			err = dc30_asic_write(dc30, ASIC_REG_DATA_LO, want_lo);
		if (err)
			goto out;
		t = ktime_get();
		err = dc30_asic_read(dc30, ASIC_REG_DATA_HI, &hi);
		if (!err)
			err = dc30_asic_read(dc30, ASIC_REG_DATA_LO, &lo);
		read_ns += ktime_to_ns(ktime_sub(ktime_get(), t));
		if (err)
			goto out;
		if (hi != want_hi || lo != want_lo) {
			if (!errors++)
				first_bad = i;
		}
	}

	seq_printf(m, "guest4 timing  0x%x (Tdur/Trec nibble)\n",
		   dc30_guest4_timing());
	seq_printf(m, "reads          %u\n", 2 * DC30_GUEST4_TEST_ROUNDS);
	seq_printf(m, "errors         %u\n", errors);
	if (errors)
		seq_printf(m, "first bad      round %u\n", first_bad);
	seq_printf(m, "ns/read        %llu\n",
		   div_u64(read_ns, 2 * DC30_GUEST4_TEST_ROUNDS));
out:
	mutex_unlock(&dc30->codec_lock);
	dc30_pm_put(dc30);
	return err;
}
DEFINE_SHOW_ATTRIBUTE(dc30_guest4_test);

void dc30_audio_debugfs_init(struct dc30_dev *dc30, struct dentry *dir)
{
	debugfs_create_file("audio_test", 0400, dir, dc30,
			    &dc30_audio_test_fops);
	debugfs_create_file("guest4_test", 0400, dir, dc30,
			    &dc30_guest4_test_fops);
}

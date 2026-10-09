// SPDX-License-Identifier: GPL-2.0-only
/*
 * dc30_alsa.c - ALSA capture device for the DC30's AD1843
 *
 * The board has no audio DMA (docs/hardware.md): the audio ASIC on
 * GuestBus guest 4 collects the AD1843's samples in a 32KB ring, and the
 * host has to pull them out byte by byte through the PostOffice. A kernel
 * thread does that every few milliseconds, converts the stream (one dummy
 * byte, then big-endian L/R samples) to S16_LE and copies it into the ALSA
 * buffer. The ring holds ~186ms at 44.1kHz stereo; a drain later than that
 * has lost data, which is reported as an xrun rather than delivering a
 * shifted stream.
 *
 * All PCM ops run in process context (pcm->nonatomic): starting the
 * capture writes a dozen AD1843 registers, each with a read-back.
 *
 * The drain needs no exact timing: the video field interrupt reads the
 * ASIC's sample counter, and that position per field goes to userspace
 * in the metadata records (dc30_meta.h).
 */

#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/wait.h>
#include <sound/control.h>
#include <sound/core.h>
#include <sound/initval.h>
#include <sound/pcm.h>
#include <sound/tlv.h>

#include "dc30.h"
#include "dc30_alsa.h"
#include "dc30_audio.h"
#include "dc30_power.h"
#include "dc30_video.h"
#include "zr36057.h"

#define DC30_ALSA_DRAIN_US	5000
#define DC30_ALSA_BUFFER_MAX	(256 * 1024)
#define DC30_ALSA_SETTLE_MS	2000	/* rate measured again after this */
#define DC30_ALSA_WINDOW_FIELDS	50	/* one second of PAL fields */
#define DC30_ALSA_WINDOW_LOG	300	/* per-window values kept for debugfs */

/* AD1843 registers used by the mixer controls (doc/ad1843.pdf). */
#define AD1843_OVL_MASK		0x0003	/* sticky, cleared by any write */
#define AD1843_OVR_SHIFT	2
#define AD1843_REG_ADC_INPUT	2
#define AD1843_ADC_SRC_MASK	0xe0e0
#define AD1843_ADC_SRC_MIC	0x2020	/* LSS/RSS = 001 */
#define AD1843_ADC_SRC_AUX2	0x6060	/* LSS/RSS = 011 */
#define AD1843_ADC_GAIN_L_SHIFT	8	/* LIG3:0, 1.5dB steps */
#define AD1843_ADC_GAIN_R_SHIFT	0
#define AD1843_ADC_GAIN_MAX	15
#define AD1843_ADC_BOOST_L_SHIFT	12	/* LMGE/RMGE, +20dB */
#define AD1843_ADC_BOOST_R_SHIFT	4
#define AD1843_REG_MIX_AUX2	5
#define AD1843_REG_MIX_MIC	7
#define AD1843_MIX_MUTE_L_SHIFT	15
#define AD1843_MIX_MUTE_R_SHIFT	7

static int dc30_alsa_index = SNDRV_DEFAULT_IDX1;
module_param_named(audio_index, dc30_alsa_index, int, 0444);
MODULE_PARM_DESC(audio_index, "ALSA card index for the DC30 audio");


struct dc30_alsa {
	struct dc30_dev *dc30;
	struct snd_card *card;
	struct snd_pcm *pcm;

	struct snd_pcm_substream *substream;	/* while open */
	bool sync_ref;		/* holds the decoder's sync outputs */
	struct task_struct *thread;
	wait_queue_head_t wq;
	bool running;

	/* Drain state. drain_lock is held for a whole drain pass and by
	 * trigger; never together with the PCM stream lock.
	 */
	struct mutex drain_lock;
	u16 ring_pos;		/* ASIC ring read position */
	unsigned int skip;	/* dummy bytes still to drop */
	int carry;		/* high byte of a split sample, or -1 */
	unsigned int channel;	/* 0 = left, 1 = right */
	unsigned int hw_pos;	/* bytes, in the ALSA buffer */
	unsigned int period_pos;
	ktime_t last_drain;
	ktime_t start_time;
	ktime_t settle_time;	/* first drain >= 2s after start */
	u64 settle_bytes;
	/* Video lock: decoder input and its signal (1/0, < 0 error) at the
	 * start. The DPLL follows whatever that input delivers.
	 */
	const char *lock_input;
	int lock_signal;
	u8 chunk[DC30_AUDIO_READ_CHUNK];

	/* Statistics since the last start (debugfs). */
	u64 bytes;
	u64 read_ns;		/* time spent in FIFO reads */
	u64 drains;
	u64 xruns;
	unsigned int max_fill;
	unsigned int max_gap_us;
	u64 full_scale[2];	/* samples at +-32767/-32768 */
	unsigned int peak[2];	/* largest magnitude */
	u64 start_polls, start_bytes;	/* PostOffice counters at start */

	/* Extended hardware position: bytes the ASIC has produced since the
	 * capture start (dummy byte included) as of counter value pos_count.
	 * Advanced by whoever reads the counter - drain thread or field
	 * interrupt - so it never goes a whole ring without an update.
	 */
	spinlock_t pos_lock;
	u16 pos_count;
	u64 pos_bytes;
	u32 epoch;		/* ALSA starts so far */
	u32 rate;
	bool videolock;

	/* ASIC counter sampled at each video field interrupt (field_lock):
	 * how many audio bytes each field got. With the sample clock locked
	 * to the video, that is constant (882 frames at 44.1kHz PAL).
	 */
	spinlock_t field_lock;
	bool field_valid;
	u16 field_prev;
	u64 fields;
	u64 field_sum, field_sumsq;
	unsigned int field_min, field_max;
	unsigned int win_fields, win_bytes;	/* 50-field window */
	u64 windows;
	unsigned int win_min, win_max;	/* not counting the first window */
	unsigned int win_log[DC30_ALSA_WINDOW_LOG];	/* bytes, first ones */
};

/* ---- PCM ---- */

static const struct snd_pcm_hardware dc30_alsa_hw = {
	.info = SNDRV_PCM_INFO_MMAP | SNDRV_PCM_INFO_MMAP_VALID |
		SNDRV_PCM_INFO_INTERLEAVED | SNDRV_PCM_INFO_BLOCK_TRANSFER,
	.formats = SNDRV_PCM_FMTBIT_S16_LE,
	.rates = SNDRV_PCM_RATE_8000_48000,
	.rate_min = 8000,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,
	.buffer_bytes_max = DC30_ALSA_BUFFER_MAX,
	.period_bytes_min = 1024,
	.period_bytes_max = DC30_ALSA_BUFFER_MAX / 2,
	.periods_min = 2,
	.periods_max = 256,
};

/* Convert raw FIFO bytes into the ALSA buffer. Returns the number of
 * periods completed. Caller holds drain_lock.
 */
static unsigned int dc30_alsa_convert(struct dc30_alsa *a,
				      struct snd_pcm_runtime *rt,
				      const u8 *src, unsigned int len)
{
	unsigned int buf_bytes = frames_to_bytes(rt, rt->buffer_size);
	unsigned int period_bytes = frames_to_bytes(rt, rt->period_size);
	unsigned int periods = 0, i;

	for (i = 0; i < len; i++) {
		unsigned int mag;
		s16 s;

		if (a->skip) {
			a->skip--;
			continue;
		}
		if (a->carry < 0) {
			a->carry = src[i];
			continue;
		}
		s = (s16)(a->carry << 8 | src[i]);
		a->carry = -1;

		*(__le16 *)(rt->dma_area + a->hw_pos) = cpu_to_le16(s);
		a->hw_pos += 2;
		if (a->hw_pos >= buf_bytes)
			a->hw_pos = 0;
		a->period_pos += 2;
		if (a->period_pos >= period_bytes) {
			a->period_pos -= period_bytes;
			periods++;
		}

		mag = abs((int)s);
		if (mag > a->peak[a->channel])
			a->peak[a->channel] = mag;
		if (mag >= 32767)
			a->full_scale[a->channel]++;
		a->channel ^= 1;
	}
	return periods;
}

/* Bytes produced since the capture start as of counter value 'count'.
 * The drain thread and the field interrupt read the counter at
 * different times, so a value can be older than the last one applied: a
 * step back of up to half the ring counts as that, not as a wrap.
 */
static s64 dc30_alsa_pos_update(struct dc30_alsa *a, u16 count)
{
	unsigned long flags;
	unsigned int delta;
	s64 pos;

	spin_lock_irqsave(&a->pos_lock, flags);
	delta = (u16)(count - a->pos_count) % DC30_AUDIO_RING_SIZE;
	if (delta < DC30_AUDIO_RING_SIZE / 2) {
		a->pos_count = count;
		a->pos_bytes += delta;
		pos = a->pos_bytes;
	} else {
		pos = a->pos_bytes - (DC30_AUDIO_RING_SIZE - delta);
	}
	spin_unlock_irqrestore(&a->pos_lock, flags);
	return pos;
}

/* One drain pass: read everything the ASIC has buffered so far. Returns
 * the number of completed periods, or -EPIPE if data was lost.
 */
static int dc30_alsa_drain(struct dc30_alsa *a)
{
	struct snd_pcm_runtime *rt = a->substream->runtime;
	unsigned int fill, periods = 0, gap_us, ring_us;
	ktime_t now = ktime_get();
	u16 count;
	int err;

	/* The ring wraps silently: a pass later than the ring's length
	 * can't tell how much was overwritten.
	 */
	gap_us = ktime_us_delta(now, a->last_drain);
	a->max_gap_us = max(a->max_gap_us, gap_us);

	/* Second rate reference point, after a PLL has had time to lock:
	 * the bytes read so far were in the ring at the previous pass.
	 */
	if (!a->settle_time &&
	    ktime_ms_delta(now, a->start_time) >= DC30_ALSA_SETTLE_MS) {
		a->settle_time = a->last_drain;
		a->settle_bytes = a->bytes;
	}
	a->last_drain = now;
	ring_us = div_u64((u64)DC30_AUDIO_RING_SIZE * USEC_PER_SEC,
			  rt->rate * DC30_ALSA_FRAME_BYTES);
	if (gap_us > ring_us * 7 / 8)
		return -EPIPE;

	err = dc30_audio_counter(a->dc30, &count);
	if (err)
		return err;
	dc30_alsa_pos_update(a, count);
	fill = (u16)(count - a->ring_pos) % DC30_AUDIO_RING_SIZE;
	a->max_fill = max(a->max_fill, fill);
	a->drains++;

	while (fill) {
		unsigned int n = min_t(unsigned int, fill, sizeof(a->chunk));

		ktime_t t0 = ktime_get();

		err = dc30_audio_fifo_read(a->dc30, a->chunk, n);
		a->read_ns += ktime_to_ns(ktime_sub(ktime_get(), t0));
		if (err)
			return err;
		a->ring_pos = (a->ring_pos + n) % DC30_AUDIO_RING_SIZE;
		a->bytes += n;
		fill -= n;
		periods += dc30_alsa_convert(a, rt, a->chunk, n);
		cond_resched();
	}
	return periods;
}

/* Called from the interrupt handler on every video field. */
void dc30_alsa_vsync(struct dc30_dev *dc30, struct dc30_audio_mark *mark)
{
	struct dc30_alsa *a = READ_ONCE(dc30->alsa);
	unsigned int d;
	u16 count;

	mark->valid = false;
	if (!a || !READ_ONCE(a->running))
		return;
	if (dc30_audio_counter(dc30, &count))
		return;

	/* The ALSA stream starts after the ASIC's leading dummy byte. */
	mark->pos = dc30_alsa_pos_update(a, count) - 1;
	mark->epoch = a->epoch;
	mark->rate = a->rate;
	mark->videolock = a->videolock;
	mark->valid = true;

	spin_lock(&a->field_lock);
	if (a->field_valid) {
		d = (u16)(count - a->field_prev) % DC30_AUDIO_RING_SIZE;
		a->fields++;
		a->field_sum += d;
		a->field_sumsq += (u64)d * d;
		a->field_min = min(a->field_min, d);
		a->field_max = max(a->field_max, d);
		a->win_bytes += d;
		if (++a->win_fields == DC30_ALSA_WINDOW_FIELDS) {
			if (a->windows < DC30_ALSA_WINDOW_LOG)
				a->win_log[a->windows] = a->win_bytes;
			/* The first window includes the capture start-up. */
			if (a->windows) {
				a->win_min = min(a->win_min, a->win_bytes);
				a->win_max = max(a->win_max, a->win_bytes);
			}
			a->windows++;
			a->win_fields = 0;
			a->win_bytes = 0;
		}
	}
	a->field_prev = count;
	a->field_valid = true;
	spin_unlock(&a->field_lock);
}

void dc30_alsa_hold(struct dc30_dev *dc30, bool hold)
{
	struct dc30_alsa *a = READ_ONCE(dc30->alsa);

	if (!a)
		return;
	if (hold)
		mutex_lock(&a->drain_lock);
	else
		mutex_unlock(&a->drain_lock);
}

static void dc30_alsa_field_reset(struct dc30_alsa *a)
{
	unsigned long flags;

	spin_lock_irqsave(&a->field_lock, flags);
	a->field_valid = false;
	a->fields = a->field_sum = a->field_sumsq = 0;
	a->field_min = UINT_MAX;
	a->field_max = 0;
	a->win_fields = a->win_bytes = 0;
	a->windows = 0;
	a->win_min = UINT_MAX;
	a->win_max = 0;
	spin_unlock_irqrestore(&a->field_lock, flags);
}

static int dc30_alsa_thread(void *data)
{
	struct dc30_alsa *a = data;

	while (!kthread_should_stop()) {
		int ret = 0;

		wait_event_interruptible(a->wq, READ_ONCE(a->running) ||
						kthread_should_stop());
		if (kthread_should_stop())
			break;

		mutex_lock(&a->drain_lock);
		if (a->running) {
			ret = dc30_alsa_drain(a);
			dc30_po_tune(a->dc30);
		}
		mutex_unlock(&a->drain_lock);

		if (ret > 0) {
			snd_pcm_period_elapsed(a->substream);
		} else if (ret < 0) {
			dev_warn_ratelimited(&a->dc30->pdev->dev,
					     "audio: %s, stopping capture\n",
					     ret == -EPIPE ? "ring overrun (drain too late)"
							   : "FIFO read failed");
			a->xruns++;
			WRITE_ONCE(a->running, false);
			snd_pcm_stop_xrun(a->substream);
		}

		usleep_range(DC30_ALSA_DRAIN_US, DC30_ALSA_DRAIN_US + 1000);
	}
	return 0;
}

/* The converters power up here rather than at the start of the capture:
 * that takes ~0.5s (AD1843 power-down exit). They go down again on close.
 * In video lock mode the decoder has to deliver HREF for SYNC1 as well.
 */
static int dc30_alsa_open(struct snd_pcm_substream *ss)
{
	struct dc30_alsa *a = snd_pcm_substream_chip(ss);
	struct snd_pcm_runtime *rt = ss->runtime;
	struct task_struct *t;
	int err;

	rt->hw = dc30_alsa_hw;
	a->sync_ref = dc30_audio_videolock_enabled();
	if (a->sync_ref) {
		rt->hw.rates = SNDRV_PCM_RATE_44100;
		rt->hw.rate_min = rt->hw.rate_max = 44100;
	}

	err = dc30_audio_codec_get(a->dc30);
	if (err)
		return err;
	if (a->sync_ref) {
		err = dc30_decoder_get(a->dc30, true);
		if (err)
			goto err_codec;
	}

	a->substream = ss;
	a->running = false;
	t = kthread_run(dc30_alsa_thread, a, "dc30-audio");
	if (IS_ERR(t)) {
		err = PTR_ERR(t);
		a->substream = NULL;
		goto err_decoder;
	}
	a->thread = t;
	return 0;

err_decoder:
	if (a->sync_ref)
		dc30_decoder_put(a->dc30, true);
err_codec:
	dc30_audio_codec_put(a->dc30);
	return err;
}

static int dc30_alsa_close(struct snd_pcm_substream *ss)
{
	struct dc30_alsa *a = snd_pcm_substream_chip(ss);

	kthread_stop(a->thread);
	a->thread = NULL;
	a->substream = NULL;
	if (a->sync_ref)
		dc30_decoder_put(a->dc30, true);
	dc30_audio_codec_put(a->dc30);
	return 0;
}

static int dc30_alsa_prepare(struct snd_pcm_substream *ss)
{
	struct dc30_alsa *a = snd_pcm_substream_chip(ss);

	mutex_lock(&a->drain_lock);
	a->hw_pos = 0;
	a->period_pos = 0;
	mutex_unlock(&a->drain_lock);
	return 0;
}

static int dc30_alsa_start(struct dc30_alsa *a, unsigned int rate)
{
	struct dc30_dev *dc30 = a->dc30;
	unsigned long flags;
	int err;

	err = dc30_audio_capture_start(dc30, rate);
	if (!err)
		err = dc30_audio_counter(dc30, &a->ring_pos);
	if (err) {
		dc30_audio_capture_stop(dc30);
		return err;
	}

	/* Position 0 is the counter as read here, a few ten microseconds
	 * after the ASIC started: the positions of this stream are off by
	 * a constant one or two audio frames at most.
	 */
	spin_lock_irqsave(&a->pos_lock, flags);
	a->pos_count = a->ring_pos;
	a->pos_bytes = 0;
	a->epoch++;
	a->rate = rate;
	a->videolock = dc30_audio_videolock_enabled();
	spin_unlock_irqrestore(&a->pos_lock, flags);

	/* Audio alone doesn't pick an input: after loading the module the
	 * decoder sits on Composite, and with the VCR on S-Video the rate
	 * came out 0.5-0.9% fast and different at every start.
	 */
	if (a->videolock) {
		a->lock_input = dc30_video_input_name(dc30);
		a->lock_signal = dc30_decoder_signal(dc30);
		if (!a->lock_signal)
			dev_warn(&dc30->pdev->dev,
				 "audio video lock: no signal on input %s, the sample rate follows no video\n",
				 a->lock_input);
	}

	a->skip = 1;
	a->carry = -1;
	a->channel = 0;
	a->last_drain = ktime_get();
	a->start_time = a->last_drain;
	dc30_alsa_field_reset(a);
	a->settle_time = 0;
	a->settle_bytes = 0;
	a->bytes = a->read_ns = a->drains = 0;
	a->max_fill = a->max_gap_us = 0;
	memset(a->full_scale, 0, sizeof(a->full_scale));
	memset(a->peak, 0, sizeof(a->peak));
	spin_lock_irqsave(&dc30->po_lock, flags);
	a->start_polls = dc30->po_stream_polls;
	a->start_bytes = dc30->po_stream_bytes;
	spin_unlock_irqrestore(&dc30->po_lock, flags);
	dc30_po_tune_start(dc30);
	return 0;
}

static int dc30_alsa_trigger(struct snd_pcm_substream *ss, int cmd)
{
	struct dc30_alsa *a = snd_pcm_substream_chip(ss);
	int err = 0;

	mutex_lock(&a->drain_lock);
	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
		err = dc30_alsa_start(a, ss->runtime->rate);
		if (!err)
			WRITE_ONCE(a->running, true);
		break;
	case SNDRV_PCM_TRIGGER_STOP:
		WRITE_ONCE(a->running, false);
		dc30_audio_capture_stop(a->dc30);
		break;
	default:
		err = -EINVAL;
	}
	mutex_unlock(&a->drain_lock);

	if (!err && cmd == SNDRV_PCM_TRIGGER_START)
		wake_up(&a->wq);
	return err;
}

static snd_pcm_uframes_t dc30_alsa_pointer(struct snd_pcm_substream *ss)
{
	struct dc30_alsa *a = snd_pcm_substream_chip(ss);

	return bytes_to_frames(ss->runtime, READ_ONCE(a->hw_pos));
}

static const struct snd_pcm_ops dc30_alsa_ops = {
	.open = dc30_alsa_open,
	.close = dc30_alsa_close,
	.prepare = dc30_alsa_prepare,
	.trigger = dc30_alsa_trigger,
	.pointer = dc30_alsa_pointer,
};

/* ---- mixer ----
 *
 * The settings live in the driver (dc30_audio_mixer_*()): the AD1843
 * forgets them while powered down, which it is whenever no PCM is open
 * and no loop-through is on. The two inputs are the AD1843's Mic and
 * Aux 2 (dc30.sys offers only those); the controls name them as the
 * manual does: External (the Mic input, external audio jack) and
 * Internal (Aux 2, internal audio connector). There is no microphone.
 */

static int dc30_alsa_source_info(struct snd_kcontrol *kc,
				 struct snd_ctl_elem_info *info)
{
	static const char * const names[] = { "External", "Internal" };

	return snd_ctl_enum_info(info, 1, ARRAY_SIZE(names), names);
}

static int dc30_alsa_source_get(struct snd_kcontrol *kc,
				struct snd_ctl_elem_value *v)
{
	struct dc30_alsa *a = snd_kcontrol_chip(kc);
	u16 reg = dc30_audio_mixer_read(a->dc30, AD1843_REG_ADC_INPUT);

	v->value.enumerated.item[0] =
		(reg & AD1843_ADC_SRC_MASK) == AD1843_ADC_SRC_AUX2;
	return 0;
}

static int dc30_alsa_source_put(struct snd_kcontrol *kc,
				struct snd_ctl_elem_value *v)
{
	struct dc30_alsa *a = snd_kcontrol_chip(kc);
	u16 src = v->value.enumerated.item[0] ? AD1843_ADC_SRC_AUX2
					       : AD1843_ADC_SRC_MIC;

	if (v->value.enumerated.item[0] > 1)
		return -EINVAL;
	return dc30_audio_mixer_update(a->dc30, AD1843_REG_ADC_INPUT,
				       AD1843_ADC_SRC_MASK, src);
}

/* Generic stereo control on one AD1843 register: private_value packs
 * register, left/right shift, field width and inversion.
 */
#define DC30_STEREO(reg, lshift, rshift, max, invert) \
	((reg) | (lshift) << 8 | (rshift) << 12 | (max) << 16 | (invert) << 24)
#define DC30_PV_REG(pv)		((pv) & 0xff)
#define DC30_PV_LSHIFT(pv)	(((pv) >> 8) & 0xf)
#define DC30_PV_RSHIFT(pv)	(((pv) >> 12) & 0xf)
#define DC30_PV_MAX(pv)		(((pv) >> 16) & 0xff)
#define DC30_PV_INVERT(pv)	(((pv) >> 24) & 1)

static int dc30_alsa_stereo_info(struct snd_kcontrol *kc,
				 struct snd_ctl_elem_info *info)
{
	unsigned long pv = kc->private_value;

	info->type = DC30_PV_MAX(pv) == 1 ? SNDRV_CTL_ELEM_TYPE_BOOLEAN
					  : SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = 2;
	info->value.integer.min = 0;
	info->value.integer.max = DC30_PV_MAX(pv);
	return 0;
}

static int dc30_alsa_stereo_get(struct snd_kcontrol *kc,
				struct snd_ctl_elem_value *v)
{
	struct dc30_alsa *a = snd_kcontrol_chip(kc);
	unsigned long pv = kc->private_value;
	unsigned int max = DC30_PV_MAX(pv);
	u16 reg = dc30_audio_mixer_read(a->dc30, DC30_PV_REG(pv));
	int ch;

	for (ch = 0; ch < 2; ch++) {
		unsigned int shift = ch ? DC30_PV_RSHIFT(pv) : DC30_PV_LSHIFT(pv);
		unsigned int val = (reg >> shift) & max;

		v->value.integer.value[ch] = DC30_PV_INVERT(pv) ? max - val : val;
	}
	return 0;
}

static int dc30_alsa_stereo_put(struct snd_kcontrol *kc,
				struct snd_ctl_elem_value *v)
{
	struct dc30_alsa *a = snd_kcontrol_chip(kc);
	unsigned long pv = kc->private_value;
	unsigned int max = DC30_PV_MAX(pv);
	u16 mask = 0, val = 0;
	int ch;

	for (ch = 0; ch < 2; ch++) {
		unsigned int shift = ch ? DC30_PV_RSHIFT(pv) : DC30_PV_LSHIFT(pv);
		long x = v->value.integer.value[ch];

		if (x < 0 || x > max)
			return -EINVAL;
		if (DC30_PV_INVERT(pv))
			x = max - x;
		mask |= max << shift;
		val |= x << shift;
	}

	return dc30_audio_mixer_update(a->dc30, DC30_PV_REG(pv), mask, val);
}

/* Sticky ADC overrange bits (register 1), per channel: 0 = below -1dBFS,
 * 1 = -1..0dB, 2 = 0..+1dB over, 3 = more than 1dB over. Reading clears.
 * Always 0 while the codec is powered down - reading doesn't wake it.
 */
static int dc30_alsa_overrange_info(struct snd_kcontrol *kc,
				    struct snd_ctl_elem_info *info)
{
	info->type = SNDRV_CTL_ELEM_TYPE_INTEGER;
	info->count = 2;
	info->value.integer.min = 0;
	info->value.integer.max = 3;
	return 0;
}

static int dc30_alsa_overrange_get(struct snd_kcontrol *kc,
				   struct snd_ctl_elem_value *v)
{
	struct dc30_alsa *a = snd_kcontrol_chip(kc);
	u16 reg;
	int err;

	err = dc30_audio_overrange(a->dc30, &reg);
	if (err)
		return err;
	v->value.integer.value[0] = reg & AD1843_OVL_MASK;
	v->value.integer.value[1] = (reg >> AD1843_OVR_SHIFT) & AD1843_OVL_MASK;
	return 0;
}

static const DECLARE_TLV_DB_SCALE(dc30_alsa_gain_tlv, 0, 150, 0);

static const struct snd_kcontrol_new dc30_alsa_controls[] = {
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Capture Source",
		.info = dc30_alsa_source_info,
		.get = dc30_alsa_source_get,
		.put = dc30_alsa_source_put,
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Capture Volume",
		.access = SNDRV_CTL_ELEM_ACCESS_READWRITE |
			  SNDRV_CTL_ELEM_ACCESS_TLV_READ,
		.info = dc30_alsa_stereo_info,
		.get = dc30_alsa_stereo_get,
		.put = dc30_alsa_stereo_put,
		.tlv.p = dc30_alsa_gain_tlv,
		.private_value = DC30_STEREO(AD1843_REG_ADC_INPUT,
					     AD1843_ADC_GAIN_L_SHIFT,
					     AD1843_ADC_GAIN_R_SHIFT,
					     AD1843_ADC_GAIN_MAX, 0),
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "External Boost Capture Switch",
		.info = dc30_alsa_stereo_info,
		.get = dc30_alsa_stereo_get,
		.put = dc30_alsa_stereo_put,
		.private_value = DC30_STEREO(AD1843_REG_ADC_INPUT,
					     AD1843_ADC_BOOST_L_SHIFT,
					     AD1843_ADC_BOOST_R_SHIFT, 1, 0),
	},
	{
		/* Analog loop-through of the external input to the output. */
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "External Playback Switch",
		.info = dc30_alsa_stereo_info,
		.get = dc30_alsa_stereo_get,
		.put = dc30_alsa_stereo_put,
		.private_value = DC30_STEREO(AD1843_REG_MIX_MIC,
					     AD1843_MIX_MUTE_L_SHIFT,
					     AD1843_MIX_MUTE_R_SHIFT, 1, 1),
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "Internal Playback Switch",
		.info = dc30_alsa_stereo_info,
		.get = dc30_alsa_stereo_get,
		.put = dc30_alsa_stereo_put,
		.private_value = DC30_STEREO(AD1843_REG_MIX_AUX2,
					     AD1843_MIX_MUTE_L_SHIFT,
					     AD1843_MIX_MUTE_R_SHIFT, 1, 1),
	},
	{
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER,
		.name = "ADC Overrange",
		.access = SNDRV_CTL_ELEM_ACCESS_READ |
			  SNDRV_CTL_ELEM_ACCESS_VOLATILE,
		.info = dc30_alsa_overrange_info,
		.get = dc30_alsa_overrange_get,
	},
};

/* ---- registration ---- */

int dc30_alsa_register(struct dc30_dev *dc30)
{
	struct snd_card *card;
	struct dc30_alsa *a;
	unsigned int i;
	int err;

	err = snd_card_new(&dc30->pdev->dev, dc30_alsa_index, "DC30",
			   THIS_MODULE, sizeof(*a), &card);
	if (err)
		return err;

	a = card->private_data;
	a->dc30 = dc30;
	a->card = card;
	mutex_init(&a->drain_lock);
	spin_lock_init(&a->field_lock);
	spin_lock_init(&a->pos_lock);
	init_waitqueue_head(&a->wq);

	strscpy(card->driver, "DC30", sizeof(card->driver));
	strscpy(card->shortname, "miro DC30", sizeof(card->shortname));
	snprintf(card->longname, sizeof(card->longname),
		 "miro DC30 (AD1843) at %s, irq %d", pci_name(dc30->pdev),
		 dc30->pdev->irq);
	strscpy(card->mixername, "AD1843", sizeof(card->mixername));

	err = snd_pcm_new(card, "DC30", 0, 0, 1, &a->pcm);
	if (err)
		goto err_free;
	a->pcm->private_data = a;
	a->pcm->nonatomic = true;
	a->pcm->info_flags = 0;
	strscpy(a->pcm->name, "DC30 Capture", sizeof(a->pcm->name));
	snd_pcm_set_ops(a->pcm, SNDRV_PCM_STREAM_CAPTURE, &dc30_alsa_ops);
	snd_pcm_set_managed_buffer_all(a->pcm, SNDRV_DMA_TYPE_VMALLOC, NULL,
				       0, 0);

	for (i = 0; i < ARRAY_SIZE(dc30_alsa_controls); i++) {
		err = snd_ctl_add(card, snd_ctl_new1(&dc30_alsa_controls[i], a));
		if (err)
			goto err_free;
	}

	err = snd_card_register(card);
	if (err)
		goto err_free;

	dc30->alsa = a;
	dev_info(&dc30->pdev->dev, "ALSA card %d registered\n", card->number);
	return 0;

err_free:
	snd_card_free(card);
	return err;
}

void dc30_alsa_unregister(struct dc30_dev *dc30)
{
	struct dc30_alsa *a = dc30->alsa;

	if (!a)
		return;
	/* The field interrupt looks at dc30->alsa. */
	WRITE_ONCE(dc30->alsa, NULL);
	synchronize_irq(dc30->pdev->irq);
	snd_card_free(a->card);
}

void dc30_alsa_stats_show(struct seq_file *m, struct dc30_dev *dc30)
{
	struct dc30_alsa *a = dc30->alsa;
	u64 polls, bytes;
	unsigned long flags;
	unsigned int i;
	s64 us;
	int ch;

	if (!a) {
		seq_puts(m, "no ALSA device\n");
		return;
	}

	spin_lock_irqsave(&dc30->po_lock, flags);
	polls = dc30->po_stream_polls - a->start_polls;
	bytes = dc30->po_stream_bytes - a->start_bytes;
	spin_unlock_irqrestore(&dc30->po_lock, flags);

	/* Racy snapshot of the drain thread's counters, fine for a dump. */
	seq_printf(m, "running        %d\n", READ_ONCE(a->running));
	seq_printf(m, "bytes          %llu\n", a->bytes);
	seq_printf(m, "drains         %llu\n", a->drains);
	/* Sample rate as the hardware delivered it: bytes read up to the
	 * last drain over the time since the start. Good to ~0.1% after a
	 * few seconds (one drain interval of uncertainty).
	 */
	us = ktime_us_delta(a->last_drain, a->start_time);
	seq_printf(m, "measured rate  %llu Hz over %lld ms\n",
		   us > 0 ? div64_u64(a->bytes * USEC_PER_SEC /
				      DC30_ALSA_FRAME_BYTES, us) : 0,
		   us / 1000);
	us = a->settle_time ? ktime_us_delta(a->last_drain, a->settle_time) : 0;
	seq_printf(m, "rate after 2s  %llu Hz over %lld ms\n",
		   us > 0 ? div64_u64((a->bytes - a->settle_bytes) *
				      USEC_PER_SEC / DC30_ALSA_FRAME_BYTES, us)
			  : 0,
		   us / 1000);
	seq_printf(m, "clock          %s\n", dc30_audio_videolock_enabled() ?
		   (dc30_audio_rate_src_word() == 0x0a0a ?
		    "video lock (SYNC2)" : "video lock (SYNC1)") : "crystal");
	seq_printf(m, "cg mode        0x%04x\n", dc30_audio_cg_mode_word());
	seq_printf(m, "rate source    0x%04x\n", dc30_audio_rate_src_word());
	if (a->lock_input)
		seq_printf(m, "lock input     %s, %s (at the last video lock start)\n",
			   a->lock_input,
			   a->lock_signal > 0 ? "signal" :
			   a->lock_signal == 0 ? "NO SIGNAL" : "status unknown");
	seq_printf(m, "max fill       %u of %u\n", a->max_fill,
		   DC30_AUDIO_RING_SIZE);
	seq_printf(m, "max gap        %u us\n", a->max_gap_us);
	seq_printf(m, "xruns          %llu\n", a->xruns);
	seq_printf(m, "POR reads/byte %llu.%02llu\n",
		   bytes ? div64_u64(polls, bytes) : 0,
		   bytes ? div64_u64(polls * 100, bytes) % 100 : 0);
	spin_lock_irqsave(&a->field_lock, flags);
	if (a->fields) {
		u64 n = a->fields, sum = a->field_sum;
		u64 mean_m = div64_u64(sum * 1000, n * DC30_ALSA_FRAME_BYTES);
		/* variance in bytes^2, then std in frames */
		u64 var = div64_u64(a->field_sumsq, n) -
			  div64_u64(sum, n) * div64_u64(sum, n);

		seq_printf(m, "fields         %llu (video interrupts while capturing)\n", n);
		seq_printf(m, "frames/field   mean %llu.%03llu, min %u, max %u, std ~%u\n",
			   div_u64(mean_m, 1000), mean_m % 1000,
			   a->field_min / DC30_ALSA_FRAME_BYTES,
			   a->field_max / DC30_ALSA_FRAME_BYTES,
			   int_sqrt64(var) / DC30_ALSA_FRAME_BYTES);
		seq_printf(m, "frames/50 fld  %llu windows, min %u, max %u (nominal 44100, first window not counted)\n",
			   a->windows,
			   a->windows > 1 ? a->win_min / DC30_ALSA_FRAME_BYTES : 0,
			   a->win_max / DC30_ALSA_FRAME_BYTES);
		seq_puts(m, "per window    ");
		for (i = 0; i < min_t(u64, a->windows, DC30_ALSA_WINDOW_LOG); i++)
			seq_printf(m, " %u", a->win_log[i] / DC30_ALSA_FRAME_BYTES);
		seq_putc(m, '\n');
	} else {
		seq_puts(m, "fields         0 (no video streaming during capture)\n");
	}
	spin_unlock_irqrestore(&a->field_lock, flags);
	seq_printf(m, "ns/byte        %llu\n",
		   a->bytes ? div64_u64(a->read_ns, a->bytes) : 0);
	dc30_po_tune_show(m, dc30);
	for (ch = 0; ch < 2; ch++)
		seq_printf(m, "%s peak %6u, full scale samples %llu\n",
			   ch ? "right" : "left ", a->peak[ch],
			   a->full_scale[ch]);
}

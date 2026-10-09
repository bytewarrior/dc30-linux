// SPDX-License-Identifier: GPL-2.0-only
#include "audiomonitor.h"

#include "audiocapture.h"
#include "clock.h"

#include <alsa/asoundlib.h>
#include <algorithm>
#include <cmath>
#include <vector>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}

namespace {

constexpr int kInRate = AudioCapture::kRate;
constexpr int kOutRate = 48000;			// the PC side's usual rate
constexpr int kInChunk = 441;			// 10 ms per step
constexpr int kOutChunk = kInChunk * kOutRate / kInRate;	// nominal
constexpr unsigned kOutBufferUs = 60000;
constexpr double kTarget = 0.150;		// s of sound in flight
constexpr double kResync = 0.200;		// s off target: jump instead
constexpr double kMaxCorrection = 0.005;	// +-0.5%
constexpr double kKp = 0.05;			// per s of error
constexpr double kKi = 0.01;			// per s of error and s
constexpr double kSmooth = 0.02;		// error low pass, ~0.5 s
constexpr double kDt = double(kInChunk) / kInRate;
constexpr double kClockGain = 0.03;		// clock smoothing, ~0.3 s
constexpr double kClockReset = 0.05 * kInRate;	// jumps beyond 50 ms

} // namespace

AudioMonitor::AudioMonitor(const AudioCapture *capture, QObject *parent)
	: QThread(parent), m_capture(capture)
{
}

AudioMonitor::~AudioMonitor()
{
	stop();
	wait();
}

void AudioMonitor::publish(double audible, unsigned generation, int64_t nowNs)
{
	/* Run the clock on its own and pull it gently towards each
	 * measurement; follow a real jump (resync) at once.
	 */
	if (m_smoothValid) {
		const double predicted = m_smoothPos +
					 double(nowNs - m_smoothNs) * 1e-9 * kInRate;
		const double err = audible - predicted;

		m_smoothPos = std::fabs(err) > kClockReset ? audible
							   : predicted + kClockGain * err;
	} else {
		m_smoothPos = audible;
		m_smoothValid = true;
	}
	m_smoothNs = nowNs;

	m_clockSeq.fetch_add(1);
	m_clockPos = m_smoothPos;
	m_clockNs = nowNs;
	m_clockGen = generation;
	m_clockValid = true;
	m_clockSeq.fetch_add(1);
}

bool AudioMonitor::audiblePosition(int64_t nowNs, double *pos,
				   unsigned *generation) const
{
	unsigned s1, s2;
	double p;
	int64_t ns;
	unsigned gen;
	bool valid;

	do {
		s1 = m_clockSeq;
		p = m_clockPos;
		ns = m_clockNs;
		gen = m_clockGen;
		valid = m_clockValid;
		s2 = m_clockSeq;
	} while ((s1 & 1) || s1 != s2);

	if (!valid)
		return false;
	/* Published every 10 ms; the sound goes on meanwhile. */
	*pos = p + double(nowNs - ns) * 1e-9 * kInRate;
	*generation = gen;
	return true;
}

void AudioMonitor::run()
{
	snd_pcm_t *pcm = nullptr;
	int err = snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0);

	if (err < 0) {
		emit failed(tr("Sound output: %1").arg(snd_strerror(err)));
		return;
	}
	err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE,
				 SND_PCM_ACCESS_RW_INTERLEAVED, 2, kOutRate, 1,
				 kOutBufferUs);
	if (err < 0) {
		emit failed(tr("Sound output parameters: %1").arg(snd_strerror(err)));
		snd_pcm_close(pcm);
		return;
	}

	SwrContext *swr = nullptr;
	AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;

	if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_S16, kOutRate,
				&stereo, AV_SAMPLE_FMT_S16, kInRate, 0, nullptr) < 0 ||
	    swr_init(swr) < 0) {
		emit failed(tr("Cannot set up the resampler"));
		swr_free(&swr);
		snd_pcm_close(pcm);
		return;
	}

	const int outCap = kOutChunk * 2 + 64;
	std::vector<int16_t> in(size_t(kInChunk) * 2), out(size_t(outCap) * 2);
	uint64_t readPos = 0;
	unsigned generation = 0;
	bool running = false;
	double eFilt = 0, integral = 0, correction = 0, frac = 0;
	double latencySum = 0;
	unsigned latencyN = 0;
	int64_t lastReport = 0;

	while (!m_stop) {
		uint64_t head;
		unsigned gen;

		m_capture->head(&head, &gen);
		if (!running || gen != generation) {
			/* (Re)start: once the target amount is captured, begin
			 * that far behind the head. The output buffer fills from
			 * there, the sound in flight stays the same.
			 */
			generation = gen;
			m_clockValid = false;
			m_smoothValid = false;
			if (head < uint64_t(kTarget * kInRate) + kInChunk) {
				msleep(5);
				continue;
			}
			readPos = head - uint64_t(kTarget * kInRate);
			eFilt = integral = correction = frac = 0;
			running = true;
		}
		if (head < readPos + kInChunk) {
			/* The capture comes in 10 ms periods: wait for the next. */
			msleep(2);
			continue;
		}
		if (!m_capture->read(readPos, kInChunk, in.data(), generation)) {
			running = false;	/* restarted or fell out of the ring */
			continue;
		}

		/* Captured sample audible now: what goes into the resampler
		 * next, minus what the resampler and the output still hold.
		 */
		const int64_t now = dc30::monotonicNs();
		snd_pcm_sframes_t delay = 0;

		if (snd_pcm_delay(pcm, &delay) < 0 || delay < 0)
			delay = 0;
		const double audible = double(readPos) -
				       double(swr_get_delay(swr, kInRate)) -
				       double(delay) * kInRate / kOutRate;
		publish(audible, generation, now);

		const double e = (double(head) - audible) / kInRate - kTarget;

		if (std::fabs(e) > kResync) {
			/* Start-up, a stall or a lost output: jump (skips or
			 * repeats up to the error once) rather than steer for
			 * minutes.
			 */
			const double to = double(readPos) + e * kInRate;

			readPos = uint64_t(std::clamp(to, 0.0, double(head - kInChunk)));
			eFilt = integral = 0;
			continue;
		}

		/* PI controller on the smoothed amount in flight: too much ->
		 * play faster (drop output samples), too little -> slower.
		 */
		eFilt += kSmooth * (e - eFilt);
		integral = std::clamp(integral + eFilt * kDt,
				      -kMaxCorrection / kKi, kMaxCorrection / kKi);
		correction = std::clamp(kKp * eFilt + kKi * integral,
					-kMaxCorrection, kMaxCorrection);
		const double delta = -correction * kOutChunk + frac;
		const int d = int(std::lround(delta));

		frac = delta - d;
		swr_set_compensation(swr, d, kOutChunk);

		const uint8_t *inPtr = reinterpret_cast<const uint8_t *>(in.data());
		uint8_t *outPtr = reinterpret_cast<uint8_t *>(out.data());
		const int n = swr_convert(swr, &outPtr, outCap, &inPtr, kInChunk);

		readPos += kInChunk;
		if (n <= 0)
			continue;

		const float vol = m_volume;
		if (vol != 1.0f)
			for (int i = 0; i < n * 2; i++)
				out[i] = int16_t(std::clamp(out[i] * vol, -32768.0f, 32767.0f));

		for (int done = 0; done < n;) {
			snd_pcm_sframes_t w = snd_pcm_writei(pcm, out.data() + done * 2,
							     n - done);
			if (w < 0) {
				if (snd_pcm_recover(pcm, int(w), 1) < 0)
					break;
				continue;
			}
			done += int(w);
		}

		latencySum += e + kTarget;
		latencyN++;
		if (now - lastReport > 500000000) {
			emit syncState(latencySum / latencyN * 1000.0, correction * 1e6);
			latencySum = 0;
			latencyN = 0;
			lastReport = now;
		}
	}

	m_clockValid = false;
	swr_free(&swr);
	snd_pcm_drop(pcm);
	snd_pcm_close(pcm);
}

// SPDX-License-Identifier: GPL-2.0-only
#include "audiocapture.h"

#include "devices.h"
#include "recorder.h"

#include <alsa/asoundlib.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace {

constexpr unsigned kRingFrames = AudioCapture::kRate * 4;	// 4 s
constexpr snd_pcm_uframes_t kPeriod = AudioCapture::kPeriod;
constexpr unsigned kPeriodsPerLevel = 5;			// ~50 ms
constexpr unsigned kBufferUs = 200000;

} // namespace

AudioCapture::AudioCapture(QObject *parent)
	: QThread(parent), m_ring(size_t(kRingFrames) * kChannels)
{
}

AudioCapture::~AudioCapture()
{
	stop();
	wait();
}

void AudioCapture::head(uint64_t *pos, unsigned *generation) const
{
	std::lock_guard<std::mutex> g(m_lock);

	*pos = m_head;
	*generation = m_generation;
}

void AudioCapture::setRecorder(Recorder *recorder)
{
	std::lock_guard<std::mutex> g(m_recorderLock);

	m_recorder = recorder;
}

bool AudioCapture::read(uint64_t pos, unsigned frames, int16_t *out,
			unsigned generation) const
{
	std::lock_guard<std::mutex> g(m_lock);

	if (generation != m_generation || pos + frames > m_head ||
	    m_head - pos > kRingFrames)
		return false;
	for (unsigned i = 0; i < frames; i++) {
		size_t r = size_t((pos + i) % kRingFrames) * kChannels;

		out[i * 2] = m_ring[r];
		out[i * 2 + 1] = m_ring[r + 1];
	}
	return true;
}

void AudioCapture::run()
{
	snd_pcm_t *pcm = nullptr;
	int err = snd_pcm_open(&pcm, dc30::alsaDevice, SND_PCM_STREAM_CAPTURE, 0);

	if (err < 0) {
		emit failed(tr("Audio device %1: %2")
				    .arg(dc30::alsaDevice, snd_strerror(err)));
		return;
	}
	// Short periods, long buffer: snd_pcm_set_params() would make the
	// period a quarter of the buffer (50 ms), and the samples would reach
	// the sync monitor in 50 ms lumps. The driver drains the card every
	// 5 ms.
	snd_pcm_hw_params_t *hw;
	snd_pcm_uframes_t period = kPeriod;
	unsigned buffer = kBufferUs;

	snd_pcm_hw_params_alloca(&hw);
	err = snd_pcm_hw_params_any(pcm, hw);
	if (err >= 0)
		err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
	if (err >= 0)
		err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
	if (err >= 0)
		err = snd_pcm_hw_params_set_channels(pcm, hw, kChannels);
	if (err >= 0)
		err = snd_pcm_hw_params_set_rate(pcm, hw, kRate, 0);
	if (err >= 0)
		err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, nullptr);
	if (err >= 0)
		err = snd_pcm_hw_params_set_buffer_time_near(pcm, hw, &buffer, nullptr);
	if (err >= 0)
		err = snd_pcm_hw_params(pcm, hw);
	if (err < 0) {
		emit failed(tr("Audio parameters: %1").arg(snd_strerror(err)));
		snd_pcm_close(pcm);
		return;
	}

	/* snd_pcm_set_params() puts the start threshold at the buffer size,
	 * which a capture read of one period never reaches: start by hand.
	 */
	snd_pcm_start(pcm);

	std::vector<int16_t> buf(kPeriod * kChannels);
	float peak[2] = {}, sum[2] = {};
	unsigned n = 0, periods = 0;
	bool clip = false;

	while (!m_stop) {
		snd_pcm_sframes_t got = snd_pcm_readi(pcm, buf.data(), kPeriod);

		if (got < 0) {
			/* xrun: the driver restarts the stream, positions and
			 * the metadata's audio epoch start over.
			 */
			emit xrun();
			snd_pcm_prepare(pcm);
			snd_pcm_start(pcm);
			std::lock_guard<std::mutex> g(m_lock);
			m_head = 0;
			m_generation++;
			continue;
		}

		uint64_t pos;
		unsigned generation;
		{
			std::lock_guard<std::mutex> g(m_lock);

			pos = m_head;
			generation = m_generation;
			for (snd_pcm_sframes_t i = 0; i < got; i++) {
				size_t w = size_t((m_head + i) % kRingFrames) * kChannels;

				m_ring[w] = buf[i * 2];
				m_ring[w + 1] = buf[i * 2 + 1];
			}
			m_head += got;
		}
		{
			std::lock_guard<std::mutex> g(m_recorderLock);

			if (m_recorder)
				m_recorder->pushAudio(generation, pos, buf.data(), got);
		}

		for (snd_pcm_sframes_t i = 0; i < got * 2; i++) {
			int s = buf[i];
			float a = std::abs(s) / 32768.0f;

			peak[i & 1] = std::max(peak[i & 1], a);
			sum[i & 1] += a * a;
			if (s >= 32767 || s <= -32768)
				clip = true;
		}
		n += got;

		if (++periods == kPeriodsPerLevel) {
			emit levels(peak[0], peak[1], std::sqrt(sum[0] / n),
				    std::sqrt(sum[1] / n), clip);
			peak[0] = peak[1] = sum[0] = sum[1] = 0;
			n = periods = 0;
			clip = false;
		}
	}
	snd_pcm_close(pcm);
}

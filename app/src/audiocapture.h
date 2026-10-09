// SPDX-License-Identifier: GPL-2.0-only
// Audio capture thread: reads hw:DC30 (S16_LE stereo 44.1kHz) for level
// metering and keeps the last seconds in a ring, indexed by the frame's
// position in the ALSA stream - the same positions the metadata records
// carry.
#pragma once

#include <QThread>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

class Recorder;

class AudioCapture : public QThread {
	Q_OBJECT
public:
	static constexpr unsigned kRate = 44100;
	static constexpr unsigned kChannels = 2;
	// ALSA period and read size, ~10 ms: new samples reach the ring in
	// steps of this, which the sync monitor has to stay behind.
	static constexpr unsigned kPeriod = 441;

	explicit AudioCapture(QObject *parent = nullptr);
	~AudioCapture() override;

	void stop() { m_stop = true; }

	// Copy 'frames' interleaved frames starting at stream position 'pos'.
	// False if that range is not (or no longer) in the ring, or the
	// stream restarted ('generation' changed).
	bool read(uint64_t pos, unsigned frames, int16_t *out,
		  unsigned generation) const;
	// Stream position of the next frame to arrive, and the stream's
	// generation (bumped on every ALSA restart, positions restart at 0).
	void head(uint64_t *pos, unsigned *generation) const;
	// Hand every block read to 'recorder' too (nullptr: stop).
	void setRecorder(Recorder *recorder);

signals:
	void failed(const QString &message);
	// ~20 times a second: peak and rms per channel (0..1 of full scale),
	// and whether a full-scale sample occurred.
	void levels(float peakL, float peakR, float rmsL, float rmsR, bool clip);
	void xrun();

protected:
	void run() override;

private:
	std::atomic<bool> m_stop{false};

	mutable std::mutex m_lock;
	std::vector<int16_t> m_ring;	// kRingFrames * kChannels
	uint64_t m_head = 0;		// stream position of the next frame
	unsigned m_generation = 0;

	std::mutex m_recorderLock;
	Recorder *m_recorder = nullptr;
};

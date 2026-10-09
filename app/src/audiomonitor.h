// SPDX-License-Identifier: GPL-2.0-only
// Plays the captured audio on the PC's sound output ("default") and is the
// clock the preview picture follows.
//
// The card and the PC sound output run on different clocks. The monitor
// keeps a fixed amount of sound in flight - not yet played from the
// capture ring plus what sits in the output's buffer - and holds it there
// by resampling a fraction of a percent faster or slower (libswresample
// compensation, steered by a PI controller). So the output neither runs
// dry nor overflows, and no sample is skipped or repeated.
//
// It also knows which captured sample is audible right now. The preview
// shows each frame when its audio position (metadata fit) is reached, so
// the picture is delayed by as much as the sound is.
#pragma once

#include <QThread>
#include <atomic>
#include <cstdint>

class AudioCapture;

class AudioMonitor : public QThread {
	Q_OBJECT
public:
	explicit AudioMonitor(const AudioCapture *capture, QObject *parent = nullptr);
	~AudioMonitor() override;

	void stop() { m_stop = true; }
	void setVolume(float v) { m_volume = v; }

	// Capture stream position (audio frames) audible at 'nowNs'
	// (CLOCK_MONOTONIC), and the capture generation it belongs to.
	// False until the output runs.
	bool audiblePosition(int64_t nowNs, double *pos, unsigned *generation) const;

signals:
	void failed(const QString &message);
	// ~2 times a second: sound in flight (capture to loudspeaker) in ms
	// and the current rate correction in ppm (+ = playing faster).
	void syncState(double latencyMs, double correctionPpm);

protected:
	void run() override;

private:
	void publish(double audible, unsigned generation, int64_t nowNs);

	const AudioCapture *m_capture;
	// Smoothed clock, run() only: the output delay moves in the sound
	// server's steps (PipeWire quantum, ~21 ms), which made frames due
	// in bursts - shown in pairs, one of them skipped.
	double m_smoothPos = 0;
	int64_t m_smoothNs = 0;
	bool m_smoothValid = false;
	std::atomic<bool> m_stop{false};
	std::atomic<float> m_volume{1.0f};

	// Last audible position, written by run(), read by the preview.
	// Seqlock-style: m_clockSeq odd while updating.
	std::atomic<unsigned> m_clockSeq{0};
	std::atomic<double> m_clockPos{0};
	std::atomic<int64_t> m_clockNs{0};
	std::atomic<unsigned> m_clockGen{0};
	std::atomic<bool> m_clockValid{false};
};

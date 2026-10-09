// SPDX-License-Identifier: GPL-2.0-only
// Metadata capture thread (dc30-meta node, dc30_meta.h). Keeps a sliding
// least-squares fit of the audio position over the frame sequence number:
// the smoothed audio position of any recent frame.
#pragma once

#include "audiofit.h"

#include <QThread>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

class Recorder;

class MetaCapture : public QThread {
	Q_OBJECT
public:
	explicit MetaCapture(std::string device, QObject *parent = nullptr);
	~MetaCapture() override;

	void stop() { m_stop = true; }

	// Audio frame index (in the ALSA stream 'epoch') that belongs to the
	// first field of video frame 'sequence', from the current fit. False
	// while there is no fit yet.
	bool audioPosition(uint32_t sequence, double *pos, uint32_t *epoch) const;
	// Start the fit over, e.g. after an input change: in video lock the
	// audio clock follows the input.
	void resetFit();
	// Hand every record to 'recorder' too (nullptr: stop).
	void setRecorder(Recorder *recorder);

signals:
	void failed(const QString &message);
	// Once a second: fitted audio frames per video frame, rms scatter of
	// single values around the fit, audio clock (DC30_META_CLOCK_*), and
	// the driver's counters.
	void syncInfo(double audioPerFrame, double scatter, int clock,
		      quint32 dropped, quint32 noField, quint32 fieldOrder);

protected:
	void run() override;

private:
	std::string m_device;
	int m_fd = -1;
	std::atomic<bool> m_stop{false};

	mutable std::mutex m_lock;
	dc30::AudioFit m_fit;		// last 20 s of one epoch

	std::mutex m_recorderLock;
	Recorder *m_recorder = nullptr;
};

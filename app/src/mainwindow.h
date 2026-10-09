// SPDX-License-Identifier: GPL-2.0-only
#pragma once

#include "alsamixer.h"

#include <QMainWindow>
#include <cstdint>
#include <memory>

class QAction;
class QCheckBox;
class QComboBox;
class QDockWidget;
class QLabel;
class QLineEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class AudioCapture;
class AudioMonitor;
class LevelMeter;
class MetaCapture;
class PreviewWidget;
class Recorder;
class VideoCapture;

class MainWindow : public QMainWindow {
	Q_OBJECT
public:
	MainWindow();
	~MainWindow() override;

	// Unattended: start recording after a short lead-in, stop after
	// 'seconds', then quit. 'dir' overrides the folder if not empty.
	void recordFor(int seconds, const QString &dir);

	// Video codec for recordings: "ffv1" or "mjpeg" (the card's codec at
	// 'kbps' kB/s, 0: keep the setting). False for an unknown name.
	bool setCodec(const QString &name, int kbps);

private:
	enum MonitorMode { MonitorOff, MonitorAnalog, MonitorSync };

	QWidget *buildControls();
	void startCapture();
	void startVideo();
	void restartVideo();
	bool mjpeg() const;
	void startRecording();
	void stopRecording();
	void recordingEnded();
	static qint64 freeBytes(const QString &dir);
	void setMonitorMode(int mode);
	bool frameDue(uint32_t sequence, int64_t nowNs) const;
	void updateGainLabel(int steps);
	void applyMute();
	void showLost(quint32 lost);
	void reportError(const QString &msg);
	void loadSettings();
	void saveSettings();

	PreviewWidget *m_preview = nullptr;
	LevelMeter *m_meter = nullptr;
	QComboBox *m_input = nullptr, *m_source = nullptr, *m_monitor = nullptr;
	QSlider *m_gain = nullptr, *m_offset = nullptr, *m_volume = nullptr;
	QLabel *m_gainLabel = nullptr, *m_offsetLabel = nullptr;
	QCheckBox *m_boost = nullptr;
	QAction *m_mute = nullptr;
	QLineEdit *m_dir = nullptr;
	QComboBox *m_codec = nullptr;
	QSpinBox *m_rate = nullptr;
	QCheckBox *m_fieldRepair = nullptr;	// pair fields by the picture
	QPushButton *m_record = nullptr;
	QLabel *m_recStatus = nullptr;
	QLabel *m_lost = nullptr;		// frames lost in the recording
	qint64 m_freeBytes = -1;
	int m_freeAge = 0;		// progress updates until the next statvfs
	// For debugging, hidden by default.
	QDockWidget *m_details = nullptr;
	QLabel *m_statVideo = nullptr, *m_statSync = nullptr, *m_statMonitor = nullptr;
	unsigned m_xruns = 0;
	int m_offsetMs = 0;

	AlsaMixer m_mixer;
	std::unique_ptr<AudioCapture> m_audio;
	std::unique_ptr<MetaCapture> m_meta;
	std::unique_ptr<VideoCapture> m_video;
	std::unique_ptr<AudioMonitor> m_monitorThread;
	std::unique_ptr<Recorder> m_recorder;
};

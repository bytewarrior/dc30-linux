// SPDX-License-Identifier: GPL-2.0-only
#include "mainwindow.h"

#include "aboutdialog.h"
#include "audiocapture.h"
#include "audiomonitor.h"
#include "dc30_meta.h"
#include "devices.h"
#include "levelmeter.h"
#include "metacapture.h"
#include "previewwidget.h"
#include "recorder.h"
#include "videocapture.h"

#include <cmath>
#include <sys/statvfs.h>

#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QDockWidget>
#include <QDir>
#include <QDebug>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

MainWindow::MainWindow()
{
	setWindowTitle(tr("DC30 Capture"));

	/* Mutes the preview's sound only; the recording keeps it. */
	m_mute = new QAction(QIcon::fromTheme(QStringLiteral("audio-volume-high")),
			     tr("&Mute preview"), this);
	m_mute->setCheckable(true);
	m_mute->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_M));
	m_mute->setToolTip(tr("Mute the monitoring (Ctrl+M); the recording keeps "
			      "its sound"));
	connect(m_mute, &QAction::toggled, this, &MainWindow::applyMute);

	auto *central = new QWidget(this);
	auto *layout = new QHBoxLayout(central);

	m_preview = new PreviewWidget(central);
	layout->addWidget(m_preview, 1);
	layout->addWidget(buildControls());
	setCentralWidget(central);

	m_details = new QDockWidget(tr("Details"), this);
	m_details->setObjectName(QStringLiteral("details"));
	m_details->setFeatures(QDockWidget::DockWidgetClosable);
	auto *detailsPanel = new QWidget(m_details);
	auto *dv = new QVBoxLayout(detailsPanel);
	m_statVideo = new QLabel(detailsPanel);
	m_statSync = new QLabel(detailsPanel);
	m_statMonitor = new QLabel(detailsPanel);
	for (QLabel *l : {m_statVideo, m_statSync, m_statMonitor}) {
		l->setTextInteractionFlags(Qt::TextSelectableByMouse);
		dv->addWidget(l);
	}
	m_details->setWidget(detailsPanel);
	addDockWidget(Qt::BottomDockWidgetArea, m_details);
	m_details->hide();

	/* The status bar is for errors only: hidden until there is one. */
	statusBar()->hide();
	connect(statusBar(), &QStatusBar::messageChanged, this,
		[this](const QString &text) { statusBar()->setVisible(!text.isEmpty()); });

	QMenu *view = menuBar()->addMenu(tr("&View"));
	QAction *showDetails = m_details->toggleViewAction();
	showDetails->setText(tr("&Details"));
	showDetails->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_D));
	view->addAction(showDetails);
	view->addAction(m_mute);

	QMenu *help = menuBar()->addMenu(tr("&Help"));
	help->addAction(tr("&About DC30 Capture …"), this, [this] {
		AboutDialog(this).exec();
	});

	loadSettings();
	startCapture();

	/* The codec's overrange bits: poll, they are sticky until read. */
	auto *clipTimer = new QTimer(this);
	connect(clipTimer, &QTimer::timeout, this, [this] {
		if (m_mixer.overrange() >= 2)
			m_meter->setHardwareClip();
	});
	clipTimer->start(250);
}

MainWindow::~MainWindow()
{
	saveSettings();
	/* The recorder still needs the capture threads for the last frames'
	 * sound.
	 */
	if (m_recorder) {
		m_recorder->finish();
		m_recorder->wait();
		recordingEnded();
	}
	/* The preview asks the monitor and the metadata for the time. */
	m_preview->setPresentationClock({});
	/* The monitor reads from the capture threads: stop it first. */
	m_monitorThread.reset();
	m_video.reset();
	m_meta.reset();
	m_audio.reset();
	m_mixer.setLoopThrough(false);
}

QWidget *MainWindow::buildControls()
{
	auto *panel = new QWidget(this);
	auto *v = new QVBoxLayout(panel);

	/* Video */
	auto *videoBox = new QGroupBox(tr("Video"), panel);
	auto *vf = new QFormLayout(videoBox);
	m_input = new QComboBox(videoBox);
	m_input->addItems({tr("Composite"), tr("S-Video"), tr("Internal")});
	vf->addRow(tr("Input"), m_input);
	vf->addRow(tr("Standard"), new QLabel(tr("PAL"), videoBox));
	v->addWidget(videoBox);

	/* Audio */
	auto *audioBox = new QGroupBox(tr("Audio"), panel);
	auto *ah = new QHBoxLayout(audioBox);
	auto *af = new QFormLayout;
	m_source = new QComboBox(audioBox);
	m_source->addItems({tr("External (socket)"), tr("Internal")});
	af->addRow(tr("Source"), m_source);
	m_gain = new QSlider(Qt::Horizontal, audioBox);
	m_gain->setRange(0, 15);
	m_gainLabel = new QLabel(audioBox);
	m_gainLabel->setMinimumWidth(60);
	auto *gh = new QHBoxLayout;
	gh->addWidget(m_gain);
	gh->addWidget(m_gainLabel);
	af->addRow(tr("Gain"), gh);
	m_boost = new QCheckBox(tr("Input boost +20 dB"), audioBox);
	m_boost->setToolTip(tr("Only affects the external socket (the codec's MIC input)"));
	af->addRow(QString(), m_boost);

	m_monitor = new QComboBox(audioBox);
	m_monitor->addItems({tr("Off"), tr("Analog (card, no delay)"),
			     tr("In sync with the picture (PC output)")});
	af->addRow(tr("Monitoring"), m_monitor);
	m_volume = new QSlider(Qt::Horizontal, audioBox);
	m_volume->setRange(0, 100);
	auto *muteButton = new QToolButton(audioBox);
	muteButton->setDefaultAction(m_mute);
	muteButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
	auto *vh = new QHBoxLayout;
	vh->addWidget(m_volume);
	vh->addWidget(muteButton);
	af->addRow(tr("Volume"), vh);
	m_offset = new QSlider(Qt::Horizontal, audioBox);
	m_offset->setRange(-200, 200);
	m_offsetLabel = new QLabel(audioBox);
	m_offsetLabel->setMinimumWidth(60);
	auto *oh = new QHBoxLayout;
	oh->addWidget(m_offset);
	oh->addWidget(m_offsetLabel);
	af->addRow(tr("Audio offset"), oh);
	m_offset->setToolTip(tr("Positive: audio later. Compensates for the delay of "
				"screen and speakers."));
	ah->addLayout(af, 1);
	m_meter = new LevelMeter(audioBox);
	ah->addWidget(m_meter);
	v->addWidget(audioBox, 1);

	/* Recording */
	auto *recBox = new QGroupBox(tr("Recording"), panel);
	auto *rf = new QVBoxLayout(recBox);
	auto *cf = new QFormLayout;
	m_codec = new QComboBox(recBox);
	m_codec->addItems({tr("FFV1 (lossless, computer)"), tr("MJPEG (card)")});
	m_codec->setToolTip(tr("MJPEG is compressed by the card itself: for a computer "
			       "too slow for FFV1. Lossy."));
	cf->addRow(tr("Video codec"), m_codec);
	m_rate = new QSpinBox(recBox);
	m_rate->setRange(1000, 6300);
	m_rate->setSingleStep(100);
	m_rate->setSuffix(tr(" kB/s"));
	m_rate->setKeyboardTracking(false);
	m_rate->setToolTip(tr("Data rate of the card's MJPEG codec; the picture "
			      "quality follows from it (6000 kB/s: nearly lossless)"));
	cf->addRow(tr("Data rate"), m_rate);
	m_fieldRepair = new QCheckBox(tr("Pair fields by the picture"), recBox);
	m_fieldRepair->setToolTip(
		tr("The decoder's field flag is unreliable with a VCR without TBC: whole "
		   "scenes come out with the fields swapped. With this on, the order "
		   "of the fields is taken from the picture itself, and a field left "
		   "alone where the signal's field grid slips becomes a frame of its own "
		   "(20 ms). Nothing is thrown away, the sound stays as it is."));
	cf->addRow(QString(), m_fieldRepair);
	rf->addLayout(cf);
	auto *fh = new QHBoxLayout;
	m_dir = new QLineEdit(recBox);
	m_dir->setToolTip(tr("Folder; the file name is made from date and time"));
	auto *browse = new QPushButton(tr("…"), recBox);
	fh->addWidget(m_dir);
	fh->addWidget(browse);
	rf->addLayout(fh);
	m_record = new QPushButton(tr("Start recording"), recBox);
	m_record->setCheckable(true);
	rf->addWidget(m_record);
	/* Right at the button: the one figure that says whether the
	 * recording is complete. Empty until the first recording.
	 */
	m_lost = new QLabel(recBox);
	rf->addWidget(m_lost);
	m_recStatus = new QLabel(recBox);
	m_recStatus->setWordWrap(true);
	rf->addWidget(m_recStatus);
	v->addWidget(recBox);

	connect(browse, &QPushButton::clicked, this, [this] {
		QString d = QFileDialog::getExistingDirectory(this, tr("Folder for recordings"),
							      m_dir->text());
		if (!d.isEmpty())
			m_dir->setText(d);
	});
	connect(m_record, &QPushButton::clicked, this, [this](bool on) {
		if (on)
			startRecording();
		else
			stopRecording();
	});
	connect(m_input, qOverload<int>(&QComboBox::currentIndexChanged), this,
		[this](int i) {
			if (!m_video)
				return;
			m_video->setInput(i);
			/* In video lock the audio clock follows the new input:
			 * the old fit no longer holds.
			 */
			if (m_meta)
				m_meta->resetFit();
		});
	/* The format is set when the video stream starts. */
	connect(m_codec, qOverload<int>(&QComboBox::currentIndexChanged), this,
		[this] {
			m_rate->setEnabled(mjpeg());
			restartVideo();
		});
	connect(m_rate, qOverload<int>(&QSpinBox::valueChanged), this, [this] {
		if (mjpeg())
			restartVideo();
	});
	connect(m_fieldRepair, &QCheckBox::toggled, this, [this](bool on) {
		if (m_video)
			m_video->setPairByFlag(!on);
	});
	connect(m_source, qOverload<int>(&QComboBox::currentIndexChanged), this,
		[this](int i) {
			m_mixer.setSource(i);
			setMonitorMode(m_monitor->currentIndex());
		});
	connect(m_gain, &QSlider::valueChanged, this, [this](int s) {
		m_mixer.setGain(s);
		updateGainLabel(s);
	});
	connect(m_boost, &QCheckBox::toggled, this,
		[this](bool on) { m_mixer.setBoost(on); });
	connect(m_monitor, qOverload<int>(&QComboBox::currentIndexChanged), this,
		&MainWindow::setMonitorMode);
	connect(m_offset, &QSlider::valueChanged, this, [this](int ms) {
		m_offsetLabel->setText(tr("%1 ms").arg(ms));
		m_offsetMs = ms;
	});
	connect(m_volume, &QSlider::valueChanged, this, &MainWindow::applyMute);

	return panel;
}

// Errors stay visible in the status bar (it appears for them) and go to
// stderr.
void MainWindow::reportError(const QString &msg)
{
	qWarning().noquote() << msg;
	statusBar()->showMessage(msg);
}

// Muted, the sync monitor keeps running at volume 0: it is the clock the
// picture follows. The analog path is the card's loop-through: off.
void MainWindow::applyMute()
{
	const bool muted = m_mute->isChecked();

	m_mute->setIcon(QIcon::fromTheme(muted ? QStringLiteral("audio-volume-muted")
					       : QStringLiteral("audio-volume-high")));
	m_mixer.setLoopThrough(m_monitor->currentIndex() == MonitorAnalog && !muted);
	if (m_monitorThread)
		m_monitorThread->setVolume(muted ? 0.0f : m_volume->value() / 100.0f);
}

// Frames the recording had to fill in (the previous one repeated): lost
// by the driver or dropped from the recorder's queue.
void MainWindow::showLost(quint32 lost)
{
	if (lost == 0) {
		m_lost->setText(tr("No frames lost"));
		m_lost->setStyleSheet(QString());
	} else {
		m_lost->setText(tr("Frames lost: %1").arg(lost));
		m_lost->setStyleSheet(QStringLiteral("color: red; font-weight: bold"));
	}
}

void MainWindow::updateGainLabel(int steps)
{
	m_gainLabel->setText(tr("+%1 dB").arg(steps * 1.5, 0, 'f', 1));
}

void MainWindow::startCapture()
{
	const std::string videoDev = dc30::videoNode(), metaDev = dc30::metaNode();

	if (videoDev.empty() || metaDev.empty()) {
		m_preview->setMessage(tr("No DC30 device found.\n"
					 "Is the dc30 module loaded?"));
		return;
	}
	if (!m_mixer.ok())
		reportError(tr("DC30 mixer not found"));

	/* The input first: in video lock the audio clock comes from the
	 * input's line rate, and the decoder may still sit on another input
	 * (after loading the module: Composite) - the clock would jump
	 * under the running audio.
	 */
	startVideo();

	/* Audio before the metadata, so every frame's record carries a
	 * position.
	 */
	m_audio = std::make_unique<AudioCapture>();
	connect(m_audio.get(), &AudioCapture::levels, m_meter, &LevelMeter::setLevels);
	connect(m_audio.get(), &AudioCapture::failed, this,
		[this](const QString &msg) { reportError(msg); });
	/* Each one restarts the ALSA stream: a gap in the sound, and the
	 * sync monitor starts over.
	 */
	connect(m_audio.get(), &AudioCapture::xrun, this, [this] {
		reportError(tr("Audio overrun (xrun) no. %1").arg(++m_xruns));
	});
	m_audio->start();

	m_meta = std::make_unique<MetaCapture>(metaDev);
	connect(m_meta.get(), &MetaCapture::syncInfo, this,
		[this](double apf, double scatter, int clock, quint32 dropped,
		       quint32, quint32) {
			m_statSync->setText(
				apf > 0 ? tr("Audio/frame %1 (%2), scatter %3 · lost %4")
						  .arg(apf, 0, 'f', 2)
						  .arg(clock == DC30_META_CLOCK_VIDEOLOCK
							       ? tr("video lock") : tr("crystal"))
						  .arg(scatter, 0, 'f', 1)
						  .arg(dropped)
					: tr("Audio/frame: no assignment yet"));
		});
	connect(m_meta.get(), &MetaCapture::failed, this,
		[this](const QString &msg) { reportError(msg); });
	m_meta->start();
	m_video->start();

	setMonitorMode(m_monitor->currentIndex());
}

bool MainWindow::mjpeg() const
{
	return m_codec->currentIndex() == 1;
}

// The video stream in the selected codec's format, on the selected input.
void MainWindow::startVideo()
{
	m_video = std::make_unique<VideoCapture>(
		dc30::videoNode(),
		mjpeg() ? VideoCapture::Format::Mjpeg : VideoCapture::Format::Yuyv,
		m_rate->value());
	m_video->setInput(m_input->currentIndex());
	/* With pairing by the picture, the driver must not re-pair the
	 * codec's frames by the field flag first.
	 */
	m_video->setPairByFlag(!m_fieldRepair->isChecked());

	connect(m_video.get(), &VideoCapture::frameReady, m_preview,
		&PreviewWidget::setFrame);
	connect(m_video.get(), &VideoCapture::failed, this,
		[this](const QString &msg) {
			m_preview->setMessage(msg);
			reportError(msg);
		});
	connect(m_video.get(), &VideoCapture::statsChanged, this,
		[this](quint64 frames, quint64 lost) {
			m_statVideo->setText(tr("Frames %1 · gaps %2 · preview skipped %3")
						     .arg(frames).arg(lost).arg(m_preview->skipped()));
		});
}

// New format or data rate: the video stream starts over (not while
// recording - the controls are off then). Sound and metadata go on; the
// sequence numbers start over, so the sync monitor's fit too.
void MainWindow::restartVideo()
{
	if (!m_video || m_recorder)
		return;
	m_video.reset();
	startVideo();
	if (m_meta)
		m_meta->resetFit();
	m_video->start();
}

bool MainWindow::setCodec(const QString &name, int kbps)
{
	const int index = name == QLatin1String("ffv1")	 ? 0
			  : name == QLatin1String("mjpeg") ? 1
							   : -1;

	if (index < 0)
		return false;
	if (kbps > 0)
		m_rate->setValue(kbps);
	m_codec->setCurrentIndex(index);
	return true;
}

// With the sync monitor running, a frame is due once the sound has
// reached its audio position - plus the offset: positive delays the
// sound, i.e. brings the picture forward.
bool MainWindow::frameDue(uint32_t sequence, int64_t nowNs) const
{
	double pos, audible;
	uint32_t epoch;
	unsigned generation;

	if (!m_monitorThread || !m_meta ||
	    !m_meta->audioPosition(sequence, &pos, &epoch) ||
	    !m_monitorThread->audiblePosition(nowNs, &audible, &generation))
		return true;
	/* Positions of another ALSA stream (restart): don't hold the picture. */
	if (std::fabs(pos - audible) > 2.0 * AudioCapture::kRate)
		return true;
	return pos <= audible + m_offsetMs * 1e-3 * AudioCapture::kRate;
}

void MainWindow::setMonitorMode(int mode)
{
	m_preview->setPresentationClock({});
	m_monitorThread.reset();
	m_mixer.setLoopThrough(mode == MonitorAnalog && !m_mute->isChecked());
	m_volume->setEnabled(mode == MonitorSync);
	m_mute->setEnabled(mode != MonitorOff);
	m_offset->setEnabled(mode == MonitorSync);

	if (mode != MonitorSync || !m_audio || !m_meta) {
		m_statMonitor->setText(mode == MonitorAnalog ? tr("Monitoring: analog")
							     : QString());
		return;
	}
	m_monitorThread = std::make_unique<AudioMonitor>(m_audio.get());
	m_monitorThread->setVolume(m_mute->isChecked() ? 0.0f
						       : m_volume->value() / 100.0f);
	connect(m_monitorThread.get(), &AudioMonitor::syncState, this,
		[this](double latencyMs, double ppm) {
			m_statMonitor->setText(tr("Monitoring: picture and sound delayed %1 ms, clock %2 ppm")
						       .arg(latencyMs, 0, 'f', 0)
						       .arg(ppm, 0, 'f', 0));
		});
	connect(m_monitorThread.get(), &AudioMonitor::failed, this,
		[this](const QString &msg) { reportError(msg); });
	m_monitorThread->start();
	m_preview->setPresentationClock([this](uint32_t seq, int64_t now) {
		return frameDue(seq, now);
	});
}

// Free space for the recording, -1 if unknown. statvfs() on the folder
// itself: QStorageInfo asks the mount's root, and the root of a GVFS
// mount (SMB shares from the file manager, /run/user/<uid>/gvfs) always
// reports 0 - only the share's own folder has the real numbers.
qint64 MainWindow::freeBytes(const QString &dir)
{
	struct statvfs st;

	if (statvfs(QFile::encodeName(dir).constData(), &st) < 0)
		return -1;
	return qint64(st.f_bavail) * qint64(st.f_frsize);
}

void MainWindow::startRecording()
{
	if (!m_video || !m_audio || !m_meta) {
		reportError(tr("Cannot record: no card"));
		m_record->setChecked(false);
		return;
	}
	QDir dir(m_dir->text());
	if (m_dir->text().isEmpty() || !dir.mkpath(QStringLiteral("."))) {
		reportError(tr("Cannot create folder %1").arg(m_dir->text()));
		m_record->setChecked(false);
		return;
	}
	const QString path = dir.absoluteFilePath(
		QStringLiteral("dc30_%1.mkv")
			.arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"))));

	m_freeAge = 0;
	m_recorder = std::make_unique<Recorder>(path, m_input->currentText(),
						mjpeg() ? Recorder::Codec::Mjpeg
							: Recorder::Codec::Ffv1,
						m_fieldRepair->isChecked());
	/* On a write error the recorder closes the file and ends by itself. */
	connect(m_recorder.get(), &Recorder::failed, this,
		[this](const QString &msg) { reportError(msg); });
	connect(m_recorder.get(), &Recorder::progress, this,
		[this](double seconds, qint64 bytes, quint32 lost,
		       quint32 fixes, int queued) {
			const qint64 s = qint64(seconds);
			showLost(lost);
			const double rate = seconds > 0 ? bytes / seconds : 0;
			/* Network shares answer slowly: ask every 10 s only. */
			if (m_freeAge-- <= 0) {
				m_freeBytes = freeBytes(m_dir->text());
				m_freeAge = 10;
			}
			/* The file's rate holds the sound too (PCM, 176.4 kB/s):
			 * the picture's alone is what the MJPEG data rate sets.
			 */
			const double audioRate = 44100.0 * 4;
			QString text = tr("%1:%2:%3 · %4 GB · %5 MB/s (picture %6)")
					       .arg(s / 3600)
					       .arg(s / 60 % 60, 2, 10, QLatin1Char('0'))
					       .arg(s % 60, 2, 10, QLatin1Char('0'))
					       .arg(bytes / 1e9, 0, 'f', 2)
					       .arg(rate / 1e6, 0, 'f', 1)
					       .arg(std::max(rate - audioRate, 0.0) / 1e6, 0, 'f', 1);

			if (rate > 0 && m_freeBytes >= 0)
				text += tr("\nfree %1 GB (≈ %2 h)")
						.arg(m_freeBytes / 1e9, 0, 'f', 0)
						.arg(m_freeBytes / rate / 3600, 0, 'f', 1);
			text += tr("\naudio realigned %1 · queue %2")
					.arg(fixes).arg(queued);
			m_recStatus->setText(text);
		});
	connect(m_recorder.get(), &Recorder::summary, this,
		[this](const QString &text) {
			/* Not in the status bar: that is for errors. */
			qInfo().noquote() << text;
			m_recStatus->setText(text);
		});
	connect(m_recorder.get(), &QThread::finished, this, &MainWindow::recordingEnded);
	m_recorder->start();

	/* Metadata and sound first: the start frame is chosen by the sound
	 * positions.
	 */
	m_meta->setRecorder(m_recorder.get());
	m_audio->setRecorder(m_recorder.get());
	m_video->setRecorder(m_recorder.get());

	m_dir->setEnabled(false);
	m_codec->setEnabled(false);
	m_rate->setEnabled(false);
	m_fieldRepair->setEnabled(false);
	m_record->setText(tr("Stop recording"));
	m_recStatus->setText(tr("Starting …\n%1").arg(path));
	showLost(0);
}

void MainWindow::recordFor(int seconds, const QString &dir)
{
	if (!dir.isEmpty())
		m_dir->setText(dir);
	QTimer::singleShot(2000, this, [this, seconds] {
		m_record->setChecked(true);
		startRecording();
		if (!m_recorder) {
			QApplication::exit(1);
			return;
		}
		connect(m_recorder.get(), &QThread::finished, qApp,
			&QApplication::quit, Qt::QueuedConnection);
		QTimer::singleShot(seconds * 1000, this, &MainWindow::stopRecording);
	});
}

// The recorder writes the last frames' sound, then ends (recordingEnded).
void MainWindow::stopRecording()
{
	if (!m_recorder)
		return;
	m_recorder->finish();
	m_record->setEnabled(false);
	m_record->setText(tr("Finishing …"));
}

void MainWindow::recordingEnded()
{
	if (!m_recorder)
		return;
	m_video->setRecorder(nullptr);
	m_audio->setRecorder(nullptr);
	m_meta->setRecorder(nullptr);
	m_recStatus->setText(m_recStatus->text() +
			     tr("\nLog: %1").arg(m_recorder->logPath()));
	showLost(m_recorder->lost());
	m_recorder.reset();

	m_dir->setEnabled(true);
	m_codec->setEnabled(true);
	m_rate->setEnabled(mjpeg());
	m_fieldRepair->setEnabled(true);
	m_record->setEnabled(true);
	m_record->setChecked(false);
	m_record->setText(tr("Start recording"));
}

void MainWindow::loadSettings()
{
	QSettings s;
	const QSignalBlocker b1(m_input), b2(m_source), b3(m_monitor),
		b4(m_codec), b5(m_rate);

	m_input->setCurrentIndex(s.value("video/input", 1).toInt());
	m_source->setCurrentIndex(m_mixer.ok() ? m_mixer.source() : 0);
	m_gain->setValue(m_mixer.ok() ? m_mixer.gain() : 0);
	updateGainLabel(m_gain->value());
	m_boost->setChecked(m_mixer.ok() && m_mixer.boost());
	m_monitor->setCurrentIndex(s.value("audio/monitor", int(MonitorSync)).toInt());
	m_volume->setValue(s.value("audio/volume", 80).toInt());
	m_offset->setValue(s.value("audio/offsetMs", 0).toInt());
	m_offsetMs = m_offset->value();
	m_offsetLabel->setText(tr("%1 ms").arg(m_offset->value()));
	m_dir->setText(s.value("record/dir",
		QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)).toString());
	m_codec->setCurrentIndex(s.value("record/codec").toString() ==
					 QLatin1String("mjpeg") ? 1 : 0);
	m_rate->setValue(s.value("record/mjpegKbps", 6000).toInt());
	m_rate->setEnabled(mjpeg());
	m_fieldRepair->setChecked(s.value("record/fieldRepair", true).toBool());
	m_details->setVisible(s.value("view/details", false).toBool());
}

void MainWindow::saveSettings()
{
	QSettings s;

	s.setValue("video/input", m_input->currentIndex());
	s.setValue("audio/monitor", m_monitor->currentIndex());
	s.setValue("audio/volume", m_volume->value());
	s.setValue("audio/offsetMs", m_offset->value());
	s.setValue("record/dir", m_dir->text());
	s.setValue("record/codec", mjpeg() ? "mjpeg" : "ffv1");
	s.setValue("record/mjpegKbps", m_rate->value());
	s.setValue("record/fieldRepair", m_fieldRepair->isChecked());
	/* isVisible() is false once the window is closed. */
	s.setValue("view/details", !m_details->isHidden());
}

// SPDX-License-Identifier: GPL-2.0-only
// Live picture, scaled to 4:3.
//
// Frames wait in a short queue until they are due: with a presentation
// clock set (the sync monitor's audible position), a frame goes on screen
// when the sound has reached it, so picture and sound leave the PC
// together. Without one, each frame shows as soon as it arrives.
#pragma once

#include <QImage>
#include <QTimer>
#include <QWidget>
#include <cstdint>
#include <deque>
#include <functional>

class PreviewWidget : public QWidget {
	Q_OBJECT
public:
	// True if frame 'sequence' is due at 'nowNs' (CLOCK_MONOTONIC).
	using DueFn = std::function<bool(uint32_t sequence, int64_t nowNs)>;

	explicit PreviewWidget(QWidget *parent = nullptr);

	QSize sizeHint() const override { return {768, 576}; }

	// Empty function: show frames at once.
	void setPresentationClock(DueFn due);
	// Frames dropped because a later one was due at the same time.
	quint64 skipped() const { return m_skipped; }

public slots:
	void setFrame(const QImage &image, quint32 sequence, qint64 timestampNs);
	void setMessage(const QString &text);

protected:
	void paintEvent(QPaintEvent *) override;

private:
	struct Frame {
		QImage image;
		uint32_t sequence;
	};

	void present();

	std::deque<Frame> m_queue;
	DueFn m_due;
	QTimer m_timer;
	QImage m_image;
	QString m_message;
	quint64 m_skipped = 0;
};

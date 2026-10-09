// SPDX-License-Identifier: GPL-2.0-only
#include "levelmeter.h"

#include <QDateTime>
#include <QPainter>
#include <algorithm>
#include <cmath>

namespace {

constexpr qint64 kHoldMs = 1500;
constexpr float kFallDbPerUpdate = 1.0f;	// ~20 dB/s at 20 updates/s

float toDb(float lin)
{
	return lin > 0 ? 20.0f * std::log10(lin) : -1000.0f;
}

} // namespace

LevelMeter::LevelMeter(QWidget *parent) : QWidget(parent)
{
	setMinimumSize(70, 160);
	setToolTip(tr("Level in dBFS. Click to reset the clip indicator."));
}

void LevelMeter::setLevels(float peakL, float peakR, float rmsL, float rmsR,
			   bool clip)
{
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	const float peaks[2] = {peakL, peakR}, rms[2] = {rmsL, rmsR};

	for (int i = 0; i < 2; i++) {
		Channel &c = m_ch[i];

		c.peakDb = std::max(kFloorDb, toDb(peaks[i]));
		c.rmsDb = std::max(kFloorDb, toDb(rms[i]));
		if (c.peakDb >= c.holdDb) {
			c.holdDb = c.peakDb;
			c.holdUntil = now + kHoldMs;
		} else if (now > c.holdUntil) {
			c.holdDb = std::max(c.peakDb, c.holdDb - kFallDbPerUpdate);
		}
	}
	if (clip)
		m_clip = true;
	update();
}

void LevelMeter::setHardwareClip()
{
	m_clip = true;
	update();
}

void LevelMeter::resetClip()
{
	m_clip = false;
	update();
}

void LevelMeter::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	const int w = width(), h = height();
	const int scaleW = 28, clipH = 16, gap = 4;
	const int barW = (w - scaleW - 3 * gap) / 2;
	const int top = clipH + gap, bottom = h - 4;
	const int span = bottom - top;
	auto yOf = [&](float db) {
		return bottom - int(span * (db - kFloorDb) / -kFloorDb);
	};

	p.fillRect(rect(), palette().window());

	/* scale */
	p.setPen(palette().text().color());
	QFont f = font();
	f.setPointSizeF(f.pointSizeF() * 0.8);
	p.setFont(f);
	for (int db = 0; db >= int(kFloorDb); db -= 6) {
		int y = yOf(float(db));

		p.drawLine(scaleW - 4, y, scaleW, y);
		p.drawText(QRect(0, y - 7, scaleW - 6, 14),
			   Qt::AlignRight | Qt::AlignVCenter, QString::number(db));
	}

	for (int i = 0; i < 2; i++) {
		const Channel &c = m_ch[i];
		const int x = scaleW + gap + i * (barW + gap);
		QLinearGradient g(0, bottom, 0, top);

		g.setColorAt(0.0, QColor(40, 170, 60));
		g.setColorAt((-12 - kFloorDb) / -kFloorDb, QColor(40, 170, 60));
		g.setColorAt((-6 - kFloorDb) / -kFloorDb, QColor(220, 200, 40));
		g.setColorAt(1.0, QColor(220, 50, 40));

		p.fillRect(QRect(x, top, barW, span), QColor(30, 30, 30));
		/* peak: dimmed, rms: full */
		p.setOpacity(0.45);
		p.fillRect(QRect(x, yOf(c.peakDb), barW, bottom - yOf(c.peakDb)), g);
		p.setOpacity(1.0);
		p.fillRect(QRect(x, yOf(c.rmsDb), barW, bottom - yOf(c.rmsDb)), g);
		/* peak hold */
		p.fillRect(QRect(x, yOf(c.holdDb) - 1, barW, 2), Qt::white);

		p.fillRect(QRect(x, 0, barW, clipH),
			   m_clip ? QColor(230, 30, 30) : QColor(70, 20, 20));
		p.drawText(QRect(x, 0, barW, clipH), Qt::AlignCenter,
			   i ? QStringLiteral("R") : QStringLiteral("L"));
	}
}

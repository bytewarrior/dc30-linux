// SPDX-License-Identifier: GPL-2.0-only
// Stereo level meter: rms bar, peak bar with peak hold, clip indicators
// that stay lit until clicked.
#pragma once

#include <QWidget>
#include <array>

class LevelMeter : public QWidget {
	Q_OBJECT
public:
	explicit LevelMeter(QWidget *parent = nullptr);

	QSize sizeHint() const override { return {90, 240}; }

public slots:
	// Linear 0..1 of full scale; clip = a full-scale sample was seen.
	void setLevels(float peakL, float peakR, float rmsL, float rmsR, bool clip);
	// The codec's own overrange detector fired (either channel).
	void setHardwareClip();
	void resetClip();

protected:
	void paintEvent(QPaintEvent *) override;
	void mousePressEvent(QMouseEvent *) override { resetClip(); }

private:
	static constexpr float kFloorDb = -60.0f;

	struct Channel {
		float peakDb = kFloorDb, rmsDb = kFloorDb;
		float holdDb = kFloorDb;
		qint64 holdUntil = 0;
	};
	std::array<Channel, 2> m_ch;
	bool m_clip = false;
};

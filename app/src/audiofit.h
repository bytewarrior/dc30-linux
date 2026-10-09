// SPDX-License-Identifier: GPL-2.0-only
// Sliding least-squares fit of the audio position over the video frame
// sequence number, for one ALSA stream (epoch) at a time. Single metadata
// values jitter by a few audio frames (interrupt latency); the fit gives
// the smoothed position of any recent frame. Not thread-safe.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>

namespace dc30 {

class AudioFit {
public:
	explicit AudioFit(size_t window = 500, size_t minPoints = 25)
		: m_window(window), m_minPoints(minPoints)
	{
	}

	void reset()
	{
		m_points.clear();
		m_have = false;
	}

	// A new epoch, a sequence number going back or a gap in the sequence
	// starts the fit over. Across a gap the sound may have run at another
	// rate (video lock during a picture search): a straight line over
	// both sides would take the whole window to settle and overshoot.
	// 'step': the frames moved against the sequence without a gap (the
	// driver changed the field pairing, MJPEG), start over too.
	// Refits over the whole window on every point - 500 points, 25 times
	// a second, is cheap.
	void add(uint32_t seq, double pos, uint32_t epoch, bool step = false)
	{
		if (epoch != m_epoch || step ||
		    (!m_points.empty() && seq != m_points.back().seq + 1)) {
			reset();
			m_epoch = epoch;
		}
		m_points.push_back({seq, pos});
		if (m_points.size() > m_window)
			m_points.pop_front();
		if (m_points.size() < m_minPoints)
			return;

		const uint32_t s0 = m_points.front().seq;
		const double y0 = m_points.front().pos;
		double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0;

		for (const auto &p : m_points) {
			double x = double(p.seq - s0), y = p.pos - y0;

			n++;
			sx += x;
			sy += y;
			sxx += x * x;
			sxy += x * y;
		}
		const double d = n * sxx - sx * sx;
		if (d == 0)
			return;
		const double b = (n * sxy - sx * sy) / d;
		const double a = (sy - b * sx) / n;
		double ss = 0;

		for (const auto &p : m_points) {
			double r = (p.pos - y0) - (a + b * double(p.seq - s0));

			ss += r * r;
		}
		m_seq0 = s0;
		m_intercept = y0 + a;
		m_slope = b;
		m_scatter = std::sqrt(ss / n);
		m_have = true;
	}

	bool valid() const { return m_have; }
	uint32_t epoch() const { return m_epoch; }
	double slope() const { return m_slope; }
	double scatter() const { return m_scatter; }
	// Audio frame index in the epoch's stream for frame 'seq'.
	double position(uint32_t seq) const
	{
		return m_intercept + m_slope * (double(seq) - double(m_seq0));
	}

private:
	struct Point {
		uint32_t seq;
		double pos;
	};

	size_t m_window, m_minPoints;
	std::deque<Point> m_points;
	uint32_t m_epoch = 0;
	bool m_have = false;
	double m_slope = 0, m_intercept = 0, m_scatter = 0;
	uint32_t m_seq0 = 0;	// the fit is pos = intercept + slope * (seq - seq0)
};

} // namespace dc30

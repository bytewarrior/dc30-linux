// SPDX-License-Identifier: GPL-2.0-only
#include "fieldaligner.h"

#include <algorithm>
#include <cmath>

namespace dc30 {

namespace {

// Field lines left out at the top and bottom: the first lines of the
// window and the head switching noise at the bottom.
constexpr int kCropTop = 8;
constexpr int kCropBottom = 12;
// Lines kept out of the comparison at either end, room for the shifts.
constexpr int kMargin = 8;
// Shifts tried, in half field lines.
constexpr int kMaxHalfShift = 6;
// |t| within 1 +- kDecisive counts.
constexpr double kDecisive = 0.4;

int64_t alt(int64_t time)
{
	return (time & 1) ? -1 : 1;
}

} // namespace

double FieldAligner::vshift(const std::vector<float> &a,
			    const std::vector<float> &b, int width, int lines,
			    double *conf)
{
	const int rows = lines - 2 * kMargin;
	double err[2 * kMaxHalfShift + 1];

	for (int j = 0; j <= 2 * kMaxHalfShift; j++) {
		const int h = j - kMaxHalfShift;	// half lines
		const int k = h >= 0 ? h / 2 : -((1 - h) / 2);	// floor(h / 2)
		const bool half = h & 1;
		double sum = 0;

		for (int r = kMargin; r < kMargin + rows; r++) {
			const float *pa = &a[size_t(r) * width];
			const float *pb = &b[size_t(r + k) * width];

			/* Per line in float, which the compiler vectorizes. */
			float line = 0;

			if (half) {
				const float *pc = pb + width;

				for (int x = 0; x < width; x++)
					line += std::fabs(pa[x] - (pb[x] + pc[x]) * 0.5f);
			} else {
				for (int x = 0; x < width; x++)
					line += std::fabs(pa[x] - pb[x]);
			}
			sum += line;
		}
		err[j] = sum / (double(rows) * width);
	}

	int best = 0;
	double mean = 0;

	for (int j = 0; j <= 2 * kMaxHalfShift; j++) {
		mean += err[j];
		if (err[j] < err[best])
			best = j;
	}
	mean /= 2 * kMaxHalfShift + 1;

	double dy = (best - kMaxHalfShift) * 0.5;

	if (best > 0 && best < 2 * kMaxHalfShift) {
		const double l = err[best - 1], m = err[best], r = err[best + 1];
		const double den = l - 2 * m + r;

		if (den > 0)
			dy += 0.25 * (l - r) / den;
	}
	if (conf)
		*conf = mean > 1e-3 ? (mean - err[best]) / mean : 0;
	return dy;
}

void FieldAligner::push(int64_t time, uint64_t id, const uint8_t *luma,
			int width, int lines, int stride)
{
	Entry e;

	e.time = time;
	e.id = id;
	e.width = width / 2;
	e.lines = std::max(lines - kCropTop - kCropBottom, 0);
	e.luma.resize(size_t(e.width) * e.lines);
	for (int y = 0; y < e.lines; y++) {
		const uint8_t *s = luma + size_t(y + kCropTop) * stride;
		float *d = &e.luma[size_t(y) * e.width];

		for (int x = 0; x < e.width; x++)
			d[x] = s[2 * x];
	}
	m_stats.fields++;

	if (!m_q.empty()) {
		Entry &p = m_q.back();

		if (p.time == time - 1 && p.width == e.width &&
		    p.lines == e.lines && e.lines > 2 * kMargin + 2) {
			p.dy = vshift(p.luma, e.luma, e.width, e.lines);
			p.haveDy = true;
		}
	}
	m_q.push_back(std::move(e));
	measure();
	decide(false);
}

void FieldAligner::finish()
{
	m_measured = m_q.size();
	decide(true);
	if (m_haveTop) {
		m_out.push_back({m_topTime, m_topId, m_topId, true});
		m_stats.singles++;
		m_stats.frames++;
		m_haveTop = false;
	}
}

bool FieldAligner::pop(Frame &out)
{
	if (m_out.empty())
		return false;
	out = m_out.front();
	m_out.pop_front();
	return true;
}

// q of every field whose dy after it is known: all but the newest.
void FieldAligner::measure()
{
	const size_t n = m_q.size();

	for (size_t j = m_measured; j + 1 < n; j++) {
		Entry &e = m_q[j];

		if (j > 0 && m_q[j - 1].haveDy && e.haveDy) {
			const double t = m_q[j - 1].dy - e.dy;

			e.q = t * double(alt(e.time));
			e.valid = std::fabs(std::fabs(t) - 1.0) < kDecisive;
			if (e.valid)
				m_stats.decisive++;
		}
	}
	if (n > 0)
		m_measured = std::max(m_measured, n - 1);
}

// Whether the decisive fields from m on start a phase of 'sign': the
// first kRun of them within kLookahead agree.
bool FieldAligner::confirms(size_t m, int sign) const
{
	int agree = 0;

	for (size_t i = m; i < m_measured && i <= m + kLookahead; i++) {
		if (!m_q[i].valid)
			continue;
		if ((m_q[i].q > 0 ? 1 : -1) != sign)
			return false;
		if (++agree == kRun)
			return true;
	}
	return false;
}

void FieldAligner::decide(bool all)
{
	while (m_decided < m_q.size()) {
		const size_t j = m_decided;

		if (!all && j + 2 * kLookahead >= m_measured)
			break;

		const Entry &e = m_q[j];

		if (m_flipIn > 0 && --m_flipIn == 0) {
			m_phase = -m_phase;
		} else if (m_flipIn == 0 && e.valid) {
			/* A decisive field against the phase starts a new one
			 * if the next decisive fields agree.
			 */
			const int sign = e.q > 0 ? 1 : -1;

			if (sign != m_phase && confirms(j, sign))
				m_phase = sign;
		} else if (m_flipIn == 0) {
			/* Not decisive - as the fields where the grid slips.
			 * If the next decisive field starts a new phase, the
			 * undecided ones between are split: the first half
			 * keeps the old phase, the rest takes the new one.
			 */
			size_t m = j + 1;

			while (m < m_measured && m <= j + kLookahead && !m_q[m].valid)
				m++;
			if (m < m_measured && m <= j + kLookahead) {
				const int sign = m_q[m].q > 0 ? 1 : -1;

				if (sign != m_phase && confirms(m, sign))
					m_flipIn = int((m - j + 1) / 2);
			}
		}
		if (!e.valid)
			m_stats.held++;

		pair(e, m_phase * alt(e.time) > 0);
		m_decided++;

		/* The field before the oldest undecided one is still needed
		 * for its q.
		 */
		while (m_decided > 1) {
			m_q.pop_front();
			m_decided--;
			m_measured--;
		}
	}
}

void FieldAligner::pair(const Entry &e, bool top)
{
	if (m_haveLast && m_lastTime == e.time - 1 && m_lastTop == top)
		m_stats.jumps++;
	m_haveLast = true;
	m_lastTime = e.time;
	m_lastTop = top;

	if (!top && m_haveTop && m_topTime == e.time - 1) {
		m_out.push_back({m_topTime, m_topId, e.id, false});
		m_stats.frames++;
		m_haveTop = false;
		return;
	}
	if (m_haveTop) {
		m_out.push_back({m_topTime, m_topId, m_topId, true});
		m_stats.singles++;
		m_stats.frames++;
		m_haveTop = false;
	}
	if (top) {
		m_haveTop = true;
		m_topTime = e.time;
		m_topId = e.id;
	} else {
		m_out.push_back({e.time, e.id, e.id, true});
		m_stats.singles++;
		m_stats.frames++;
	}
}

} // namespace dc30

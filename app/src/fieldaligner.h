// SPDX-License-Identifier: GPL-2.0-only
// Pairs the fields of an interlaced recording by their true parity, taken
// from the picture instead of the decoder's field flag.
//
// Neither field flag of the VPX3220 is reliable on a VCR without TBC:
// toggling, it misses field jumps and leaves whole scenes swapped;
// following the input, it misreads the odd/even in some scenes every few
// frames. The picture tells: the vertical offset dy between consecutive
// fields is motion - 0.5 field lines from a top to a bottom field and
// motion + 0.5 from a bottom to a top one, so per field
//   t = dy(before) - dy(after)
// is +1 for a top field and -1 for a bottom one, whatever the vertical
// motion (checked by eye on a difficult tape).
// Along an undisturbed tape the parity alternates with the field time:
// q = t * (-1)^time keeps its sign. Where it changes, the field grid of
// the signal slipped by half a line - at cuts, and on worn tape every few
// frames in some scenes (a worn tape: ~150 slips in 5 min, swapped
// stretches of 5-8 frames; a median over a second missed them). The
// phase changes where kRun decisive fields in a row show the other sign;
// fields that are not decisive (titles, black, blurred pans, the slipping
// field itself) keep the current one.
//
// Fields go in with their time on the 20 ms field grid of the input
// (gaps allowed) and come out as frames: a top field with the bottom
// field right after it, or a single field where a jump leaves one alone.
// Output lags 2 * kLookahead fields behind the input. Not thread-safe.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace dc30 {

class FieldAligner {
public:
	// Decisive fields in a row that change the phase, and how far ahead
	// a decision looks for them.
	static constexpr int kRun = 3;
	static constexpr int kLookahead = 12;

	struct Frame {
		int64_t time;		// field time of the first (top) field
		uint64_t top;		// caller's ids
		uint64_t bottom;	// == top for a single field
		bool single;
	};

	struct Stats {
		uint64_t fields = 0;
		uint64_t decisive = 0;	// |t| close to 1
		uint64_t held = 0;	// no decisive field in the window
		uint64_t jumps = 0;	// parity repeated: a field jump
		uint64_t frames = 0;
		uint64_t singles = 0;
	};

	// A field: its time on the field grid (strictly increasing), the
	// caller's id, and its luma (width x lines, stride bytes per line).
	void push(int64_t time, uint64_t id, const uint8_t *luma, int width,
		  int lines, int stride);
	// No more fields: decide the rest.
	void finish();
	// The next frame, if one is ready.
	bool pop(Frame &out);

	const Stats &stats() const { return m_stats; }

	// Vertical offset of b against a in field lines (b(r + dy) ~ a(r)),
	// and how clear the minimum is. Exposed for the tests.
	static double vshift(const std::vector<float> &a,
			     const std::vector<float> &b, int width, int lines,
			     double *conf = nullptr);

private:
	struct Entry {
		int64_t time;
		uint64_t id;
		std::vector<float> luma;	// every 2nd column, as float
		int width = 0, lines = 0;
		bool haveDy = false;	// dy to the next field (consecutive)
		double dy = 0;
		bool valid = false;	// q is decisive
		double q = 0;
	};

	void measure();
	bool confirms(size_t m, int sign) const;
	void decide(bool all);
	void pair(const Entry &e, bool top);

	std::deque<Entry> m_q;		// fields not decided yet, and the one before
	size_t m_measured = 0;		// m_q[0..m_measured) have their q
	size_t m_decided = 0;		// m_q[0..m_decided) are decided
	int m_phase = 1;		// current decision: sign of q
	int m_flipIn = 0;		// fields until a decided phase change
	// Pairing: a top field waiting for its bottom field.
	bool m_haveTop = false;
	int64_t m_topTime = 0;
	uint64_t m_topId = 0;
	bool m_lastTop = false;		// parity of the previous field, for jumps
	bool m_haveLast = false;
	int64_t m_lastTime = 0;
	std::deque<Frame> m_out;
	Stats m_stats;
};

} // namespace dc30

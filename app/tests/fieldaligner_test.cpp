// SPDX-License-Identifier: GPL-2.0-only
// FieldAligner on synthetic field sequences: a texture sampled at the
// rows of a top or bottom field, moving vertically, with field jumps and
// a stretch without vertical detail.
#include "fieldaligner.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr int kW = 768, kLines = 288;

// Field of parity 'bottom' at field time n: frame row y = 2 * l + bottom,
// content moved down by 'motion' frame rows per field. 'flat': no
// vertical detail.
std::vector<uint8_t> field(int n, bool bottom, double motion, bool flat)
{
	std::vector<uint8_t> f(size_t(kW) * kLines);

	for (int l = 0; l < kLines; l++) {
		const double y = 2 * l + bottom - motion * n;

		for (int x = 0; x < kW; x++) {
			double v = 128 + 50 * std::sin(x / 23.0);

			if (!flat)
				v += 40 * std::sin(y / 3.1 + x / 41.0) +
				     25 * std::sin(y / 1.7 - x / 13.0);
			f[size_t(l) * kW + x] = uint8_t(std::lround(std::fmin(std::fmax(v, 0), 255)));
		}
	}
	return f;
}

struct Case {
	const char *name;
	double motion;
	std::vector<bool> parity;	// true = bottom, per field time
	int flatFrom = -1, flatTo = -1;
	int expectSingles;		// -1: not checked
};

std::vector<bool> alternating(int n, bool startBottom)
{
	std::vector<bool> p(n);

	for (int i = 0; i < n; i++)
		p[i] = (i & 1) != startBottom;
	return p;
}

// Jumps: at each listed field time the parity repeats the one before.
std::vector<bool> withJumps(int n, std::vector<int> jumps)
{
	std::vector<bool> p(n);
	bool cur = false;
	size_t k = 0;

	for (int i = 0; i < n; i++) {
		if (i > 0) {
			if (k < jumps.size() && jumps[k] == i)
				k++;
			else
				cur = !cur;
		}
		p[i] = cur;
	}
	return p;
}

bool run(const Case &c)
{
	dc30::FieldAligner al;
	std::vector<dc30::FieldAligner::Frame> out;
	dc30::FieldAligner::Frame f;
	const int n = int(c.parity.size());

	for (int i = 0; i < n; i++) {
		const bool flat = i >= c.flatFrom && i < c.flatTo;
		const auto pic = field(i, c.parity[i], c.motion, flat);

		al.push(i, uint64_t(i), pic.data(), kW, kLines, kW);
		while (al.pop(f))
			out.push_back(f);
	}
	al.finish();
	while (al.pop(f))
		out.push_back(f);

	int wrong = 0, singles = 0, fields = 0;

	for (const auto &fr : out) {
		if (fr.single) {
			singles++;
			fields++;
			continue;
		}
		fields += 2;
		// Without vertical detail the pairing cannot be told - and
		// does not show either.
		if (int(fr.top) >= c.flatFrom && int(fr.top) < c.flatTo)
			continue;
		if (c.parity[fr.top] || !c.parity[fr.bottom] ||
		    fr.bottom != fr.top + 1)
			wrong++;
	}
	const bool ok = wrong == 0 && fields == n &&
			(c.expectSingles < 0 || singles == c.expectSingles);

	std::printf("%-28s frames %3zu wrong %d singles %d (want %d) fields %d/%d  %s\n",
		    c.name, out.size(), wrong, singles, c.expectSingles, fields, n,
		    ok ? "ok" : "FAIL");
	return ok;
}

} // namespace

int main()
{
	const Case cases[] = {
		{"static, top first", 0.0, alternating(200, false), -1, -1, 0},
		{"static, bottom first", 0.0, alternating(200, true), -1, -1, 2},
		{"moving 1.4 rows/field", 1.4, alternating(200, false), -1, -1, 0},
		{"moving -0.9 rows/field", -0.9, alternating(200, false), -1, -1, 0},
		{"two jumps", 0.6, withJumps(300, {101, 202}), -1, -1, 2},
		{"jump inside flat stretch", 0.0, withJumps(300, {150}), 120, 180, 2},
		/* As on a worn tape: the grid slips every 12-16 fields. */
		{"slips every 12-16 fields", -0.7,
		 withJumps(400, {40, 53, 68, 80, 95, 108, 124, 137, 150, 166, 300}),
		 -1, -1, -1},
	};
	bool ok = true;

	for (const auto &c : cases)
		ok = run(c) && ok;
	return ok ? 0 : 1;
}

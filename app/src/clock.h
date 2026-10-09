// SPDX-License-Identifier: GPL-2.0-only
// CLOCK_MONOTONIC in ns - the clock of the driver's V4L2 timestamps.
#pragma once

#include <cstdint>
#include <ctime>

namespace dc30 {

inline int64_t monotonicNs()
{
	timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}

} // namespace dc30

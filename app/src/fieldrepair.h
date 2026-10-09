// SPDX-License-Identifier: GPL-2.0-only
// Field pairing by the picture while recording (dc30-capture, "pair
// fields by the picture"): the recorder's frames go in with their place k
// on the recording's time line, frames paired by FieldAligner come out,
// each with the time of its first field (fields 2k and 2k + 1, 20 ms
// apart) - the same as dc30-capture --fix does afterwards. Not
// thread-safe; lags about half a second behind.
#pragma once

#include "fieldaligner.h"
#include "fieldutil.h"

#include <cstdint>
#include <deque>
#include <map>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace dc30 {

class FieldRepair {
public:
	struct Out {
		int64_t time;		// first field, in fields
		int64_t duration;	// 2, or 1 for a single field
		AVPacket *pkt;		// MJPEG: both field JPEGs
		AVFrame *frame;		// FFV1: planar 4:2:2
		bool single;
		bool crossed;		// paired across the recorded frames
	};

	explicit FieldRepair(bool mjpeg);
	~FieldRepair();
	FieldRepair(const FieldRepair &) = delete;
	FieldRepair &operator=(const FieldRepair &) = delete;

	// Frame k; takes over pkt (MJPEG) or frame (FFV1, planar 4:2:2).
	// False: it could not be split or decoded, and was dropped.
	bool push(int64_t k, AVPacket *pkt, AVFrame *frame);
	void finish();
	// The caller owns out.pkt / out.frame.
	bool pop(Out &out);

	const FieldAligner::Stats &stats() const { return m_al.stats(); }

private:
	struct Src {
		AVPacket *pkt = nullptr;
		AVFrame *frame = nullptr;
		FieldJpegs jpegs;
	};

	bool pushJpeg(const uint8_t *d, size_t n, int64_t time, uint64_t id);
	void collect();
	void release(int64_t below);

	const bool m_mjpeg;
	AVCodecContext *m_dec = nullptr;	// MJPEG, luma only
	AVPacket *m_jp = nullptr;
	AVFrame *m_luma = nullptr;
	FieldAligner m_al;
	std::map<int64_t, Src> m_src;		// frames with fields not out yet
	std::deque<Out> m_out;
};

} // namespace dc30

// SPDX-License-Identifier: GPL-2.0-only
// Fields of the DC30's frames, for FieldAligner's users: the two field
// JPEGs of an MJPEG frame (one complete JPEG per field, the second right
// after the first one's EOI), and the lines of a planar 4:2:2 frame.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

extern "C" {
#include <libavutil/frame.h>
}

namespace dc30 {

// Length of the JPEG at d through its EOI, 0 if none: marker segments up
// to SOS, then entropy-coded data, where the ZR36050 stuffs 0xFF with 0x00
// and writes no RST markers (as the driver's dc30_jpeg_end()).
inline size_t jpegEnd(const uint8_t *d, size_t n)
{
	size_t i = 2;
	uint8_t marker;

	if (n < 4 || d[0] != 0xff || d[1] != 0xd8)
		return 0;
	do {
		if (i + 4 > n || d[i] != 0xff)
			return 0;
		marker = d[i + 1];
		i += 2 + (size_t(d[i + 2]) << 8 | d[i + 3]);
	} while (marker != 0xda);
	for (; i + 1 < n; i++) {
		if (d[i] != 0xff || d[i + 1] == 0x00 || d[i + 1] == 0xff)
			continue;
		return d[i + 1] == 0xd9 ? i + 2 : 0;
	}
	return 0;
}

struct FieldJpegs {
	size_t len0 = 0, start1 = 0, len1 = 0;
};

inline bool splitFieldJpegs(const uint8_t *d, size_t n, FieldJpegs &s)
{
	s.len0 = jpegEnd(d, n);
	if (!s.len0)
		return false;
	s.start1 = s.len0;
	s.len1 = jpegEnd(d + s.start1, n - s.start1);
	return s.len1 != 0;
}

// Both JPEGs the same: a single field the driver sent alone.
inline bool sameFieldJpegs(const uint8_t *d, const FieldJpegs &s)
{
	return s.len0 == s.len1 && !std::memcmp(d, d + s.start1, s.len0);
}

// Planar frame whose two fields are the same lines doubled: a single field
// the driver sent alone.
inline bool sameFieldLines(const AVFrame *f)
{
	for (int y = 0; y + 1 < f->height; y += 2)
		if (std::memcmp(f->data[0] + size_t(y) * f->linesize[0],
				f->data[0] + size_t(y + 1) * f->linesize[0],
				size_t(f->width)))
			return false;
	return true;
}

// Planar 4:2:2 frame 'out' from two fields: the lines of parity topRow of
// topSrc, then those of parity botRow of botSrc (the same lines again for
// a single field).
inline void buildFieldFrame(AVFrame *out, const AVFrame *topSrc, int topRow,
			    const AVFrame *botSrc, int botRow)
{
	for (int p = 0; p < 3; p++) {
		const int w = p ? (out->width + 1) / 2 : out->width;

		for (int y = 0; y + 1 < out->height; y += 2) {
			std::memcpy(out->data[p] + size_t(y) * out->linesize[p],
				    topSrc->data[p] + size_t(y + topRow) * topSrc->linesize[p],
				    size_t(w));
			std::memcpy(out->data[p] + size_t(y + 1) * out->linesize[p],
				    botSrc->data[p] + size_t(y + botRow) * botSrc->linesize[p],
				    size_t(w));
		}
	}
}

} // namespace dc30

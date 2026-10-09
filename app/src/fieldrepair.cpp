// SPDX-License-Identifier: GPL-2.0-only
#include "fieldrepair.h"

#include <algorithm>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace dc30 {

FieldRepair::FieldRepair(bool mjpeg) : m_mjpeg(mjpeg)
{
	if (!m_mjpeg)
		return;
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);

	m_dec = codec ? avcodec_alloc_context3(codec) : nullptr;
	if (m_dec) {
		/* Luma only would do, but distribution builds of FFmpeg
		 * leave gray decoding out (and say so for every decoder).
		 */
		m_dec->thread_count = 1;
		if (avcodec_open2(m_dec, codec, nullptr) < 0)
			avcodec_free_context(&m_dec);
	}
	m_jp = av_packet_alloc();
	m_luma = av_frame_alloc();
}

FieldRepair::~FieldRepair()
{
	release(INT64_MAX);
	for (auto &o : m_out) {
		av_packet_free(&o.pkt);
		av_frame_free(&o.frame);
	}
	if (m_jp) {
		m_jp->data = nullptr;
		m_jp->size = 0;
	}
	av_packet_free(&m_jp);
	av_frame_free(&m_luma);
	avcodec_free_context(&m_dec);
}

bool FieldRepair::pushJpeg(const uint8_t *d, size_t n, int64_t time,
			   uint64_t id)
{
	if (!m_dec)
		return false;
	m_jp->data = const_cast<uint8_t *>(d);
	m_jp->size = int(n);
	if (avcodec_send_packet(m_dec, m_jp) < 0 ||
	    avcodec_receive_frame(m_dec, m_luma) < 0)
		return false;
	m_al.push(time, id, m_luma->data[0], m_luma->width, m_luma->height,
		  m_luma->linesize[0]);
	av_frame_unref(m_luma);
	return true;
}

bool FieldRepair::push(int64_t k, AVPacket *pkt, AVFrame *frame)
{
	Src src;
	bool ok;

	src.pkt = pkt;
	src.frame = frame;
	if (m_mjpeg) {
		ok = pkt && splitFieldJpegs(pkt->data, size_t(pkt->size), src.jpegs);
		if (ok) {
			const bool single = sameFieldJpegs(pkt->data, src.jpegs);

			/* Kept before the decode: the aligner may hand the
			 * field out at once (finish).
			 */
			m_src[k] = src;
			ok = pushJpeg(pkt->data, src.jpegs.len0, 2 * k, uint64_t(k) << 1);
			if (ok && !single)
				ok = pushJpeg(pkt->data + src.jpegs.start1, src.jpegs.len1,
					      2 * k + 1, uint64_t(k) << 1 | 1);
		}
	} else {
		ok = frame != nullptr;
		if (ok) {
			const int ls = frame->linesize[0];

			m_src[k] = src;
			m_al.push(2 * k, uint64_t(k) << 1, frame->data[0], frame->width,
				  frame->height / 2, 2 * ls);
			if (!sameFieldLines(frame))
				m_al.push(2 * k + 1, uint64_t(k) << 1 | 1, frame->data[0] + ls,
					  frame->width, frame->height / 2, 2 * ls);
		}
	}
	if (!ok) {
		/* A field already in the aligner keeps its source. */
		if (!m_src.count(k)) {
			av_packet_free(&pkt);
			av_frame_free(&frame);
		}
	}
	collect();
	return ok;
}

void FieldRepair::finish()
{
	m_al.finish();
	collect();
}

bool FieldRepair::pop(Out &out)
{
	if (m_out.empty())
		return false;
	out = m_out.front();
	m_out.pop_front();
	return true;
}

// Frames the aligner has paired, put together from their sources.
void FieldRepair::collect()
{
	FieldAligner::Frame f;

	while (m_al.pop(f)) {
		auto t = m_src.find(int64_t(f.top >> 1));
		auto b = m_src.find(int64_t(f.bottom >> 1));
		Out o{f.time, f.single ? 1 : 2, nullptr, nullptr, f.single,
		      !f.single && (f.top & 1)};

		if (t == m_src.end() || b == m_src.end())
			continue;
		if (m_mjpeg) {
			const uint8_t *d0 = t->second.pkt->data +
					    ((f.top & 1) ? t->second.jpegs.start1 : 0);
			const size_t n0 = (f.top & 1) ? t->second.jpegs.len1
						      : t->second.jpegs.len0;
			const uint8_t *d1 = b->second.pkt->data +
					    ((f.bottom & 1) ? b->second.jpegs.start1 : 0);
			const size_t n1 = (f.bottom & 1) ? b->second.jpegs.len1
							 : b->second.jpegs.len0;

			o.pkt = av_packet_alloc();
			if (!o.pkt || av_new_packet(o.pkt, int(n0 + n1)) < 0) {
				av_packet_free(&o.pkt);
				continue;
			}
			std::memcpy(o.pkt->data, d0, n0);
			std::memcpy(o.pkt->data + n0, d1, n1);
			o.pkt->flags |= AV_PKT_FLAG_KEY;
		} else {
			const AVFrame *ts = t->second.frame, *bs = b->second.frame;

			o.frame = av_frame_alloc();
			o.frame->format = ts->format;
			o.frame->width = ts->width;
			o.frame->height = ts->height;
			if (av_frame_get_buffer(o.frame, 0) < 0) {
				av_frame_free(&o.frame);
				continue;
			}
			buildFieldFrame(o.frame, ts, int(f.top & 1), bs,
					int(f.single ? (f.top & 1) : (f.bottom & 1)));
			o.frame->flags |= AV_FRAME_FLAG_INTERLACED |
					  AV_FRAME_FLAG_TOP_FIELD_FIRST;
		}
		m_out.push_back(o);
		/* Frames before this one's fields are done with. */
		release(int64_t(std::min(f.top, f.bottom) >> 1));
	}
}

void FieldRepair::release(int64_t below)
{
	while (!m_src.empty() && m_src.begin()->first < below) {
		av_packet_free(&m_src.begin()->second.pkt);
		av_frame_free(&m_src.begin()->second.frame);
		m_src.erase(m_src.begin());
	}
}

} // namespace dc30

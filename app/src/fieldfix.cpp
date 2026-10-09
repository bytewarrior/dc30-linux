// SPDX-License-Identifier: GPL-2.0-only
#include "fieldfix.h"

#include "fieldaligner.h"
#include "fieldutil.h"

#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace dc30 {

namespace {

constexpr int kFps = 25;
constexpr int kRate = 44100;
constexpr int kAudioPerFrame = kRate / kFps;	// 1764
constexpr int kAudioFrameBytes = 4;		// S16 stereo

std::string avError(int err)
{
	char buf[AV_ERROR_MAX_STRING_SIZE];

	av_strerror(err, buf, sizeof(buf));
	return buf;
}

struct Input {
	AVFormatContext *fmt = nullptr;
	int video = -1, audio = -1;

	~Input() { avformat_close_input(&fmt); }

	bool open(const std::string &path)
	{
		int err = avformat_open_input(&fmt, path.c_str(), nullptr, nullptr);

		if (err < 0) {
			std::fprintf(stderr, "%s: %s\n", path.c_str(), avError(err).c_str());
			return false;
		}
		avformat_find_stream_info(fmt, nullptr);
		video = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
		audio = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
		if (video < 0) {
			std::fprintf(stderr, "%s: no video stream\n", path.c_str());
			return false;
		}
		return true;
	}

	AVStream *vst() const { return fmt->streams[video]; }

	// Frame index of a video packet on the 25 fps grid.
	int64_t frameIndex(const AVPacket *p) const
	{
		return av_rescale_q(p->pts, vst()->time_base, {1, kFps});
	}
};

// One output frame: field ids are (input frame << 1) | field; time and
// duration in fields (20 ms).
struct OutFrame {
	int64_t time;
	int64_t duration;
	uint64_t top, bottom;
	bool single;
};

AVCodecContext *openDecoder(const Input &in)
{
	const AVCodecParameters *par = in.vst()->codecpar;
	const AVCodec *codec = avcodec_find_decoder(par->codec_id);
	AVCodecContext *dec = codec ? avcodec_alloc_context3(codec) : nullptr;

	if (!dec)
		return nullptr;
	avcodec_parameters_to_context(dec, par);
	/* Luma would do (AV_CODEC_FLAG_GRAY), but distribution builds of
	 * FFmpeg leave gray decoding out and say so.
	 */
	/* Slices only: one frame out per packet in. */
	dec->thread_count = par->codec_id == AV_CODEC_ID_FFV1 ? 0 : 1;
	dec->thread_type = FF_THREAD_SLICE;
	if (avcodec_open2(dec, codec, nullptr) < 0)
		avcodec_free_context(&dec);
	return dec;
}

// Pass 1: the pairing.
bool analyse(Input &in, std::vector<OutFrame> &frames, FieldAligner &al,
	     uint64_t &fillers)
{
	const bool mjpeg = in.vst()->codecpar->codec_id == AV_CODEC_ID_MJPEG;
	AVCodecContext *dec = openDecoder(in);
	AVPacket *pkt = av_packet_alloc();
	AVPacket *jp = av_packet_alloc();
	AVFrame *fr = av_frame_alloc();
	std::vector<uint8_t> prev;
	FieldAligner::Frame f;
	bool ok = true;
	int64_t k = -1;

	if (!dec) {
		std::fprintf(stderr, "no decoder for the video stream\n");
		ok = false;
	}

	auto pushField = [&](const uint8_t *d, size_t n, int64_t time,
			     uint64_t id) -> bool {
		jp->data = const_cast<uint8_t *>(d);
		jp->size = int(n);
		if (avcodec_send_packet(dec, jp) < 0 ||
		    avcodec_receive_frame(dec, fr) < 0) {
			std::fprintf(stderr, "frame %lld: field JPEG does not decode\n",
				     (long long)(id >> 1));
			return false;
		}
		al.push(time, id, fr->data[0], fr->width, fr->height,
			fr->linesize[0]);
		av_frame_unref(fr);
		return true;
	};

	while (ok && av_read_frame(in.fmt, pkt) >= 0) {
		if (pkt->stream_index != in.video) {
			av_packet_unref(pkt);
			continue;
		}
		k = in.frameIndex(pkt);
		if (prev.size() == size_t(pkt->size) &&
		    !std::memcmp(prev.data(), pkt->data, prev.size())) {
			/* A frame dc30-capture repeated for a lost one. */
			fillers++;
			av_packet_unref(pkt);
			continue;
		}
		prev.assign(pkt->data, pkt->data + pkt->size);

		if (!mjpeg) {
			if (avcodec_send_packet(dec, pkt) < 0 ||
			    avcodec_receive_frame(dec, fr) < 0) {
				std::fprintf(stderr, "frame %lld does not decode, left out\n",
					     (long long)k);
				av_packet_unref(pkt);
				continue;
			}
			const int ls = fr->linesize[0];

			al.push(2 * k, uint64_t(k) << 1, fr->data[0], fr->width,
				fr->height / 2, 2 * ls);
			if (!sameFieldLines(fr))
				al.push(2 * k + 1, uint64_t(k) << 1 | 1, fr->data[0] + ls,
					fr->width, fr->height / 2, 2 * ls);
			av_frame_unref(fr);
			av_packet_unref(pkt);
			while (al.pop(f))
				frames.push_back({f.time, 0, f.top, f.bottom, f.single});
			if (k % 5000 == 0)
				std::fprintf(stderr, "\ranalysing: frame %lld", (long long)k);
			continue;
		}

		FieldJpegs s;

		if (!splitFieldJpegs(pkt->data, size_t(pkt->size), s)) {
			std::fprintf(stderr, "frame %lld: not two field JPEGs, left out\n",
				     (long long)k);
			av_packet_unref(pkt);
			continue;
		}
		const bool single = sameFieldJpegs(pkt->data, s);

		ok = pushField(pkt->data, s.len0, 2 * k, uint64_t(k) << 1);
		if (ok && !single)
			ok = pushField(pkt->data + s.start1, s.len1, 2 * k + 1,
				       uint64_t(k) << 1 | 1);
		av_packet_unref(pkt);

		while (al.pop(f))
			frames.push_back({f.time, 0, f.top, f.bottom, f.single});
		if (k % 5000 == 0)
			std::fprintf(stderr, "\ranalysing: frame %lld", (long long)k);
	}
	std::fprintf(stderr, "\n");
	al.finish();
	while (al.pop(f))
		frames.push_back({f.time, 0, f.top, f.bottom, f.single});

	av_frame_free(&fr);
	jp->data = nullptr;
	jp->size = 0;
	av_packet_free(&jp);
	av_packet_free(&pkt);
	avcodec_free_context(&dec);
	return ok;
}

// Each frame lasts until the next one starts: 40 ms, 20 ms for a single
// field, longer over fields the recording lost.
void setDurations(std::vector<OutFrame> &frames)
{
	for (size_t i = 0; i < frames.size(); i++)
		frames[i].duration = i + 1 < frames.size() ?
			frames[i + 1].time - frames[i].time :
			(frames[i].single ? 1 : 2);
}

struct Writer {
	AVFormatContext *fmt = nullptr;
	AVStream *vst = nullptr, *ast = nullptr;
	AVRational inAudioBase{1, 1000};

	~Writer()
	{
		if (fmt) {
			if (!(fmt->oformat->flags & AVFMT_NOFILE))
				avio_closep(&fmt->pb);
			avformat_free_context(fmt);
		}
	}

	// 'enc': FFV1 encoder for the video, else its packets are copied.
	bool open(const std::string &path, const Input &in,
		  const AVCodecContext *enc)
	{
		int err = avformat_alloc_output_context2(&fmt, nullptr, "matroska",
							 path.c_str());

		if (err < 0)
			return false;
		vst = avformat_new_stream(fmt, nullptr);
		if (enc)
			avcodec_parameters_from_context(vst->codecpar, enc);
		else
			avcodec_parameters_copy(vst->codecpar, in.vst()->codecpar);
		vst->codecpar->codec_tag = 0;
		vst->codecpar->field_order = AV_FIELD_TT;
		/* Field rate: a frame starts at its first field. */
		vst->time_base = {1, 2 * kFps};
		vst->sample_aspect_ratio = in.vst()->sample_aspect_ratio;
		if (in.audio >= 0) {
			ast = avformat_new_stream(fmt, nullptr);
			avcodec_parameters_copy(ast->codecpar,
						in.fmt->streams[in.audio]->codecpar);
			ast->codecpar->codec_tag = 0;
			ast->time_base = {1, kRate};
		}
		av_dict_copy(&fmt->metadata, in.fmt->metadata, 0);
		av_dict_set(&fmt->metadata, "comment",
			    "field pairing fixed by dc30-capture --fix", 0);
		err = avio_open(&fmt->pb, path.c_str(), AVIO_FLAG_WRITE);
		if (err < 0) {
			std::fprintf(stderr, "%s: %s\n", path.c_str(), avError(err).c_str());
			return false;
		}
		err = avformat_write_header(fmt, nullptr);
		if (err < 0) {
			std::fprintf(stderr, "%s: %s\n", path.c_str(), avError(err).c_str());
			return false;
		}
		return true;
	}

	bool write(AVStream *st, const uint8_t *d, size_t n, int64_t pts,
		   int64_t duration, AVRational base)
	{
		AVPacket *p = av_packet_alloc();

		if (!p || av_new_packet(p, int(n)) < 0) {
			av_packet_free(&p);
			return false;
		}
		std::memcpy(p->data, d, n);
		p->flags |= AV_PKT_FLAG_KEY;
		p->stream_index = st->index;
		p->pts = p->dts = av_rescale_q(pts, base, st->time_base);
		p->duration = av_rescale_q(duration, base, st->time_base);
		const int err = av_interleaved_write_frame(fmt, p);

		av_packet_free(&p);
		if (err < 0)
			std::fprintf(stderr, "write: %s\n", avError(err).c_str());
		return err >= 0;
	}

	// An encoder's packet, time stamps in 'base'.
	bool writeEncoded(AVPacket *p, int64_t duration, AVRational base)
	{
		p->stream_index = vst->index;
		p->duration = duration;
		av_packet_rescale_ts(p, base, vst->time_base);
		const int err = av_interleaved_write_frame(fmt, p);

		if (err < 0)
			std::fprintf(stderr, "write: %s\n", avError(err).c_str());
		return err >= 0;
	}
};

// FFV1 as dc30-capture records it (Recorder::addFfv1Stream()), on the
// field time base.
AVCodecContext *openFfv1Encoder(const AVCodecParameters *in)
{
	const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_FFV1);
	AVCodecContext *enc = codec ? avcodec_alloc_context3(codec) : nullptr;
	AVDictionary *opts = nullptr;

	if (!enc)
		return nullptr;
	enc->width = in->width;
	enc->height = in->height;
	enc->pix_fmt = AV_PIX_FMT_YUV422P;
	enc->time_base = {1, 2 * kFps};
	enc->sample_aspect_ratio = in->sample_aspect_ratio;
	enc->field_order = AV_FIELD_TT;
	enc->color_primaries = in->color_primaries;
	enc->color_trc = in->color_trc;
	enc->colorspace = in->color_space;
	enc->color_range = in->color_range;
	enc->chroma_sample_location = in->chroma_location;
	enc->level = 3;
	enc->gop_size = 1;
	enc->slices = 16;
	enc->thread_count = 0;
	enc->thread_type = FF_THREAD_SLICE;
	enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;	/* Matroska */
	av_dict_set(&opts, "coder", "range_tab", 0);
	av_dict_set(&opts, "context", "0", 0);
	av_dict_set(&opts, "slicecrc", "1", 0);
	if (avcodec_open2(enc, codec, &opts) < 0)
		avcodec_free_context(&enc);
	av_dict_free(&opts);
	return enc;
}

// Pass 2: the new file. The sound goes over as it is.
bool rewrite(Input &in, Writer &w, const std::vector<OutFrame> &frames)
{
	AVPacket *pkt = av_packet_alloc();
	// Video packets read but not used up yet, by frame index.
	std::map<int64_t, std::vector<uint8_t>> cache;
	int64_t readUpTo = -1;
	bool eof = false;
	std::vector<uint8_t> out;
	bool ok = true;

	// Read on until video frame 'k' is in the cache (or the input ends).
	auto readTo = [&](int64_t k) -> bool {
		while (!eof && readUpTo < k) {
			if (av_read_frame(in.fmt, pkt) < 0) {
				eof = true;
				break;
			}
			if (pkt->stream_index == in.video) {
				readUpTo = in.frameIndex(pkt);
				cache[readUpTo].assign(pkt->data, pkt->data + pkt->size);
			} else if (w.ast && pkt->stream_index == in.audio) {
				AVStream *ist = in.fmt->streams[in.audio];

				if (!w.write(w.ast, pkt->data, size_t(pkt->size), pkt->pts,
					     pkt->duration, ist->time_base)) {
					av_packet_unref(pkt);
					return false;
				}
			}
			av_packet_unref(pkt);
		}
		return true;
	};

	auto fieldBytes = [&](uint64_t id, const uint8_t *&d, size_t &n) -> bool {
		auto it = cache.find(int64_t(id >> 1));
		FieldJpegs s;

		if (it == cache.end() ||
		    !splitFieldJpegs(it->second.data(), it->second.size(), s))
			return false;
		if (id & 1) {
			d = it->second.data() + s.start1;
			n = s.len1;
		} else {
			d = it->second.data();
			n = s.len0;
		}
		return true;
	};

	for (const auto &f : frames) {
		const int64_t need = int64_t(std::max(f.top, f.bottom) >> 1);
		const uint8_t *d0, *d1;
		size_t n0, n1;

		if (!(ok = readTo(need)))
			break;
		if (!fieldBytes(f.top, d0, n0) || !fieldBytes(f.bottom, d1, n1)) {
			std::fprintf(stderr, "field %llu not found\n",
				     (unsigned long long)f.top);
			ok = false;
			break;
		}
		out.assign(d0, d0 + n0);
		out.insert(out.end(), d1, d1 + n1);
		if (!(ok = w.write(w.vst, out.data(), out.size(), f.time, f.duration,
				   {1, 2 * kFps})))
			break;
		/* Packets of frames before this one's fields are done with. */
		const int64_t done = int64_t(std::min(f.top, f.bottom) >> 1);

		while (!cache.empty() && cache.begin()->first < done)
			cache.erase(cache.begin());
		if ((f.time / 2) % 5000 == 0)
			std::fprintf(stderr, "\rwriting: frame %lld", (long long)(f.time / 2));
	}
	std::fprintf(stderr, "\n");
	/* The rest of the sound. */
	if (ok)
		ok = readTo(INT64_MAX);
	av_packet_free(&pkt);
	return ok;
}

// Pass 2 for FFV1: decode, put the fields' lines together anew, encode.
bool rewriteFfv1(Input &in, Writer &w, AVCodecContext *enc,
		 const std::vector<OutFrame> &frames)
{
	AVCodecContext *dec = openDecoder(in);
	AVPacket *pkt = av_packet_alloc();
	AVPacket *opkt = av_packet_alloc();
	// Decoded frames not used up yet, by frame index.
	std::map<int64_t, AVFrame *> cache;
	std::vector<uint8_t> prev;
	std::map<int64_t, int64_t> durations;	// by pts, for the packets
	int64_t readUpTo = -1;
	bool eof = false, ok = dec != nullptr;

	auto drain = [&](bool flush) -> bool {
		int err;

		if (flush)
			avcodec_send_frame(enc, nullptr);
		while ((err = avcodec_receive_packet(enc, opkt)) == 0) {
			auto it = durations.find(opkt->pts);
			const int64_t d = it != durations.end() ? it->second : 2;

			if (it != durations.end())
				durations.erase(it);
			if (!w.writeEncoded(opkt, d, enc->time_base))
				return false;
			av_packet_unref(opkt);
		}
		return err == AVERROR(EAGAIN) || err == AVERROR_EOF;
	};

	auto readTo = [&](int64_t k) -> bool {
		while (!eof && readUpTo < k) {
			if (av_read_frame(in.fmt, pkt) < 0) {
				eof = true;
				break;
			}
			if (pkt->stream_index == in.video) {
				const int64_t idx = in.frameIndex(pkt);
				const bool repeat = prev.size() == size_t(pkt->size) &&
						    !std::memcmp(prev.data(), pkt->data, prev.size());

				readUpTo = idx;
				if (!repeat) {
					AVFrame *fr = av_frame_alloc();

					prev.assign(pkt->data, pkt->data + pkt->size);
					if (avcodec_send_packet(dec, pkt) >= 0 &&
					    avcodec_receive_frame(dec, fr) >= 0)
						cache[idx] = fr;
					else
						av_frame_free(&fr);
				}
			} else if (w.ast && pkt->stream_index == in.audio) {
				AVStream *ist = in.fmt->streams[in.audio];

				if (!w.write(w.ast, pkt->data, size_t(pkt->size), pkt->pts,
					     pkt->duration, ist->time_base)) {
					av_packet_unref(pkt);
					return false;
				}
			}
			av_packet_unref(pkt);
		}
		return true;
	};

	for (const auto &f : frames) {
		if (!ok)
			break;
		const int64_t need = int64_t(std::max(f.top, f.bottom) >> 1);

		if (!(ok = readTo(need)))
			break;
		auto t = cache.find(int64_t(f.top >> 1));
		auto b = cache.find(int64_t(f.bottom >> 1));

		if (t == cache.end() || b == cache.end()) {
			std::fprintf(stderr, "field %llu not found\n",
				     (unsigned long long)f.top);
			ok = false;
			break;
		}
		AVFrame *out = av_frame_alloc();

		out->format = AV_PIX_FMT_YUV422P;
		out->width = enc->width;
		out->height = enc->height;
		if (av_frame_get_buffer(out, 0) < 0) {
			av_frame_free(&out);
			ok = false;
			break;
		}
		/* A single field: its own lines into both rows. */
		buildFieldFrame(out, t->second, int(f.top & 1), b->second,
			   int(f.single ? (f.top & 1) : (f.bottom & 1)));
		out->flags |= AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST;
		out->pts = f.time;
		durations[f.time] = f.duration;
		ok = avcodec_send_frame(enc, out) >= 0 && drain(false);
		av_frame_free(&out);

		const int64_t done = int64_t(std::min(f.top, f.bottom) >> 1);

		while (!cache.empty() && cache.begin()->first < done) {
			av_frame_free(&cache.begin()->second);
			cache.erase(cache.begin());
		}
		if ((f.time / 2) % 5000 == 0)
			std::fprintf(stderr, "\rwriting: frame %lld", (long long)(f.time / 2));
	}
	std::fprintf(stderr, "\n");
	if (ok)
		ok = drain(true) && readTo(INT64_MAX);
	for (auto &c : cache)
		av_frame_free(&c.second);
	av_packet_free(&opkt);
	av_packet_free(&pkt);
	avcodec_free_context(&dec);
	return ok;
}

std::string when(int64_t fieldTime)
{
	const int64_t ms = fieldTime * 1000 / (2 * kFps);
	char buf[32];

	std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld.%03lld",
		      (long long)(ms / 3600000), (long long)(ms / 60000 % 60),
		      (long long)(ms / 1000 % 60), (long long)(ms % 1000));
	return buf;
}

// Where the new file differs from the recording (times are the same in
// both): stretches paired across the recorded frames, single-field
// frames.
void writeEvents(FILE *log, const std::vector<OutFrame> &frames)
{
	bool crossed = false;

	std::fputs("\nEvents:\n", log);
	for (const auto &f : frames) {
		if (f.single) {
			std::fprintf(log, "[%s] single field (field grid slipped)\n",
				     when(f.time).c_str());
			continue;
		}
		/* The top field from the recorded bottom rows: paired across. */
		const bool c = f.top & 1;

		if (c != crossed)
			std::fprintf(log, "[%s] %s\n", when(f.time).c_str(),
				     c ? "re-paired from here (recording had the fields swapped)"
				       : "as recorded from here");
		crossed = c;
	}
}

} // namespace

int fixRecording(const std::string &inPath, const std::string &outPath)
{
	Input in;

	if (!in.open(inPath))
		return 1;
	const AVCodecID codec = in.vst()->codecpar->codec_id;

	if (codec != AV_CODEC_ID_MJPEG && codec != AV_CODEC_ID_FFV1) {
		std::fprintf(stderr, "%s: neither MJPEG nor FFV1\n", inPath.c_str());
		return 1;
	}

	FieldAligner al;
	std::vector<OutFrame> frames;
	uint64_t fillers = 0;

	if (!analyse(in, frames, al, fillers))
		return 1;

	setDurations(frames);

	Input in2;
	Writer w;

	AVCodecContext *enc = nullptr;

	if (codec == AV_CODEC_ID_FFV1 &&
	    !(enc = openFfv1Encoder(in.vst()->codecpar))) {
		std::fprintf(stderr, "no FFV1 encoder\n");
		return 1;
	}
	const bool ok = in2.open(inPath) && w.open(outPath, in2, enc) &&
			(enc ? rewriteFfv1(in2, w, enc, frames) : rewrite(in2, w, frames));

	avcodec_free_context(&enc);
	if (!ok)
		return 1;
	av_write_trailer(w.fmt);

	const auto &st = al.stats();
	uint64_t crossed = 0;

	for (const auto &f : frames)
		crossed += !f.single && (f.top & 1);

	const std::string logPath = outPath.substr(0, outPath.rfind('.')) + ".log";
	FILE *log = std::fopen(logPath.c_str(), "w");
	char text[1024];

	std::snprintf(text, sizeof(text),
		      "dc30-capture --fix %s\n"
		      "fields %llu, decisive %.1f %%\n"
		      "field grid slips %llu; frames %llu, re-paired %llu, single fields %llu\n"
		      "frames repeated by the recording (left out): %llu\n"
		      "time stamps: field exact (single fields last 20 ms), sound unchanged\n",
		      inPath.c_str(), (unsigned long long)st.fields,
		      st.fields ? 100.0 * st.decisive / st.fields : 0.0,
		      (unsigned long long)st.jumps, (unsigned long long)st.frames,
		      (unsigned long long)crossed, (unsigned long long)st.singles,
		      (unsigned long long)fillers);
	std::fputs(text, stdout);
	if (log) {
		std::fputs(text, log);
		writeEvents(log, frames);
		std::fclose(log);
	}
	return 0;
}

} // namespace dc30

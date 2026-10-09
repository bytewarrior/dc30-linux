// SPDX-License-Identifier: GPL-2.0-only
#include "recorder.h"

#include <QDateTime>
#include <QFileInfo>
#include <QTextStream>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
}

namespace {

// Frames waiting for the encoder or the file, ~3 s (0.9 MB each for FFV1,
// 0.25 MB for MJPEG). A full queue drops the frame - it is then repeated
// like a frame the driver lost.
constexpr size_t kMaxQueued = 75;
// Sound further off than this (10 ms) is set right with silence or by
// skipping samples. In video lock it never is; with the crystal clock
// (1764.08 per frame) every ~4 minutes.
constexpr double kAudioTolerance = Recorder::kRate / 100.0;
// Frames without a sound position before the recording starts anyway.
constexpr size_t kMaxStartWait = 2 * Recorder::kFps;
// Sound that cannot be placed yet (no metadata for its ALSA stream).
constexpr size_t kMaxPendingAudio = 10 * Recorder::kRate;
// After the last frame, wait this long for its sound.
constexpr auto kAudioTail = std::chrono::milliseconds(1000);
constexpr AVRational kVideoBase{1, int(Recorder::kFps)};
constexpr AVRational kAudioBase{1, int(Recorder::kRate)};

QString avError(int err)
{
	char buf[AV_ERROR_MAX_STRING_SIZE];

	av_strerror(err, buf, sizeof(buf));
	return QString::fromLocal8Bit(buf);
}

QString logPathFor(const QString &path)
{
	QFileInfo fi(path);

	return fi.path() + QLatin1Char('/') + fi.completeBaseName() +
	       QStringLiteral(".log");
}

} // namespace

Recorder::Recorder(QString path, QString input, Codec codec, bool fieldRepair,
		   QObject *parent)
	: QThread(parent), m_path(std::move(path)), m_logPath(logPathFor(m_path)),
	  m_input(std::move(input)), m_codec(codec),
	  m_repair(fieldRepair ? std::make_unique<dc30::FieldRepair>(codec == Codec::Mjpeg)
			       : nullptr),
	  m_videoBase(fieldRepair ? AVRational{1, 2 * int(kFps)} : kVideoBase),
	  m_log(m_logPath)
{
}

void Recorder::freeItem(VideoItem &v)
{
	av_frame_free(&v.frame);
	av_packet_free(&v.pkt);
}

Recorder::~Recorder()
{
	finish();
	wait();
	closeOutput();
	for (auto &v : m_videoQ)
		freeItem(v);
}

void Recorder::finish()
{
	std::lock_guard<std::mutex> g(m_lock);

	m_finish = true;
	m_cv.notify_all();
}

void Recorder::pushVideo(uint32_t seq, const uint8_t *yuyv, int width,
			 int height, int stride)
{
	{
		std::lock_guard<std::mutex> g(m_lock);

		if (m_finish)
			return;
		if (m_videoQ.size() >= kMaxQueued) {
			m_queueDropped.insert(seq);
			return;
		}
	}

	AVFrame *f = av_frame_alloc();

	if (!f)
		return;
	f->format = AV_PIX_FMT_YUV422P;
	f->width = width;
	f->height = height;
	if (av_frame_get_buffer(f, 0) < 0) {
		av_frame_free(&f);
		return;
	}
	/* YUYV -> planar 4:2:2, both fields as they are: nothing is scaled,
	 * filtered or mixed between the lines.
	 */
	for (int y = 0; y < height; y++) {
		const uint8_t *s = yuyv + size_t(y) * stride;
		uint8_t *py = f->data[0] + size_t(y) * f->linesize[0];
		uint8_t *pu = f->data[1] + size_t(y) * f->linesize[1];
		uint8_t *pv = f->data[2] + size_t(y) * f->linesize[2];

		for (int x = 0; x + 1 < width; x += 2, s += 4) {
			py[x] = s[0];
			pu[x / 2] = s[1];
			py[x + 1] = s[2];
			pv[x / 2] = s[3];
		}
	}
	f->flags |= AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST;

	std::lock_guard<std::mutex> g(m_lock);

	if (m_finish) {
		av_frame_free(&f);
		return;
	}
	m_videoQ.push_back({seq, f, nullptr, width, height});
	m_cv.notify_all();
}

void Recorder::pushVideoPacket(uint32_t seq, const uint8_t *data, size_t size,
			       int width, int height)
{
	{
		std::lock_guard<std::mutex> g(m_lock);

		if (m_finish)
			return;
		if (m_videoQ.size() >= kMaxQueued) {
			m_queueDropped.insert(seq);
			return;
		}
	}

	AVPacket *p = av_packet_alloc();

	if (!p || av_new_packet(p, int(size)) < 0) {
		av_packet_free(&p);
		return;
	}
	std::memcpy(p->data, data, size);
	p->flags |= AV_PKT_FLAG_KEY;

	std::lock_guard<std::mutex> g(m_lock);

	if (m_finish) {
		av_packet_free(&p);
		return;
	}
	m_videoQ.push_back({seq, nullptr, p, width, height});
	m_cv.notify_all();
}

void Recorder::pushAudio(unsigned generation, uint64_t pos,
			 const int16_t *frames, unsigned count)
{
	AudioItem a{generation, pos, std::vector<int16_t>(frames, frames + count * 2)};
	std::lock_guard<std::mutex> g(m_lock);

	m_audioQ.push_back(std::move(a));
	m_cv.notify_all();
}

void Recorder::pushMeta(const dc30_meta &meta)
{
	if (meta.version != DC30_META_VERSION || meta.size != sizeof(meta))
		return;

	MetaItem m{};

	m.seq = meta.sequence;
	m.audio = (meta.field[0].flags & DC30_META_FIELD_AUDIO) &&
		  meta.audio_frame_bytes;
	if (m.audio)
		m.pos = double(meta.field[0].audio_pos) / meta.audio_frame_bytes;
	m.epoch = meta.field[0].audio_epoch;
	m.clock = meta.audio_clock;
	m.dropped = meta.frames_dropped;
	m.noField = meta.no_field;
	m.lateIrq = meta.late_irq;
	m.fieldOrder = meta.field_order;
	m.fifoOverflows = meta.fifo_overflows;
	m.codecRestarts = meta.codec_restarts;
	m.codecSkipped = meta.codec_skipped;
	m.framesRejected = meta.frames_rejected;
	m.pairingSwitches = meta.pairing_switches;

	std::lock_guard<std::mutex> g(m_lock);

	m_metaQ.push_back(m);
	m_cv.notify_all();
}

// Position in the recording, as a timecode hh:mm:ss:ff.
QString Recorder::when(int64_t frame) const
{
	const int64_t s = frame / kFps;

	return QStringLiteral("%1:%2:%3:%4")
		.arg(s / 3600, 2, 10, QLatin1Char('0'))
		.arg(s / 60 % 60, 2, 10, QLatin1Char('0'))
		.arg(s % 60, 2, 10, QLatin1Char('0'))
		.arg(frame % kFps, 2, 10, QLatin1Char('0'));
}

void Recorder::log(const QString &line)
{
	logAt(m_frames, line);
}

void Recorder::logAt(int64_t frame, const QString &line)
{
	if (!m_log.isOpen())
		return;
	QTextStream(&m_log) << QStringLiteral("[%1] ").arg(when(frame)) << line
			    << '\n';
	m_log.flush();
}

// FFV1 encoder and its stream.
bool Recorder::addFfv1Stream(int width, int height)
{
	const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_FFV1);
	int err;

	if (!codec) {
		emit failed(tr("No FFV1 encoder in libavcodec"));
		return false;
	}
	m_enc = avcodec_alloc_context3(codec);
	m_enc->width = width;
	m_enc->height = height;
	m_enc->pix_fmt = AV_PIX_FMT_YUV422P;
	m_enc->time_base = m_videoBase;
	m_enc->framerate = {int(kFps), 1};
	/* Square pixels (768 samples per active PAL line), 4:3. */
	m_enc->sample_aspect_ratio = {1, 1};
	/* Interlaced, top field first: the driver always delivers it so. */
	m_enc->field_order = AV_FIELD_TT;
	/* PAL, BT.601, limited range; chroma co-sited with the left luma. */
	m_enc->color_primaries = AVCOL_PRI_BT470BG;
	m_enc->color_trc = AVCOL_TRC_BT709;
	m_enc->colorspace = AVCOL_SPC_BT470BG;
	m_enc->color_range = AVCOL_RANGE_MPEG;
	m_enc->chroma_sample_location = AVCHROMA_LOC_LEFT;
	/* FFV1 version 3 as archives use it: every frame on its own, CRC
	 * per slice, range coder; the slices also let it use the cores.
	 */
	m_enc->level = 3;
	m_enc->gop_size = 1;
	m_enc->slices = 16;
	m_enc->thread_count = 0;
	m_enc->thread_type = FF_THREAD_SLICE;
	if (m_fmt->oformat->flags & AVFMT_GLOBALHEADER)
		m_enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	AVDictionary *opts = nullptr;

	av_dict_set(&opts, "coder", "range_tab", 0);
	av_dict_set(&opts, "context", "0", 0);
	av_dict_set(&opts, "slicecrc", "1", 0);
	err = avcodec_open2(m_enc, codec, &opts);
	av_dict_free(&opts);
	if (err < 0) {
		emit failed(tr("FFV1 encoder: %1").arg(avError(err)));
		return false;
	}

	m_vst = avformat_new_stream(m_fmt, nullptr);
	avcodec_parameters_from_context(m_vst->codecpar, m_enc);
	m_vst->time_base = m_videoBase;
	m_vst->avg_frame_rate = {int(kFps), 1};
	m_vst->sample_aspect_ratio = m_enc->sample_aspect_ratio;
	return true;
}

// MJPEG: no encoder, the stream takes the card's frames as they are. One
// packet = both field JPEGs (APP0 "AVI1"), which decoders put together
// as an interlaced frame of the stream's height.
void Recorder::addMjpegStream(int width, int height)
{
	m_vst = avformat_new_stream(m_fmt, nullptr);
	AVCodecParameters *vp = m_vst->codecpar;
	vp->codec_type = AVMEDIA_TYPE_VIDEO;
	vp->codec_id = AV_CODEC_ID_MJPEG;
	vp->width = width;
	vp->height = height;
	vp->format = AV_PIX_FMT_YUV422P;
	vp->sample_aspect_ratio = {1, 1};
	vp->field_order = AV_FIELD_TT;
	/* Limited range despite JPEG: the ZR36016 passes the decoder's
	 * BT.601 levels through.
	 */
	vp->color_primaries = AVCOL_PRI_BT470BG;
	vp->color_trc = AVCOL_TRC_BT709;
	vp->color_space = AVCOL_SPC_BT470BG;
	vp->color_range = AVCOL_RANGE_MPEG;
	vp->chroma_location = AVCHROMA_LOC_LEFT;
	m_vst->time_base = m_videoBase;
	m_vst->avg_frame_rate = {int(kFps), 1};
	m_vst->sample_aspect_ratio = vp->sample_aspect_ratio;
}

bool Recorder::openOutput(int width, int height)
{
	const QByteArray path = m_path.toLocal8Bit();
	int err = avformat_alloc_output_context2(&m_fmt, nullptr, "matroska",
						 path.constData());

	if (err < 0) {
		emit failed(tr("Matroska: %1").arg(avError(err)));
		return false;
	}

	if (m_codec == Codec::Mjpeg)
		addMjpegStream(width, height);
	else if (!addFfv1Stream(width, height))
		return false;

	m_ast = avformat_new_stream(m_fmt, nullptr);
	AVCodecParameters *ap = m_ast->codecpar;
	ap->codec_type = AVMEDIA_TYPE_AUDIO;
	ap->codec_id = AV_CODEC_ID_PCM_S16LE;
	ap->format = AV_SAMPLE_FMT_S16;
	ap->sample_rate = kRate;
	ap->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
	ap->bits_per_coded_sample = 16;
	ap->block_align = 4;
	ap->bit_rate = int64_t(kRate) * 32;
	m_ast->time_base = kAudioBase;

	const QByteArray comment =
		tr("miro DC30, input %1, audio 44.1 kHz %2").arg(m_input,
			m_clock == DC30_META_CLOCK_VIDEOLOCK ? tr("video lock")
							     : tr("crystal")).toUtf8();
	av_dict_set(&m_fmt->metadata, "COMMENT", comment.constData(), 0);
	av_dict_set(&m_fmt->metadata, "DATE_RECORDED",
		    QDateTime::currentDateTime().toString(Qt::ISODate).toUtf8().constData(),
		    0);

	err = avio_open(&m_fmt->pb, path.constData(), AVIO_FLAG_WRITE);
	if (err < 0) {
		emit failed(tr("%1: %2").arg(m_path, avError(err)));
		return false;
	}
	err = avformat_write_header(m_fmt, nullptr);
	if (err < 0) {
		emit failed(tr("Matroska header: %1").arg(avError(err)));
		return false;
	}
	m_lastPkt = av_packet_alloc();
	m_width = width;
	m_height = height;
	return true;
}

void Recorder::closeOutput()
{
	if (m_fmt && m_fmt->pb) {
		av_write_trailer(m_fmt);
		avio_closep(&m_fmt->pb);
	}
	avformat_free_context(m_fmt);
	m_fmt = nullptr;
	avcodec_free_context(&m_enc);
	av_packet_free(&m_lastPkt);
}

// Takes over the packet's data. pts/duration in the stream's native base
// (video frames, audio samples).
bool Recorder::writePacket(AVPacket *pkt, AVStream *st, int64_t pts,
			   int64_t duration)
{
	const AVRational base = st == m_vst ? m_videoBase : kAudioBase;

	pkt->stream_index = st->index;
	pkt->pts = pkt->dts = av_rescale_q(pts, base, st->time_base);
	pkt->duration = av_rescale_q(duration, base, st->time_base);
	int err = av_interleaved_write_frame(m_fmt, pkt);

	if (err < 0) {
		emit failed(tr("Write failed: %1").arg(avError(err)));
		return false;
	}
	return true;
}

bool Recorder::encodeVideo(AVFrame *frame, int64_t pts, int64_t duration)
{
	frame->pts = pts;
	m_durations[pts] = duration;
	int err = avcodec_send_frame(m_enc, frame);

	if (err < 0) {
		emit failed(tr("FFV1: %1").arg(avError(err)));
		return false;
	}
	AVPacket *pkt = av_packet_alloc();
	bool ok = true;

	while (ok && (err = avcodec_receive_packet(m_enc, pkt)) == 0) {
		auto d = m_durations.find(pkt->pts);
		const int64_t dur = d != m_durations.end() ? d->second : 1;

		if (d != m_durations.end())
			m_durations.erase(d);
		av_packet_unref(m_lastPkt);
		av_packet_ref(m_lastPkt, pkt);
		ok = writePacket(pkt, m_vst, m_lastPkt->pts, dur);
	}
	av_packet_free(&pkt);
	if (ok && err != AVERROR(EAGAIN)) {
		emit failed(tr("FFV1: %1").arg(avError(err)));
		ok = false;
	}
	return ok;
}

// Field repair: write the frames FieldRepair has paired.
bool Recorder::writeRepaired()
{
	dc30::FieldRepair::Out o;
	bool ok = true;

	while (ok && m_repair->pop(o)) {
		if (!o.single && o.crossed != m_crossed) {
			logAt(o.time / 2, o.crossed ? tr("Fields re-paired from here (the card had them swapped)")
						    : tr("Fields as the card paired them from here"));
			m_crossed = o.crossed;
		}
		if (o.pkt)
			ok = writePacket(o.pkt, m_vst, o.time, o.duration);
		else if (o.frame)
			ok = encodeVideo(o.frame, o.time, o.duration);
		av_packet_free(&o.pkt);
		av_frame_free(&o.frame);
		m_frames = std::max(m_frames, (o.time + o.duration + 1) / 2);
	}
	return ok;
}

// MJPEG: the card's frame goes into the file as it is.
bool Recorder::writeVideoPacket(AVPacket *pkt)
{
	av_packet_unref(m_lastPkt);
	av_packet_ref(m_lastPkt, pkt);
	const bool ok = writePacket(pkt, m_vst, m_frames, 1);

	m_frames++;
	return ok;
}

// Every FFV1 and MJPEG frame stands alone: a repeat is the same packet
// again.
bool Recorder::repeatVideo(unsigned count)
{
	for (unsigned i = 0; i < count; i++, m_frames++, m_repeated++) {
		AVPacket *p = av_packet_clone(m_lastPkt);

		if (!p || !writePacket(p, m_vst, m_frames, 1)) {
			av_packet_free(&p);
			return false;
		}
		av_packet_free(&p);
	}
	return true;
}

int Recorder::epochIndex(uint32_t epoch)
{
	auto it = std::find(m_epochs.begin(), m_epochs.end(), epoch);

	if (it != m_epochs.end())
		return int(it - m_epochs.begin());
	m_epochs.push_back(epoch);
	return int(m_epochs.size()) - 1;
}

int Recorder::generationIndex(unsigned generation)
{
	auto it = std::find(m_generations.begin(), m_generations.end(), generation);

	if (it != m_generations.end())
		return int(it - m_generations.begin());
	m_generations.push_back(generation);
	return int(m_generations.size()) - 1;
}

void Recorder::handleMeta(const MetaItem &m)
{
	m_clock = m.clock;
	if (m.audio) {
		epochIndex(m.epoch);
		/* 10 points (0.4 s) are enough to place the sound: single
		 * values are off by a few samples only.
		 */
		auto it = m_fits.try_emplace(m.epoch, 500, 10).first;

		/* A field jump moves the frames by a field against the
		 * sequence: the lone field's frame and the next one start
		 * the fit over.
		 */
		const bool jump = m_haveMeta &&
			(m.pairingSwitches != m_lastMeta.pairingSwitches ||
			 m.fieldOrder != m_lastMeta.fieldOrder);

		it->second.add(m.seq, m.pos, m.epoch, jump || m_jumpBefore);
		m_jumpBefore = jump;
		m_lastSeqOfEpoch[m.epoch] = m.seq;
		/* Only the newest two ALSA streams can still get sound. */
		while (m_fits.size() > 2)
			m_fits.erase(m_fits.begin());
	}

	/* The driver's reasons for lost frames, as they happen. */
	if (m_haveMeta && m_started) {
		QStringList why;
		auto delta = [&why](uint32_t now, uint32_t before, const char *name) {
			if (now > before)
				why << QStringLiteral("%1 +%2").arg(tr(name))
						.arg(now - before);
		};
		delta(m.dropped, m_lastMeta.dropped, QT_TR_NOOP("frames lost"));
		delta(m.noField, m_lastMeta.noField, QT_TR_NOOP("no field"));
		delta(m.fieldOrder, m_lastMeta.fieldOrder, QT_TR_NOOP("field order"));
		delta(m.lateIrq, m_lastMeta.lateIrq, QT_TR_NOOP("late interrupt"));
		delta(m.fifoOverflows, m_lastMeta.fifoOverflows, QT_TR_NOOP("FIFO overflow"));
		delta(m.codecRestarts, m_lastMeta.codecRestarts, QT_TR_NOOP("codec restarted"));
		delta(m.codecSkipped, m_lastMeta.codecSkipped, QT_TR_NOOP("skipped by the codec"));
		delta(m.framesRejected, m_lastMeta.framesRejected,
		      QT_TR_NOOP("dropped by the driver"));
		delta(m.pairingSwitches, m_lastMeta.pairingSwitches,
		      QT_TR_NOOP("field pairing switched"));
		if (!why.isEmpty())
			log(tr("Driver (frame %1): %2").arg(m.seq).arg(why.join(", ")));
	}
	m_lastMeta = m;
	m_haveMeta = true;
}

// Picks the first frame: the first one whose sound position lies at or
// after the first sound received, so the file starts with sound. Without
// sound positions it starts anyway after kMaxStartWait frames.
bool Recorder::tryStart(std::deque<VideoItem> &pending)
{
	if (pending.empty())
		return false;

	const dc30::AudioFit *fit = nullptr;

	if (m_haveAudio && !m_epochs.empty()) {
		auto it = m_fits.find(m_epochs[0]);

		if (it != m_fits.end() && it->second.valid())
			fit = &it->second;
	}
	if (fit) {
		while (!pending.empty() &&
		       fit->position(pending.front().seq) < double(m_firstAudioPos)) {
			freeItem(pending.front());
			pending.pop_front();
		}
		if (pending.empty())
			return false;
	} else if (pending.size() < kMaxStartWait) {
		return false;
	}

	const VideoItem &f = pending.front();

	if (!openOutput(f.width, f.height)) {
		m_openFailed = true;
		return false;
	}
	m_seqStart = f.seq;
	m_started = true;
	log(tr("Recording started: %1 (frame %2, %3x%4, input %5)")
		    .arg(m_path).arg(m_seqStart).arg(f.width).arg(f.height)
		    .arg(m_input));
	if (m_codec == Codec::Mjpeg)
		log(tr("Video MJPEG from the card's codec, both fields per frame, "
		       "top field first; audio PCM 16 bit stereo 44100 Hz"));
	else
		log(tr("Video FFV1 version 3, 4:2:2, interlaced top field first, "
		       "16 slices with CRC; audio PCM 16 bit stereo 44100 Hz"));
	if (!fit)
		log(tr("WARNING: no audio position from the driver - audio will be "
		       "placed later"));
	if (m_clock != DC30_META_CLOCK_VIDEOLOCK)
		log(tr("WARNING: audio clock is the crystal instead of video lock - the audio will be "
		       "realigned by %1 ms every few minutes").arg(1000 * kAudioTolerance / kRate));
	return true;
}

bool Recorder::handleVideo(VideoItem &v)
{
	const int64_t seq = v.seq;
	bool ok = true;

	if (m_lastSeq >= 0 && seq <= m_lastSeq) {
		log(tr("Frame number jumps back (%1 after %2) - realigned")
			    .arg(seq).arg(m_lastSeq));
		m_seqStart = seq - (m_repair ? m_nextK : m_frames);
	}

	const int64_t k = seq - m_seqStart;

	if (m_repair) {
		/* No repeats: a lost frame is a gap in the time stamps. */
		if (k > m_nextK) {
			log(tr("%1 frame(s) missing (frame %2 to %3)")
				    .arg(k - m_nextK).arg(seq - (k - m_nextK)).arg(seq - 1));
			m_lost += uint32_t(k - m_nextK);
		}
		m_nextK = k + 1;
		if (v.width == m_width && v.height == m_height) {
			m_repair->push(k, v.pkt, v.frame);
			v.pkt = nullptr;
			v.frame = nullptr;
		}
		ok = writeRepaired();
		m_lastSeq = seq;
		freeItem(v);
		return ok;
	}

	if (k > m_frames) {
		const int64_t missing = k - m_frames;
		int64_t queue = 0;
		{
			std::lock_guard<std::mutex> g(m_lock);

			for (auto it = m_queueDropped.begin();
			     it != m_queueDropped.end() && int64_t(*it) < seq;) {
				queue += int64_t(*it) > m_lastSeq;
				it = m_queueDropped.erase(it);
			}
		}
		log(tr("%1 frame(s) missing (frame %2 to %3)%4 - previous frame repeated")
			    .arg(missing).arg(seq - missing).arg(seq - 1)
			    .arg(queue ? tr(", %1 of them due to a full queue "
					    "(computer too slow?)").arg(queue)
				       : QString()));
		m_lost += uint32_t(missing);
		ok = repeatVideo(unsigned(missing));
	}
	if (ok && v.width == m_width && v.height == m_height)
		ok = v.pkt ? writeVideoPacket(v.pkt) : encodeVideo(v.frame, m_frames++, 1);
	else if (ok)
		ok = repeatVideo(1);
	m_lastSeq = seq;
	freeItem(v);
	return ok;
}

// Appends sound at file position fileIdx (audio frames): a gap before it
// becomes silence, an overlap with what is written already is skipped.
bool Recorder::writeAudio(int64_t fileIdx, const int16_t *s, size_t n)
{
	int64_t skip = m_audioWritten - fileIdx;

	if (skip < 0) {
		log(tr("Audio: %1 ms of silence inserted").arg(-skip * 1000.0 / kRate, 0, 'f', 1));
		m_silence += -skip;
		m_audioBuf.insert(m_audioBuf.end(), size_t(-skip) * 2, 0);
		m_audioWritten = fileIdx;
		skip = 0;
	}
	if (skip >= int64_t(n)) {
		if (fileIdx + int64_t(n) > 0)
			m_skipped += std::min<int64_t>(n, fileIdx + n);
		return true;
	}
	/* Sound from before the first frame is not an overlap. */
	if (skip > 0 && fileIdx + skip > 0)
		m_skipped += std::min(skip, fileIdx + skip);
	m_audioBuf.insert(m_audioBuf.end(), s + skip * 2, s + n * 2);
	m_audioWritten += int64_t(n) - skip;
	return flushAudioPackets(false);
}

// One packet per video frame's worth of sound. The sound never runs
// ahead of the video in the file; 'all' writes the rest.
bool Recorder::flushAudioPackets(bool all)
{
	size_t have = m_audioBuf.size() / 2, done = 0;
	bool ok = true;

	while (ok && have - done > 0) {
		size_t n = std::min<size_t>(kAudioPerFrame, have - done);

		if (!all && (n < kAudioPerFrame ||
			     m_audioPackets + int64_t(n) > m_frames * kAudioPerFrame))
			break;

		AVPacket *pkt = av_packet_alloc();

		if (!pkt || av_new_packet(pkt, int(n * 4)) < 0) {
			av_packet_free(&pkt);
			emit failed(tr("Out of memory for audio packet"));
			return false;
		}
		std::memcpy(pkt->data, m_audioBuf.data() + done * 2, n * 4);
		ok = writePacket(pkt, m_ast, m_audioPackets, int64_t(n));
		av_packet_free(&pkt);
		m_audioPackets += int64_t(n);
		done += n;
	}
	m_audioBuf.erase(m_audioBuf.begin(), m_audioBuf.begin() + done * 2);
	return ok;
}

// Places the waiting sound blocks, in order, once their ALSA stream's
// position in the file is known.
bool Recorder::drainAudio()
{
	size_t pending = 0;

	for (const auto &a : m_audioPending)
		pending += a.samples.size() / 2;

	while (!m_audioPending.empty()) {
		AudioItem &a = m_audioPending.front();
		auto off = m_offset.find(a.generation);

		if (off == m_offset.end()) {
			const size_t gi = size_t(generationIndex(a.generation));
			auto fit = gi < m_epochs.size() ? m_fits.find(m_epochs[gi])
							: m_fits.end();

			if (fit != m_fits.end() && fit->second.valid()) {
				const uint32_t s = m_lastSeqOfEpoch[m_epochs[gi]];
				const double ideal =
					double(int64_t(s) - m_seqStart) * kAudioPerFrame -
					fit->second.position(s);

				off = m_offset.emplace(a.generation, std::llround(ideal)).first;
				if (gi > 0)
					log(tr("Audio: new ALSA stream (after overrun) placed"));
			} else if (pending > kMaxPendingAudio) {
				pending -= a.samples.size() / 2;
				m_audioPending.pop_front();
				continue;
			} else {
				return true;
			}
		}
		if (!writeAudio(int64_t(a.pos) + off->second, a.samples.data(),
				a.samples.size() / 2))
			return false;
		pending -= a.samples.size() / 2;
		m_audioPending.pop_front();
	}
	return true;
}

// Is the newest ALSA stream still where the metadata says it belongs?
void Recorder::checkAudioOffset()
{
	if (m_generations.empty())
		return;

	const unsigned gen = m_generations.back();
	const size_t gi = m_generations.size() - 1;
	auto off = m_offset.find(gen);

	if (off == m_offset.end() || gi >= m_epochs.size())
		return;
	auto fit = m_fits.find(m_epochs[gi]);
	if (fit == m_fits.end() || !fit->second.valid())
		return;

	const uint32_t s = m_lastSeqOfEpoch[m_epochs[gi]];
	const double ideal = double(int64_t(s) - m_seqStart) * kAudioPerFrame -
			     fit->second.position(s);

	if (std::fabs(ideal - double(off->second)) <= kAudioTolerance)
		return;
	log(tr("Audio realigned by %1 ms (%2 samples per frame instead of %3)")
		    .arg((ideal - double(off->second)) * 1000.0 / kRate, 0, 'f', 1)
		    .arg(fit->second.slope(), 0, 'f', 3).arg(kAudioPerFrame));
	off->second = std::llround(ideal);
	m_audioFixes++;
}

void Recorder::run()
{
	if (!m_log.open(QIODevice::WriteOnly | QIODevice::Text))
		emit failed(tr("Cannot create log %1").arg(m_logPath));

	std::deque<VideoItem> video;
	bool ok = true, finishing = false;
	std::chrono::steady_clock::time_point tailEnd;
	int64_t lastProgress = -1;

	while (ok) {
		std::deque<VideoItem> v;
		std::deque<AudioItem> a;
		std::deque<MetaItem> m;
		bool fin;
		{
			std::unique_lock<std::mutex> g(m_lock);

			m_cv.wait_for(g, std::chrono::milliseconds(50), [this] {
				return !m_videoQ.empty() || !m_audioQ.empty() ||
				       !m_metaQ.empty() || m_finish;
			});
			v.swap(m_videoQ);
			a.swap(m_audioQ);
			m.swap(m_metaQ);
			fin = m_finish;
		}

		for (const auto &item : m)
			handleMeta(item);
		for (auto &item : a) {
			generationIndex(item.generation);
			if (!m_haveAudio) {
				m_haveAudio = true;
				m_firstAudioPos = item.pos;
			}
			m_audioPending.push_back(std::move(item));
		}
		for (auto &item : v)
			video.push_back(item);
		m_queued = int(video.size());

		if (!m_started && !tryStart(video)) {
			if (fin || m_openFailed)
				break;
			/* Without a start the queue would grow forever. */
			while (video.size() > kMaxQueued) {
				freeItem(video.front());
				video.pop_front();
			}
			continue;
		}
		while (ok && !video.empty()) {
			ok = handleVideo(video.front());
			video.pop_front();
		}
		if (!ok)
			break;
		checkAudioOffset();
		ok = drainAudio();

		if (m_frames / kFps != lastProgress) {
			lastProgress = m_frames / kFps;
			emit progress(double(m_frames) / kFps,
				      m_fmt && m_fmt->pb ? avio_tell(m_fmt->pb) : 0,
				      m_lost, m_audioFixes, m_queued);
		}

		/* After the last frame: wait a moment for its sound. */
		if (fin && !finishing) {
			finishing = true;
			if (m_repair) {
				m_repair->finish();
				ok = writeRepaired();
			}
			tailEnd = std::chrono::steady_clock::now() + kAudioTail;
		}
		if (finishing && (m_audioWritten >= m_frames * kAudioPerFrame ||
				  std::chrono::steady_clock::now() >= tailEnd))
			break;
	}

	for (auto &item : video)
		freeItem(item);

	if (m_started) {
		const int64_t target = m_frames * kAudioPerFrame;

		if (ok && m_audioWritten < target) {
			log(tr("End: %1 ms of silence added").arg((target - m_audioWritten) * 1000.0 / kRate,
								 0, 'f', 1));
			m_silence += target - m_audioWritten;
			m_audioBuf.insert(m_audioBuf.end(), size_t(target - m_audioWritten) * 2, 0);
			m_audioWritten = target;
		}
		/* Sound past the last frame is not written. */
		const int64_t extra = m_audioWritten - target;
		if (extra > 0)
			m_audioBuf.resize(m_audioBuf.size() - std::min<size_t>(m_audioBuf.size(),
									       size_t(extra) * 2));
		if (ok)
			ok = flushAudioPackets(true);

		const qint64 bytes = m_fmt && m_fmt->pb ? avio_tell(m_fmt->pb) : 0;
		closeOutput();

		const QString sum =
			tr("Recording finished: %1 frames (%2), %3 MB, %4 lost, "
			   "audio %5 ms silence / %6 ms skipped, %7 realignments")
				.arg(m_frames).arg(when(m_frames))
				.arg(bytes / 1000000)
				.arg(m_lost)
				.arg(m_silence * 1000.0 / kRate, 0, 'f', 1)
				.arg(m_skipped * 1000.0 / kRate, 0, 'f', 1)
				.arg(m_audioFixes);
		if (m_repair) {
			const auto &st = m_repair->stats();

			log(tr("Fields paired by the picture: %1 % decisive, %2 slips of the field grid, "
			       "%3 single fields (20 ms frames)")
				    .arg(st.fields ? 100.0 * st.decisive / st.fields : 0.0, 0, 'f', 1)
				    .arg(st.jumps).arg(st.singles));
		}
		log(sum);
		emit summary(sum);
	} else {
		emit summary(tr("Recording aborted before a frame was written"));
	}
	m_log.close();
}

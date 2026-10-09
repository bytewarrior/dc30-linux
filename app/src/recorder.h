// SPDX-License-Identifier: GPL-2.0-only
// Recorder thread: writes the card's video (FFV1, lossless, 4:2:2
// interlaced - or the MJPEG of the card's own codec, as it comes) and
// sound (PCM) to Matroska, on the card's timeline.
//
// The capture threads push every video frame, audio block and metadata
// record. Output frame k is the frame with sequence number seqStart + k;
// a frame the driver lost (or the queue had to drop) is filled by
// repeating the previous one. The sound is the ALSA stream itself, placed
// so that frame k starts at audio frame k * 1764: in video lock that is
// exact, so no resampling. Where the metadata says the sound is off by
// more than kAudioTolerance (crystal clock, an ALSA restart after an
// xrun), silence is inserted or samples are skipped. Every such event
// goes to a log file next to the recording.
#pragma once

#include "audiofit.h"
#include "dc30_meta.h"
#include "fieldrepair.h"

#include <QFile>
#include <QString>
#include <QThread>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct AVStream;

class Recorder : public QThread {
	Q_OBJECT
public:
	static constexpr unsigned kRate = 44100;
	static constexpr unsigned kFps = 25;
	static constexpr unsigned kAudioPerFrame = kRate / kFps;	// 1764

	enum class Codec { Ffv1, Mjpeg };

	// 'input': name of the video input, for the log and the file tags.
	// 'codec': FFV1 encodes the pushVideo() frames, MJPEG writes the
	// pushVideoPacket() frames unchanged. 'fieldRepair': pair the fields
	// by the picture (FieldRepair): every frame then has the time of its
	// first field, a field left alone by a slip of the signal's field
	// grid is a 20 ms frame, lost frames leave a gap instead of a repeat.
	Recorder(QString path, QString input, Codec codec, bool fieldRepair,
		 QObject *parent = nullptr);
	~Recorder() override;

	QString path() const { return m_path; }
	QString logPath() const { return m_logPath; }
	// Frames the driver or the queue lost (filled in by repeating the
	// previous one, or a gap in the time stamps with field repair).
	quint32 lost() const { return m_lost; }

	// From the capture threads. Cheap: pushVideo converts to planar
	// 4:2:2 and queues, the rest only queue.
	void pushVideo(uint32_t seq, const uint8_t *yuyv, int width, int height,
		       int stride);
	// MJPEG: one frame = the two field JPEGs of the card, copied.
	void pushVideoPacket(uint32_t seq, const uint8_t *data, size_t size,
			     int width, int height);
	void pushAudio(unsigned generation, uint64_t pos, const int16_t *frames,
		       unsigned count);
	void pushMeta(const dc30_meta &meta);

	// Take no more video; write the sound up to the last frame, close
	// the file, end the thread (then finished() is emitted).
	void finish();

signals:
	// About once a second.
	void progress(double seconds, qint64 bytes, quint32 lost,
		      quint32 audioFixes, int queued);
	void failed(const QString &message);
	// Last words for the status bar: summary of the recording.
	void summary(const QString &text);

protected:
	void run() override;

private:
	// Either an FFV1 input frame or an MJPEG packet.
	struct VideoItem {
		uint32_t seq;
		AVFrame *frame;
		AVPacket *pkt;
		int width, height;
	};
	struct AudioItem {
		unsigned generation;
		uint64_t pos;
		std::vector<int16_t> samples;
	};
	struct MetaItem {
		uint32_t seq;
		double pos;		// audio frame index, if audio
		uint32_t epoch;
		bool audio;
		uint16_t clock;
		uint32_t dropped, noField, lateIrq, fieldOrder, fifoOverflows;
		uint32_t codecRestarts;
		uint32_t codecSkipped, framesRejected;
		uint32_t pairingSwitches;	// cut or field jump (MJPEG)
	};
	bool m_jumpBefore = false;	// the last record had a field jump

	bool openOutput(int width, int height);
	bool addFfv1Stream(int width, int height);
	void addMjpegStream(int width, int height);
	void closeOutput();
	bool writePacket(AVPacket *pkt, AVStream *st, int64_t pts, int64_t duration);
	bool encodeVideo(AVFrame *frame, int64_t pts, int64_t duration);
	bool writeVideoPacket(AVPacket *pkt);
	bool writeRepaired();
	void logAt(int64_t frame, const QString &line);
	static void freeItem(VideoItem &v);
	bool repeatVideo(unsigned count);
	void handleMeta(const MetaItem &m);
	bool tryStart(std::deque<VideoItem> &pending);
	bool handleVideo(VideoItem &v);
	bool drainAudio();
	bool writeAudio(int64_t fileIdx, const int16_t *s, size_t n);
	bool flushAudioPackets(bool all);
	void checkAudioOffset();
	int epochIndex(uint32_t epoch);
	int generationIndex(unsigned generation);
	void log(const QString &line);
	QString when(int64_t frame) const;

	const QString m_path, m_logPath, m_input;
	const Codec m_codec;
	std::unique_ptr<dc30::FieldRepair> m_repair;
	// Video time base: frames, or fields with field repair.
	const AVRational m_videoBase;
	QFile m_log;

	// Queues, filled by the capture threads.
	std::mutex m_lock;
	std::condition_variable m_cv;
	std::deque<VideoItem> m_videoQ;
	std::deque<AudioItem> m_audioQ;
	std::deque<MetaItem> m_metaQ;
	std::set<uint32_t> m_queueDropped;	// sequence numbers dropped here
	bool m_finish = false;

	// Worker state.
	AVFormatContext *m_fmt = nullptr;
	AVCodecContext *m_enc = nullptr;
	AVStream *m_vst = nullptr, *m_ast = nullptr;
	AVPacket *m_lastPkt = nullptr;		// last frame, for repeats
	int m_width = 0, m_height = 0;
	bool m_started = false;			// seqStart chosen
	bool m_openFailed = false;
	int64_t m_seqStart = 0;
	int64_t m_lastSeq = -1;
	int64_t m_frames = 0;			// video frames written (k);
						// field repair: frame slots covered
	int64_t m_nextK = 0;			// field repair: next frame expected
	std::map<int64_t, int64_t> m_durations;	// FFV1 pts -> duration
	bool m_crossed = false;			// field repair: pairing across

	// Audio. ALSA streams (generations) and metadata epochs both start
	// over on every ALSA restart and are paired in order of appearance.
	std::vector<unsigned> m_generations;
	std::vector<uint32_t> m_epochs;
	std::map<uint32_t, dc30::AudioFit> m_fits;	// by epoch
	std::map<unsigned, int64_t> m_offset;	// by generation: file idx - stream pos
	std::map<uint32_t, uint32_t> m_lastSeqOfEpoch;
	std::deque<AudioItem> m_audioPending;
	uint64_t m_firstAudioPos = 0;		// of the first generation
	bool m_haveAudio = false;
	std::vector<int16_t> m_audioBuf;	// not yet in a packet
	int64_t m_audioWritten = 0;		// audio frames in the file + buffer
	int64_t m_audioPackets = 0;		// audio frames in packets

	// Statistics.
	MetaItem m_lastMeta{};
	bool m_haveMeta = false;
	uint16_t m_clock = 0;
	uint32_t m_repeated = 0, m_lost = 0, m_audioFixes = 0;
	int64_t m_silence = 0, m_skipped = 0;
	int m_queued = 0;
};

// SPDX-License-Identifier: GPL-2.0-only
// Video capture thread: streams /dev/videoN - raw YUYV, or MJPEG from the
// card's own codec - and hands out a preview image per frame.
#pragma once

#include <QImage>
#include <QThread>
#include <atomic>
#include <mutex>
#include <string>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
class Recorder;

class VideoCapture : public QThread {
	Q_OBJECT
public:
	enum class Format { Yuyv, Mjpeg };

	// 'mjpegKbps': data rate of the card's codec, kB/s (MJPEG only).
	VideoCapture(std::string device, Format format, int mjpegKbps,
		     QObject *parent = nullptr);
	~VideoCapture() override;

	Format format() const { return m_format; }

	// V4L2 input: 0 Composite, 1 S-Video, 2 Internal. Works while
	// streaming.
	bool setInput(int input);
	// MJPEG: false = the codec's frames as they come (the recorder pairs
	// the fields by the picture), true = the driver pairs them by the
	// decoder's field flag. Takes effect with the next frame; false
	// with a driver without the control.
	bool setPairByFlag(bool on);
	void stop() { m_stop = true; }
	// Hand every frame to 'recorder' too (nullptr: stop).
	void setRecorder(Recorder *recorder);

signals:
	// Preview of every frame: its first field, line-doubled (no
	// combing). The preview queues them until they are due.
	void frameReady(const QImage &preview, quint32 sequence, qint64 timestampNs);
	void failed(const QString &message);
	void statsChanged(quint64 frames, quint64 lost);

protected:
	void run() override;

private:
	QImage makePreview(const uint8_t *yuyv, int width, int height, int stride);
	QImage makePreviewMjpeg(const uint8_t *data, size_t size);
	bool setFormat(int *width, int *height, int *stride);

	std::string m_device;
	const Format m_format;
	const int m_kbps;
	AVCodecContext *m_dec = nullptr;	// MJPEG preview
	AVFrame *m_decFrame = nullptr;
	AVPacket *m_decPkt = nullptr;
	int m_fd = -1;
	std::atomic<bool> m_stop{false};

	std::mutex m_recorderLock;
	Recorder *m_recorder = nullptr;
};

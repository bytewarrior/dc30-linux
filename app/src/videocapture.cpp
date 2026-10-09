// SPDX-License-Identifier: GPL-2.0-only
#include "videocapture.h"

#include "recorder.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace {

constexpr int kBuffers = 6;

int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do
		r = ioctl(fd, req, arg);
	while (r < 0 && errno == EINTR);
	return r;
}

inline uint8_t clamp8(int v)
{
	return uint8_t(std::clamp(v, 0, 255));
}

// BT.601 limited range -> RGB32.
inline uint32_t rgb601(int y, int u, int v)
{
	const int c = 298 * (y - 16) + 128;

	u -= 128;
	v -= 128;
	return 0xff000000u | clamp8((c + 409 * v) >> 8) << 16 |
	       clamp8((c - 100 * u - 208 * v) >> 8) << 8 |
	       clamp8((c + 516 * u) >> 8);
}

// End of the first JPEG in an MJPEG frame (its EOI): the entropy coded
// data never contains FF D9, 0xff there is always stuffed with a 0.
size_t firstJpegSize(const uint8_t *d, size_t n)
{
	for (size_t i = 2; i + 1 < n; i++)
		if (d[i] == 0xff && d[i + 1] == 0xd9)
			return i + 2;
	return n;
}

} // namespace

VideoCapture::VideoCapture(std::string device, Format format, int mjpegKbps,
			   QObject *parent)
	: QThread(parent), m_device(std::move(device)), m_format(format),
	  m_kbps(mjpegKbps)
{
	m_fd = ::open(m_device.c_str(), O_RDWR);
}

VideoCapture::~VideoCapture()
{
	stop();
	wait();
	if (m_fd >= 0)
		::close(m_fd);
	avcodec_free_context(&m_dec);
	av_frame_free(&m_decFrame);
	av_packet_free(&m_decPkt);
}

// The driver keeps the last format set, whoever set it: always set ours.
bool VideoCapture::setFormat(int *width, int *height, int *stride)
{
	const uint32_t want = m_format == Format::Mjpeg ? V4L2_PIX_FMT_MJPEG
						       : V4L2_PIX_FMT_YUYV;
	v4l2_format fmt{};

	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl(m_fd, VIDIOC_G_FMT, &fmt) < 0) {
		emit failed(tr("VIDIOC_G_FMT failed"));
		return false;
	}
	fmt.fmt.pix.pixelformat = want;
	if (xioctl(m_fd, VIDIOC_S_FMT, &fmt) < 0 || fmt.fmt.pix.pixelformat != want) {
		emit failed(m_format == Format::Mjpeg
				    ? tr("The driver offers no MJPEG (dc30 module too old?)")
				    : tr("VIDIOC_S_FMT failed"));
		return false;
	}
	if (m_format == Format::Mjpeg) {
		v4l2_control c{V4L2_CID_MPEG_VIDEO_BITRATE, m_kbps * 8000};

		if (xioctl(m_fd, VIDIOC_S_CTRL, &c) < 0) {
			emit failed(tr("Cannot set the MJPEG data rate to %1 kB/s")
					    .arg(m_kbps));
			return false;
		}
	}
	*width = fmt.fmt.pix.width;
	*height = fmt.fmt.pix.height;
	*stride = fmt.fmt.pix.bytesperline;
	return true;
}

bool VideoCapture::setInput(int input)
{
	return m_fd >= 0 && xioctl(m_fd, VIDIOC_S_INPUT, &input) == 0;
}

bool VideoCapture::setPairByFlag(bool on)
{
	v4l2_control c{DC30_CID_PAIR_BY_FLAG, on ? 1 : 0};

	return m_fd >= 0 && xioctl(m_fd, VIDIOC_S_CTRL, &c) == 0;
}

void VideoCapture::setRecorder(Recorder *recorder)
{
	std::lock_guard<std::mutex> g(m_recorderLock);

	m_recorder = recorder;
}

// BT.601 limited range YUYV -> RGB32, first field only, each line twice.
QImage VideoCapture::makePreview(const uint8_t *yuyv, int width, int height,
				 int stride)
{
	QImage img(width, height, QImage::Format_RGB32);

	for (int y = 0; y + 1 < height; y += 2) {
		const uint8_t *src = yuyv + size_t(y) * stride;
		auto *dst = reinterpret_cast<uint32_t *>(img.scanLine(y));

		for (int x = 0; x + 1 < width; x += 2, src += 4) {
			dst[x] = rgb601(src[0], src[1], src[3]);
			dst[x + 1] = rgb601(src[2], src[1], src[3]);
		}
		std::memcpy(img.scanLine(y + 1), img.scanLine(y), width * 4);
	}
	return img;
}

// MJPEG: the first field only, decoded at half size (the codec's lowres
// mode skips most of the work - the preview must stay cheap, the point of
// MJPEG is a computer too slow for FFV1), each line twice. The JPEGs
// carry BT.601 limited range like the raw frames, whatever the decoder's
// "yuvj" format claims.
QImage VideoCapture::makePreviewMjpeg(const uint8_t *data, size_t size)
{
	if (!m_dec) {
		const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);

		if (!codec)
			return {};
		m_dec = avcodec_alloc_context3(codec);
		m_dec->lowres = 1;
		m_dec->thread_count = 1;
		m_decFrame = av_frame_alloc();
		m_decPkt = av_packet_alloc();
		if (avcodec_open2(m_dec, codec, nullptr) < 0) {
			avcodec_free_context(&m_dec);
			return {};
		}
	}

	/* The decoder only reads the packet during the send. */
	m_decPkt->data = const_cast<uint8_t *>(data);
	m_decPkt->size = int(firstJpegSize(data, size));
	if (avcodec_send_packet(m_dec, m_decPkt) < 0 ||
	    avcodec_receive_frame(m_dec, m_decFrame) < 0)
		return {};

	const AVFrame *f = m_decFrame;
	const bool planar422 = f->format == AV_PIX_FMT_YUVJ422P ||
			       f->format == AV_PIX_FMT_YUV422P;
	QImage img(f->width, f->height * 2, QImage::Format_RGB32);

	if (!planar422)
		return {};
	for (int y = 0; y < f->height; y++) {
		const uint8_t *py = f->data[0] + size_t(y) * f->linesize[0];
		const uint8_t *pu = f->data[1] + size_t(y) * f->linesize[1];
		const uint8_t *pv = f->data[2] + size_t(y) * f->linesize[2];
		auto *dst = reinterpret_cast<uint32_t *>(img.scanLine(2 * y));

		for (int x = 0; x < f->width; x++)
			dst[x] = rgb601(py[x], pu[x / 2], pv[x / 2]);
		std::memcpy(img.scanLine(2 * y + 1), dst, size_t(f->width) * 4);
	}
	av_frame_unref(m_decFrame);
	return img;
}

void VideoCapture::run()
{
	if (m_fd < 0) {
		emit failed(tr("Cannot open video device %1: %2")
				    .arg(QString::fromStdString(m_device),
					 QString::fromLocal8Bit(strerror(errno))));
		return;
	}

	int width, height, stride;

	if (!setFormat(&width, &height, &stride))
		return;

	v4l2_requestbuffers req{};
	req.count = kBuffers;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(m_fd, VIDIOC_REQBUFS, &req) < 0) {
		emit failed(tr("VIDIOC_REQBUFS failed (device busy?)"));
		return;
	}

	std::vector<std::pair<void *, size_t>> maps;
	for (unsigned i = 0; i < req.count; i++) {
		v4l2_buffer b{};
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		xioctl(m_fd, VIDIOC_QUERYBUF, &b);
		void *p = mmap(nullptr, b.length, PROT_READ, MAP_SHARED, m_fd,
			       b.m.offset);
		maps.emplace_back(p, b.length);
		xioctl(m_fd, VIDIOC_QBUF, &b);
	}

	v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (xioctl(m_fd, VIDIOC_STREAMON, &type) < 0) {
		emit failed(tr("VIDIOC_STREAMON failed"));
		return;
	}

	quint64 frames = 0, lost = 0;
	long long lastSeq = -1;

	while (!m_stop) {
		pollfd pfd{m_fd, POLLIN, 0};

		if (poll(&pfd, 1, 500) <= 0)
			continue;

		v4l2_buffer b{};
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b.memory = V4L2_MEMORY_MMAP;
		if (xioctl(m_fd, VIDIOC_DQBUF, &b) < 0)
			continue;

		frames++;
		if (lastSeq >= 0 && b.sequence > lastSeq + 1)
			lost += b.sequence - lastSeq - 1;
		lastSeq = b.sequence;

		const qint64 ts = qint64(b.timestamp.tv_sec) * 1000000000 +
				  b.timestamp.tv_usec * 1000;

		const auto *data = static_cast<const uint8_t *>(maps[b.index].first);
		const bool mjpeg = m_format == Format::Mjpeg;
		{
			std::lock_guard<std::mutex> g(m_recorderLock);

			if (m_recorder && mjpeg)
				m_recorder->pushVideoPacket(b.sequence, data, b.bytesused,
							    width, height);
			else if (m_recorder)
				m_recorder->pushVideo(b.sequence, data, width, height,
						      stride);
		}
		QImage preview = mjpeg ? makePreviewMjpeg(data, b.bytesused)
				       : makePreview(data, width, height, stride);
		if (!preview.isNull())
			emit frameReady(preview, b.sequence, ts);
		if (frames % 25 == 0)
			emit statsChanged(frames, lost);

		xioctl(m_fd, VIDIOC_QBUF, &b);
	}

	xioctl(m_fd, VIDIOC_STREAMOFF, &type);
	for (auto &m : maps)
		munmap(m.first, m.second);
	req.count = 0;
	xioctl(m_fd, VIDIOC_REQBUFS, &req);
}

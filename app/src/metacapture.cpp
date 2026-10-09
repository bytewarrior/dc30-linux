// SPDX-License-Identifier: GPL-2.0-only
#include "metacapture.h"

#include "dc30_meta.h"
#include "recorder.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr int kBuffers = 16;

int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do
		r = ioctl(fd, req, arg);
	while (r < 0 && errno == EINTR);
	return r;
}

} // namespace

MetaCapture::MetaCapture(std::string device, QObject *parent)
	: QThread(parent), m_device(std::move(device))
{
	m_fd = ::open(m_device.c_str(), O_RDWR);
}

MetaCapture::~MetaCapture()
{
	stop();
	wait();
	if (m_fd >= 0)
		::close(m_fd);
}

bool MetaCapture::audioPosition(uint32_t sequence, double *pos,
				uint32_t *epoch) const
{
	std::lock_guard<std::mutex> g(m_lock);

	if (!m_fit.valid())
		return false;
	*pos = m_fit.position(sequence);
	*epoch = m_fit.epoch();
	return true;
}

void MetaCapture::resetFit()
{
	std::lock_guard<std::mutex> g(m_lock);

	m_fit.reset();
}

void MetaCapture::setRecorder(Recorder *recorder)
{
	std::lock_guard<std::mutex> g(m_recorderLock);

	m_recorder = recorder;
}

void MetaCapture::run()
{
	if (m_fd < 0) {
		emit failed(tr("Cannot open metadata device %1: %2")
				    .arg(QString::fromStdString(m_device),
					 QString::fromLocal8Bit(strerror(errno))));
		return;
	}

	v4l2_requestbuffers req{};
	req.count = kBuffers;
	req.type = V4L2_BUF_TYPE_META_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(m_fd, VIDIOC_REQBUFS, &req) < 0) {
		emit failed(tr("Metadata: VIDIOC_REQBUFS failed"));
		return;
	}
	std::vector<std::pair<void *, size_t>> maps;
	for (unsigned i = 0; i < req.count; i++) {
		v4l2_buffer b{};
		b.type = V4L2_BUF_TYPE_META_CAPTURE;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		xioctl(m_fd, VIDIOC_QUERYBUF, &b);
		maps.emplace_back(mmap(nullptr, b.length, PROT_READ, MAP_SHARED,
				       m_fd, b.m.offset), b.length);
		xioctl(m_fd, VIDIOC_QBUF, &b);
	}
	v4l2_buf_type type = V4L2_BUF_TYPE_META_CAPTURE;
	if (xioctl(m_fd, VIDIOC_STREAMON, &type) < 0) {
		emit failed(tr("Metadata: VIDIOC_STREAMON failed"));
		return;
	}

	unsigned records = 0;
	uint32_t pairingSwitches = 0;	// of the last record
	uint32_t fieldOrder = 0;

	while (!m_stop) {
		pollfd pfd{m_fd, POLLIN, 0};

		if (poll(&pfd, 1, 500) <= 0)
			continue;

		v4l2_buffer b{};
		b.type = V4L2_BUF_TYPE_META_CAPTURE;
		b.memory = V4L2_MEMORY_MMAP;
		if (xioctl(m_fd, VIDIOC_DQBUF, &b) < 0)
			continue;

		const auto *m = static_cast<const dc30_meta *>(maps[b.index].first);

		if (m->version == DC30_META_VERSION && m->size == sizeof(*m) &&
		    (m->field[0].flags & DC30_META_FIELD_AUDIO) &&
		    m->audio_frame_bytes) {
			std::lock_guard<std::mutex> g(m_lock);

			m_fit.add(m->sequence,
				  double(m->field[0].audio_pos) / m->audio_frame_bytes,
				  m->field[0].audio_epoch,
				  m->pairing_switches != pairingSwitches ||
				  m->field_order != fieldOrder);
		}
		pairingSwitches = m->pairing_switches;
		fieldOrder = m->field_order;
		{
			std::lock_guard<std::mutex> g(m_recorderLock);

			if (m_recorder)
				m_recorder->pushMeta(*m);
		}

		if (++records % 25 == 0) {
			double slope, scatter;
			bool have;
			{
				std::lock_guard<std::mutex> g(m_lock);
				have = m_fit.valid();
				slope = m_fit.slope();
				scatter = m_fit.scatter();
			}
			emit syncInfo(have ? slope : 0, have ? scatter : 0,
				      m->audio_clock, m->frames_dropped,
				      m->no_field, m->field_order);
		}
		xioctl(m_fd, VIDIOC_QBUF, &b);
	}

	xioctl(m_fd, VIDIOC_STREAMOFF, &type);
	for (auto &mp : maps)
		munmap(mp.first, mp.second);
	req.count = 0;
	xioctl(m_fd, VIDIOC_REQBUFS, &req);
}

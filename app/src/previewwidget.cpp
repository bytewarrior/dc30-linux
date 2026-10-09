// SPDX-License-Identifier: GPL-2.0-only
#include "previewwidget.h"

#include "clock.h"

#include <QPainter>
#include <utility>

namespace {

// 1 s of frames. The sound's delay is ~150 ms; more than this means the
// clock doesn't fit the frames (e.g. an audio restart) - they are shown
// anyway rather than piling up.
constexpr size_t kMaxQueue = 25;
constexpr int kTickMs = 4;

} // namespace

PreviewWidget::PreviewWidget(QWidget *parent) : QWidget(parent)
{
	setMinimumSize(384, 288);
	setAttribute(Qt::WA_OpaquePaintEvent);
	m_timer.setTimerType(Qt::PreciseTimer);
	connect(&m_timer, &QTimer::timeout, this, &PreviewWidget::present);
}

void PreviewWidget::setPresentationClock(DueFn due)
{
	m_due = std::move(due);
	if (m_due)
		m_timer.start(kTickMs);
	else
		m_timer.stop();
	present();
}

void PreviewWidget::setFrame(const QImage &image, quint32 sequence, qint64)
{
	m_queue.push_back({image, sequence});
	m_message.clear();
	present();
}

void PreviewWidget::setMessage(const QString &text)
{
	m_message = text;
	update();
}

// Show the newest due frame, drop the ones before it.
void PreviewWidget::present()
{
	const int64_t now = dc30::monotonicNs();
	bool shown = false;

	while (!m_queue.empty()) {
		const bool due = !m_due || m_queue.size() > kMaxQueue ||
				 m_due(m_queue.front().sequence, now);
		if (!due)
			break;
		if (shown)
			m_skipped++;
		m_image = std::move(m_queue.front().image);
		m_queue.pop_front();
		shown = true;
	}
	if (shown)
		update();
}

void PreviewWidget::paintEvent(QPaintEvent *)
{
	QPainter p(this);

	p.fillRect(rect(), Qt::black);
	if (!m_image.isNull()) {
		QSize s(width(), width() * 3 / 4);

		if (s.height() > height())
			s = QSize(height() * 4 / 3, height());
		QRect r(QPoint((width() - s.width()) / 2,
			       (height() - s.height()) / 2), s);

		p.setRenderHint(QPainter::SmoothPixmapTransform);
		p.drawImage(r, m_image);
	}
	if (!m_message.isEmpty()) {
		p.setPen(Qt::white);
		p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, m_message);
	}
}

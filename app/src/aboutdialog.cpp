// SPDX-License-Identifier: GPL-2.0-only
#include "aboutdialog.h"

#include "devices.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QSysInfo>
#include <QVBoxLayout>
#include <alsa/asoundlib.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

extern "C" {
#include <libavutil/avutil.h>
}

namespace {

QString readSysfs(const QString &path)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
		return {};
	return QString::fromUtf8(f.readAll()).trimmed();
}

struct DriverInfo {
	QString driver, card, pciAddress;	// empty: no dc30 node
};

// VIDIOC_QUERYCAP on the dc30 video node.
DriverInfo queryDriver()
{
	DriverInfo info;
	const std::string node = dc30::videoNode();
	if (node.empty())
		return info;
	int fd = ::open(node.c_str(), O_RDWR | O_NONBLOCK);
	if (fd < 0)
		return info;
	v4l2_capability cap = {};
	if (::ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
		info.driver = QString::fromUtf8(reinterpret_cast<const char *>(cap.driver));
		info.card = QString::fromUtf8(reinterpret_cast<const char *>(cap.card));
		const QString bus = QString::fromUtf8(reinterpret_cast<const char *>(cap.bus_info));
		if (bus.startsWith(QLatin1String("PCI:")))
			info.pciAddress = bus.mid(4);
	}
	::close(fd);
	return info;
}

// ALSA's long name of the card ("miro DC30 (AD1843) at ..., irq ...").
QString alsaCardName()
{
	snd_ctl_t *ctl;
	if (snd_ctl_open(&ctl, dc30::alsaDevice, 0) < 0)
		return {};
	snd_ctl_card_info_t *info;
	snd_ctl_card_info_alloca(&info);
	QString name;
	if (snd_ctl_card_info(ctl, info) == 0)
		name = QString::fromUtf8(snd_ctl_card_info_get_longname(info));
	snd_ctl_close(ctl);
	return name;
}

QString row(const QString &label, const QString &value)
{
	return QStringLiteral("<tr><td style='padding-right:12px'>%1</td><td>%2</td></tr>")
		.arg(label.toHtmlEscaped(), value.toHtmlEscaped());
}

QString heading(const QString &text)
{
	return QStringLiteral("<tr><td colspan='2' style='padding-top:8px'><b>%1</b></td></tr>")
		.arg(text.toHtmlEscaped());
}

} // namespace

AboutDialog::AboutDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(tr("About DC30 Capture"));

	const QString unknown = tr("unknown");
	const QString none = tr("not loaded");
	auto orElse = [](const QString &s, const QString &alt) { return s.isEmpty() ? alt : s; };

	const DriverInfo drv = queryDriver();
	const QString dc30Src = readSysfs(QStringLiteral("/sys/module/dc30/srcversion"));
	const QString vpxSrc = readSysfs(QStringLiteral("/sys/module/dc30_vpx3220/srcversion"));

	QString html = QStringLiteral("<table cellspacing='0' cellpadding='1'>");
	html += heading(tr("Software"));
	html += row(tr("Qt"), QString::fromLatin1(qVersion()));
	html += row(tr("FFmpeg"), QString::fromUtf8(av_version_info()));
	html += row(tr("ALSA library"), QString::fromUtf8(snd_asoundlib_version()));

	html += heading(tr("Driver"));
	html += row(tr("Kernel"), QSysInfo::kernelVersion());
	html += row(tr("V4L2 driver"), orElse(drv.driver, none));
	html += row(tr("dc30 module"),
		    dc30Src.isEmpty() ? none : tr("srcversion %1").arg(dc30Src));
	html += row(tr("dc30_vpx3220 module"),
		    vpxSrc.isEmpty() ? none : tr("srcversion %1").arg(vpxSrc));

	html += heading(tr("Card"));
	html += row(tr("Name"), orElse(drv.card, unknown));
	if (!drv.pciAddress.isEmpty()) {
		const QString dev = QStringLiteral("/sys/bus/pci/devices/") + drv.pciAddress;
		bool ok;
		const int rev = readSysfs(dev + QStringLiteral("/revision")).toInt(&ok, 16);
		html += row(tr("PCI"), drv.pciAddress);
		html += row(tr("ZR36057"), ok ? tr("revision %1").arg(rev) : unknown);
	}
	html += row(tr("ALSA"), orElse(alsaCardName(), none));
	// The DC30's components (the driver reports no revisions for them).
	html += row(tr("Video decoder"), QStringLiteral("VPX3220A"));
	html += row(tr("Video encoder"), QStringLiteral("ADV7176"));
	html += row(tr("JPEG codec"), QStringLiteral("ZR36050, ZR36016"));
	html += row(tr("Audio codec"), QStringLiteral("AD1843"));
	html += row(tr("Video crosspoint"), QStringLiteral("TEA6415C"));
	html += QStringLiteral("</table>");

	auto *logo = new QLabel(this);
	QPixmap pix(QStringLiteral(":/app_logo.png"));
	const qreal dpr = devicePixelRatioF();
	pix = pix.scaledToWidth(qRound(220 * dpr), Qt::SmoothTransformation);
	pix.setDevicePixelRatio(dpr);
	logo->setPixmap(pix);
	logo->setAlignment(Qt::AlignTop);

	auto *title = new QLabel(QStringLiteral("<h2>%1</h2>%2")
					 .arg(tr("DC30 Capture"),
					      tr("Version %1").arg(QApplication::applicationVersion())),
				 this);
	auto *about = new QLabel(tr("Live preview, level metering and audio/video recording "
				    "(lossless FFV1 or the card's MJPEG) for the miro DC30 "
				    "with the dc30 Linux driver."),
				 this);
	about->setWordWrap(true);
	auto *details = new QLabel(html, this);
	details->setTextInteractionFlags(Qt::TextSelectableByMouse);
	auto *license = new QLabel(tr("License: GPL-2.0-only"), this);

	auto *text = new QVBoxLayout;
	text->addWidget(title);
	text->addWidget(about);
	text->addWidget(details);
	text->addWidget(license);
	text->addStretch(1);

	auto *top = new QHBoxLayout;
	top->addWidget(logo);
	top->addSpacing(12);
	top->addLayout(text, 1);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	auto *layout = new QVBoxLayout(this);
	layout->addLayout(top);
	layout->addWidget(buttons);
}

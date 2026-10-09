// SPDX-License-Identifier: GPL-2.0-only
#include "colorscheme.h"

#include <QApplication>
#include <QDBusConnection>
#include <QDBusVariant>
#include <QPalette>
#include <QStyle>

namespace dc30 {

namespace {

bool isDark(const QPalette &p)
{
	return p.color(QPalette::Window).lightness() < 128;
}

bool samePalette(const QPalette &a, const QPalette &b)
{
	for (int g = 0; g < QPalette::NColorGroups; g++)
		for (int r = 0; r < QPalette::NColorRoles; r++) {
			const auto group = QPalette::ColorGroup(g);
			const auto role = QPalette::ColorRole(r);

			if (a.color(group, role) != b.color(group, role))
				return false;
		}
	return true;
}

} // namespace

ColorScheme::ColorScheme(QObject *parent) : QObject(parent)
{
	const QPalette start = QApplication::palette();

	if (QApplication::style()->name() != QLatin1String("fusion") ||
	    !samePalette(start, fusionPalette(isDark(start))))
		return;
	QDBusConnection::sessionBus().connect(
		QStringLiteral("org.freedesktop.portal.Desktop"),
		QStringLiteral("/org/freedesktop/portal/desktop"),
		QStringLiteral("org.freedesktop.portal.Settings"),
		QStringLiteral("SettingChanged"), this,
		SLOT(settingChanged(QString, QString, QDBusVariant)));
}

// color-scheme: 1 dark, 2 light, 0 no preference (Cinnamon's "mixed": light
// windows).
void ColorScheme::settingChanged(const QString &ns, const QString &key,
				 const QDBusVariant &value)
{
	if (ns != QLatin1String("org.freedesktop.appearance") ||
	    key != QLatin1String("color-scheme"))
		return;

	const bool dark = value.variant().toUInt() == 1;

	if (dark != isDark(QApplication::palette()))
		QApplication::setPalette(fusionPalette(dark));
}

QPalette ColorScheme::fusionPalette(bool dark)
{
	const QColor windowText = dark ? QColor(240, 240, 240) : QColor(Qt::black);
	const QColor background = dark ? QColor(50, 50, 50) : QColor(239, 239, 239);
	const QColor light = background.lighter(150);
	const QColor mid = background.darker(130);
	const QColor midLight = mid.lighter(110);
	const QColor base = dark ? background.darker(140) : QColor(Qt::white);
	const QColor shade = background.darker(150);
	const QColor shadeDisabled = QColor(209, 209, 209).darker(110);
	const QColor text = dark ? windowText : QColor(Qt::black);
	const QColor highlight(48, 140, 198);
	const QColor highlightedText = dark ? windowText : QColor(Qt::white);
	const QColor disabledText = dark ? QColor(130, 130, 130) : QColor(190, 190, 190);
	const QColor shadow = shade.darker(135);
	QColor placeholder = text;

	placeholder.setAlpha(128);

	QPalette p(windowText, background, light, shade, mid, text, base);
	p.setBrush(QPalette::Midlight, midLight);
	p.setBrush(QPalette::Button, background);
	p.setBrush(QPalette::Shadow, shadow);
	p.setBrush(QPalette::HighlightedText, highlightedText);
	p.setBrush(QPalette::Disabled, QPalette::Text, disabledText);
	p.setBrush(QPalette::Disabled, QPalette::WindowText, disabledText);
	p.setBrush(QPalette::Disabled, QPalette::ButtonText, disabledText);
	p.setBrush(QPalette::Disabled, QPalette::Base, background);
	p.setBrush(QPalette::Disabled, QPalette::Dark, shadeDisabled);
	p.setBrush(QPalette::Disabled, QPalette::Shadow, shadow.lighter(150));
	p.setBrush(QPalette::Active, QPalette::Highlight, highlight);
	p.setBrush(QPalette::Inactive, QPalette::Highlight, highlight);
	p.setBrush(QPalette::Disabled, QPalette::Highlight, QColor(145, 145, 145));
	p.setBrush(QPalette::PlaceholderText, placeholder);
	if (dark)
		p.setBrush(QPalette::Link, highlight);
	return p;
}

} // namespace dc30

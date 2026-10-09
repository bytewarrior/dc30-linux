// SPDX-License-Identifier: GPL-2.0-only
// Follows the desktop's switch between light and dark while running. Qt
// 6.4 picks the palette once at start (on Cinnamon: Fusion, light or
// dark by the GTK theme) and keeps it. The desktop portal announces the
// switch (org.freedesktop.appearance color-scheme, set e.g. by
// Cinnamon's theme settings), and this sets Fusion's palette for it -
// only if the start
// palette was one of those, so other desktops' palettes stay untouched.
#pragma once

#include <QObject>

class QDBusVariant;
class QPalette;

namespace dc30 {

class ColorScheme : public QObject {
	Q_OBJECT
public:
	explicit ColorScheme(QObject *parent = nullptr);

	// Qt's Fusion palette (qt_fusionPalette() of later Qt versions,
	// colour for colour the one Qt 6.4 starts with).
	static QPalette fusionPalette(bool dark);

private slots:
	void settingChanged(const QString &ns, const QString &key,
			    const QDBusVariant &value);
};

} // namespace dc30

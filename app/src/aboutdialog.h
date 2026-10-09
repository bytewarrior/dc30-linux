// SPDX-License-Identifier: GPL-2.0-only
// About box: logo, versions of the app, its libraries and the driver, and
// what the driver tells about the card.
#pragma once

#include <QDialog>

class AboutDialog : public QDialog {
	Q_OBJECT
public:
	explicit AboutDialog(QWidget *parent = nullptr);
};

// SPDX-License-Identifier: GPL-2.0-only
// dc30-capture - live preview, level metering and lossless A/V capture
// for the miro DC30 with the dc30 Linux driver.
#include "colorscheme.h"
#include "fieldfix.h"
#include "mainwindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include <QLibraryInfo>
#include <QLocale>
#include <QTimer>
#include <QTranslator>

#include <cstring>
#include <string>

int main(int argc, char **argv)
{
	/* --fix <in> [--out <out>]: no window, no card. */
	for (int i = 1; i < argc; i++) {
		if (std::strcmp(argv[i], "--fix") || i + 1 >= argc)
			continue;
		const std::string in = argv[i + 1];
		std::string out = in.substr(0, in.rfind('.')) + "_fixed.mkv";

		for (int j = 1; j + 1 < argc; j++)
			if (!std::strcmp(argv[j], "--out"))
				out = argv[j + 1];
		return dc30::fixRecording(in, out);
	}

	QApplication app(argc, argv);

	QApplication::setOrganizationName(QStringLiteral("dc30-linux"));
	QApplication::setApplicationName(QStringLiteral("dc30-capture"));
	QApplication::setApplicationVersion(QStringLiteral(APP_VERSION));
	QApplication::setWindowIcon(QIcon(QStringLiteral(":/app_icon.png")));
	dc30::ColorScheme colorScheme;

	/* The sources are English; other languages come from the system
	 * locale. Without a matching translation the English text stays. */
	QTranslator qt, own;
	if (qt.load(QLocale(), QStringLiteral("qtbase"), QStringLiteral("_"),
		    QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
		QApplication::installTranslator(&qt);
	if (own.load(QLocale(), QStringLiteral("dc30-capture"), QStringLiteral("_"),
		     QStringLiteral(":/i18n")))
		QApplication::installTranslator(&own);

	QCommandLineParser args;
	args.addHelpOption();
	const QCommandLineOption record(
		QStringLiteral("record"),
		QCoreApplication::translate(
			"main", "After a 2 s lead-in, record for <seconds>, then quit."),
		QCoreApplication::translate("main", "seconds"));
	args.addOption(record);
	const QCommandLineOption dir(QStringLiteral("dir"),
				     QCoreApplication::translate("main", "Folder for the recording."),
				     QCoreApplication::translate("main", "folder"));
	args.addOption(dir);
	const QCommandLineOption codec(
		QStringLiteral("codec"),
		QCoreApplication::translate("main", "Video codec for recording: ffv1 or mjpeg."),
		QCoreApplication::translate("main", "codec"));
	args.addOption(codec);
	const QCommandLineOption rate(
		QStringLiteral("rate"),
		QCoreApplication::translate("main", "MJPEG data rate in kB/s (1000-6300)."),
		QCoreApplication::translate("main", "kbps"));
	args.addOption(rate);
	/* Handled above, here for --help. */
	args.addOption({QStringLiteral("fix"),
			QCoreApplication::translate(
				"main", "Re-pair the fields of a finished recording (MJPEG or FFV1) by "
					"the picture, write <file>_fixed.mkv (or --out), then quit."),
			QCoreApplication::translate("main", "file")});
	args.addOption({QStringLiteral("out"),
			QCoreApplication::translate("main", "Output file for --fix."),
			QCoreApplication::translate("main", "file")});
	args.process(app);

	MainWindow w;
	w.resize(1200, 700);
	w.show();
	if ((args.isSet(codec) || args.isSet(rate)) &&
	    !w.setCodec(args.isSet(codec) ? args.value(codec) : QStringLiteral("mjpeg"),
			args.value(rate).toInt())) {
		qCritical("%s", qPrintable(QCoreApplication::translate("main", "Unknown codec: %1")
						   .arg(args.value(codec))));
		return 1;
	}
	if (args.isSet(record))
		w.recordFor(args.value(record).toInt(), args.value(dir));
	return app.exec();
}

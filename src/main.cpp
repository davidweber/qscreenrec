#include "MainWindow.h"
#include "Recorder.h"
#include "audio/SystemAudio.h"

#include <QAudioDevice>
#include <QMediaDevices>
#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QPainter>
#include <QPixmap>
#include <QScreen>
#include <QTextStream>
#include <QTimer>
#include <QWindowCapture>

static QIcon makeIcon()
{
    QIcon icon;
    for (int size : { 16, 32, 64, 128, 256 }) {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(40, 44, 52));
        p.drawRoundedRect(QRectF(0, 0, size, size), size * 0.22, size * 0.22);
        p.setBrush(QColor(235, 60, 60));
        p.drawEllipse(QPointF(size / 2.0, size / 2.0), size * 0.27, size * 0.27);
        icon.addPixmap(pm);
    }
    return icon;
}

// Command-line recording: qscreenrec -o out.mp4 [--region x,y,w,h | --window TITLE] -d 10
static int runHeadless(QApplication &app, const QCommandLineParser &cli)
{
    QTextStream out(stdout), err(stderr);
    const auto screens = QGuiApplication::screens();

    Recorder::Settings s;
    s.outputPath = QFileInfo(cli.value("output")).absoluteFilePath();
    s.frameRate = qBound(1, cli.value("fps").toInt(), 120);

    const QString suffix = QFileInfo(s.outputPath).suffix().toLower();
    QStringList suffixes;
    bool known = false;
    for (auto f : Recorder::supportedFileFormats()) {
        suffixes << Recorder::suffixFor(f);
        if (suffixes.last() == suffix) {
            s.fileFormat = f;
            known = true;
        }
    }
    if (!known) {
        err << "Cannot encode ." << suffix << " with this Qt build; available: " << suffixes.join(", ") << "\n";
        return 2;
    }

    const int screenIndex = cli.value("screen").toInt() - 1;
    if (screenIndex < 0 || screenIndex >= screens.size()) {
        err << "No such screen: " << cli.value("screen") << " (see --list)\n";
        return 2;
    }
    s.screen = screens.at(screenIndex);

    if (cli.isSet("region")) {
        const QStringList parts = cli.value("region").split(QLatin1Char(','));
        if (parts.size() != 4) {
            err << "--region expects x,y,width,height\n";
            return 2;
        }
        s.mode = Recorder::Mode::Region;
        s.region = QRect(parts[0].toInt(), parts[1].toInt(), parts[2].toInt(), parts[3].toInt());
    } else if (cli.isSet("window")) {
        s.mode = Recorder::Mode::Window;
        const QString wanted = cli.value("window");
        const auto windows = QWindowCapture::capturableWindows();
        for (const QCapturableWindow &w : windows) {
            if (w.description().contains(wanted, Qt::CaseInsensitive)) {
                s.window = w;
                break;
            }
        }
        if (!s.window.isValid()) {
            err << "No window with a title containing \"" << wanted << "\" (see --list)\n";
            return 2;
        }
    }

    const QString audio = cli.value("audio").toLower();
    if (audio == QLatin1String("system")) {
        s.audio = Recorder::Audio::System;
    } else if (audio == QLatin1String("mic")) {
        s.audio = Recorder::Audio::Device;
        s.audioDevice = QMediaDevices::defaultAudioInput();
    } else if (audio != QLatin1String("none")) {
        err << "--audio expects none, system or mic\n";
        return 2;
    }

    Recorder recorder;
    int exitCode = 0;
    QObject::connect(&recorder, &Recorder::finished, &app, [&](const QString &path) {
        out << "Saved " << path << Qt::endl;
        app.quit();
    });
    QObject::connect(&recorder, &Recorder::failed, &app, [&](const QString &message) {
        err << "Error: " << message << Qt::endl;
        exitCode = 1;
        QTimer::singleShot(0, &app, &QCoreApplication::quit);
    });

    const double seconds = qMax(0.5, cli.value("duration").toDouble());
    QTimer::singleShot(0, &recorder, [&] {
        if (recorder.start(s))
            QTimer::singleShot(int(seconds * 1000), &recorder, &Recorder::stop);
    });
    app.exec();
    return exitCode;
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Archstone"));
    QCoreApplication::setApplicationName(QStringLiteral("QScreenRec"));
    QCoreApplication::setApplicationVersion(QStringLiteral(APP_VERSION));
    QApplication::setWindowIcon(makeIcon());
    QApplication::setQuitOnLastWindowClosed(false);   // the window hides while selecting / recording

    QCommandLineParser cli;
    cli.setApplicationDescription(QStringLiteral(
        "Record the screen, an area of it, or a single window to a video file.\n"
        "Run without options for the graphical interface."));
    cli.addHelpOption();
    cli.addVersionOption();
    cli.addOptions({
        { { "o", "output" }, "Record without the GUI and write to <file> (.mp4, .mkv, .mov, ...).", "file" },
        { { "r", "region" }, "Record an area: x,y,width,height in logical pixels relative to the screen.", "rect" },
        { { "w", "window" }, "Record the first window whose title contains <title>.", "title" },
        { { "s", "screen" }, "Screen number to record (default 1).", "n", "1" },
        { { "d", "duration" }, "Seconds to record with --output (default 10).", "seconds", "10" },
        { { "f", "fps" }, "Frame rate (default 30).", "fps", "30" },
        { { "a", "audio" }, "Audio with --output: none (default), system (what you hear) or mic (default input).", "source", "none" },
        { { "l", "list" }, "List screens and capturable windows, then exit." },
    });
    cli.process(app);

    if (cli.isSet("list")) {
        QTextStream out(stdout);
        out << "Screens:\n";
        const auto screens = QGuiApplication::screens();
        for (int i = 0; i < screens.size(); ++i) {
            const QRect g = screens.at(i)->geometry();
            out << "  " << i + 1 << ": " << screens.at(i)->name() << "  " << g.width() << "x" << g.height()
                << " @" << screens.at(i)->devicePixelRatio() << "x\n";
        }
        out << "Windows:\n";
        const auto windows = QWindowCapture::capturableWindows();
        for (const QCapturableWindow &w : windows)
            out << "  " << w.description() << "\n";
        out << "Formats this Qt build can encode:\n";
        for (auto f : Recorder::supportedFileFormats()) {
            QStringList codecs;
            for (auto c : QMediaFormat(f).supportedVideoCodecs(QMediaFormat::Encode))
                codecs << QMediaFormat::videoCodecName(c);
            out << "  ." << Recorder::suffixFor(f) << ": " << codecs.join(", ") << "\n";
        }
        out << "Audio:\n  system audio: " << (SystemAudio::isSupported() ? "available" : "not available") << "\n";
        const auto inputs = QMediaDevices::audioInputs();
        for (const QAudioDevice &d : inputs)
            out << "  input: " << d.description() << "\n";
        return 0;
    }
    if (cli.isSet("output"))
        return runHeadless(app, cli);

    MainWindow window;
    window.show();
    return app.exec();
}

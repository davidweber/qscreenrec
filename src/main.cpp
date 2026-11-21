#include "MainWindow.h"
#include "Recorder.h"
#include "audio/SystemAudio.h"

#include <QAudioDevice>
#include <QMediaDevices>
#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QPainter>
#include <QPalette>
#include <QStyleFactory>
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

// Dark theme: Fusion + a dark palette for the standard controls, and a stylesheet that adds the
// gradients (window background, cards, buttons). Selectors are scoped by object name / class
// so helper windows such as the area-selection overlay stay untouched.
static void applyTheme(QApplication &app)
{
    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));

    QPalette p;
    const QColor text(232, 236, 244), dim(140, 148, 165), base(24, 28, 38), panel(38, 44, 58);
    p.setColor(QPalette::Window, QColor(28, 32, 44));
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, panel);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, panel);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::ToolTipBase, panel);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, dim);
    p.setColor(QPalette::Highlight, QColor(79, 124, 255));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, QColor(120, 170, 255));
    for (auto role : { QPalette::WindowText, QPalette::Text, QPalette::ButtonText })
        p.setColor(QPalette::Disabled, role, QColor(110, 116, 130));
    app.setPalette(p);

    app.setStyleSheet(QStringLiteral(R"(
        QWidget#mainWindow, QWidget#playerWindow, QDialog#urlAudioDialog, WindowPicker {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                                        stop:0 #232a3d, stop:0.55 #1b2030, stop:1 #251d38);
        }
        QGroupBox {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                        stop:0 rgba(255,255,255,22), stop:1 rgba(255,255,255,8));
            border: 1px solid rgba(255,255,255,28);
            border-radius: 10px;
            margin-top: 14px;
            padding: 10px 8px 8px 8px;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 12px;
            padding: 0 4px;
            color: #9fb4ff;
            font-weight: 600;
        }
        QPushButton {
            color: #e8ecf4;
            border: 1px solid rgba(255,255,255,40);
            border-radius: 7px;
            padding: 5px 14px;
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #475069, stop:1 #333a4e);
        }
        QPushButton:hover {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #56607d, stop:1 #3d455c);
        }
        QPushButton:pressed {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #2c3244, stop:1 #3a4258);
        }
        QPushButton:disabled {
            color: #6e7482;
            border-color: rgba(255,255,255,18);
            background: rgba(255,255,255,10);
        }
        QPushButton#recordButton {
            font-weight: 600;
            border: 1px solid #ff8f8f;
            background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #ff5f6d, stop:1 #c81d4e);
        }
        QPushButton#recordButton:hover {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #ff7480, stop:1 #dc2a5c);
        }
        QPushButton#recordButton[recording="true"] {
            border: 1px solid #7fb0ff;
            background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #4f7cff, stop:1 #7a3cf0);
        }
        QPushButton#recordButton[recording="true"]:hover {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #6690ff, stop:1 #8c52f5);
        }
        QRadioButton::indicator {
            width: 13px; height: 13px;
            border-radius: 7px;
            border: 1px solid #8792b0;
            background: #171b26;
        }
        QRadioButton::indicator:hover { border-color: #b9c6ff; }
        QRadioButton::indicator:checked {
            border: 1px solid #b9c6ff;
            background: qradialgradient(cx:0.5, cy:0.5, radius:0.5, fx:0.5, fy:0.5,
                                        stop:0 #ffffff, stop:0.38 #ffffff, stop:0.45 #4f7cff, stop:1 #7a3cf0);
        }
        QRadioButton::indicator:disabled { border-color: #4a5063; }
        QCheckBox::indicator {
            width: 13px; height: 13px;
            border-radius: 3px;
            border: 1px solid #8792b0;
            background: #171b26;
        }
        QCheckBox::indicator:hover { border-color: #b9c6ff; }
        QCheckBox::indicator:checked {
            border: 1px solid #b9c6ff;
            background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #4f7cff, stop:1 #7a3cf0);
        }
        QCheckBox::indicator:disabled { border-color: #4a5063; }
        QListWidget#recordingList {
            border: 1px solid rgba(255,255,255,28);
            border-radius: 8px;
            background: rgba(10,12,18,120);
        }
        QListWidget#recordingList::item { padding: 5px 6px; }
        QListWidget#recordingList::item:selected {
            color: white;
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #4f7cff, stop:1 #7a3cf0);
        }
        QLabel#playerPlaceholder {
            color: #9aa6c4;
            font-size: 15pt;
            border-radius: 8px;
            background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #11141d, stop:1 #1d1630);
        }
        QPushButton#playButton {
            font-weight: 600;
            border: 1px solid #7fb0ff;
            background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #4f7cff, stop:1 #7a3cf0);
        }
        QPushButton#playButton:hover {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #6690ff, stop:1 #8c52f5);
        }
        QProgressBar {
            border: 1px solid rgba(255,255,255,40);
            border-radius: 6px;
            background: rgba(10,12,18,140);
            text-align: center;
            min-height: 18px;
        }
        QProgressBar::chunk {
            border-radius: 5px;
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #4f7cff, stop:1 #7a3cf0);
        }
        QToolButton#windowTile {
            color: #e8ecf4;
            border: 1px solid rgba(255,255,255,30);
            border-radius: 8px;
            padding: 6px;
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                        stop:0 rgba(255,255,255,24), stop:1 rgba(255,255,255,8));
        }
        QToolButton#windowTile:hover {
            border: 1px solid #7fb0ff;
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                                        stop:0 rgba(120,160,255,70), stop:1 rgba(120,160,255,25));
        }
    )"));
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
    bool audioOnly = false;
    for (auto f : Recorder::supportedFileFormats()) {
        suffixes << Recorder::suffixFor(f);
        if (suffixes.last() == suffix) {
            s.fileFormat = f;
            known = true;
        }
    }
    for (auto f : Recorder::supportedAudioFileFormats()) {     // an audio suffix = audio-only recording
        suffixes << Recorder::suffixFor(f);
        if (suffixes.last() == suffix) {
            s.fileFormat = f;
            known = audioOnly = true;
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
    } else if (cli.isSet("pipewire-node")) {
        s.mode = Recorder::Mode::Window;
        s.pipeWireNode = cli.value("pipewire-node").toUInt();
    } else if (cli.isSet("window") && Recorder::captureNeedsPortal()) {
        s.mode = Recorder::Mode::Window;        // Wayland: the desktop's picker chooses the window
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

    if (audioOnly) {
        s.mode = Recorder::Mode::AudioOnly;
        if (!cli.isSet("audio")) {
            err << "Audio-only recording needs --audio system or --audio mic\n";
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

    s.viaPortal = Recorder::captureNeedsPortal();

    Recorder recorder;
    int exitCode = 0;
    QObject::connect(&recorder, &Recorder::finished, &app, [&](const QString &path) {
        out << "Saved " << path << Qt::endl;
        app.quit();
    });
    QObject::connect(&recorder, &Recorder::cancelled, &app, [&] {
        err << "Cancelled" << Qt::endl;
        exitCode = 1;
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
#if defined(Q_OS_LINUX)
    // Run through X11 / XWayland unless told otherwise. The area overlay, the on-screen area
    // frame and window placement rely on X11 semantics, and the bundled Qt ships no Wayland
    // platform plugin (which otherwise logs a "could not find plugin" warning at startup).
    // Screen contents are still captured natively on Wayland, through the desktop portal.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "xcb");
    // Play recordings back with software decoding. Hardware (VA-API) decoding fails on some
    // GPU/driver combinations ("Failed to upload decode parameters") and leaves the picture
    // blank; recordings are small enough that the CPU handles them easily. A list holding no
    // device type turns hardware decoding off; set the variable yourself to override.
    if (!qEnvironmentVariableIsSet("QT_FFMPEG_DECODING_HW_DEVICE_TYPES"))
        qputenv("QT_FFMPEG_DECODING_HW_DEVICE_TYPES", ",");
#endif
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Archstone"));
    QCoreApplication::setApplicationName(QStringLiteral("QScreenRec"));
    QCoreApplication::setApplicationVersion(QStringLiteral(APP_VERSION));
    QApplication::setWindowIcon(makeIcon());
    QApplication::setQuitOnLastWindowClosed(false);
    applyTheme(app);   // the window hides while selecting / recording

    QCommandLineParser cli;
    cli.setApplicationDescription(QStringLiteral(
        "Record the screen, an area of it, or a single window to a video file.\n"
        "Run without options for the graphical interface."));
    cli.addHelpOption();
    cli.addVersionOption();
    cli.addOptions({
        { { "o", "output" }, "Record without the GUI and write to <file> (.mp4, .mkv, .mov, ...). "
                             "An audio suffix (.m4a, .flac, .wav, ...) records audio only.", "file" },
        { { "r", "region" }, "Record an area: x,y,width,height in logical pixels relative to the screen.", "rect" },
        { { "w", "window" }, "Record the first window whose title contains <title>.", "title" },
        { { "s", "screen" }, "Screen number to record (default 1).", "n", "1" },
        { { "d", "duration" }, "Seconds to record with --output (default 10).", "seconds", "10" },
        { { "f", "fps" }, "Frame rate (default 30).", "fps", "30" },
        { { "a", "audio" }, "Audio with --output: none (default), system (what you hear) or mic (default input).", "source", "none" },
        { { "l", "list" }, "List screens and capturable windows, then exit." },
    });
    QCommandLineOption pipeWireNode(QStringLiteral("pipewire-node"),
                                    QStringLiteral("Test hook: record this PipeWire video node."),
                                    QStringLiteral("id"));
    pipeWireNode.setFlags(QCommandLineOption::HiddenFromHelp);
    cli.addOption(pipeWireNode);
    cli.process(app);

    if (cli.isSet("list")) {
        QTextStream out(stdout);
        out << "Session: Qt platform " << QGuiApplication::platformName()
            << ", XDG_SESSION_TYPE=" << qEnvironmentVariable("XDG_SESSION_TYPE", QStringLiteral("(unset)"))
            << ", capture via " << (Recorder::captureNeedsPortal() ? "desktop portal (system picker)" : "Qt") << "\n";
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
        QStringList audioFormats;
        for (auto f : Recorder::supportedAudioFileFormats())
            audioFormats << QLatin1Char('.') + Recorder::suffixFor(f);
        out << "  audio only: " << audioFormats.join(", ") << "\n";
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

#include "MainWindow.h"
#include "RegionSelector.h"
#include "audio/SystemAudio.h"

#include <QAudioDevice>
#include <QMediaDevices>
#include <QAction>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QSettings>
#include <QShortcut>
#include <QSpinBox>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QUrl>
#if QT_CONFIG(permissions)
#  include <QPermissions>
#endif
#include <QVBoxLayout>
#include <QWindowCapture>

namespace {
QString formatDuration(qint64 ms)
{
    const qint64 s = ms / 1000;
    return QStringLiteral("%1:%2:%3")
        .arg(s / 3600, 2, 10, QLatin1Char('0'))
        .arg((s / 60) % 60, 2, 10, QLatin1Char('0'))
        .arg(s % 60, 2, 10, QLatin1Char('0'));
}
}

MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent)
{
    setWindowTitle(QStringLiteral("QScreenRec %1").arg(QStringLiteral(APP_VERSION)));
    buildUi();
    refreshScreens();
    refreshWindows();
    refreshAudioSources();
    loadSettings();

    auto *devices = new QMediaDevices(this);
    connect(devices, &QMediaDevices::audioInputsChanged, this, &MainWindow::refreshAudioSources);

    connect(&m_recorder, &Recorder::started, this, &MainWindow::updateUiState);
    connect(&m_recorder, &Recorder::pausedChanged, this, &MainWindow::updateUiState);
    connect(&m_recorder, &Recorder::finished, this, &MainWindow::onFinished);
    connect(&m_recorder, &Recorder::failed, this, &MainWindow::onFailed);
    connect(&m_recorder, &Recorder::durationChanged, this, [this](qint64 ms) {
        m_lastDurationMs = ms;
        updateUiState();
    });

    m_countdownTimer.setInterval(1000);
    connect(&m_countdownTimer, &QTimer::timeout, this, [this] {
        if (--m_countdown > 0) {
            m_statusLabel->setText(tr("Starting in %1…").arg(m_countdown));
        } else {
            m_countdownTimer.stop();
            startRecording();
        }
    });

    connect(qGuiApp, &QGuiApplication::screenAdded, this, &MainWindow::refreshScreens);
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, &MainWindow::refreshScreens);

    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        m_tray = new QSystemTrayIcon(windowIcon(), this);
        auto *menu = new QMenu(this);
        menu->addAction(tr("Start / stop recording"), this, &MainWindow::toggleRecording);
        menu->addAction(tr("Show window"), this, &MainWindow::restoreWindow);
        menu->addSeparator();
        menu->addAction(tr("Quit"), this, &QWidget::close);
        m_tray->setContextMenu(menu);
        m_tray->setToolTip(windowTitle());
        connect(m_tray, &QSystemTrayIcon::activated, this,
                [this](QSystemTrayIcon::ActivationReason reason) {
                    if (reason != QSystemTrayIcon::Trigger)
                        return;
                    if (m_recorder.isActive() || m_countdownTimer.isActive())
                        toggleRecording();      // one click on the tray icon stops the recording
                    else
                        restoreWindow();
                });
        m_tray->show();
    }

    updateUiState();
}

MainWindow::~MainWindow()
{
    delete m_regionFrame;
}

void MainWindow::buildUi()
{
    // --- Source -------------------------------------------------------------------------
    auto *sourceBox = new QGroupBox(tr("What to record"));
    m_fullScreenRadio = new QRadioButton(tr("Entire screen"));
    m_regionRadio = new QRadioButton(tr("Area of the screen"));
    m_windowRadio = new QRadioButton(tr("Window"));
    m_fullScreenRadio->setChecked(true);
    auto *group = new QButtonGroup(this);
    for (auto *b : { m_fullScreenRadio, m_regionRadio, m_windowRadio }) {
        group->addButton(b);
        connect(b, &QRadioButton::toggled, this, &MainWindow::updateUiState);
    }

    m_screenCombo = new QComboBox;
    connect(m_screenCombo, &QComboBox::activated, this, [this] {
        m_region = {};              // a selection belongs to the screen it was made on
        updateUiState();
    });

    m_selectRegionButton = new QPushButton(tr("Select area…"));
    m_regionLabel = new QLabel;
    connect(m_selectRegionButton, &QPushButton::clicked, this, &MainWindow::selectRegion);

    m_windowCombo = new QComboBox;
    m_windowCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_windowCombo->setMinimumContentsLength(28);
    m_refreshWindowsButton = new QPushButton(tr("Refresh"));
    connect(m_refreshWindowsButton, &QPushButton::clicked, this, &MainWindow::refreshWindows);

    auto *modeRow = new QHBoxLayout;
    modeRow->addWidget(m_fullScreenRadio);
    modeRow->addWidget(m_regionRadio);
    modeRow->addWidget(m_windowRadio);
    modeRow->addStretch();

    auto *regionRow = new QHBoxLayout;
    regionRow->addWidget(m_selectRegionButton);
    regionRow->addWidget(m_regionLabel, 1);

    auto *windowRow = new QHBoxLayout;
    windowRow->addWidget(m_windowCombo, 1);
    windowRow->addWidget(m_refreshWindowsButton);

    m_audioCombo = new QComboBox;
    m_audioCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);

    auto *sourceForm = new QFormLayout(sourceBox);
    sourceForm->addRow(modeRow);
    sourceForm->addRow(tr("Screen:"), m_screenCombo);
    sourceForm->addRow(tr("Area:"), regionRow);
    sourceForm->addRow(tr("Window:"), windowRow);
    sourceForm->addRow(tr("Audio:"), m_audioCombo);

    // --- Output -------------------------------------------------------------------------
    auto *outputBox = new QGroupBox(tr("Output"));
    m_folderEdit = new QLineEdit;
    auto *browse = new QPushButton(tr("Browse…"));
    connect(browse, &QPushButton::clicked, this, &MainWindow::browseOutputFolder);
    auto *folderRow = new QHBoxLayout;
    folderRow->addWidget(m_folderEdit, 1);
    folderRow->addWidget(browse);

    m_formatCombo = new QComboBox;
    for (auto f : Recorder::supportedFileFormats()) {
        m_formatCombo->addItem(QStringLiteral("%1 (.%2)").arg(QMediaFormat::fileFormatName(f),
                                                               Recorder::suffixFor(f)),
                               QVariant::fromValue(int(f)));
    }

    m_qualityCombo = new QComboBox;
    m_qualityCombo->addItem(tr("Low"), int(QMediaRecorder::LowQuality));
    m_qualityCombo->addItem(tr("Normal"), int(QMediaRecorder::NormalQuality));
    m_qualityCombo->addItem(tr("High"), int(QMediaRecorder::HighQuality));
    m_qualityCombo->addItem(tr("Very high"), int(QMediaRecorder::VeryHighQuality));
    m_qualityCombo->setCurrentIndex(2);

    m_fpsSpin = new QSpinBox;
    m_fpsSpin->setRange(1, 120);
    m_fpsSpin->setValue(30);
    m_fpsSpin->setSuffix(tr(" fps"));

    auto *outputForm = new QFormLayout(outputBox);
    outputForm->addRow(tr("Save to:"), folderRow);
    outputForm->addRow(tr("Format:"), m_formatCombo);
    outputForm->addRow(tr("Quality:"), m_qualityCombo);
    outputForm->addRow(tr("Frame rate:"), m_fpsSpin);

    // --- Options ------------------------------------------------------------------------
    m_delaySpin = new QSpinBox;
    m_delaySpin->setRange(0, 60);
    m_delaySpin->setValue(3);
    m_delaySpin->setSuffix(tr(" s"));
    m_minimizeCheck = new QCheckBox(tr("Minimize this window while recording"));
    m_minimizeCheck->setChecked(true);
    auto *optionsRow = new QHBoxLayout;
    optionsRow->addWidget(new QLabel(tr("Start delay:")));
    optionsRow->addWidget(m_delaySpin);
    optionsRow->addSpacing(16);
    optionsRow->addWidget(m_minimizeCheck);
    optionsRow->addStretch();

    // --- Controls -----------------------------------------------------------------------
    m_recordButton = new QPushButton;
    m_recordButton->setMinimumHeight(36);
    m_recordButton->setDefault(true);
    m_pauseButton = new QPushButton;
    m_pauseButton->setMinimumHeight(36);
    connect(m_recordButton, &QPushButton::clicked, this, &MainWindow::toggleRecording);
    connect(m_pauseButton, &QPushButton::clicked, this,
            [this] { m_recorder.setPaused(!m_recorder.isPaused()); });
    auto *shortcut = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_R), this);
    connect(shortcut, &QShortcut::activated, this, &MainWindow::toggleRecording);

    auto *buttonRow = new QHBoxLayout;
    buttonRow->addWidget(m_recordButton, 2);
    buttonRow->addWidget(m_pauseButton, 1);

    m_statusLabel = new QLabel(tr("Ready"));
    m_statusLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_statusLabel->setOpenExternalLinks(true);
    m_statusLabel->setWordWrap(true);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(sourceBox);
    layout->addWidget(outputBox);
    layout->addLayout(optionsRow);
    layout->addLayout(buttonRow);
    layout->addWidget(m_statusLabel);
}

void MainWindow::loadSettings()
{
    QSettings s;
    QString folder = s.value("output/folder").toString();
    if (folder.isEmpty() || !QDir(folder).exists()) {
        folder = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
        if (folder.isEmpty())
            folder = QDir::homePath();
    }
    m_folderEdit->setText(QDir::toNativeSeparators(folder));

    int i = m_formatCombo->findData(s.value("output/format", int(QMediaFormat::MPEG4)));
    m_formatCombo->setCurrentIndex(qMax(0, i));
    i = m_qualityCombo->findData(s.value("output/quality", int(QMediaRecorder::HighQuality)));
    m_qualityCombo->setCurrentIndex(i < 0 ? 2 : i);
    m_fpsSpin->setValue(s.value("output/fps", 30).toInt());
    m_delaySpin->setValue(s.value("options/delay", 3).toInt());
    m_minimizeCheck->setChecked(s.value("options/minimize", true).toBool());
    i = m_audioCombo->findData(s.value("audio/source", m_audioCombo->itemData(0)));
    m_audioCombo->setCurrentIndex(qMax(0, i));
}

void MainWindow::saveSettings() const
{
    QSettings s;
    s.setValue("output/folder", QDir::fromNativeSeparators(m_folderEdit->text()));
    s.setValue("output/format", m_formatCombo->currentData());
    s.setValue("output/quality", m_qualityCombo->currentData());
    s.setValue("output/fps", m_fpsSpin->value());
    s.setValue("options/delay", m_delaySpin->value());
    s.setValue("options/minimize", m_minimizeCheck->isChecked());
    s.setValue("audio/source", m_audioCombo->currentData());
}

void MainWindow::refreshScreens()
{
    const QString previous = m_screenCombo->currentData().toString();
    m_screenCombo->clear();
    const auto screens = QGuiApplication::screens();
    for (int i = 0; i < screens.size(); ++i) {
        const QScreen *sc = screens.at(i);
        const QSize px = sc->geometry().size() * sc->devicePixelRatio();
        QString text = tr("%1: %2 (%3 × %4)").arg(i + 1).arg(sc->name()).arg(px.width()).arg(px.height());
        if (sc == QGuiApplication::primaryScreen())
            text += tr(" – primary");
        m_screenCombo->addItem(text, sc->name());
    }
    const int i = m_screenCombo->findData(previous);
    if (i >= 0) {
        m_screenCombo->setCurrentIndex(i);
    } else {
        m_screenCombo->setCurrentIndex(qMax(0, int(screens.indexOf(QGuiApplication::primaryScreen()))));
        m_region = {};
    }
    updateUiState();
}

void MainWindow::refreshWindows()
{
    const QString previous = m_windowCombo->currentText();
    m_windows.clear();
    m_windowCombo->clear();
    const auto windows = QWindowCapture::capturableWindows();
    for (const QCapturableWindow &w : windows) {
        if (!w.isValid() || w.description().trimmed().isEmpty())
            continue;
        m_windows.append(w);
        m_windowCombo->addItem(w.description());
    }
    const int i = m_windowCombo->findText(previous);
    if (i >= 0)
        m_windowCombo->setCurrentIndex(i);
    updateUiState();
}

// Item data: "system", "none", or "device:<id>" for an audio input device.
void MainWindow::refreshAudioSources()
{
    const QVariant previous = m_audioCombo->currentData();
    m_audioCombo->clear();
    if (SystemAudio::isSupported())
        m_audioCombo->addItem(tr("System audio (what you hear)"), QStringLiteral("system"));
    m_audioCombo->addItem(tr("No audio"), QStringLiteral("none"));
    const auto inputs = QMediaDevices::audioInputs();
    for (const QAudioDevice &d : inputs) {
        m_audioCombo->addItem(tr("Input: %1").arg(d.description()),
                              QStringLiteral("device:") + QString::fromLatin1(d.id().toHex()));
    }
    const int i = m_audioCombo->findData(previous);
    m_audioCombo->setCurrentIndex(qMax(0, i));
}

// Returns true when recording can proceed now. On macOS the first use shows the system
// prompt; recording is started again from the callback once the user has answered.
bool MainWindow::ensureMicrophonePermission()
{
#if QT_CONFIG(permissions)
    const QMicrophonePermission permission;
    switch (qApp->checkPermission(permission)) {
    case Qt::PermissionStatus::Granted:
        return true;
    case Qt::PermissionStatus::Undetermined:
        qApp->requestPermission(permission, this, [this](const QPermission &p) {
            if (p.status() == Qt::PermissionStatus::Granted)
                beginCountdown();
            else
                onFailed(tr("Microphone access was denied."));
        });
        return false;
    case Qt::PermissionStatus::Denied:
        onFailed(tr("Microphone access is denied. Allow it for QScreenRec in the system "
                    "privacy settings, or choose a different audio source."));
        return false;
    }
#endif
    return true;
}

QScreen *MainWindow::selectedScreen() const
{
    const QString name = m_screenCombo->currentData().toString();
    const auto screens = QGuiApplication::screens();
    for (QScreen *sc : screens) {
        if (sc->name() == name)
            return sc;
    }
    return QGuiApplication::primaryScreen();
}

Recorder::Mode MainWindow::mode() const
{
    if (m_regionRadio->isChecked())
        return Recorder::Mode::Region;
    if (m_windowRadio->isChecked())
        return Recorder::Mode::Window;
    return Recorder::Mode::FullScreen;
}

void MainWindow::selectRegion()
{
    QScreen *screen = selectedScreen();
    if (!screen)
        return;
    hide();     // get out of the way so the user can see what is underneath
    QTimer::singleShot(250, this, [this, screen = QPointer<QScreen>(screen)] {
        if (!screen) {
            show();
            return;
        }
        auto *selector = new RegionSelector(screen, m_region);
        connect(selector, &RegionSelector::selected, this, [this](const QRect &r) {
            m_region = r;
            m_regionRadio->setChecked(true);
        });
        connect(selector, &QObject::destroyed, this, [this] {
            restoreWindow();
            updateUiState();
        });
        selector->begin();
    });
}

void MainWindow::browseOutputFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Save recordings to"),
                                                          m_folderEdit->text());
    if (!dir.isEmpty())
        m_folderEdit->setText(QDir::toNativeSeparators(dir));
}

QString MainWindow::nextOutputPath() const
{
    const QDir dir(QDir::fromNativeSeparators(m_folderEdit->text()));
    const auto format = QMediaFormat::FileFormat(m_formatCombo->currentData().toInt());
    const QString base = QStringLiteral("Recording %1")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH.mm.ss")));
    return dir.filePath(base + QLatin1Char('.') + Recorder::suffixFor(format));
}

void MainWindow::toggleRecording()
{
    if (m_countdownTimer.isActive()) {          // cancel a pending start
        m_countdownTimer.stop();
        delete m_regionFrame;
        restoreWindow();
        m_statusLabel->setText(tr("Cancelled"));
        updateUiState();
        return;
    }
    if (m_recorder.isActive()) {
        m_statusLabel->setText(tr("Finishing…"));
        m_recorder.stop();
        return;
    }
    beginCountdown();
}

void MainWindow::beginCountdown()
{
    if (m_formatCombo->count() == 0) {
        onFailed(tr("This Qt installation has no video encoder available "
                    "(is the Qt Multimedia FFmpeg plugin installed?)."));
        return;
    }
    QDir dir(QDir::fromNativeSeparators(m_folderEdit->text()));
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        onFailed(tr("Cannot create the folder %1").arg(m_folderEdit->text()));
        return;
    }
    if (mode() == Recorder::Mode::Region && m_region.isEmpty()) {
        selectRegion();
        return;
    }
    if (mode() == Recorder::Mode::Window && m_windowCombo->currentIndex() < 0) {
        onFailed(tr("No window selected. Click Refresh and pick one."));
        return;
    }
    if (m_audioCombo->currentData().toString().startsWith(QLatin1String("device:"))
        && !ensureMicrophonePermission())
        return;
    saveSettings();

    if (mode() == Recorder::Mode::Region && RegionFrame::isSupported()) {
        delete m_regionFrame;
        m_regionFrame = new RegionFrame(selectedScreen(), m_region);
        m_regionFrame->show();
    }
    if (m_minimizeCheck->isChecked())
        showMinimized();

    m_countdown = m_delaySpin->value();
    if (m_countdown > 0) {
        m_statusLabel->setText(tr("Starting in %1…").arg(m_countdown));
        m_countdownTimer.start();
    } else {
        // Give the window manager a moment to finish the minimize animation.
        m_countdown = 1;
        m_countdownTimer.start(m_minimizeCheck->isChecked() ? 500 : 0);
    }
    updateUiState();
}

void MainWindow::startRecording()
{
    m_countdownTimer.setInterval(1000);

    Recorder::Settings s;
    s.mode = mode();
    s.screen = selectedScreen();
    s.region = m_region;
    if (s.mode == Recorder::Mode::Window)
        s.window = m_windows.value(m_windowCombo->currentIndex());
    s.outputPath = nextOutputPath();
    s.fileFormat = QMediaFormat::FileFormat(m_formatCombo->currentData().toInt());
    s.quality = QMediaRecorder::Quality(m_qualityCombo->currentData().toInt());
    s.frameRate = m_fpsSpin->value();

    const QString audio = m_audioCombo->currentData().toString();
    if (audio == QLatin1String("system")) {
        s.audio = Recorder::Audio::System;
    } else if (audio.startsWith(QLatin1String("device:"))) {
        const QByteArray id = QByteArray::fromHex(audio.mid(7).toLatin1());
        const auto inputs = QMediaDevices::audioInputs();
        for (const QAudioDevice &d : inputs) {
            if (d.id() == id) {
                s.audio = Recorder::Audio::Device;
                s.audioDevice = d;
            }
        }
        if (s.audio != Recorder::Audio::Device) {
            onFailed(tr("The selected audio input is no longer connected."));
            return;
        }
    }

    m_lastDurationMs = 0;
    m_recorder.start(s);        // failures are reported through Recorder::failed
    updateUiState();
}

void MainWindow::onFinished(const QString &path)
{
    delete m_regionFrame;
    restoreWindow();
    const QUrl folder = QUrl::fromLocalFile(QFileInfo(path).absolutePath());
    m_statusLabel->setText(tr("Saved %1 — <a href=\"%2\">open folder</a>")
                               .arg(QDir::toNativeSeparators(path).toHtmlEscaped(),
                                    folder.toString(QUrl::FullyEncoded)));
    if (m_tray && !isActiveWindow())
        m_tray->showMessage(tr("Recording saved"), QDir::toNativeSeparators(path));
    updateUiState();
}

void MainWindow::onFailed(const QString &message)
{
    m_countdownTimer.stop();
    delete m_regionFrame;
    restoreWindow();
    m_statusLabel->setText(tr("Error: %1").arg(message.toHtmlEscaped()));
    updateUiState();
    QMessageBox::warning(this, tr("Recording failed"), message);
}

void MainWindow::restoreWindow()
{
    if (isMinimized())
        showNormal();
    else
        show();
    raise();
    activateWindow();
}

void MainWindow::updateUiState()
{
    const bool pending = m_countdownTimer.isActive();
    const bool busy = pending || m_recorder.isActive();
    const Recorder::Mode m = mode();

    for (QWidget *w : std::initializer_list<QWidget *>{
             m_fullScreenRadio, m_regionRadio, m_windowRadio, m_audioCombo, m_folderEdit, m_formatCombo,
             m_qualityCombo, m_fpsSpin, m_delaySpin, m_minimizeCheck })
        w->setEnabled(!busy);
    m_screenCombo->setEnabled(!busy && m != Recorder::Mode::Window);
    m_selectRegionButton->setEnabled(!busy && m == Recorder::Mode::Region);
    m_regionLabel->setEnabled(m == Recorder::Mode::Region);
    m_windowCombo->setEnabled(!busy && m == Recorder::Mode::Window);
    m_refreshWindowsButton->setEnabled(!busy && m == Recorder::Mode::Window);

    if (m_region.isEmpty()) {
        m_regionLabel->setText(tr("none selected"));
    } else {
        m_regionLabel->setText(tr("%1 × %2 at (%3, %4)")
                                   .arg(m_region.width()).arg(m_region.height())
                                   .arg(m_region.x()).arg(m_region.y()));
    }

    m_recordButton->setText(pending ? tr("Cancel") : busy ? tr("■  Stop") : tr("●  Record"));
    m_pauseButton->setEnabled(m_recorder.isActive());
    m_pauseButton->setText(m_recorder.isPaused() ? tr("Resume") : tr("Pause"));
    if (m_recorder.isActive()) {
        m_statusLabel->setText((m_recorder.isPaused() ? tr("Paused  %1") : tr("Recording  %1"))
                                   .arg(formatDuration(m_lastDurationMs)));
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    saveSettings();
    m_countdownTimer.stop();
    delete m_regionFrame;
    if (m_recorder.isActive()) {
        // Finalize the file before quitting, otherwise the container may be left unplayable.
        connect(&m_recorder, &Recorder::finished, qApp, &QCoreApplication::quit);
        connect(&m_recorder, &Recorder::failed, qApp, &QCoreApplication::quit);
        m_recorder.stop();
        hide();
        event->ignore();
        return;
    }
    event->accept();
    qApp->quit();
}

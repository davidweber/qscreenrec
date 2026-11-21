#include "PlayerWindow.h"

#include <QAudioOutput>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMediaPlayer>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSplitter>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QVideoWidget>

namespace {
QString formatTime(qint64 ms)
{
    const qint64 s = qMax<qint64>(0, ms) / 1000;
    if (s >= 3600)
        return QStringLiteral("%1:%2:%3").arg(s / 3600).arg((s / 60) % 60, 2, 10, QLatin1Char('0'))
                .arg(s % 60, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}
}

QStringList PlayerWindow::mediaSuffixes()
{
    return { QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("mov"), QStringLiteral("avi"),
             QStringLiteral("webm"), QStringLiteral("m4a"), QStringLiteral("mp3"), QStringLiteral("flac"),
             QStringLiteral("wav"), QStringLiteral("ogg") };
}

PlayerWindow::PlayerWindow(QWidget *parent)
    : QWidget(parent, Qt::Window)
{
    setObjectName(QStringLiteral("playerWindow"));
    setWindowTitle(tr("QScreenRec – Recordings"));
    resize(980, 560);

    m_player = new QMediaPlayer(this);
    m_audio = new QAudioOutput(this);
    m_player->setAudioOutput(m_audio);

    // --- Left: recordings in the output folder -------------------------------------------
    m_list = new QListWidget;
    m_list->setObjectName(QStringLiteral("recordingList"));
    m_list->setMinimumWidth(220);
    connect(m_list, &QListWidget::itemActivated, this,
            [this](QListWidgetItem *item) { play(item->data(Qt::UserRole).toString()); });
    connect(m_list, &QListWidget::itemClicked, this,
            [this](QListWidgetItem *item) { play(item->data(Qt::UserRole).toString()); });

    auto *openButton = new QPushButton(tr("Open file…"));
    connect(openButton, &QPushButton::clicked, this, &PlayerWindow::openFile);
    auto *folderButton = new QPushButton(tr("Show folder"));
    connect(folderButton, &QPushButton::clicked, this, [this] {
        const QString dir = m_current.isEmpty() ? m_folder : QFileInfo(m_current).absolutePath();
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
    auto *listButtons = new QHBoxLayout;
    listButtons->addWidget(openButton);
    listButtons->addWidget(folderButton);

    auto *left = new QWidget;
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addWidget(new QLabel(tr("Recordings")));
    leftLayout->addWidget(m_list, 1);
    leftLayout->addLayout(listButtons);

    // --- Right: picture + transport ------------------------------------------------------
    m_video = new QVideoWidget;
    m_video->setMinimumSize(320, 180);
    m_player->setVideoOutput(m_video);

    m_placeholder = new QLabel(tr("Select a recording to play"));
    m_placeholder->setObjectName(QStringLiteral("playerPlaceholder"));
    m_placeholder->setAlignment(Qt::AlignCenter);
    m_placeholder->setMinimumSize(320, 180);

    m_stack = new QStackedWidget;
    m_stack->addWidget(m_placeholder);
    m_stack->addWidget(m_video);

    m_title = new QLabel;
    m_title->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_playButton = new QPushButton;
    m_playButton->setObjectName(QStringLiteral("playButton"));
    m_playButton->setMinimumSize(84, 32);
    connect(m_playButton, &QPushButton::clicked, this, &PlayerWindow::togglePlay);

    m_seek = new QSlider(Qt::Horizontal);
    m_seek->setRange(0, 0);
    connect(m_seek, &QSlider::sliderPressed, this, [this] { m_seeking = true; });
    connect(m_seek, &QSlider::sliderReleased, this, [this] {
        m_seeking = false;
        m_player->setPosition(m_seek->value());
    });
    connect(m_seek, &QSlider::sliderMoved, this, [this](int value) {
        m_time->setText(QStringLiteral("%1 / %2").arg(formatTime(value), formatTime(m_player->duration())));
    });
    // Clicking the groove (page step) jumps as well.
    connect(m_seek, &QSlider::actionTriggered, this, [this](int action) {
        if (action == QAbstractSlider::SliderPageStepAdd || action == QAbstractSlider::SliderPageStepSub)
            m_player->setPosition(m_seek->sliderPosition());
    });

    m_time = new QLabel(QStringLiteral("0:00 / 0:00"));
    m_time->setMinimumWidth(96);
    m_time->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    m_volume = new QSlider(Qt::Horizontal);
    m_volume->setRange(0, 100);
    m_volume->setFixedWidth(90);
    m_volume->setToolTip(tr("Volume"));
    m_volume->setValue(QSettings().value("player/volume", 80).toInt());
    m_audio->setVolume(m_volume->value() / 100.0f);
    connect(m_volume, &QSlider::valueChanged, this, [this](int value) {
        m_audio->setVolume(value / 100.0f);
        QSettings().setValue("player/volume", value);
    });

    auto *transport = new QHBoxLayout;
    transport->addWidget(m_playButton);
    transport->addWidget(m_seek, 1);
    transport->addWidget(m_time);
    transport->addWidget(new QLabel(tr("Vol")));
    transport->addWidget(m_volume);

    auto *right = new QWidget;
    auto *rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->addWidget(m_stack, 1);
    rightLayout->addWidget(m_title);
    rightLayout->addLayout(transport);

    auto *splitter = new QSplitter;
    splitter->addWidget(left);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({ 260, 720 });

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(splitter);

    connect(m_player, &QMediaPlayer::positionChanged, this, &PlayerWindow::updatePosition);
    connect(m_player, &QMediaPlayer::durationChanged, this, [this](qint64 ms) {
        m_seek->setRange(0, int(ms));
        m_seek->setPageStep(int(qMax<qint64>(1000, ms / 10)));
        updatePosition(m_player->position());
    });
    connect(m_player, &QMediaPlayer::playbackStateChanged, this, &PlayerWindow::updateControls);
    connect(m_player, &QMediaPlayer::hasVideoChanged, this, &PlayerWindow::updateControls);
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this, [this](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia) {       // rewind so Play starts over
            m_player->setPosition(0);
            m_player->pause();
        }
        updateControls();
    });
    connect(m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString &text) {
        m_placeholder->setText(tr("Cannot play this file:\n%1").arg(text));
        m_stack->setCurrentWidget(m_placeholder);
    });
    updateControls();
}

void PlayerWindow::setFolder(const QString &folder)
{
    m_folder = folder;
    refreshList();
}

void PlayerWindow::refreshList()
{
    m_list->clear();
    QStringList filters;
    for (const QString &suffix : mediaSuffixes())
        filters << QStringLiteral("*.") + suffix;
    const QFileInfoList files = QDir(m_folder).entryInfoList(filters, QDir::Files, QDir::Time);
    for (const QFileInfo &file : files) {
        auto *item = new QListWidgetItem(file.fileName(), m_list);
        item->setData(Qt::UserRole, file.absoluteFilePath());
        item->setToolTip(tr("%1\n%2  ·  %3 MB").arg(QDir::toNativeSeparators(file.absoluteFilePath()),
                                                   QLocale().toString(file.lastModified(), QLocale::ShortFormat))
                                 .arg(file.size() / 1048576.0, 0, 'f', 1));
        if (file.absoluteFilePath() == m_current)
            m_list->setCurrentItem(item);
    }
}

void PlayerWindow::openFile()
{
    QStringList patterns;
    for (const QString &suffix : mediaSuffixes())
        patterns << QStringLiteral("*.") + suffix;
    const QString file = QFileDialog::getOpenFileName(
            this, tr("Open recording"), m_current.isEmpty() ? m_folder : QFileInfo(m_current).absolutePath(),
            tr("Media files (%1);;All files (*)").arg(patterns.join(QLatin1Char(' '))));
    if (!file.isEmpty())
        play(file);
}

void PlayerWindow::play(const QString &filePath)
{
    if (filePath.isEmpty())
        return;
    m_current = QFileInfo(filePath).absoluteFilePath();
    m_title->setText(QDir::toNativeSeparators(m_current));
    m_placeholder->setText(tr("♪  Audio recording"));
    refreshList();
    m_player->setSource(QUrl::fromLocalFile(m_current));
    m_player->play();
    updateControls();
}

void PlayerWindow::stop()
{
    m_player->stop();
    m_player->setSource(QUrl());        // release the file (it may be about to be overwritten)
    updateControls();
}

void PlayerWindow::togglePlay()
{
    if (m_player->source().isEmpty()) {
        if (m_list->currentItem())
            play(m_list->currentItem()->data(Qt::UserRole).toString());
        else if (m_list->count() > 0)
            play(m_list->item(0)->data(Qt::UserRole).toString());
        return;
    }
    if (m_player->playbackState() == QMediaPlayer::PlayingState)
        m_player->pause();
    else
        m_player->play();
}

void PlayerWindow::updatePosition(qint64 ms)
{
    if (!m_seeking)
        m_seek->setValue(int(ms));
    if (!m_seeking)
        m_time->setText(QStringLiteral("%1 / %2").arg(formatTime(ms), formatTime(m_player->duration())));
}

void PlayerWindow::updateControls()
{
    const bool playing = m_player->playbackState() == QMediaPlayer::PlayingState;
    m_playButton->setText(playing ? tr("❚❚  Pause") : tr("▶  Play"));
    m_seek->setEnabled(!m_player->source().isEmpty());
    if (!m_player->source().isEmpty() && m_player->error() == QMediaPlayer::NoError)
        m_stack->setCurrentWidget(m_player->hasVideo() ? static_cast<QWidget *>(m_video) : m_placeholder);
}

void PlayerWindow::closeEvent(QCloseEvent *event)
{
    m_player->pause();
    QWidget::closeEvent(event);
}

void PlayerWindow::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Space:
        togglePlay();
        break;
    case Qt::Key_Left:
        m_player->setPosition(qMax<qint64>(0, m_player->position() - 5000));
        break;
    case Qt::Key_Right:
        m_player->setPosition(qMin(m_player->duration(), m_player->position() + 5000));
        break;
    default:
        QWidget::keyPressEvent(event);
    }
}

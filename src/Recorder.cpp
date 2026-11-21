#include "Recorder.h"
#include "audio/SystemAudio.h"

#include <QAudioBuffer>
#include <QAudioBufferInput>
#include <QAudioInput>
#include <QImage>
#include <QScreen>
#include <QScreenCapture>
#include <QUrl>
#include <QVideoFrame>
#include <QVideoFrameInput>
#include <QWindowCapture>

Recorder::Recorder(QObject *parent)
    : QObject(parent)
{
    m_session.setRecorder(&m_recorder);

    connect(&m_recorder, &QMediaRecorder::recorderStateChanged,
            this, &Recorder::onRecorderStateChanged);
    connect(&m_recorder, &QMediaRecorder::durationChanged,
            this, &Recorder::durationChanged);
    connect(&m_recorder, &QMediaRecorder::errorOccurred, this,
            [this](QMediaRecorder::Error, const QString &text) {
                // Typically a container the available encoder cannot write a header for
                // (e.g. software H.264 into Matroska with Qt 6.8's FFmpeg backend).
                if (m_settings.fileFormat != QMediaFormat::MPEG4)
                    fail(tr("%1\n\nThis format may not work with the encoders installed here; "
                            "try MP4.").arg(text));
                else
                    fail(text);
            });
    connect(&m_sink, &QVideoSink::videoFrameChanged,
            this, &Recorder::onFrame);

    m_audioGapTimer.setInterval(100);
    connect(&m_audioGapTimer, &QTimer::timeout, this, &Recorder::fillAudioGap);
}

Recorder::~Recorder()
{
    m_running = false;
    m_recorder.disconnect(this);
    if (m_recorder.recorderState() != QMediaRecorder::StoppedState)
        m_recorder.stop();
    teardown();
}

QList<QMediaFormat::FileFormat> Recorder::supportedFileFormats()
{
    static const QMediaFormat::FileFormat preferred[] = {
        QMediaFormat::MPEG4, QMediaFormat::Matroska, QMediaFormat::WebM,
        QMediaFormat::QuickTime, QMediaFormat::AVI,
    };
    const auto available = QMediaFormat().supportedFileFormats(QMediaFormat::Encode);
    QList<QMediaFormat::FileFormat> result;
    for (auto f : preferred) {
        if (available.contains(f))
            result.append(f);
    }
    return result;
}

QString Recorder::suffixFor(QMediaFormat::FileFormat format)
{
    switch (format) {
    case QMediaFormat::Matroska:  return QStringLiteral("mkv");
    case QMediaFormat::WebM:      return QStringLiteral("webm");
    case QMediaFormat::QuickTime: return QStringLiteral("mov");
    case QMediaFormat::AVI:       return QStringLiteral("avi");
    default:                      return QStringLiteral("mp4");
    }
}

bool Recorder::isActive() const
{
    return m_running;
}

bool Recorder::isPaused() const
{
    return m_recorder.recorderState() == QMediaRecorder::PausedState;
}

bool Recorder::start(const Settings &settings)
{
    if (m_running)
        return false;

    teardown();
    m_settings = settings;
    m_failed = false;
    m_lastFrameUs = -1;
    m_recordStartUs = -1;
    m_pauseStartUs = -1;
    m_pausedTotalUs = 0;
    m_audioFramesSent = 0;

    auto captureError = [this](const QString &what, const QString &text) {
        fail(text.isEmpty() ? what : what + QStringLiteral(": ") + text);
    };

    switch (settings.mode) {
    case Mode::FullScreen:
    case Mode::Region: {
        if (!settings.screen) {
            fail(tr("The selected screen is no longer available."));
            return false;
        }
        if (settings.mode == Mode::Region && settings.region.isEmpty()) {
            fail(tr("No area has been selected."));
            return false;
        }
        m_screenCapture = new QScreenCapture(this);
        m_screenCapture->setScreen(settings.screen);
        connect(m_screenCapture, &QScreenCapture::errorOccurred, this,
                [captureError](QScreenCapture::Error, const QString &text) {
                    captureError(tr("Screen capture failed"), text);
                });
        m_grabSession.setScreenCapture(m_screenCapture);
        break;
    }
    case Mode::Window: {
        if (!settings.window.isValid()) {
            fail(tr("The selected window is no longer available."));
            return false;
        }
        m_windowCapture = new QWindowCapture(this);
        m_windowCapture->setWindow(settings.window);
        connect(m_windowCapture, &QWindowCapture::errorOccurred, this,
                [captureError](QWindowCapture::Error, const QString &text) {
                    captureError(tr("Window capture failed"), text);
                });
        m_grabSession.setWindowCapture(m_windowCapture);
        break;
    }
    }

    m_frameInput = new QVideoFrameInput(this);
    m_session.setVideoFrameInput(m_frameInput);
    m_grabSession.setVideoSink(&m_sink);

    switch (settings.audio) {
    case Audio::None:
        break;
    case Audio::Device:
        if (settings.audioDevice.isNull()) {
            fail(tr("The selected audio input is no longer available."));
            return false;
        }
        m_audioInput = new QAudioInput(settings.audioDevice, this);
        m_session.setAudioInput(m_audioInput);
        break;
    case Audio::System: {
        m_systemAudio = SystemAudio::create();
        std::string error;
        // The backend calls us on its own thread; hop to ours before touching anything.
        auto forward = [this, backend = m_systemAudio.get()](const float *samples, std::size_t frames) {
            QByteArray data(reinterpret_cast<const char *>(samples),
                            qsizetype(frames * std::size_t(backend->format().channels) * sizeof(float)));
            QMetaObject::invokeMethod(this, [this, data] { onSystemAudio(data); }, Qt::QueuedConnection);
        };
        if (!m_systemAudio || !m_systemAudio->start(forward, error)) {
            m_systemAudio.reset();
            fail(tr("System audio capture is not available: %1")
                     .arg(error.empty() ? tr("not supported on this system") : QString::fromStdString(error)));
            return false;
        }
        const SystemAudio::Format actual = m_systemAudio->format();
        m_audioFormat.setSampleFormat(QAudioFormat::Float);
        m_audioFormat.setSampleRate(actual.sampleRate);
        m_audioFormat.setChannelCount(actual.channels);
        // Deliberately created without a format: the recorder then takes the format from the
        // first buffer. With a preset format Qt 6.8's FFmpeg backend never reports the input as
        // ready, so every sendAudioBuffer() call is rejected.
        m_audioBufferInput = new QAudioBufferInput(this);
        m_session.setAudioBufferInput(m_audioBufferInput);
        break;
    }
    }

    // Prefer the most widely playable codecs the installed encoder set offers for this
    // container (left to itself Qt may pick e.g. HEVC), then let Qt fill in anything missing.
    QMediaFormat format(settings.fileFormat);
    using VC = QMediaFormat::VideoCodec;
    using AC = QMediaFormat::AudioCodec;
    const bool webm = settings.fileFormat == QMediaFormat::WebM;
    const QList<VC> wantedVideo = webm ? QList<VC>{ VC::VP9, VC::VP8, VC::AV1 }
                                       : QList<VC>{ VC::H264, VC::H265, VC::MPEG4 };
    const QList<AC> wantedAudio = webm ? QList<AC>{ AC::Opus, AC::Vorbis }
                                       : QList<AC>{ AC::AAC, AC::MP3, AC::Opus };
    const auto videoCodecs = format.supportedVideoCodecs(QMediaFormat::Encode);
    for (VC codec : wantedVideo) {
        if (videoCodecs.contains(codec)) {
            format.setVideoCodec(codec);
            break;
        }
    }
    const auto audioCodecs = format.supportedAudioCodecs(QMediaFormat::Encode);
    for (AC codec : wantedAudio) {
        if (audioCodecs.contains(codec)) {
            format.setAudioCodec(codec);
            break;
        }
    }
    format.resolveForEncoding(QMediaFormat::RequiresVideo);
    m_recorder.setMediaFormat(format);
    m_recorder.setEncodingMode(QMediaRecorder::ConstantQualityEncoding);
    m_recorder.setQuality(settings.quality);
    m_recorder.setVideoFrameRate(settings.frameRate);
    m_recorder.setOutputLocation(QUrl::fromLocalFile(settings.outputPath));

    m_running = true;
    m_clock.start();
    if (m_screenCapture)
        m_screenCapture->start();
    if (m_windowCapture)
        m_windowCapture->start();
    if (m_failed)            // capture source refused to start (e.g. no permission)
        return false;

    m_recorder.record();
    return !m_failed;
}

void Recorder::stop()
{
    if (!m_running)
        return;
    if (m_recorder.recorderState() == QMediaRecorder::StoppedState) {
        // Recorder never got going; nothing to finalize.
        m_running = false;
        teardown();
        return;
    }
    m_recorder.stop();      // finished() is emitted once the file has been finalized
}

void Recorder::setPaused(bool paused)
{
    if (!m_running)
        return;
    if (paused)
        m_recorder.pause();
    else
        m_recorder.record();
}

QVideoFrame Recorder::cropped(const QVideoFrame &frame) const
{
    const QImage image = frame.toImage();
    if (image.isNull() || !m_settings.screen)
        return {};

    // The selection is in logical pixels, frames are in physical pixels.
    const QSizeF logical = m_settings.screen->geometry().size();
    const qreal sx = image.width() / logical.width();
    const qreal sy = image.height() / logical.height();
    const QRect &r = m_settings.region;
    QRect crop(qRound(r.x() * sx), qRound(r.y() * sy),
               qRound(r.width() * sx), qRound(r.height() * sy));
    crop &= image.rect();
    // Most encoders (H.264 / 4:2:0) need even dimensions.
    crop.setWidth(crop.width() & ~1);
    crop.setHeight(crop.height() & ~1);
    if (crop.width() < 2 || crop.height() < 2)
        return {};
    return QVideoFrame(image.copy(crop));
}

// Every captured frame passes through here: throttle to the requested frame rate,
// crop to the selected rectangle in Region mode, then hand it to the encoder.
void Recorder::onFrame(const QVideoFrame &frame)
{
    if (!m_running || !m_frameInput || !frame.isValid())
        return;
    if (m_recorder.recorderState() != QMediaRecorder::RecordingState)
        return;

    // Presentation time: recording time with pauses removed (the recorder does not close
    // that gap for pushed frames). Audio uses the same clock, which keeps the two in sync.
    const qint64 startUs = activeUs();

    // Throttle to the requested frame rate (screens usually deliver 60 Hz or more).
    const qint64 intervalUs = 1'000'000 / qMax(1, m_settings.frameRate);
    if (m_lastFrameUs >= 0 && startUs - m_lastFrameUs < intervalUs * 9 / 10)
        return;

    QVideoFrame out = frame;
    if (m_settings.mode == Mode::Region) {
        out = cropped(frame);
        if (!out.isValid())
            return;
    }
    out.setStartTime(startUs);
    out.setEndTime(startUs + intervalUs);
    if (m_frameInput->sendVideoFrame(out))    // false = encoder queue is full, drop this frame
        m_lastFrameUs = startUs;
}

// Microseconds of actual recording so far, i.e. excluding time spent paused.
qint64 Recorder::activeUs()
{
    const qint64 nowUs = m_clock.nsecsElapsed() / 1000;
    if (m_recordStartUs < 0)
        m_recordStartUs = nowUs;
    if (m_pauseStartUs >= 0)
        return m_pauseStartUs - m_recordStartUs - m_pausedTotalUs;
    return nowUs - m_recordStartUs - m_pausedTotalUs;
}

void Recorder::sendAudio(const QByteArray &samples)
{
    if (!m_audioBufferInput || samples.isEmpty())
        return;
    if (m_audioBufferInput->sendAudioBuffer(QAudioBuffer(samples, m_audioFormat)))
        m_audioFramesSent += samples.size() / m_audioFormat.bytesPerFrame();
}

void Recorder::onSystemAudio(const QByteArray &samples)
{
    if (!m_running || m_recorder.recorderState() != QMediaRecorder::RecordingState)
        return;
    fillAudioGap();
    // ...and the reverse: a backend that starts late and then delivers its backlog in a burst
    // would leave the audio permanently behind the picture, so drop what is ahead of the clock.
    const qint64 rate = m_audioFormat.sampleRate();
    if (m_audioFramesSent - activeUs() * rate / 1'000'000 > rate / 5)
        return;
    sendAudio(samples);
}

// The encoder timestamps audio by counting samples, so the stream must be continuous. Some
// backends deliver nothing while the system is silent (WASAPI loopback) and any backend can
// drop out; pad with silence whenever we fall noticeably behind the recording clock.
void Recorder::fillAudioGap()
{
    if (!m_running || !m_audioBufferInput
        || m_recorder.recorderState() != QMediaRecorder::RecordingState)
        return;
    const qint64 rate = m_audioFormat.sampleRate();
    const qint64 expected = activeUs() * rate / 1'000'000;
    const qint64 missing = expected - m_audioFramesSent;
    if (missing > rate / 5)         // more than 200 ms behind
        sendAudio(QByteArray(qsizetype((missing - rate / 20) * m_audioFormat.bytesPerFrame()), '\0'));
}

void Recorder::onRecorderStateChanged(QMediaRecorder::RecorderState state)
{
    switch (state) {
    case QMediaRecorder::RecordingState:
        if (m_pauseStartUs >= 0) {
            m_pausedTotalUs += m_clock.nsecsElapsed() / 1000 - m_pauseStartUs;
            m_pauseStartUs = -1;
        }
        activeUs();                 // latches the recording start time on first entry
        if (m_audioBufferInput)
            m_audioGapTimer.start();
        emit pausedChanged(false);
        emit started();
        break;
    case QMediaRecorder::PausedState:
        if (m_pauseStartUs < 0)
            m_pauseStartUs = m_clock.nsecsElapsed() / 1000;
        emit pausedChanged(true);
        break;
    case QMediaRecorder::StoppedState:
        if (m_running) {
            m_running = false;
            const QString path = m_recorder.actualLocation().toLocalFile();
            teardown();
            if (!m_failed)
                emit finished(path.isEmpty() ? m_settings.outputPath : path);
        }
        break;
    }
}

void Recorder::fail(const QString &message)
{
    if (m_failed)
        return;
    m_failed = true;
    emit failed(message.isEmpty() ? tr("Recording failed.") : message);
    if (m_running) {
        if (m_recorder.recorderState() != QMediaRecorder::StoppedState) {
            m_recorder.stop();
        } else {
            m_running = false;
            teardown();
        }
    }
}

void Recorder::teardown()
{
    m_audioGapTimer.stop();
    if (m_systemAudio) {
        m_systemAudio->stop();      // joins the capture thread; queued buffers are ignored
        m_systemAudio.reset();
    }
    m_session.setAudioInput(nullptr);
    m_session.setAudioBufferInput(nullptr);
    if (m_audioInput)       { m_audioInput->deleteLater();       m_audioInput = nullptr; }
    if (m_audioBufferInput) { m_audioBufferInput->deleteLater(); m_audioBufferInput = nullptr; }

    if (m_screenCapture)
        m_screenCapture->stop();
    if (m_windowCapture)
        m_windowCapture->stop();

    m_grabSession.setVideoSink(nullptr);
    m_grabSession.setScreenCapture(nullptr);
    m_grabSession.setWindowCapture(nullptr);
    m_session.setVideoFrameInput(nullptr);

    if (m_screenCapture) { m_screenCapture->deleteLater(); m_screenCapture = nullptr; }
    if (m_windowCapture) { m_windowCapture->deleteLater(); m_windowCapture = nullptr; }
    if (m_frameInput)    { m_frameInput->deleteLater();    m_frameInput = nullptr; }
}

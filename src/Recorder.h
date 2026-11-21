#pragma once

#include <QAudioDevice>
#include <QAudioFormat>
#include <QCapturableWindow>
#include <QElapsedTimer>
#include <QMediaCaptureSession>
#include <QMediaFormat>
#include <QMediaRecorder>
#include <QObject>
#include <QPointer>
#include <QRect>
#include <QTimer>
#include <QVideoSink>

#include <memory>

class PortalCapture;
class QAudioBufferInput;
class QAudioInput;
class QScreen;
class SystemAudio;
class QScreenCapture;
class QWindowCapture;
class QVideoFrame;
class QVideoFrameInput;

// Records a full screen, a rectangular area of a screen, or a single window
// to a video file using Qt Multimedia.
//
//   QScreenCapture / QWindowCapture ▶ QVideoSink ▶ onFrame() ▶ QVideoFrameInput ▶ QMediaRecorder
//
// onFrame() throttles to the requested frame rate and, in Region mode, crops each frame.
//
// Audio is optional:
//   Device : QAudioInput (microphone, line-in, ...) ─────────────────────▶ QMediaRecorder
//   System : SystemAudio backend ▶ onSystemAudio() ▶ QAudioBufferInput ──▶ QMediaRecorder
class Recorder : public QObject
{
    Q_OBJECT
public:
    enum class Mode { FullScreen, Region, Window };
    enum class Audio { None, System, Device };

    struct Settings {
        Mode mode = Mode::FullScreen;
        QPointer<QScreen> screen;          // FullScreen / Region
        QRect region;                      // Region: logical pixels, relative to the screen's top-left
        QCapturableWindow window;          // Window
        bool viaPortal = false;            // Wayland: the desktop's own picker chooses the screen/window
        quint32 pipeWireNode = 0;          // test hook: capture this PipeWire node instead
        QString outputPath;
        QMediaFormat::FileFormat fileFormat = QMediaFormat::MPEG4;
        QMediaRecorder::Quality quality = QMediaRecorder::HighQuality;
        int frameRate = 30;
        Audio audio = Audio::None;
        QAudioDevice audioDevice;          // Audio::Device
    };

    explicit Recorder(QObject *parent = nullptr);
    ~Recorder() override;

    bool start(const Settings &settings);
    void stop();
    void setPaused(bool paused);

    bool isActive() const;      // recording or paused
    bool isPaused() const;

    // Container formats this Qt build can actually encode, in display order.
    static QList<QMediaFormat::FileFormat> supportedFileFormats();
    static QString suffixFor(QMediaFormat::FileFormat format);

    // True on Linux Wayland sessions with a screen-sharing portal: applications cannot grab the
    // screen or list windows there, so capture goes through the desktop's own picker dialog.
    static bool captureNeedsPortal();

signals:
    void started();
    void pausedChanged(bool paused);
    void durationChanged(qint64 milliseconds);
    void finished(const QString &filePath);
    void cancelled();                   // stopped before anything was recorded
    void failed(const QString &message);

private:
    void onFrame(const QVideoFrame &frame);
    QVideoFrame cropped(const QVideoFrame &frame) const;
    void onSystemAudio(const QByteArray &samples);
    void fillAudioGap();
    void sendAudio(const QByteArray &samples);
    qint64 activeUs();
    void onRecorderStateChanged(QMediaRecorder::RecorderState state);
    void fail(const QString &message);
    void teardown();

    Settings m_settings;
    QMediaCaptureSession m_grabSession;   // capture source ▶ m_sink
    QMediaCaptureSession m_session;       // m_frameInput ▶ m_recorder
    QMediaRecorder m_recorder;
    QVideoSink m_sink;
    QScreenCapture *m_screenCapture = nullptr;
    QWindowCapture *m_windowCapture = nullptr;
    QVideoFrameInput *m_frameInput = nullptr;
    PortalCapture *m_portal = nullptr;

    QAudioInput *m_audioInput = nullptr;
    QAudioBufferInput *m_audioBufferInput = nullptr;
    std::unique_ptr<SystemAudio> m_systemAudio;
    QAudioFormat m_audioFormat;
    QTimer m_audioGapTimer;
    qint64 m_audioFramesSent = 0;

    QElapsedTimer m_clock;
    qint64 m_lastFrameUs = -1;      // all in microseconds
    qint64 m_recordStartUs = -1;
    qint64 m_pauseStartUs = -1;
    qint64 m_pausedTotalUs = 0;
    bool m_running = false;
    bool m_failed = false;
};

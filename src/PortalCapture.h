#pragma once

#include <QDBusObjectPath>
#include <QObject>
#include <QSize>
#include <QVariantMap>
#include <QVideoFrame>

#include <atomic>

struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_stream;
struct spa_pod;

// Linux only. Screen/window capture through the xdg-desktop-portal ScreenCast interface and
// PipeWire. This is the only way to capture an individual window on Wayland: the desktop shows
// its own picker (the user clicks the window to share) and hands us a PipeWire video stream.
// Qt 6.8's own portal support only ever asks for whole monitors.
//
//   start() ─▶ portal: CreateSession ▶ SelectSources ▶ Start (system picker) ▶ OpenPipeWireRemote
//           ─▶ PipeWire stream ─▶ frame(QVideoFrame) on the object's thread
class PortalCapture : public QObject
{
    Q_OBJECT
public:
    enum SourceType { Monitor = 1, Window = 2 };

    explicit PortalCapture(QObject *parent = nullptr);
    ~PortalCapture() override;

    // True when the desktop offers the ScreenCast portal for the given source type.
    static bool isAvailable(SourceType type);
    // True when the session is Wayland, i.e. windows cannot be enumerated or grabbed directly.
    static bool isWaylandSession();

    void setMaximumFrameRate(int fps) { m_maxFps = fps > 0 ? fps : 30; }
    void start(SourceType type);        // asynchronous; ends in started(), cancelled() or failed()
    void stop();

    // Test hook: skip the portal and connect to a node of the default PipeWire instance.
    void startOnNode(quint32 nodeId);

signals:
    void started();
    void cancelled();                   // the user dismissed the system picker
    void failed(const QString &message);
    void frame(const QVideoFrame &frame);

private slots:
    void onPortalResponse(uint response, const QVariantMap &results);

private:
    enum class Step { Idle, CreateSession, SelectSources, Start, Streaming };

    bool call(const QString &method, const QVariantList &args, const QString &what);
    QString nextToken();
    bool openStream(int fd, quint32 nodeId);
    void closeStream();
    void fail(const QString &message);

    // PipeWire callbacks (PipeWire loop thread).
    static void onStreamParamChanged(void *self, uint32_t id, const spa_pod *param);
    static void onStreamProcess(void *self);
    static void onStreamStateChanged(void *self, int oldState, int newState, const char *error);

    Step m_step = Step::Idle;
    SourceType m_type = Window;
    QDBusObjectPath m_session;
    int m_tokenCounter = 0;
    int m_maxFps = 30;

    pw_thread_loop *m_loop = nullptr;
    pw_context *m_context = nullptr;
    pw_core *m_core = nullptr;
    pw_stream *m_stream = nullptr;
    struct Hook;
    Hook *m_hook = nullptr;

    // Written on the PipeWire thread only.
    QSize m_size;
    int m_spaFormat = 0;
    qint64 m_lastFrameNs = 0;
    std::atomic<bool> m_announced { false };
};

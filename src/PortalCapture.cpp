#include "PortalCapture.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QImage>
#include <QRandomGenerator>

#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <spa/utils/result.h>

#include <cstring>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

namespace {
const QString kService = QStringLiteral("org.freedesktop.portal.Desktop");
const QString kPath = QStringLiteral("/org/freedesktop/portal/desktop");
const QString kScreenCast = QStringLiteral("org.freedesktop.portal.ScreenCast");

qint64 monotonicNs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return qint64(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}
}

struct PortalCapture::Hook {
    spa_hook listener {};
};

PortalCapture::PortalCapture(QObject *parent)
    : QObject(parent)
{
}

PortalCapture::~PortalCapture()
{
    stop();
}

bool PortalCapture::isWaylandSession()
{
    return qEnvironmentVariable("XDG_SESSION_TYPE").compare(QLatin1String("wayland"), Qt::CaseInsensitive) == 0
           || !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY");
}

bool PortalCapture::isAvailable(SourceType type)
{
    static uint available = [] {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!bus.isConnected())
            return 0u;
        QDBusInterface properties(kService, kPath, QStringLiteral("org.freedesktop.DBus.Properties"), bus);
        properties.setTimeout(2000);
        const QDBusReply<QVariant> reply = properties.call(QStringLiteral("Get"), kScreenCast,
                                                           QStringLiteral("AvailableSourceTypes"));
        return reply.isValid() ? reply.value().toUInt() : 0u;
    }();
    return (available & uint(type)) != 0;
}

QString PortalCapture::nextToken()
{
    return QStringLiteral("qscreenrec_%1_%2").arg(QRandomGenerator::global()->bounded(1, 1 << 30)).arg(++m_tokenCounter);
}

bool PortalCapture::call(const QString &method, const QVariantList &args, const QString &what)
{
    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kScreenCast, method);
    message.setArguments(args);
    const QDBusMessage reply = QDBusConnection::sessionBus().call(message, QDBus::Block, 10000);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        fail(tr("%1 failed: %2").arg(what, reply.errorMessage()));
        return false;
    }
    return true;
}

void PortalCapture::start(SourceType type)
{
    stop();
    m_type = type;

    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected() || !isAvailable(type)) {
        fail(tr("The desktop's screen-sharing portal (xdg-desktop-portal ScreenCast) is not available."));
        return;
    }
    // Every portal request answers through a Response signal on its own request object.
    bus.connect(kService, QString(), QStringLiteral("org.freedesktop.portal.Request"),
                QStringLiteral("Response"), this, SLOT(onPortalResponse(uint, QVariantMap)));

    m_step = Step::CreateSession;
    const QVariantMap options {
        { QStringLiteral("handle_token"), nextToken() },
        { QStringLiteral("session_handle_token"), nextToken() },
    };
    call(QStringLiteral("CreateSession"), { options }, tr("Creating the screen-sharing session"));
}

void PortalCapture::onPortalResponse(uint response, const QVariantMap &results)
{
    if (m_step == Step::Idle || m_step == Step::Streaming)
        return;
    if (response != 0) {
        // 1 = the user cancelled the dialog, 2 = other failure.
        const bool userCancelled = response == 1;
        stop();
        if (userCancelled)
            emit cancelled();
        else
            emit failed(tr("The desktop refused the screen-sharing request."));
        return;
    }

    switch (m_step) {
    case Step::CreateSession: {
        m_session = QDBusObjectPath(results.value(QStringLiteral("session_handle")).toString());
        m_step = Step::SelectSources;
        const QVariantMap options {
            { QStringLiteral("handle_token"), nextToken() },
            { QStringLiteral("types"), uint(m_type) },
            { QStringLiteral("multiple"), false },
            { QStringLiteral("cursor_mode"), uint(2) },      // 2 = cursor embedded in the frames
        };
        call(QStringLiteral("SelectSources"), { QVariant::fromValue(m_session), options },
             tr("Selecting the capture source"));
        break;
    }
    case Step::SelectSources: {
        m_step = Step::Start;                               // shows the system picker
        const QVariantMap options { { QStringLiteral("handle_token"), nextToken() } };
        call(QStringLiteral("Start"), { QVariant::fromValue(m_session), QString(), options },
             tr("Starting screen sharing"));
        break;
    }
    case Step::Start: {
        // results["streams"] has D-Bus type a(ua{sv}): PipeWire node id + properties.
        quint32 nodeId = 0;
        bool found = false;
        const QDBusArgument streams = results.value(QStringLiteral("streams")).value<QDBusArgument>();
        streams.beginArray();
        while (!streams.atEnd()) {
            quint32 id = 0;
            QVariantMap properties;
            streams.beginStructure();
            streams >> id >> properties;
            streams.endStructure();
            if (!found) {
                nodeId = id;
                found = true;
            }
        }
        streams.endArray();
        if (!found) {
            fail(tr("The desktop did not provide a video stream."));
            return;
        }

        QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kScreenCast,
                                                              QStringLiteral("OpenPipeWireRemote"));
        message.setArguments({ QVariant::fromValue(m_session), QVariantMap() });
        const QDBusReply<QDBusUnixFileDescriptor> reply = QDBusConnection::sessionBus().call(message);
        if (!reply.isValid()) {
            fail(tr("Opening the PipeWire connection failed: %1").arg(reply.error().message()));
            return;
        }
        m_step = Step::Streaming;
        if (!openStream(reply.value().fileDescriptor(), nodeId))
            fail(tr("Connecting to the PipeWire video stream failed."));
        break;
    }
    default:
        break;
    }
}

void PortalCapture::startOnNode(quint32 nodeId)
{
    stop();
    m_step = Step::Streaming;
    if (!openStream(-1, nodeId))
        fail(tr("Connecting to the PipeWire video stream failed."));
}

void PortalCapture::fail(const QString &message)
{
    stop();
    emit failed(message);
}

void PortalCapture::stop()
{
    closeStream();
    if (!m_session.path().isEmpty()) {
        QDBusMessage close = QDBusMessage::createMethodCall(kService, m_session.path(),
                                                            QStringLiteral("org.freedesktop.portal.Session"),
                                                            QStringLiteral("Close"));
        QDBusConnection::sessionBus().call(close, QDBus::NoBlock);
        m_session = QDBusObjectPath();
    }
    if (m_step != Step::Idle) {
        QDBusConnection::sessionBus().disconnect(kService, QString(),
                                                 QStringLiteral("org.freedesktop.portal.Request"),
                                                 QStringLiteral("Response"), this,
                                                 SLOT(onPortalResponse(uint, QVariantMap)));
    }
    m_step = Step::Idle;
}

// ---------------------------------------------------------------------------------------------
// PipeWire

bool PortalCapture::openStream(int fd, quint32 nodeId)
{
    static const pw_stream_events streamEvents = [] {
        pw_stream_events e {};
        e.version = PW_VERSION_STREAM_EVENTS;
        e.state_changed = [](void *self, pw_stream_state oldState, pw_stream_state newState, const char *error) {
            PortalCapture::onStreamStateChanged(self, int(oldState), int(newState), error);
        };
        e.param_changed = &PortalCapture::onStreamParamChanged;
        e.process = &PortalCapture::onStreamProcess;
        return e;
    }();

    pw_init(nullptr, nullptr);
    m_announced = false;
    m_lastFrameNs = 0;
    m_size = {};

    m_loop = pw_thread_loop_new("qscreenrec-capture", nullptr);
    if (!m_loop)
        return false;
    m_context = pw_context_new(pw_thread_loop_get_loop(m_loop), nullptr, 0);
    if (!m_context || pw_thread_loop_start(m_loop) < 0)
        return false;

    pw_thread_loop_lock(m_loop);
    // pw_context_connect_fd takes ownership of the descriptor, so hand it a duplicate.
    m_core = fd >= 0 ? pw_context_connect_fd(m_context, fcntl(fd, F_DUPFD_CLOEXEC, 3), nullptr, 0)
                     : pw_context_connect(m_context, nullptr, 0);
    bool ok = m_core != nullptr;
    if (ok) {
        m_stream = pw_stream_new(m_core, "QScreenRec capture",
                                 pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
                                                   PW_KEY_MEDIA_CATEGORY, "Capture",
                                                   PW_KEY_MEDIA_ROLE, "Screen", nullptr));
        ok = m_stream != nullptr;
    }
    if (ok) {
        m_hook = new Hook;
        pw_stream_add_listener(m_stream, &m_hook->listener, &streamEvents, this);

        uint8_t buffer[1024];
        spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        spa_rectangle defSize = SPA_RECTANGLE(1920, 1080);
        spa_rectangle minSize = SPA_RECTANGLE(1, 1);
        spa_rectangle maxSize = SPA_RECTANGLE(16384, 16384);
        spa_fraction defRate = SPA_FRACTION(uint32_t(m_maxFps), 1);
        spa_fraction minRate = SPA_FRACTION(0, 1);
        spa_fraction maxRate = SPA_FRACTION(360, 1);
        // No modifier property: this asks for plain memory buffers rather than DMA-BUFs.
        const spa_pod *params[1];
        params[0] = static_cast<const spa_pod *>(spa_pod_builder_add_object(
                &builder,
                SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
                SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5,
                        SPA_VIDEO_FORMAT_BGRx,
                        SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA,
                        SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA),
                SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&defSize, &minSize, &maxSize),
                SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&defRate, &minRate, &maxRate)));
        ok = pw_stream_connect(m_stream, PW_DIRECTION_INPUT, nodeId,
                               static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
                               params, 1) >= 0;
    }
    pw_thread_loop_unlock(m_loop);
    return ok;
}

void PortalCapture::closeStream()
{
    if (!m_loop)
        return;
    pw_thread_loop_lock(m_loop);
    if (m_stream) {
        if (m_hook)
            spa_hook_remove(&m_hook->listener);
        pw_stream_disconnect(m_stream);
        pw_stream_destroy(m_stream);
        m_stream = nullptr;
    }
    if (m_core) {
        pw_core_disconnect(m_core);
        m_core = nullptr;
    }
    pw_thread_loop_unlock(m_loop);
    pw_thread_loop_stop(m_loop);            // joins the loop thread: no callbacks after this
    if (m_context) {
        pw_context_destroy(m_context);
        m_context = nullptr;
    }
    pw_thread_loop_destroy(m_loop);
    m_loop = nullptr;
    delete m_hook;
    m_hook = nullptr;
}

void PortalCapture::onStreamStateChanged(void *selfPtr, int, int newState, const char *error)
{
    auto *self = static_cast<PortalCapture *>(selfPtr);
    if (newState == PW_STREAM_STATE_ERROR) {
        const QString message = tr("The video stream failed: %1").arg(QString::fromUtf8(error ? error : "unknown error"));
        QMetaObject::invokeMethod(self, [self, message] { self->fail(message); }, Qt::QueuedConnection);
    }
}

void PortalCapture::onStreamParamChanged(void *selfPtr, uint32_t id, const spa_pod *param)
{
    auto *self = static_cast<PortalCapture *>(selfPtr);
    if (!param || id != SPA_PARAM_Format)
        return;

    spa_video_info_raw info {};
    if (spa_format_video_raw_parse(param, &info) < 0)
        return;
    self->m_size = QSize(int(info.size.width), int(info.size.height));
    self->m_spaFormat = int(info.format);

    // Ask for CPU-readable buffers and for the crop rectangle some compositors attach to
    // window streams.
    uint8_t buffer[512];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const spa_pod *params[2];
    params[0] = static_cast<const spa_pod *>(spa_pod_builder_add_object(
            &builder,
            SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
            SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd))));
    params[1] = static_cast<const spa_pod *>(spa_pod_builder_add_object(
            &builder,
            SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
            SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop),
            SPA_PARAM_META_size, SPA_POD_Int(int(sizeof(spa_meta_region)))));
    pw_stream_update_params(self->m_stream, params, 2);
}

void PortalCapture::onStreamProcess(void *selfPtr)
{
    auto *self = static_cast<PortalCapture *>(selfPtr);

    // Keep only the newest queued buffer.
    pw_buffer *newest = nullptr;
    while (pw_buffer *next = pw_stream_dequeue_buffer(self->m_stream)) {
        if (newest)
            pw_stream_queue_buffer(self->m_stream, newest);
        newest = next;
    }
    if (!newest)
        return;

    const qint64 now = monotonicNs();
    const qint64 interval = 1'000'000'000 / self->m_maxFps;
    spa_buffer *buf = newest->buffer;
    spa_data &data = buf->datas[0];
    const QSize size = self->m_size;

    // Throttle here, before paying for the copy.
    const bool due = self->m_lastFrameNs == 0 || now - self->m_lastFrameNs >= interval * 9 / 10;
    if (due && data.data && data.chunk && data.chunk->size > 0 && !size.isEmpty()) {
        const int stride = data.chunk->stride > 0 ? data.chunk->stride : size.width() * 4;
        QRect area(QPoint(0, 0), size);
        if (auto *crop = static_cast<spa_meta_region *>(
                    spa_buffer_find_meta_data(buf, SPA_META_VideoCrop, sizeof(spa_meta_region)))) {
            if (spa_meta_region_is_valid(crop)) {
                area &= QRect(crop->region.position.x, crop->region.position.y,
                              int(crop->region.size.width), int(crop->region.size.height));
            }
        }
        // Even dimensions keep 4:2:0 encoders happy (window sizes are often odd).
        area.setWidth(area.width() & ~1);
        area.setHeight(area.height() & ~1);

        const bool bgr = self->m_spaFormat == SPA_VIDEO_FORMAT_BGRx || self->m_spaFormat == SPA_VIDEO_FORMAT_BGRA;
        const qsizetype needed = qsizetype(stride) * (area.bottom() + 1);
        if (area.width() >= 2 && area.height() >= 2 && stride >= size.width() * 4
            && needed <= qsizetype(data.maxsize)) {
            QImage image(area.size(), bgr ? QImage::Format_RGB32 : QImage::Format_RGBX8888);
            const uchar *src = static_cast<const uchar *>(data.data) + data.chunk->offset
                               + qsizetype(area.y()) * stride + qsizetype(area.x()) * 4;
            for (int y = 0; y < area.height(); ++y)
                std::memcpy(image.scanLine(y), src + qsizetype(y) * stride, size_t(area.width()) * 4);

            self->m_lastFrameNs = now;
            const QVideoFrame videoFrame(image);
            const bool first = !self->m_announced.exchange(true);
            QMetaObject::invokeMethod(self, [self, videoFrame, first] {
                if (self->m_step != Step::Streaming)
                    return;
                if (first)
                    emit self->started();
                emit self->frame(videoFrame);
            }, Qt::QueuedConnection);
        }
    }
    pw_stream_queue_buffer(self->m_stream, newest);
}

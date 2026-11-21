#include "SystemAudio.h"

#include <pulse/pulseaudio.h>

// Records the monitor of the default sink. Works with PulseAudio and with PipeWire's pulse
// server (pipewire-pulse), which is what current desktop distributions run.
//
// Uses the asynchronous API so that PA_STREAM_ADJUST_LATENCY can be requested: without it a
// monitor source may hand data over in blocks of a second or more, which would put the audio
// that far behind the video.
namespace {

class PulseSystemAudio final : public SystemAudio
{
public:
    ~PulseSystemAudio() override { stop(); }

    bool start(Callback callback, std::string &error) override
    {
        stop();
        m_callback = std::move(callback);

        m_loop = pa_threaded_mainloop_new();
        if (!m_loop) {
            error = "PulseAudio: cannot create main loop";
            return false;
        }
        m_context = pa_context_new(pa_threaded_mainloop_get_api(m_loop), "QScreenRec");
        pa_context_set_state_callback(m_context, &PulseSystemAudio::notify, this);

        pa_threaded_mainloop_lock(m_loop);
        bool ok = pa_context_connect(m_context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) >= 0
                  && pa_threaded_mainloop_start(m_loop) >= 0;

        // Wait for the connection to the sound server.
        while (ok) {
            const pa_context_state_t state = pa_context_get_state(m_context);
            if (state == PA_CONTEXT_READY)
                break;
            if (!PA_CONTEXT_IS_GOOD(state))
                ok = false;
            else
                pa_threaded_mainloop_wait(m_loop);
        }

        if (ok) {
            pa_sample_spec spec;
            spec.format = PA_SAMPLE_FLOAT32NE;
            spec.rate = static_cast<uint32_t>(m_format.sampleRate);
            spec.channels = static_cast<uint8_t>(m_format.channels);
            m_stream = pa_stream_new(m_context, "System audio", &spec, nullptr);
            ok = m_stream != nullptr;
        }
        if (ok) {
            pa_stream_set_state_callback(m_stream, &PulseSystemAudio::notifyStream, this);
            pa_stream_set_read_callback(m_stream, &PulseSystemAudio::onRead, this);

            pa_buffer_attr attr;
            attr.maxlength = static_cast<uint32_t>(-1);
            attr.tlength = static_cast<uint32_t>(-1);
            attr.prebuf = static_cast<uint32_t>(-1);
            attr.minreq = static_cast<uint32_t>(-1);
            attr.fragsize = static_cast<uint32_t>(kFragmentFrames * m_format.channels * sizeof(float));
            ok = pa_stream_connect_record(m_stream, "@DEFAULT_MONITOR@", &attr,
                                          PA_STREAM_ADJUST_LATENCY) >= 0;
        }
        while (ok) {
            const pa_stream_state_t state = pa_stream_get_state(m_stream);
            if (state == PA_STREAM_READY)
                break;
            if (!PA_STREAM_IS_GOOD(state))
                ok = false;
            else
                pa_threaded_mainloop_wait(m_loop);
        }
        if (!ok)
            error = std::string("PulseAudio: ") + pa_strerror(pa_context_errno(m_context));
        pa_threaded_mainloop_unlock(m_loop);

        if (!ok)
            stop();
        return ok;
    }

    void stop() override
    {
        if (!m_loop)
            return;
        pa_threaded_mainloop_lock(m_loop);
        if (m_stream) {
            pa_stream_set_read_callback(m_stream, nullptr, nullptr);
            pa_stream_set_state_callback(m_stream, nullptr, nullptr);
            pa_stream_disconnect(m_stream);
            pa_stream_unref(m_stream);
            m_stream = nullptr;
        }
        if (m_context) {
            pa_context_set_state_callback(m_context, nullptr, nullptr);
            pa_context_disconnect(m_context);
            pa_context_unref(m_context);
            m_context = nullptr;
        }
        pa_threaded_mainloop_unlock(m_loop);
        pa_threaded_mainloop_stop(m_loop);      // joins the loop thread: no callbacks after this
        pa_threaded_mainloop_free(m_loop);
        m_loop = nullptr;
        m_callback = nullptr;
    }

    Format format() const override { return m_format; }

private:
    static constexpr std::size_t kFragmentFrames = 960;   // 20 ms at 48 kHz

    static void notify(pa_context *, void *self)
    {
        pa_threaded_mainloop_signal(static_cast<PulseSystemAudio *>(self)->m_loop, 0);
    }
    static void notifyStream(pa_stream *, void *self)
    {
        pa_threaded_mainloop_signal(static_cast<PulseSystemAudio *>(self)->m_loop, 0);
    }

    // Runs on the PulseAudio loop thread.
    static void onRead(pa_stream *stream, std::size_t, void *selfPtr)
    {
        auto *self = static_cast<PulseSystemAudio *>(selfPtr);
        const std::size_t frameBytes = static_cast<std::size_t>(self->m_format.channels) * sizeof(float);
        for (;;) {
            const void *data = nullptr;
            std::size_t bytes = 0;
            if (pa_stream_peek(stream, &data, &bytes) < 0 || bytes == 0)
                break;
            if (data && self->m_callback)       // data == nullptr marks a hole in the stream
                self->m_callback(static_cast<const float *>(data), bytes / frameBytes);
            pa_stream_drop(stream);
        }
    }

    Format m_format;
    Callback m_callback;
    pa_threaded_mainloop *m_loop = nullptr;
    pa_context *m_context = nullptr;
    pa_stream *m_stream = nullptr;
};

} // namespace

bool SystemAudio::isSupported() { return true; }
std::unique_ptr<SystemAudio> SystemAudio::create() { return std::make_unique<PulseSystemAudio>(); }

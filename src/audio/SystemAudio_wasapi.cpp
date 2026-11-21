#include "SystemAudio.h"

#ifndef NOMINMAX
#  define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <atomic>
#include <cstdint>
#include <future>
#include <thread>
#include <vector>

// WASAPI loopback capture of the default render endpoint ("what you hear").
namespace {

template <typename T>
struct ComPtr {
    T *p = nullptr;
    ~ComPtr() { if (p) p->Release(); }
    T *operator->() const { return p; }
    T **operator&() { return &p; }
    explicit operator bool() const { return p != nullptr; }
};

std::string hresultText(const char *what, HRESULT hr)
{
    char buf[160];
    wsprintfA(buf, "%s failed (HRESULT 0x%08lX)", what, static_cast<unsigned long>(hr));
    return buf;
}

class WasapiSystemAudio final : public SystemAudio
{
public:
    ~WasapiSystemAudio() override { stop(); }

    bool start(Callback callback, std::string &error) override
    {
        stop();
        m_running = true;
        std::promise<std::string> ready;
        std::future<std::string> result = ready.get_future();
        m_thread = std::thread([this, cb = std::move(callback), p = std::move(ready)]() mutable {
            run(cb, p);
        });
        error = result.get();          // empty string = initialized and capturing
        if (!error.empty()) {
            stop();
            return false;
        }
        return true;
    }

    void stop() override
    {
        m_running = false;
        if (m_thread.joinable())
            m_thread.join();
    }

    Format format() const override { return m_format; }

private:
    void run(const Callback &cb, std::promise<std::string> &ready)
    {
        const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const std::string err = capture(cb, ready);
        if (!err.empty())
            ready.set_value(err);       // only reached when initialization failed
        if (SUCCEEDED(coInit))
            CoUninitialize();
    }

    // Returns a non-empty error if initialization failed; otherwise fulfils `ready` with ""
    // and loops until stop().
    std::string capture(const Callback &cb, std::promise<std::string> &ready)
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void **>(&enumerator));
        if (FAILED(hr))
            return hresultText("Creating the audio device enumerator", hr);

        ComPtr<IMMDevice> device;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr))
            return "No default audio output device to record from.";

        ComPtr<IAudioClient> client;
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void **>(&client));
        if (FAILED(hr))
            return hresultText("Activating the audio client", hr);

        WAVEFORMATEX *mix = nullptr;
        hr = client->GetMixFormat(&mix);
        if (FAILED(hr) || !mix)
            return hresultText("Querying the mix format", hr);

        bool isFloat = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        bool isPcm = mix->wFormatTag == WAVE_FORMAT_PCM;
        if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            // SubFormat GUIDs start with the classic format tag.
            const auto *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(mix);
            isFloat = ext->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT;
            isPcm = ext->SubFormat.Data1 == WAVE_FORMAT_PCM;
        }
        const int srcChannels = mix->nChannels;
        const int bits = mix->wBitsPerSample;
        const int blockAlign = mix->nBlockAlign;
        const bool supported = (isFloat && bits == 32) || (isPcm && (bits == 16 || bits == 32));

        m_format.sampleRate = static_cast<int>(mix->nSamplesPerSec);
        m_format.channels = srcChannels >= 2 ? 2 : 1;

        const REFERENCE_TIME bufferDuration = 2000000;      // 200 ms, in 100 ns units
        if (supported) {
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                    bufferDuration, 0, mix, nullptr);
        }
        CoTaskMemFree(mix);
        if (!supported)
            return "Unsupported audio mix format on the default output device.";
        if (FAILED(hr))
            return hresultText("Initializing loopback capture", hr);

        ComPtr<IAudioCaptureClient> capture;
        hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void **>(&capture));
        if (FAILED(hr))
            return hresultText("Getting the capture service", hr);
        hr = client->Start();
        if (FAILED(hr))
            return hresultText("Starting loopback capture", hr);

        ready.set_value(std::string());

        const int outChannels = m_format.channels;
        std::vector<float> out;
        while (m_running) {
            Sleep(10);
            UINT32 packet = 0;
            while (m_running && SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0) {
                BYTE *data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
                    break;
                out.assign(static_cast<std::size_t>(frames) * outChannels, 0.0f);
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && data) {
                    for (UINT32 f = 0; f < frames; ++f) {
                        const BYTE *src = data + static_cast<std::size_t>(f) * blockAlign;
                        for (int c = 0; c < outChannels; ++c) {     // keep front left / right
                            float v;
                            if (isFloat)
                                v = reinterpret_cast<const float *>(src)[c];
                            else if (bits == 16)
                                v = reinterpret_cast<const int16_t *>(src)[c] / 32768.0f;
                            else
                                v = reinterpret_cast<const int32_t *>(src)[c] / 2147483648.0f;
                            out[static_cast<std::size_t>(f) * outChannels + c] = v;
                        }
                    }
                }
                capture->ReleaseBuffer(frames);
                if (frames > 0)
                    cb(out.data(), frames);
            }
        }
        client->Stop();
        return std::string();
    }

    Format m_format;
    std::thread m_thread;
    std::atomic<bool> m_running { false };
};

} // namespace

bool SystemAudio::isSupported() { return true; }
std::unique_ptr<SystemAudio> SystemAudio::create() { return std::make_unique<WasapiSystemAudio>(); }

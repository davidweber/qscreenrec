#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

// Captures "what you hear" (the default output device's mix). Deliberately free of Qt so each
// platform backend is a small, self-contained translation unit:
//
//   Linux   : PulseAudio / PipeWire-pulse monitor source   (SystemAudio_pulse.cpp)
//   Windows : WASAPI loopback                              (SystemAudio_wasapi.cpp)
//   macOS   : ScreenCaptureKit audio, macOS 13+            (SystemAudio_mac.mm)
//
// Samples are always delivered as interleaved 32-bit float, on a backend-owned thread.
class SystemAudio
{
public:
    struct Format {
        int sampleRate = 48000;
        int channels = 2;
    };
    using Callback = std::function<void(const float *samples, std::size_t frames)>;

    virtual ~SystemAudio() = default;

    static bool isSupported();                       // on this platform / OS version / build
    static std::unique_ptr<SystemAudio> create();    // nullptr when !isSupported()

    virtual bool start(Callback callback, std::string &error) = 0;
    virtual void stop() = 0;
    virtual Format format() const = 0;               // valid after a successful start()
};

#include "SystemAudio.h"

#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <vector>

// System audio on macOS via ScreenCaptureKit (macOS 13+). Uses the same "Screen Recording"
// permission the video capture already needs; no virtual audio driver required.
// Compiled with ARC (see CMakeLists.txt).

API_AVAILABLE(macos(13.0))
@interface QSRSystemAudioOutput : NSObject <SCStreamOutput, SCStreamDelegate> {
@public
    SystemAudio::Callback callback;
    int channels;
    std::vector<float> scratch;
}
@end

@implementation QSRSystemAudioOutput

- (void)stream:(SCStream *)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type
{
    (void)stream;
    if (type != SCStreamOutputTypeAudio || !callback || !CMSampleBufferIsValid(sampleBuffer))
        return;

    CMFormatDescriptionRef description = CMSampleBufferGetFormatDescription(sampleBuffer);
    const AudioStreamBasicDescription *asbd =
            description ? CMAudioFormatDescriptionGetStreamBasicDescription(description) : nullptr;
    if (!asbd || !(asbd->mFormatFlags & kAudioFormatFlagIsFloat) || asbd->mBitsPerChannel != 32)
        return;

    const size_t frames = static_cast<size_t>(CMSampleBufferGetNumSamples(sampleBuffer));
    if (frames == 0)
        return;

    size_t listSize = 0;
    CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sampleBuffer, &listSize, nullptr, 0,
                                                            nullptr, nullptr, 0, nullptr);
    if (listSize == 0)
        return;
    std::vector<uint8_t> listStorage(listSize);
    AudioBufferList *list = reinterpret_cast<AudioBufferList *>(listStorage.data());
    CMBlockBufferRef block = nullptr;
    const OSStatus status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sampleBuffer, nullptr, list, listSize, nullptr, nullptr,
            kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &block);
    if (status != noErr || !block)
        return;

    scratch.assign(frames * static_cast<size_t>(channels), 0.0f);
    if (list->mNumberBuffers == 1 && list->mBuffers[0].mNumberChannels > 1) {
        // Interleaved source.
        const int srcChannels = static_cast<int>(list->mBuffers[0].mNumberChannels);
        const float *src = static_cast<const float *>(list->mBuffers[0].mData);
        const size_t available = list->mBuffers[0].mDataByteSize / (sizeof(float) * srcChannels);
        const size_t n = frames < available ? frames : available;
        for (size_t f = 0; f < n; ++f) {
            for (int c = 0; c < channels; ++c)
                scratch[f * channels + c] = src[f * srcChannels + (c < srcChannels ? c : 0)];
        }
    } else {
        // Planar source: one buffer per channel.
        const int srcChannels = static_cast<int>(list->mNumberBuffers);
        for (int c = 0; c < channels; ++c) {
            const AudioBuffer &buffer = list->mBuffers[c < srcChannels ? c : 0];
            const float *src = static_cast<const float *>(buffer.mData);
            const size_t available = buffer.mDataByteSize / sizeof(float);
            const size_t n = frames < available ? frames : available;
            for (size_t f = 0; f < n; ++f)
                scratch[f * channels + c] = src[f];
        }
    }
    CFRelease(block);
    callback(scratch.data(), frames);
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
    (void)stream;
    (void)error;
}

@end

namespace {

class API_AVAILABLE(macos(13.0)) MacSystemAudio final : public SystemAudio
{
public:
    ~MacSystemAudio() override { stop(); }

    bool start(Callback callback, std::string &error) override
    {
        stop();

        // 1. Find a display to attach the stream to (audio is system-wide regardless).
        __block SCShareableContent *content = nil;
        __block NSError *contentError = nil;
        dispatch_semaphore_t done = dispatch_semaphore_create(0);
        [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent *c, NSError *e) {
            content = c;
            contentError = e;
            dispatch_semaphore_signal(done);
        }];
        if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) != 0) {
            error = "Timed out waiting for ScreenCaptureKit.";
            return false;
        }
        if (!content || content.displays.count == 0) {
            error = contentError ? std::string(contentError.localizedDescription.UTF8String)
                                 : std::string("No display available for audio capture.");
            error += " (Check System Settings > Privacy & Security > Screen Recording.)";
            return false;
        }

        // 2. Audio-only in practice: the video side is as small and slow as allowed.
        SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:content.displays.firstObject
                                                          excludingWindows:@[]];
        SCStreamConfiguration *config = [[SCStreamConfiguration alloc] init];
        config.capturesAudio = YES;
        config.excludesCurrentProcessAudio = YES;
        config.sampleRate = m_format.sampleRate;
        config.channelCount = m_format.channels;
        config.width = 2;
        config.height = 2;
        config.minimumFrameInterval = CMTimeMake(1, 1);
        config.showsCursor = NO;

        m_output = [[QSRSystemAudioOutput alloc] init];
        m_output->callback = std::move(callback);
        m_output->channels = m_format.channels;

        m_queue = dispatch_queue_create("qscreenrec.systemaudio", DISPATCH_QUEUE_SERIAL);
        m_stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:m_output];

        NSError *addError = nil;
        if (![m_stream addStreamOutput:m_output type:SCStreamOutputTypeAudio
                    sampleHandlerQueue:m_queue error:&addError]) {
            error = addError ? addError.localizedDescription.UTF8String : "Cannot add audio output.";
            m_stream = nil;
            m_output = nil;
            return false;
        }
        // A screen output keeps ScreenCaptureKit from logging a dropped frame every second.
        [m_stream addStreamOutput:m_output type:SCStreamOutputTypeScreen
               sampleHandlerQueue:m_queue error:nil];

        // 3. Start.
        __block NSError *startError = nil;
        dispatch_semaphore_t started = dispatch_semaphore_create(0);
        [m_stream startCaptureWithCompletionHandler:^(NSError *e) {
            startError = e;
            dispatch_semaphore_signal(started);
        }];
        if (dispatch_semaphore_wait(started, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)) != 0
            || startError) {
            error = startError ? std::string(startError.localizedDescription.UTF8String)
                               : std::string("Timed out starting audio capture.");
            m_stream = nil;
            m_output = nil;
            return false;
        }
        return true;
    }

    void stop() override
    {
        if (!m_stream)
            return;
        dispatch_semaphore_t stopped = dispatch_semaphore_create(0);
        [m_stream stopCaptureWithCompletionHandler:^(NSError *) {
            dispatch_semaphore_signal(stopped);
        }];
        dispatch_semaphore_wait(stopped, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
        // Let any in-flight sample handler finish before the callback target goes away.
        dispatch_sync(m_queue, ^{});
        m_output->callback = nullptr;
        m_stream = nil;
        m_output = nil;
    }

    Format format() const override { return m_format; }

private:
    Format m_format;
    SCStream *m_stream = nil;
    QSRSystemAudioOutput *m_output = nil;
    dispatch_queue_t m_queue = nullptr;
};

} // namespace

bool SystemAudio::isSupported()
{
    if (@available(macOS 13.0, *))
        return true;
    return false;
}

std::unique_ptr<SystemAudio> SystemAudio::create()
{
    if (@available(macOS 13.0, *))
        return std::make_unique<MacSystemAudio>();
    return nullptr;
}

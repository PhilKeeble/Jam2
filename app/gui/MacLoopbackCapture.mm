#include "MacLoopbackCapture.hpp"

#include "RecordingTiming.hpp"
#include "audio_device.hpp"
#include "audio_ring.hpp"
#include "coreaudio_buffer_processing.hpp"

#import <Foundation/Foundation.h>
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>

#include <QUrl>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>

namespace jam2::gui {
namespace {

using namespace std::chrono_literals;
constexpr std::size_t kMaximumChannels = 64;
constexpr std::size_t kTransferFrames = 1024;
constexpr int kQueueSeconds = 2;

void checkStatus(OSStatus status, const char* operation)
{
    if (status != noErr) {
        throw std::runtime_error(QStringLiteral(
            "%1 (Core Audio OSStatus %2). If access was denied, allow Jam2 in "
            "System Settings > Privacy & Security > Screen & System Audio Recording.")
            .arg(QString::fromUtf8(operation)).arg(status).toStdString());
    }
}

template <typename T>
T property(AudioObjectID object, AudioObjectPropertySelector selector,
    AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
{
    AudioObjectPropertyAddress address{selector, scope, kAudioObjectPropertyElementMain};
    T value{};
    UInt32 size = sizeof(value);
    checkStatus(AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, &value),
        "Could not read loopback audio property");
    if (size != sizeof(value)) throw std::runtime_error("Unexpected loopback audio property size");
    return value;
}

QString stringProperty(AudioObjectID object, AudioObjectPropertySelector selector)
{
    NSString* value = CFBridgingRelease(property<CFStringRef>(object, selector));
    if (value == nullptr) throw std::runtime_error("Missing loopback device name or UID");
    return QString::fromNSString(value);
}

class NativeApi final : public MacLoopbackApi {
public:
    std::vector<MacLoopbackOutput> outputs() override
    {
        AudioObjectPropertyAddress address{kAudioHardwarePropertyDevices,
            kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        UInt32 size = 0;
        checkStatus(AudioObjectGetPropertyDataSize(kAudioObjectSystemObject,
            &address, 0, nullptr, &size), "Could not enumerate loopback outputs");
        std::vector<AudioDeviceID> devices(size / sizeof(AudioDeviceID));
        checkStatus(AudioObjectGetPropertyData(kAudioObjectSystemObject, &address,
            0, nullptr, &size, devices.data()), "Could not read loopback outputs");
        devices.resize(size / sizeof(AudioDeviceID));
        std::vector<MacLoopbackOutput> result;
        for (AudioDeviceID device : devices) {
            AudioObjectPropertyAddress streams{kAudioDevicePropertyStreams,
                kAudioDevicePropertyScopeOutput, kAudioObjectPropertyElementMain};
            UInt32 streamBytes = 0;
            // Devices may disappear during enumeration. Do not discard the
            // remaining usable outputs because one hot-plug device vanished.
            if (AudioObjectGetPropertyDataSize(device, &streams, 0, nullptr,
                    &streamBytes) != noErr || streamBytes == 0) continue;
            try {
                result.push_back({device,
                    stringProperty(device, kAudioDevicePropertyDeviceUID),
                    stringProperty(device, kAudioObjectPropertyName)});
            } catch (const std::exception&) {
                if (outputAlive(device)) throw;
            }
        }
        return result;
    }

    AudioDeviceID defaultOutput() override
    {
        return property<AudioDeviceID>(kAudioObjectSystemObject,
            kAudioHardwarePropertyDefaultOutputDevice);
    }

    AudioObjectID createTap(const MacLoopbackOutput& output) override
    {
        if (@available(macOS 14.2, *)) {
            CATapDescription* description = [[CATapDescription alloc]
                initExcludingProcesses:@[] andDeviceUID:output.uid.toNSString() withStream:0];
            description.name = @"Jam2 Loopback";
            description.privateTap = YES;
            description.muteBehavior = CATapUnmuted;
            AudioObjectID tap = kAudioObjectUnknown;
            checkStatus(AudioHardwareCreateProcessTap(description, &tap),
                "Could not create system audio capture tap");
            return tap;
        }
        throw std::runtime_error("System loopback recording requires macOS 14.2 or later");
    }

    AudioStreamBasicDescription tapFormat(AudioObjectID tap) override
    {
        return property<AudioStreamBasicDescription>(tap, kAudioTapPropertyFormat);
    }

    AudioDeviceID createAggregate(AudioObjectID tap) override
    {
        const QString uid = stringProperty(tap, kAudioTapPropertyUID);
        NSDictionary* description = @{
            @kAudioAggregateDeviceNameKey: @"Jam2 Loopback Capture",
            @kAudioAggregateDeviceUIDKey: NSUUID.UUID.UUIDString,
            @kAudioAggregateDeviceIsPrivateKey: @YES,
            @kAudioAggregateDeviceTapAutoStartKey: @NO,
            @kAudioAggregateDeviceTapListKey: @[@{
                @kAudioSubTapUIDKey: uid.toNSString(),
                @kAudioSubTapDriftCompensationKey: @NO,
            }],
        };
        AudioDeviceID device = kAudioObjectUnknown;
        checkStatus(AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)description,
            &device), "Could not create system audio capture device");
        return device;
    }

    AudioDeviceIOProcID createIoProc(AudioDeviceID device,
        AudioDeviceIOProc callback, void* context) override
    {
        AudioDeviceIOProcID proc = nullptr;
        checkStatus(AudioDeviceCreateIOProcID(device, callback, context, &proc),
            "Could not create system audio capture callback");
        return proc;
    }

    void start(AudioDeviceID device, AudioDeviceIOProcID proc) override
    {
        checkStatus(AudioDeviceStart(device, proc), "Could not start system audio capture");
    }

    bool outputAlive(AudioDeviceID device) override
    {
        AudioObjectPropertyAddress address{kAudioDevicePropertyDeviceIsAlive,
            kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        UInt32 alive = 0;
        UInt32 size = sizeof(alive);
        return AudioObjectGetPropertyData(device, &address, 0, nullptr,
            &size, &alive) == noErr && alive != 0;
    }

    OSStatus stop(AudioDeviceID device, AudioDeviceIOProcID proc) noexcept override
    { return AudioDeviceStop(device, proc); }
    OSStatus destroyIoProc(AudioDeviceID device, AudioDeviceIOProcID proc) noexcept override
    { return AudioDeviceDestroyIOProcID(device, proc); }
    OSStatus destroyAggregate(AudioDeviceID device) noexcept override
    { return AudioHardwareDestroyAggregateDevice(device); }
    OSStatus destroyTap(AudioObjectID tap) noexcept override
    {
        if (@available(macOS 14.2, *)) return AudioHardwareDestroyProcessTap(tap);
        return kAudioHardwareUnsupportedOperationError;
    }
};

QString sourceId(const MacLoopbackOutput& output)
{
    return QStringLiteral("coreaudio:") + QString::fromLatin1(
        QUrl::toPercentEncoding(output.uid));
}

MacLoopbackOutput resolveOutput(MacLoopbackApi& api, const QString& source)
{
    const auto outputs = api.outputs();
    const bool useDefault = source.isEmpty() || source == QStringLiteral("default");
    const auto defaultDevice = useDefault ? api.defaultOutput() : kAudioObjectUnknown;
    for (const auto& output : outputs) {
        if ((useDefault && output.device == defaultDevice) ||
            (!useDefault && sourceId(output) == source)) return output;
    }
    throw std::runtime_error("The selected loopback output is unavailable. Refresh the output list or select the default output.");
}

bool sameFormat(const AudioStreamBasicDescription& a, const AudioStreamBasicDescription& b)
{
    return a.mSampleRate == b.mSampleRate && a.mFormatID == b.mFormatID &&
        a.mFormatFlags == b.mFormatFlags && a.mBytesPerPacket == b.mBytesPerPacket &&
        a.mFramesPerPacket == b.mFramesPerPacket && a.mBytesPerFrame == b.mBytesPerFrame &&
        a.mChannelsPerFrame == b.mChannelsPerFrame && a.mBitsPerChannel == b.mBitsPerChannel;
}

void validateFormat(const AudioStreamBasicDescription& format)
{
    const bool planar = (format.mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
    if (format.mFormatID != kAudioFormatLinearPCM ||
        (format.mFormatFlags & kAudioFormatFlagIsFloat) == 0 ||
        (format.mFormatFlags & kAudioFormatFlagIsBigEndian) != 0 ||
        format.mBitsPerChannel != 32 || format.mChannelsPerFrame == 0 ||
        format.mChannelsPerFrame > kMaximumChannels ||
        format.mBytesPerFrame != sizeof(float) * (planar ? 1 : format.mChannelsPerFrame) ||
        !std::isfinite(format.mSampleRate) || format.mSampleRate < 8000 ||
        format.mSampleRate > 384000 || std::floor(format.mSampleRate) != format.mSampleRate) {
        throw std::runtime_error("Unsupported Core Audio loopback sample format");
    }
}

struct CaptureBuffers {
    explicit CaptureBuffers(const AudioStreamBasicDescription& format)
        : channels(format.mChannelsPerFrame),
          queue(static_cast<std::size_t>(format.mSampleRate) * kQueueSeconds) {}

    UInt32 channels;
    audio::MonoRingBuffer queue;
    std::atomic<std::uint64_t> callbacks{0};
    std::atomic<bool> invalidBuffers{false};

    static OSStatus receive(AudioObjectID, const AudioTimeStamp*,
        const AudioBufferList* input, const AudioTimeStamp*,
        AudioBufferList*, const AudioTimeStamp*, void* context) noexcept
    {
        auto& self = *static_cast<CaptureBuffers*>(context);
        self.callbacks.fetch_add(1, std::memory_order_relaxed);
        if (input == nullptr || input->mNumberBuffers == 0) return noErr;
        UInt32 channelCount = 0;
        for (UInt32 index = 0; index < input->mNumberBuffers; ++index) {
            const auto& buffer = input->mBuffers[index];
            if (buffer.mNumberChannels == 0 || buffer.mNumberChannels > self.channels ||
                buffer.mDataByteSize % (sizeof(float) * buffer.mNumberChannels) != 0) {
                self.invalidBuffers.store(true, std::memory_order_release);
                return noErr;
            }
            channelCount += buffer.mNumberChannels;
        }
        if (channelCount != self.channels) {
            self.invalidBuffers.store(true, std::memory_order_release);
            return noErr;
        }
        const auto frames = audio::coreaudio_processing::buffer_frames(input);
        std::array<audio::coreaudio_processing::ConstFloatChannelView, kMaximumChannels> views{};
        std::array<double, kMaximumChannels> peaks{};
        for (UInt32 channel = 0; channel < self.channels; ++channel) {
            auto& view = views[channel];
            view = audio::coreaudio_processing::input_channel(input, channel);
            if (view.available()) {
                if (view.frames != frames) {
                    self.invalidBuffers.store(true, std::memory_order_release);
                    return noErr;
                }
                for (std::size_t frame = 0; frame < frames; ++frame) {
                    const float sample = view.samples[frame * view.stride];
                    if (std::isfinite(sample)) peaks[channel] = std::max(peaks[channel],
                        std::abs(static_cast<double>(sample)));
                }
            }
        }
        const auto active = audio::block_downmix_active_channels(
            std::span<const double>(peaks.data(), self.channels));
        const double loudest = *std::max_element(peaks.begin(), peaks.begin() + self.channels);
        std::array<std::int32_t, kTransferFrames> samples{};
        for (std::size_t first = 0; first < frames; first += kTransferFrames) {
            const auto count = std::min(kTransferFrames, frames - first);
            for (std::size_t frame = 0; frame < count; ++frame) {
                double mono = 0;
                for (UInt32 channel = 0; channel < self.channels; ++channel) {
                    const auto& view = views[channel];
                    if (view.available() && audio::block_downmix_channel_active(peaks[channel], loudest)) {
                        const float sample = view.samples[(first + frame) * view.stride];
                        if (std::isfinite(sample)) mono += std::clamp(static_cast<double>(sample), -1.0, 1.0);
                    }
                }
                samples[frame] = normalized_to_pcm16(active > 0 ? mono / active : 0);
            }
            self.queue.push(std::span<const std::int32_t>(samples.data(), count));
        }
        return noErr;
    }
};

// Destruction also covers every partially initialized or cancelled session.
struct CaptureSession {
    MacLoopbackApi& api;
    AudioObjectID tap = kAudioObjectUnknown;
    AudioDeviceID aggregate = kAudioObjectUnknown;
    AudioDeviceIOProcID proc = nullptr;
    bool started = false;

    OSStatus close() noexcept
    {
        OSStatus firstError = noErr;
        const auto remember = [&firstError](OSStatus status) {
            if (firstError == noErr) firstError = status;
        };
        if (started) { remember(api.stop(aggregate, proc)); started = false; }
        if (proc) { remember(api.destroyIoProc(aggregate, proc)); proc = nullptr; }
        if (aggregate != kAudioObjectUnknown) {
            remember(api.destroyAggregate(aggregate)); aggregate = kAudioObjectUnknown;
        }
        if (tap != kAudioObjectUnknown) {
            remember(api.destroyTap(tap)); tap = kAudioObjectUnknown;
        }
        return firstError;
    }
    ~CaptureSession() { (void)close(); }
};

} // namespace

QStringList mac_loopback_sources(MacLoopbackApi& api)
{
    const auto outputs = api.outputs();
    const auto defaultDevice = api.defaultOutput();
    QStringList result;
    for (const auto& output : outputs) {
        if (output.device == defaultDevice) {
            result << QStringLiteral("[default] %1 (system output)").arg(output.name);
            break;
        }
    }
    for (const auto& output : outputs) {
        result << QStringLiteral("[%1] %2").arg(sourceId(output), output.name);
    }
    return result;
}

QStringList mac_loopback_sources(QString* error)
{
    @autoreleasepool {
        try {
            if (@available(macOS 14.2, *)) {
                NativeApi api;
                if (error) error->clear();
                return mac_loopback_sources(api);
            }
            throw std::runtime_error("System loopback recording requires macOS 14.2 or later");
        } catch (const std::exception& ex) {
            if (error) *error = QString::fromUtf8(ex.what());
            return {};
        }
    }
}

GuiLoopbackCaptureResult capture_mac_loopback(const GuiLoopbackOptions& options,
    const std::atomic<bool>& stopRequested, MacLoopbackApi& api)
{
    GuiLoopbackCaptureResult result;
    // Buffers must outlive the session, including exception unwinding while
    // callbacks are still active. The session is always destroyed first.
    std::unique_ptr<CaptureBuffers> buffers;
    CaptureSession session{api};
    try {
        const auto cancelled = [&] { return stopRequested.load(std::memory_order_acquire); };
        if (cancelled()) throw std::runtime_error("Loopback recording stopped before capture started");
        const auto output = resolveOutput(api, options.source);
        session.tap = api.createTap(output);
        if (cancelled()) throw std::runtime_error("Loopback recording stopped before capture started");
        const auto format = api.tapFormat(session.tap);
        validateFormat(format);
        const int sampleRate = static_cast<int>(format.mSampleRate);
        buffers = std::make_unique<CaptureBuffers>(format);
        session.aggregate = api.createAggregate(session.tap);
        session.proc = api.createIoProc(session.aggregate, &CaptureBuffers::receive, buffers.get());
        if (cancelled()) throw std::runtime_error("Loopback recording stopped before capture started");
        api.start(session.aggregate, session.proc);
        session.started = true;

        const auto barFrames = recording_frames_for_bars(options.durationBars,
            options.beatsPerBar, options.bpm, sampleRate, options.tempoPulseUnits);
        LoopbackTakeAccumulator capture(options, sampleRate, barFrames > 0
            ? barFrames : std::numeric_limits<std::uint64_t>::max());
        std::array<std::int32_t, kTransferFrames> samples{};
        auto nextHealthCheck = std::chrono::steady_clock::now();
        const auto drain = [&] {
            while (!capture.reachedDuration()) {
                const auto count = std::min(samples.size(), buffers->queue.available_read());
                if (count == 0) break;
                const auto read = buffers->queue.pop(std::span<std::int32_t>(samples.data(), count), false);
                for (std::size_t index = 0; index < read && !capture.reachedDuration(); ++index) {
                    capture.push(static_cast<std::int16_t>(samples[index]));
                }
            }
        };
        while (!capture.reachedDuration() && !cancelled()) {
            drain();
            if (buffers->invalidBuffers.load(std::memory_order_acquire)) {
                throw std::runtime_error("The loopback audio buffer format changed during recording. Start a new take.");
            }
            if (buffers->queue.stats().overruns != 0) {
                throw std::runtime_error("Loopback capture queue overflowed; the incomplete take was not saved");
            }
            if (std::chrono::steady_clock::now() >= nextHealthCheck) {
                if (!api.outputAlive(output.device)) {
                    throw std::runtime_error("The selected loopback output was disconnected. Start a new take.");
                }
                if (!sameFormat(format, api.tapFormat(session.tap))) {
                    throw std::runtime_error("The loopback sample format changed during recording. Start a new take.");
                }
                if ((options.source.isEmpty() || options.source == QStringLiteral("default")) &&
                    api.defaultOutput() != output.device) {
                    throw std::runtime_error("The system output changed during loopback recording. Start a new take to capture the new output.");
                }
                nextHealthCheck = std::chrono::steady_clock::now() + 100ms;
            }
            if (!capture.reachedDuration()) std::this_thread::sleep_for(2ms);
        }
        checkStatus(session.close(), "Could not close system audio capture");
        drain();
        const auto stats = buffers->queue.stats();
        if (buffers->invalidBuffers.load(std::memory_order_acquire) || stats.overruns != 0) {
            throw std::runtime_error("Loopback capture lost audio frames; the incomplete take was not saved");
        }
        auto retained = capture.finish();
        auto samplesOut = resample_pcm16_mono(retained, sampleRate, options.targetSampleRate);
        result.diagnostics = QStringLiteral(
            "loopback backend=coreaudio output=\"%1\" output_uid=\"%2\" stream=0 "
            "channels=%3 source_sample_rate=%4 target_sample_rate=%5 resample_ratio=%6 "
            "capture_frames=%7 source_frames=%8 output_frames=%9 callbacks=%10 "
            "queue_capacity_frames=%11 queue_overrun_frames=%12 poll_ms=2 health_check_ms=100 "
            "tap_drift_compensation=no fold_down=active-average-30dB recorded_peak_dbfs=%13")
            .arg(output.name, output.uid).arg(format.mChannelsPerFrame).arg(sampleRate)
            .arg(options.targetSampleRate)
            .arg(static_cast<double>(options.targetSampleRate) / sampleRate, 0, 'f', 8)
            .arg(capture.rawFrames()).arg(retained.size()).arg(samplesOut.size())
            .arg(buffers->callbacks.load()).arg(buffers->queue.capacity()).arg(stats.overruns)
            .arg(capture.peakDbfs(), 0, 'f', 2);
        result.error = loopback_capture_content_error(
            classify_loopback_capture_content(capture.rawFrames(), retained.size()));
        if (result.error.isEmpty()) {
            write_loopback_wav_pcm16(options.outputPath, options.targetSampleRate, samplesOut);
            result.ok = true;
        }
    } catch (const std::exception& ex) {
        result.error = QString::fromUtf8(ex.what());
        const auto cleanup = session.close();
        if (buffers) result.diagnostics = QStringLiteral(
            "loopback backend=coreaudio callbacks=%1 queue_capacity_frames=%2 queue_overrun_frames=%3 cleanup_status=%4")
            .arg(buffers->callbacks.load()).arg(buffers->queue.capacity())
            .arg(buffers->queue.stats().overruns).arg(cleanup);
    }
    return result;
}

GuiLoopbackCaptureResult capture_mac_loopback(const GuiLoopbackOptions& options,
    const std::atomic<bool>& stopRequested)
{
    @autoreleasepool {
        if (@available(macOS 14.2, *)) {
            NativeApi api;
            return capture_mac_loopback(options, stopRequested, api);
        }
        return {false, QStringLiteral("System loopback recording requires macOS 14.2 or later"), {}};
    }
}

} // namespace jam2::gui

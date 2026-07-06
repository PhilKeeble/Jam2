#include "loopback_capture_macos.hpp"

#import <CoreAudio/CoreAudio.h>
#import <dispatch/dispatch.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <dlfcn.h>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#if defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && __MAC_OS_X_VERSION_MAX_ALLOWED >= 140200

namespace {

void check_osstatus(OSStatus status, const char* message)
{
    if (status != noErr) {
        std::ostringstream out;
        out << message << " status=" << status;
        throw std::runtime_error(out.str());
    }
}

void write_u16(std::ofstream& out, std::uint16_t value)
{
    const std::array<char, 2> bytes{
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8) & 0xffU),
    };
    out.write(bytes.data(), bytes.size());
}

void write_u32(std::ofstream& out, std::uint32_t value)
{
    const std::array<char, 4> bytes{
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8) & 0xffU),
        static_cast<char>((value >> 16) & 0xffU),
        static_cast<char>((value >> 24) & 0xffU),
    };
    out.write(bytes.data(), bytes.size());
}

void write_wav_header(std::ofstream& out, int sample_rate, std::uint32_t frames)
{
    constexpr std::uint16_t channels = 1;
    constexpr std::uint16_t bits = 16;
    const std::uint32_t data_bytes = frames * channels * (bits / 8);
    out.write("RIFF", 4);
    write_u32(out, 36U + data_bytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    write_u32(out, 16);
    write_u16(out, 1);
    write_u16(out, channels);
    write_u32(out, static_cast<std::uint32_t>(sample_rate));
    write_u32(out, static_cast<std::uint32_t>(sample_rate * channels * (bits / 8)));
    write_u16(out, channels * (bits / 8));
    write_u16(out, bits);
    out.write("data", 4);
    write_u32(out, data_bytes);
}

void write_wav_file(const std::filesystem::path& output, int sample_rate, const std::vector<std::int16_t>& samples)
{
    const std::filesystem::path parent = output.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
    std::ofstream out(output, std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("failed to open output WAV: " + output.string());
    }
    write_wav_header(out, sample_rate, static_cast<std::uint32_t>(samples.size()));
    for (const std::int16_t sample : samples) {
        write_u16(out, static_cast<std::uint16_t>(sample));
    }
}

std::int16_t double_to_i16(double value)
{
    const double clamped = std::clamp(value, -1.0, 1.0);
    return static_cast<std::int16_t>(std::lrint(clamped * 32767.0));
}

double amp_from_db(double db)
{
    return std::pow(10.0, db / 20.0);
}

double amp_i16(std::int16_t sample)
{
    return std::abs(static_cast<double>(sample)) / 32768.0;
}

struct CaptureResult {
    std::vector<std::int16_t> samples;
    std::uint64_t raw_frames = 0;
    std::uint64_t pre_roll_used_frames = 0;
    std::uint64_t trimmed_leading_frames = 0;
    std::uint64_t trimmed_trailing_frames = 0;
    bool trigger_enabled = false;
    bool trigger_fired = false;
    bool stopped_by_request = false;
    double peak = 0.0;
};

class CaptureAccumulator {
public:
    CaptureAccumulator(const MacLoopbackOptions& options, int sample_rate)
        : options_(options),
          trigger_threshold_(amp_from_db(options.trigger_threshold_db)),
          tail_threshold_(amp_from_db(options.tail_silence_db)),
          trigger_hold_frames_(static_cast<std::uint64_t>(std::max(0, options.trigger_hold_ms)) * sample_rate / 1000),
          pre_roll_frames_(static_cast<std::size_t>(std::max(0, options.pre_roll_ms) * sample_rate / 1000)),
          tail_silence_frames_(static_cast<std::uint64_t>(std::max(0, options.tail_silence_ms)) * sample_rate / 1000)
    {
        result_.trigger_enabled = options.trigger;
        pre_roll_.reserve(pre_roll_frames_);
    }

    void push(std::int16_t sample)
    {
        ++result_.raw_frames;
        const double amp = amp_i16(sample);
        result_.peak = std::max(result_.peak, amp);
        if (options_.trigger && !result_.trigger_fired) {
            push_pre_roll(sample);
            if (amp >= trigger_threshold_) {
                ++trigger_hold_count_;
            } else {
                trigger_hold_count_ = 0;
            }
            if (trigger_hold_count_ >= std::max<std::uint64_t>(trigger_hold_frames_, 1)) {
                result_.trigger_fired = true;
                result_.pre_roll_used_frames = pre_roll_.size();
                append_pre_roll();
                pre_roll_.clear();
            }
            return;
        }
        result_.trigger_fired = true;
        result_.samples.push_back(sample);
    }

    CaptureResult finish(bool stopped_by_request)
    {
        result_.stopped_by_request = stopped_by_request;
        trim();
        return result_;
    }

private:
    void push_pre_roll(std::int16_t sample)
    {
        if (pre_roll_frames_ == 0) {
            return;
        }
        if (pre_roll_.size() < pre_roll_frames_) {
            pre_roll_.push_back(sample);
            return;
        }
        pre_roll_[pre_roll_index_] = sample;
        pre_roll_index_ = (pre_roll_index_ + 1) % pre_roll_.size();
    }

    void append_pre_roll()
    {
        if (pre_roll_.empty()) {
            return;
        }
        if (pre_roll_.size() < pre_roll_frames_) {
            result_.samples.insert(result_.samples.end(), pre_roll_.begin(), pre_roll_.end());
            return;
        }
        result_.samples.insert(result_.samples.end(), pre_roll_.begin() + static_cast<std::ptrdiff_t>(pre_roll_index_), pre_roll_.end());
        result_.samples.insert(result_.samples.end(), pre_roll_.begin(), pre_roll_.begin() + static_cast<std::ptrdiff_t>(pre_roll_index_));
    }

    void trim()
    {
        if (result_.samples.empty()) {
            return;
        }
        std::size_t first = 0;
        std::size_t last = result_.samples.size();
        if (options_.trim_leading_silence) {
            while (first < last && amp_i16(result_.samples[first]) < tail_threshold_) {
                ++first;
            }
        }
        if (options_.trim_trailing_silence) {
            std::uint64_t silent = 0;
            while (last > first) {
                if (amp_i16(result_.samples[last - 1]) < tail_threshold_) {
                    ++silent;
                    --last;
                    if (tail_silence_frames_ > 0 && silent >= tail_silence_frames_) {
                        while (last > first && amp_i16(result_.samples[last - 1]) < tail_threshold_) {
                            --last;
                        }
                        break;
                    }
                } else {
                    break;
                }
            }
        }
        result_.trimmed_leading_frames = first;
        result_.trimmed_trailing_frames = result_.samples.size() - last;
        if (first > 0 || last < result_.samples.size()) {
            result_.samples = std::vector<std::int16_t>(result_.samples.begin() + static_cast<std::ptrdiff_t>(first), result_.samples.begin() + static_cast<std::ptrdiff_t>(last));
        }
    }

    const MacLoopbackOptions& options_;
    double trigger_threshold_ = 0.0;
    double tail_threshold_ = 0.0;
    std::uint64_t trigger_hold_frames_ = 0;
    std::size_t pre_roll_frames_ = 0;
    std::uint64_t tail_silence_frames_ = 0;
    std::uint64_t trigger_hold_count_ = 0;
    std::vector<std::int16_t> pre_roll_;
    std::size_t pre_roll_index_ = 0;
    CaptureResult result_;
};

std::string json_escape(const std::string& text)
{
    std::string out;
    for (const char c : text) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

void write_summary(const MacLoopbackOptions& options, int sample_rate, const CaptureResult& result)
{
    const std::filesystem::path absolute = std::filesystem::absolute(options.output);
    const std::filesystem::path sidecar = absolute.string() + ".json";
    std::ostringstream json;
    json << "{"
         << "\"event\":\"capture.summary\","
         << "\"mode\":\"record-loopback\","
         << "\"output\":\"" << json_escape(absolute.string()) << "\","
         << "\"sidecar\":\"" << json_escape(sidecar.string()) << "\","
         << "\"sample_rate\":" << sample_rate << ","
         << "\"channels\":1,"
         << "\"frames_written\":" << result.samples.size() << ","
         << "\"raw_frames\":" << result.raw_frames << ","
         << "\"duration_ms\":" << (result.samples.size() * 1000ULL / static_cast<unsigned long long>(std::max(sample_rate, 1))) << ","
         << "\"peak\":" << result.peak << ","
         << "\"trigger_enabled\":" << (result.trigger_enabled ? "true" : "false") << ","
         << "\"trigger_fired\":" << (result.trigger_fired ? "true" : "false") << ","
         << "\"stopped_by_request\":" << (result.stopped_by_request ? "true" : "false") << ","
         << "\"pre_roll_used_frames\":" << result.pre_roll_used_frames << ","
         << "\"trimmed_leading_frames\":" << result.trimmed_leading_frames << ","
         << "\"trimmed_trailing_frames\":" << result.trimmed_trailing_frames
         << "}";
    std::ofstream meta(sidecar, std::ios::binary | std::ios::trunc);
    meta << json.str() << "\n";
    if (options.summary_json) {
        std::cout << json.str() << "\n";
    }
}

std::string ns_to_utf8(NSString* value)
{
    return value ? std::string([value UTF8String]) : std::string();
}

using AudioHardwareCreateProcessTapFn = OSStatus (*)(id, AudioObjectID*);
using AudioHardwareDestroyProcessTapFn = OSStatus (*)(AudioObjectID);

AudioHardwareCreateProcessTapFn create_process_tap()
{
    return reinterpret_cast<AudioHardwareCreateProcessTapFn>(dlsym(RTLD_DEFAULT, "AudioHardwareCreateProcessTap"));
}

AudioHardwareDestroyProcessTapFn destroy_process_tap()
{
    return reinterpret_cast<AudioHardwareDestroyProcessTapFn>(dlsym(RTLD_DEFAULT, "AudioHardwareDestroyProcessTap"));
}

void require_process_tap_api()
{
    if (create_process_tap() == nullptr || destroy_process_tap() == nullptr || NSClassFromString(@"CATapDescription") == nil) {
        throw std::runtime_error("macOS direct loopback requires macOS 14.2+ CoreAudio process taps. On older macOS, use BlackHole/Loopback as an input device with record-input.");
    }
}

id make_process_tap_description(NSUUID* tapUuid)
{
    Class tapClass = NSClassFromString(@"CATapDescription");
    if (tapClass == nil) {
        throw std::runtime_error("CATapDescription is not available on this macOS runtime");
    }

    SEL initSelector = NSSelectorFromString(@"initStereoGlobalTapButExcludeProcesses:");
    id tapDescription = [tapClass alloc];
    if (![tapDescription respondsToSelector:initSelector]) {
        throw std::runtime_error("CATapDescription does not support stereo global process taps");
    }

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Warc-performSelector-leaks"
    tapDescription = [tapDescription performSelector:initSelector withObject:@[]];
#pragma clang diagnostic pop

    if (tapDescription == nil) {
        throw std::runtime_error("failed to allocate CATapDescription");
    }

    [tapDescription setValue:tapUuid forKey:@"UUID"];
    [tapDescription setValue:@YES forKey:@"private"];
    [tapDescription setValue:@0 forKey:@"muteBehavior"];
    return tapDescription;
}

struct CaptureBuffer {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<std::int16_t> samples;
    std::uint64_t target_frames = 0;
    UInt32 channels = 0;
    bool done = false;
    double peak = 0.0;
};

double device_nominal_sample_rate(AudioObjectID device)
{
    Float64 sampleRate = 0.0;
    UInt32 size = sizeof(sampleRate);
    AudioObjectPropertyAddress address{
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain,
    };
    check_osstatus(AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &sampleRate), "failed to read process tap sample rate");
    if (sampleRate <= 0.0) {
        throw std::runtime_error("CoreAudio process tap reported an invalid sample rate");
    }
    return sampleRate;
}

UInt32 input_channel_count(const AudioBufferList* input)
{
    if (input == nullptr || input->mNumberBuffers == 0) {
        return 1;
    }
    UInt32 channels = 0;
    for (UInt32 bufferIndex = 0; bufferIndex < input->mNumberBuffers; ++bufferIndex) {
        channels += std::max<UInt32>(input->mBuffers[bufferIndex].mNumberChannels, 1);
    }
    return std::max<UInt32>(channels, 1);
}

UInt32 input_frame_count(const AudioBufferList* input)
{
    if (input == nullptr || input->mNumberBuffers == 0 || input->mBuffers[0].mData == nullptr) {
        return 0;
    }
    const AudioBuffer& buffer = input->mBuffers[0];
    const UInt32 channels = std::max<UInt32>(buffer.mNumberChannels, 1);
    return buffer.mDataByteSize / (sizeof(float) * channels);
}

double read_interleaved_float_sample(const AudioBuffer& buffer, UInt32 frame, UInt32 channel)
{
    if (buffer.mData == nullptr || buffer.mNumberChannels == 0) {
        return 0.0;
    }
    const UInt32 clampedChannel = std::min<UInt32>(channel, buffer.mNumberChannels - 1);
    const float* samples = static_cast<const float*>(buffer.mData);
    return samples[static_cast<std::size_t>(frame) * buffer.mNumberChannels + clampedChannel];
}

double read_float_sample(const AudioBufferList* input, UInt32 frame, UInt32 channel)
{
    if (input == nullptr || input->mNumberBuffers == 0) {
        return 0.0;
    }
    if (input->mNumberBuffers == 1) {
        return read_interleaved_float_sample(input->mBuffers[0], frame, channel);
    }
    UInt32 remaining = channel;
    for (UInt32 bufferIndex = 0; bufferIndex < input->mNumberBuffers; ++bufferIndex) {
        const AudioBuffer& buffer = input->mBuffers[bufferIndex];
        const UInt32 bufferChannels = std::max<UInt32>(buffer.mNumberChannels, 1);
        if (remaining < bufferChannels) {
            return read_interleaved_float_sample(buffer, frame, remaining);
        }
        remaining -= bufferChannels;
    }
    const AudioBuffer& buffer = input->mBuffers[input->mNumberBuffers - 1];
    if (buffer.mData == nullptr) {
        return 0.0;
    }
    const float* samples = static_cast<const float*>(buffer.mData);
    return samples[frame];
}

} // namespace

int list_loopback_sources_macos()
{
    @autoreleasepool {
        require_process_tap_api();
        std::cout << "[default] CoreAudio process-tap system mix\n";
        return 0;
    }
}

int record_loopback_macos(const MacLoopbackOptions& options)
{
    @autoreleasepool {
        if (!options.source.empty() && options.source != "default") {
            throw std::runtime_error("macOS process-tap loopback currently supports only --source default");
        }
        require_process_tap_api();
        const auto createTap = create_process_tap();
        const auto destroyTap = destroy_process_tap();

        const std::filesystem::path parent = options.output.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent);
        }

        NSUUID* tapUuid = [NSUUID UUID];
        id tapDescription = make_process_tap_description(tapUuid);

        AudioObjectID tapId = kAudioObjectUnknown;
        check_osstatus(createTap(tapDescription, &tapId), "failed to create CoreAudio process tap");

        NSString* aggregateUid = [[NSUUID UUID] UUIDString];
        NSDictionary* aggregateDescription = @{
            @kAudioAggregateDeviceNameKey: @"Jam2 Capture Loopback",
            @kAudioAggregateDeviceUIDKey: aggregateUid,
            @kAudioAggregateDeviceIsPrivateKey: @YES,
            @kAudioAggregateDeviceTapListKey: @[
                @{
                    @kAudioSubTapUIDKey: [tapUuid UUIDString],
                    @kAudioSubTapDriftCompensationKey: @YES,
                }
            ],
        };

        AudioObjectID aggregateId = kAudioObjectUnknown;
        try {
            check_osstatus(AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)aggregateDescription, &aggregateId),
                "failed to create CoreAudio aggregate tap device");

            const int sampleRate = static_cast<int>(std::llround(device_nominal_sample_rate(aggregateId)));
            const std::uint64_t targetFrames = options.duration_ms > 0
                ? static_cast<std::uint64_t>((static_cast<std::int64_t>(sampleRate) * options.duration_ms) / 1000)
                : std::numeric_limits<std::uint64_t>::max();

            CaptureBuffer capture;
            capture.target_frames = targetFrames;
            if (options.duration_ms > 0) {
                capture.samples.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(targetFrames, std::numeric_limits<std::size_t>::max())));
            }

            AudioDeviceIOProcID ioProcId = nullptr;
            CaptureBuffer* capturePtr = &capture;
            check_osstatus(AudioDeviceCreateIOProcIDWithBlock(
                               &ioProcId,
                               aggregateId,
                               dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
                               ^(const AudioTimeStamp*,
                                   const AudioBufferList* inputData,
                                   const AudioTimeStamp*,
                                   AudioBufferList*,
                                   const AudioTimeStamp*) {
                                   CaptureBuffer& capture = *capturePtr;
                                   std::lock_guard<std::mutex> lock(capture.mutex);
                                   if (capture.done) {
                                       return;
                                   }
                                   const UInt32 frames = input_frame_count(inputData);
                                   const UInt32 channels = input_channel_count(inputData);
                                   capture.channels = std::max(capture.channels, channels);
                                   for (UInt32 frame = 0; frame < frames && capture.samples.size() < capture.target_frames; ++frame) {
                                       double mono = 0.0;
                                       for (UInt32 channel = 0; channel < channels; ++channel) {
                                           mono += read_float_sample(inputData, frame, channel);
                                       }
                                       mono /= static_cast<double>(channels);
                                       capture.peak = std::max(capture.peak, std::abs(mono));
                                       capture.samples.push_back(double_to_i16(mono));
                                   }
                                   if (capture.samples.size() >= capture.target_frames) {
                                       capture.done = true;
                                       capture.cv.notify_one();
                                   }
                               }),
                "failed to create CoreAudio process tap IO proc");

            check_osstatus(AudioDeviceStart(aggregateId, ioProcId), "failed to start CoreAudio process tap capture");
            {
                std::unique_lock<std::mutex> lock(capture.mutex);
                if (options.duration_ms > 0) {
                    capture.cv.wait_for(lock, std::chrono::milliseconds(options.duration_ms + 3000), [&capture] { return capture.done; });
                } else {
                    while (!capture.done &&
                           !(options.stop_requested != nullptr && options.stop_requested->load(std::memory_order_relaxed))) {
                        capture.cv.wait_for(lock, std::chrono::milliseconds(50));
                    }
                }
                capture.done = true;
            }
            check_osstatus(AudioDeviceStop(aggregateId, ioProcId), "failed to stop CoreAudio process tap capture");
            check_osstatus(AudioDeviceDestroyIOProcID(aggregateId, ioProcId), "failed to destroy CoreAudio process tap IO proc");

            CaptureAccumulator accumulator(options, sampleRate);
            for (const std::int16_t sample : capture.samples) {
                accumulator.push(sample);
            }
            CaptureResult result = accumulator.finish(options.stop_requested != nullptr && options.stop_requested->load(std::memory_order_relaxed));
            write_wav_file(options.output, sampleRate, result.samples);
            write_summary(options, sampleRate, result);

            std::cout << "Mode: record-loopback\n";
            std::cout << "Source: default CoreAudio process-tap system mix\n";
            std::cout << "Mix sample rate: " << sampleRate << "\n";
            std::cout << "Mix channels: " << capture.channels << "\n";
            std::cout << "Duration ms: " << (options.duration_ms > 0 ? std::to_string(options.duration_ms) : std::string("manual")) << "\n";
            std::cout << "Frames written: " << result.samples.size() << "\n";
            std::cout << "Raw frames: " << result.raw_frames << "\n";
            std::cout << "Captured frames: " << capture.samples.size() << "\n";
            std::cout << "Peak: " << result.peak << "\n";
            std::cout << "Trimmed leading frames: " << result.trimmed_leading_frames << "\n";
            std::cout << "Trimmed trailing frames: " << result.trimmed_trailing_frames << "\n";
            std::cout << "Output: " << std::filesystem::absolute(options.output).string() << "\n";
        } catch (...) {
            if (aggregateId != kAudioObjectUnknown) {
                AudioHardwareDestroyAggregateDevice(aggregateId);
            }
            if (tapId != kAudioObjectUnknown) {
                destroyTap(tapId);
            }
            throw;
        }

        if (aggregateId != kAudioObjectUnknown) {
            check_osstatus(AudioHardwareDestroyAggregateDevice(aggregateId), "failed to destroy CoreAudio aggregate tap device");
        }
        if (tapId != kAudioObjectUnknown) {
            check_osstatus(destroyTap(tapId), "failed to destroy CoreAudio process tap");
        }
        return 0;
    }
}

#else

int list_loopback_sources_macos()
{
    std::cout << "macOS direct loopback requires the macOS 14.2 SDK and runtime. Use BlackHole/Loopback as an input device with record-input on this build.\n";
    return 0;
}

int record_loopback_macos(const MacLoopbackOptions&)
{
    std::cerr << "macOS direct loopback requires the macOS 14.2 SDK and runtime. Use BlackHole/Loopback as an input device with record-input on this build.\n";
    return 2;
}

#endif

#include "audio_device.hpp"

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace jam2::audio {
namespace {

constexpr AudioObjectPropertyElement kMainElement = kAudioObjectPropertyElementMain;

std::string osstatus_text(OSStatus status)
{
    std::ostringstream out;
    out << status;
    return out.str();
}

void require_ok(OSStatus status, const char* operation)
{
    if (status != noErr) {
        throw std::runtime_error(std::string(operation) + " failed with OSStatus " + osstatus_text(status));
    }
}

AudioObjectPropertyAddress address(
    AudioObjectPropertySelector selector,
    AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal,
    AudioObjectPropertyElement element = kMainElement)
{
    return AudioObjectPropertyAddress{selector, scope, element};
}

bool has_property(AudioObjectID object, const AudioObjectPropertyAddress& property)
{
    return AudioObjectHasProperty(object, &property);
}

UInt32 property_size(AudioObjectID object, AudioObjectPropertyAddress property)
{
    UInt32 size = 0;
    require_ok(AudioObjectGetPropertyDataSize(object, &property, 0, nullptr, &size), "AudioObjectGetPropertyDataSize");
    return size;
}

template <typename T>
T get_property(AudioObjectID object, AudioObjectPropertyAddress property, const char* operation)
{
    T value{};
    UInt32 size = sizeof(value);
    require_ok(AudioObjectGetPropertyData(object, &property, 0, nullptr, &size, &value), operation);
    return value;
}

template <typename T>
bool try_get_property(AudioObjectID object, AudioObjectPropertyAddress property, T& value)
{
    if (!has_property(object, property)) {
        return false;
    }
    UInt32 size = sizeof(value);
    return AudioObjectGetPropertyData(object, &property, 0, nullptr, &size, &value) == noErr;
}

std::vector<AudioObjectID> get_device_ids()
{
    auto property = address(kAudioHardwarePropertyDevices);
    const UInt32 size = property_size(kAudioObjectSystemObject, property);
    std::vector<AudioObjectID> devices(size / sizeof(AudioObjectID));
    if (!devices.empty()) {
        UInt32 mutable_size = size;
        require_ok(
            AudioObjectGetPropertyData(kAudioObjectSystemObject, &property, 0, nullptr, &mutable_size, devices.data()),
            "AudioObjectGetPropertyData devices");
    }
    return devices;
}

std::string cf_string_to_string(CFStringRef value)
{
    if (value == nullptr) {
        return {};
    }
    char stack[512]{};
    if (CFStringGetCString(value, stack, sizeof(stack), kCFStringEncodingUTF8)) {
        return stack;
    }
    const CFIndex length = CFStringGetLength(value);
    const CFIndex max_size = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string out(static_cast<std::size_t>(max_size), '\0');
    if (!CFStringGetCString(value, out.data(), max_size, kCFStringEncodingUTF8)) {
        return {};
    }
    out.resize(std::strlen(out.c_str()));
    return out;
}

std::string get_cf_string_property(AudioObjectID object, AudioObjectPropertySelector selector)
{
    CFStringRef value = nullptr;
    auto property = address(selector);
    if (!has_property(object, property)) {
        return {};
    }
    UInt32 size = sizeof(value);
    if (AudioObjectGetPropertyData(object, &property, 0, nullptr, &size, &value) != noErr || value == nullptr) {
        return {};
    }
    std::string out = cf_string_to_string(value);
    CFRelease(value);
    return out;
}

std::uint32_t get_u32_property_or_zero(
    AudioObjectID object,
    AudioObjectPropertySelector selector,
    AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal)
{
    UInt32 value = 0;
    (void)try_get_property(object, address(selector, scope), value);
    return value;
}

double get_double_property_or_zero(AudioObjectID object, AudioObjectPropertySelector selector)
{
    Float64 value = 0.0;
    (void)try_get_property(object, address(selector), value);
    return value;
}

std::uint32_t stream_channels(const AudioBufferList& buffers)
{
    std::uint32_t channels = 0;
    for (UInt32 i = 0; i < buffers.mNumberBuffers; ++i) {
        channels += buffers.mBuffers[i].mNumberChannels;
    }
    return channels;
}

std::uint32_t device_channels(AudioObjectID device, AudioObjectPropertyScope scope)
{
    auto property = address(kAudioDevicePropertyStreamConfiguration, scope);
    if (!has_property(device, property)) {
        return 0;
    }
    const UInt32 size = property_size(device, property);
    std::vector<std::uint8_t> storage(size);
    UInt32 mutable_size = size;
    require_ok(
        AudioObjectGetPropertyData(device, &property, 0, nullptr, &mutable_size, storage.data()),
        "AudioObjectGetPropertyData stream configuration");
    return stream_channels(*reinterpret_cast<const AudioBufferList*>(storage.data()));
}

bool is_default_device(AudioObjectID device, AudioObjectPropertySelector selector)
{
    AudioObjectID default_device = kAudioObjectUnknown;
    if (!try_get_property(kAudioObjectSystemObject, address(selector), default_device)) {
        return false;
    }
    return default_device == device;
}

std::string transport_name(UInt32 transport)
{
    switch (transport) {
    case kAudioDeviceTransportTypeBuiltIn:
        return "built-in";
    case kAudioDeviceTransportTypeUSB:
        return "usb";
    case kAudioDeviceTransportTypeBluetooth:
        return "bluetooth";
    case kAudioDeviceTransportTypeBluetoothLE:
        return "bluetooth-le";
    case kAudioDeviceTransportTypeHDMI:
        return "hdmi";
    case kAudioDeviceTransportTypeDisplayPort:
        return "displayport";
    case kAudioDeviceTransportTypeVirtual:
        return "virtual";
    case kAudioDeviceTransportTypeAggregate:
        return "aggregate";
    default:
        return "transport-" + std::to_string(transport);
    }
}

std::string format_flags(UInt32 flags)
{
    std::vector<std::string> names;
    if ((flags & kAudioFormatFlagIsFloat) != 0) {
        names.push_back("float");
    }
    if ((flags & kAudioFormatFlagIsSignedInteger) != 0) {
        names.push_back("signed-int");
    }
    if ((flags & kAudioFormatFlagIsPacked) != 0) {
        names.push_back("packed");
    }
    if ((flags & kAudioFormatFlagIsNonInterleaved) != 0) {
        names.push_back("non-interleaved");
    }
    if (names.empty()) {
        return std::to_string(flags);
    }
    std::string out = names.front();
    for (std::size_t i = 1; i < names.size(); ++i) {
        out += "+";
        out += names[i];
    }
    return out;
}

std::string format_description(AudioObjectID device, AudioObjectPropertyScope scope)
{
    AudioStreamBasicDescription desc{};
    if (!try_get_property(device, address(kAudioDevicePropertyStreamFormat, scope), desc)) {
        return "format=unknown";
    }
    std::ostringstream out;
    out << "format_id=" << desc.mFormatID
        << " rate=" << desc.mSampleRate
        << " channels_per_frame=" << desc.mChannelsPerFrame
        << " bits_per_channel=" << desc.mBitsPerChannel
        << " bytes_per_frame=" << desc.mBytesPerFrame
        << " frames_per_packet=" << desc.mFramesPerPacket
        << " flags=" << format_flags(desc.mFormatFlags);
    return out.str();
}

bool sample_rate_supported(AudioObjectID device, double sample_rate)
{
    auto property = address(kAudioDevicePropertyAvailableNominalSampleRates);
    if (!has_property(device, property)) {
        return false;
    }
    const UInt32 size = property_size(device, property);
    std::vector<AudioValueRange> ranges(size / sizeof(AudioValueRange));
    if (ranges.empty()) {
        return false;
    }
    UInt32 mutable_size = size;
    require_ok(
        AudioObjectGetPropertyData(device, &property, 0, nullptr, &mutable_size, ranges.data()),
        "AudioObjectGetPropertyData available sample rates");
    return std::any_of(ranges.begin(), ranges.end(), [sample_rate](const AudioValueRange& range) {
        return sample_rate >= range.mMinimum && sample_rate <= range.mMaximum;
    });
}

AudioValueRange buffer_frame_range(AudioObjectID device)
{
    AudioValueRange range{};
    (void)try_get_property(device, address(kAudioDevicePropertyBufferFrameSizeRange), range);
    return range;
}

DeviceInfo make_device_info(int index, AudioObjectID device)
{
    DeviceInfo info;
    info.id = index;
    info.backend = "CoreAudio";
    info.name = get_cf_string_property(device, kAudioObjectPropertyName);
    if (info.name.empty()) {
        info.name = "device-" + std::to_string(device);
    }
    info.clsid = get_cf_string_property(device, kAudioDevicePropertyDeviceUID);

    const UInt32 transport = get_u32_property_or_zero(device, kAudioDevicePropertyTransportType);
    const UInt32 input_channels = device_channels(device, kAudioDevicePropertyScopeInput);
    const UInt32 output_channels = device_channels(device, kAudioDevicePropertyScopeOutput);
    const UInt32 buffer_frames = get_u32_property_or_zero(device, kAudioDevicePropertyBufferFrameSize);
    const double sample_rate = get_double_property_or_zero(device, kAudioDevicePropertyNominalSampleRate);
    std::ostringstream details;
    details << "coreaudio_id=" << device
            << " uid=" << (info.clsid.empty() ? "<none>" : info.clsid)
            << " transport=" << transport_name(transport)
            << " default_input=" << (is_default_device(device, kAudioHardwarePropertyDefaultInputDevice) ? "yes" : "no")
            << " default_output=" << (is_default_device(device, kAudioHardwarePropertyDefaultOutputDevice) ? "yes" : "no")
            << " input_channels=" << input_channels
            << " output_channels=" << output_channels
            << " sample_rate=" << sample_rate
            << " buffer_frames=" << buffer_frames
            << " input_" << format_description(device, kAudioDevicePropertyScopeInput)
            << " output_" << format_description(device, kAudioDevicePropertyScopeOutput);
    info.driver_path = details.str();
    return info;
}

struct SelectedDevice {
    DeviceInfo info;
    AudioObjectID object = kAudioObjectUnknown;
};

SelectedDevice select_device(int id)
{
    const auto devices = get_device_ids();
    if (id < 0 || static_cast<std::size_t>(id) >= devices.size()) {
        throw std::runtime_error("audio device id is out of range");
    }
    return SelectedDevice{make_device_info(id, devices[static_cast<std::size_t>(id)]), devices[static_cast<std::size_t>(id)]};
}

struct ProbeContext {
    std::atomic<long> callbacks{0};
    std::atomic<int> input_peak_ppm{0};
};

struct RingContext {
    MonoRingBuffer* ring = nullptr;
    std::vector<std::int32_t> capture_scratch;
    std::vector<std::int32_t> playback_scratch;
    InputChannels input_channels = InputChannels::Mono;
    std::atomic<long> callbacks{0};
};

std::size_t buffer_frames(const AudioBufferList* buffers);
float read_float_channel(const AudioBufferList* buffers, std::size_t frame, UInt32 channel);
void write_float_channel(AudioBufferList* buffers, std::size_t frame, UInt32 channel, float value);
std::int32_t float_to_i32(float sample);
float i32_to_float(std::int32_t sample);

void clear_output(AudioBufferList* output)
{
    if (output == nullptr) {
        return;
    }
    for (UInt32 i = 0; i < output->mNumberBuffers; ++i) {
        std::memset(output->mBuffers[i].mData, 0, output->mBuffers[i].mDataByteSize);
    }
}

void update_peak(std::atomic<int>& peak, int candidate)
{
    int current = peak.load(std::memory_order_relaxed);
    while (candidate > current &&
           !peak.compare_exchange_weak(current, candidate, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

void observe_float_peak(const AudioBufferList* input, ProbeContext& context)
{
    if (input == nullptr) {
        return;
    }
    double peak = 0.0;
    for (UInt32 buffer_index = 0; buffer_index < input->mNumberBuffers; ++buffer_index) {
        const AudioBuffer& buffer = input->mBuffers[buffer_index];
        if (buffer.mData == nullptr || buffer.mDataByteSize == 0) {
            continue;
        }
        const auto* samples = static_cast<const float*>(buffer.mData);
        const std::size_t sample_count = buffer.mDataByteSize / sizeof(float);
        for (std::size_t i = 0; i < sample_count; ++i) {
            peak = std::max(peak, std::abs(static_cast<double>(samples[i])));
        }
    }
    update_peak(context.input_peak_ppm, static_cast<int>(std::clamp(peak, 0.0, 1.0) * 1000000.0));
}

OSStatus exploratory_io_proc(
    AudioObjectID,
    const AudioTimeStamp*,
    const AudioBufferList* input,
    const AudioTimeStamp*,
    AudioBufferList* output,
    const AudioTimeStamp*,
    void* client_data)
{
    auto* context = static_cast<ProbeContext*>(client_data);
    if (context != nullptr) {
        observe_float_peak(input, *context);
        context->callbacks.fetch_add(1, std::memory_order_relaxed);
    }
    clear_output(output);
    return noErr;
}

OSStatus ring_io_proc(
    AudioObjectID,
    const AudioTimeStamp*,
    const AudioBufferList* input,
    const AudioTimeStamp*,
    AudioBufferList* output,
    const AudioTimeStamp*,
    void* client_data)
{
    auto* context = static_cast<RingContext*>(client_data);
    if (context == nullptr || context->ring == nullptr) {
        clear_output(output);
        return noErr;
    }

    const std::size_t input_frames = std::min(buffer_frames(input), context->capture_scratch.size());
    if (input_frames > 0) {
        for (std::size_t frame = 0; frame < input_frames; ++frame) {
            const float left = read_float_channel(input, frame, 0);
            if (context->input_channels == InputChannels::Stereo) {
                const float right = read_float_channel(input, frame, 1);
                context->capture_scratch[frame] = float_to_i32((left + right) * 0.5F);
            } else {
                context->capture_scratch[frame] = float_to_i32(left);
            }
        }
        context->ring->push(std::span<const std::int32_t>(context->capture_scratch.data(), input_frames));
    }

    const std::size_t output_frames = std::min(buffer_frames(output), context->playback_scratch.size());
    if (output_frames > 0) {
        auto playback = std::span<std::int32_t>(context->playback_scratch.data(), output_frames);
        context->ring->pop(playback);
        for (std::size_t frame = 0; frame < output_frames; ++frame) {
            const float sample = i32_to_float(context->playback_scratch[frame]);
            write_float_channel(output, frame, 0, sample);
            write_float_channel(output, frame, 1, sample);
        }
    }

    context->callbacks.fetch_add(1, std::memory_order_relaxed);
    return noErr;
}

DeviceMeterResult run_exploratory_io(
    int id,
    double requested_sample_rate,
    long requested_buffer_size,
    int duration_ms,
    std::size_t ring_capacity_frames)
{
    if (duration_ms <= 0) {
        throw std::runtime_error("duration must be positive");
    }
    const auto selected = select_device(id);
    const UInt32 input_channels = device_channels(selected.object, kAudioDevicePropertyScopeInput);
    const UInt32 output_channels = device_channels(selected.object, kAudioDevicePropertyScopeOutput);
    if (input_channels == 0 && output_channels == 0) {
        throw std::runtime_error("selected CoreAudio device has no input or output channels");
    }

    if (requested_sample_rate > 0.0 && sample_rate_supported(selected.object, requested_sample_rate)) {
        Float64 rate = requested_sample_rate;
        auto property = address(kAudioDevicePropertyNominalSampleRate);
        require_ok(
            AudioObjectSetPropertyData(selected.object, &property, 0, nullptr, sizeof(rate), &rate),
            "AudioObjectSetPropertyData nominal sample rate");
    }

    if (requested_buffer_size > 0) {
        const AudioValueRange range = buffer_frame_range(selected.object);
        if (range.mMinimum > 0.0 &&
            (requested_buffer_size < static_cast<long>(range.mMinimum) ||
             requested_buffer_size > static_cast<long>(range.mMaximum))) {
            throw std::runtime_error("requested CoreAudio buffer size is outside the device min/max range");
        }
        UInt32 frames = static_cast<UInt32>(requested_buffer_size);
        auto property = address(kAudioDevicePropertyBufferFrameSize);
        require_ok(
            AudioObjectSetPropertyData(selected.object, &property, 0, nullptr, sizeof(frames), &frames),
            "AudioObjectSetPropertyData buffer frame size");
    }

    const UInt32 buffer_frames = get_u32_property_or_zero(selected.object, kAudioDevicePropertyBufferFrameSize);
    if (ring_capacity_frames != 0 && buffer_frames != 0 && ring_capacity_frames < buffer_frames) {
        throw std::runtime_error("ring capacity must be at least one CoreAudio buffer");
    }

    ProbeContext context;
    AudioDeviceIOProcID proc_id = nullptr;
    require_ok(
        AudioDeviceCreateIOProcID(selected.object, exploratory_io_proc, &context, &proc_id),
        "AudioDeviceCreateIOProcID");

    struct ProcGuard {
        AudioObjectID device = kAudioObjectUnknown;
        AudioDeviceIOProcID proc = nullptr;
        bool started = false;
        ~ProcGuard()
        {
            if (proc != nullptr) {
                if (started) {
                    (void)AudioDeviceStop(device, proc);
                }
                (void)AudioDeviceDestroyIOProcID(device, proc);
            }
        }
    } guard{selected.object, proc_id, false};

    require_ok(AudioDeviceStart(selected.object, proc_id), "AudioDeviceStart");
    guard.started = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));

    DeviceMeterResult result;
    result.device = selected.info;
    result.sample_rate = get_double_property_or_zero(selected.object, kAudioDevicePropertyNominalSampleRate);
    result.buffer_size = static_cast<long>(get_u32_property_or_zero(selected.object, kAudioDevicePropertyBufferFrameSize));
    result.callbacks = context.callbacks.load(std::memory_order_relaxed);
    result.input_sample_type = static_cast<long>(input_channels);
    result.output_sample_type = static_cast<long>(output_channels);
    result.input_peak = static_cast<double>(context.input_peak_ppm.load(std::memory_order_relaxed)) / 1000000.0;
    return result;
}

void configure_device(AudioObjectID device, double requested_sample_rate, long requested_buffer_size)
{
    if (requested_sample_rate > 0.0) {
        if (!sample_rate_supported(device, requested_sample_rate)) {
            throw std::runtime_error("requested CoreAudio sample rate is not supported");
        }
        Float64 rate = requested_sample_rate;
        auto property = address(kAudioDevicePropertyNominalSampleRate);
        require_ok(
            AudioObjectSetPropertyData(device, &property, 0, nullptr, sizeof(rate), &rate),
            "AudioObjectSetPropertyData nominal sample rate");
    }

    if (requested_buffer_size > 0) {
        const AudioValueRange range = buffer_frame_range(device);
        if (range.mMinimum > 0.0 &&
            (requested_buffer_size < static_cast<long>(range.mMinimum) ||
             requested_buffer_size > static_cast<long>(range.mMaximum))) {
            throw std::runtime_error("requested CoreAudio buffer size is outside the device min/max range");
        }
        UInt32 frames = static_cast<UInt32>(requested_buffer_size);
        auto property = address(kAudioDevicePropertyBufferFrameSize);
        require_ok(
            AudioObjectSetPropertyData(device, &property, 0, nullptr, sizeof(frames), &frames),
            "AudioObjectSetPropertyData buffer frame size");
    }
}

bool is_supported_float32_format(AudioObjectID device, AudioObjectPropertyScope scope)
{
    AudioStreamBasicDescription desc{};
    if (!try_get_property(device, address(kAudioDevicePropertyStreamFormat, scope), desc)) {
        return false;
    }
    return desc.mFormatID == kAudioFormatLinearPCM &&
        (desc.mFormatFlags & kAudioFormatFlagIsFloat) != 0 &&
        desc.mBitsPerChannel == 32 &&
        desc.mBytesPerFrame >= sizeof(float) &&
        desc.mFramesPerPacket == 1;
}

std::size_t buffer_frames(const AudioBufferList* buffers)
{
    if (buffers == nullptr || buffers->mNumberBuffers == 0) {
        return 0;
    }
    std::size_t frames = static_cast<std::size_t>(-1);
    for (UInt32 i = 0; i < buffers->mNumberBuffers; ++i) {
        const AudioBuffer& buffer = buffers->mBuffers[i];
        if (buffer.mData == nullptr || buffer.mNumberChannels == 0) {
            return 0;
        }
        frames = std::min(
            frames,
            static_cast<std::size_t>(buffer.mDataByteSize) /
                (sizeof(float) * static_cast<std::size_t>(buffer.mNumberChannels)));
    }
    return frames == static_cast<std::size_t>(-1) ? 0 : frames;
}

float read_float_channel(const AudioBufferList* buffers, std::size_t frame, UInt32 channel)
{
    if (buffers == nullptr) {
        return 0.0F;
    }
    UInt32 base_channel = 0;
    for (UInt32 buffer_index = 0; buffer_index < buffers->mNumberBuffers; ++buffer_index) {
        const AudioBuffer& buffer = buffers->mBuffers[buffer_index];
        if (buffer.mData == nullptr || buffer.mNumberChannels == 0) {
            continue;
        }
        if (channel >= base_channel && channel < base_channel + buffer.mNumberChannels) {
            const UInt32 local_channel = channel - base_channel;
            const auto* samples = static_cast<const float*>(buffer.mData);
            return samples[(frame * static_cast<std::size_t>(buffer.mNumberChannels)) + local_channel];
        }
        base_channel += buffer.mNumberChannels;
    }
    return 0.0F;
}

void write_float_channel(AudioBufferList* buffers, std::size_t frame, UInt32 channel, float value)
{
    if (buffers == nullptr) {
        return;
    }
    UInt32 base_channel = 0;
    for (UInt32 buffer_index = 0; buffer_index < buffers->mNumberBuffers; ++buffer_index) {
        AudioBuffer& buffer = buffers->mBuffers[buffer_index];
        if (buffer.mData == nullptr || buffer.mNumberChannels == 0) {
            continue;
        }
        if (channel >= base_channel && channel < base_channel + buffer.mNumberChannels) {
            const UInt32 local_channel = channel - base_channel;
            auto* samples = static_cast<float*>(buffer.mData);
            samples[(frame * static_cast<std::size_t>(buffer.mNumberChannels)) + local_channel] = value;
            return;
        }
        base_channel += buffer.mNumberChannels;
    }
}

std::int32_t float_to_i32(float sample)
{
    const double clamped = std::clamp(static_cast<double>(sample), -1.0, 1.0);
    return static_cast<std::int32_t>(clamped * 2147483647.0);
}

float i32_to_float(std::int32_t sample)
{
    return static_cast<float>(std::clamp(static_cast<double>(sample) / 2147483648.0, -1.0, 1.0));
}

struct CoreAudioDuplexContext {
    InputChannels input_channels = InputChannels::Mono;
    ChannelSelection channels;
    MonoRingBuffer* capture = nullptr;
    MonoRingBuffer* playback = nullptr;
    StreamControl* control = nullptr;
    std::vector<std::int32_t> capture_scratch;
    std::vector<std::int32_t> playback_scratch;
    std::size_t playback_prefill_frames = 0;
    double sample_rate = 48000.0;
    std::uint64_t metronome_sample_counter = 0;
    std::uint64_t metronome_beat_index = 0;
    int click_remaining = 0;
    int click_total = 0;
    double click_phase = 0.0;
    double click_phase_step = 0.0;
    std::int32_t resample_current = 0;
    std::int32_t resample_next = 0;
    bool resample_has_current = false;
    bool resample_has_next = false;
    double resample_phase = 0.0;
    std::atomic<long> callbacks{0};
    std::atomic<bool> playback_prefilled{false};
};

std::int32_t pop_one_frame(MonoRingBuffer& ring)
{
    std::array<std::int32_t, 1> frame{};
    (void)ring.pop(frame);
    return frame[0];
}

void pop_resampled_playback(CoreAudioDuplexContext& context, std::span<std::int32_t> output)
{
    if (context.playback == nullptr || context.control == nullptr) {
        std::fill(output.begin(), output.end(), 0);
        return;
    }

    const int ratio_ppm = context.control->playback_ratio_ppm.load(std::memory_order_relaxed);
    const double ratio = static_cast<double>(std::clamp(ratio_ppm, 995000, 1005000)) / 1000000.0;
    if (ratio_ppm == 1000000) {
        context.resample_has_current = false;
        context.resample_has_next = false;
        context.resample_phase = 0.0;
        context.playback->pop(output);
        return;
    }

    if (!context.resample_has_current) {
        context.resample_current = pop_one_frame(*context.playback);
        context.resample_has_current = true;
    }
    if (!context.resample_has_next) {
        context.resample_next = pop_one_frame(*context.playback);
        context.resample_has_next = true;
    }

    for (std::int32_t& sample : output) {
        const double mixed =
            static_cast<double>(context.resample_current) +
            (static_cast<double>(context.resample_next - context.resample_current) * context.resample_phase);
        sample = static_cast<std::int32_t>(std::clamp(mixed, -2147483648.0, 2147483647.0));

        context.resample_phase += ratio;
        while (context.resample_phase >= 1.0) {
            context.resample_phase -= 1.0;
            context.resample_current = context.resample_next;
            context.resample_next = pop_one_frame(*context.playback);
        }
    }
}

void mix_metronome_click(CoreAudioDuplexContext& context, std::span<std::int32_t> output)
{
    if (context.control == nullptr ||
        !context.control->metronome_enabled.load(std::memory_order_relaxed) ||
        context.sample_rate <= 0.0) {
        context.click_remaining = 0;
        context.click_total = 0;
        return;
    }

    const int bpm = context.control->metronome_bpm.load(std::memory_order_relaxed);
    if (bpm <= 0) {
        return;
    }
    const int level_ppm = context.control->metronome_level_ppm.load(std::memory_order_relaxed);
    const double level = static_cast<double>(std::clamp(level_ppm, 0, 1000000)) / 1000000.0;
    const std::uint64_t beat_interval =
        static_cast<std::uint64_t>((60.0 * context.sample_rate) / static_cast<double>(bpm));

    for (std::int32_t& sample : output) {
        if (beat_interval > 0 && (context.metronome_sample_counter % beat_interval) == 0) {
            const bool accent = (context.metronome_beat_index % 4ULL) == 0ULL;
            const double frequency = accent ? 1800.0 : 1200.0;
            context.click_total = static_cast<int>(context.sample_rate * (accent ? 0.012 : 0.008));
            context.click_remaining = context.click_total;
            context.click_phase = 0.0;
            context.click_phase_step = 2.0 * 3.14159265358979323846 * frequency / context.sample_rate;
            ++context.metronome_beat_index;
        }

        if (context.click_remaining > 0 && context.click_total > 0) {
            const double envelope = static_cast<double>(context.click_remaining) / static_cast<double>(context.click_total);
            const bool accent = context.click_total > static_cast<int>(context.sample_rate * 0.010);
            const double click_level = std::clamp(level * (accent ? 1.6 : 1.0), 0.0, 1.0);
            const double click = std::sin(context.click_phase) * envelope * click_level;
            const double mixed = static_cast<double>(sample) + (click * 2147483647.0);
            sample = static_cast<std::int32_t>(std::clamp(mixed, -2147483648.0, 2147483647.0));
            context.click_phase += context.click_phase_step;
            --context.click_remaining;
        }
        ++context.metronome_sample_counter;
    }
}

OSStatus duplex_io_proc(
    AudioObjectID,
    const AudioTimeStamp*,
    const AudioBufferList* input,
    const AudioTimeStamp*,
    AudioBufferList* output,
    const AudioTimeStamp*,
    void* client_data)
{
    auto* context = static_cast<CoreAudioDuplexContext*>(client_data);
    if (context == nullptr || context->capture == nullptr || context->playback == nullptr) {
        clear_output(output);
        return noErr;
    }

    const std::size_t input_frames = std::min(buffer_frames(input), context->capture_scratch.size());
    if (input_frames > 0) {
        for (std::size_t frame = 0; frame < input_frames; ++frame) {
            const float left = read_float_channel(input, frame, static_cast<UInt32>(context->channels.input_left));
            if (context->input_channels == InputChannels::Stereo) {
                const float right = read_float_channel(input, frame, static_cast<UInt32>(context->channels.input_right));
                context->capture_scratch[frame] = float_to_i32((left + right) * 0.5F);
            } else {
                context->capture_scratch[frame] = float_to_i32(left);
            }
        }
        context->capture->push(std::span<const std::int32_t>(context->capture_scratch.data(), input_frames));
    }

    const std::size_t output_frames = std::min(buffer_frames(output), context->playback_scratch.size());
    if (output_frames > 0) {
        auto playback = std::span<std::int32_t>(context->playback_scratch.data(), output_frames);
        if (!context->playback_prefilled.load(std::memory_order_relaxed)) {
            if (context->playback->available_read() >= context->playback_prefill_frames) {
                context->playback_prefilled.store(true, std::memory_order_relaxed);
            } else {
                std::fill(playback.begin(), playback.end(), 0);
            }
        }
        if (context->playback_prefilled.load(std::memory_order_relaxed)) {
            pop_resampled_playback(*context, playback);
        }
        mix_metronome_click(*context, playback);
        for (std::size_t frame = 0; frame < output_frames; ++frame) {
            const float sample = i32_to_float(context->playback_scratch[frame]);
            write_float_channel(output, frame, static_cast<UInt32>(context->channels.output_left), sample);
            if (context->channels.output_right >= 0) {
                write_float_channel(output, frame, static_cast<UInt32>(context->channels.output_right), sample);
            }
        }
    }

    context->callbacks.fetch_add(1, std::memory_order_relaxed);
    return noErr;
}

class CoreAudioDeviceStream final : public DeviceStream {
public:
    CoreAudioDeviceStream(
        AudioObjectID device,
        long buffer_size,
        InputChannels input_channels,
        ChannelSelection channels,
        MonoRingBuffer& capture_ring,
        MonoRingBuffer& playback_ring,
        std::size_t playback_prefill_frames,
        StreamControl& control,
        double sample_rate)
        : device_(device)
    {
        context_.input_channels = input_channels;
        context_.channels = channels;
        context_.capture = &capture_ring;
        context_.playback = &playback_ring;
        context_.control = &control;
        context_.capture_scratch.resize(static_cast<std::size_t>(buffer_size));
        context_.playback_scratch.resize(static_cast<std::size_t>(buffer_size));
        context_.playback_prefill_frames = playback_prefill_frames;
        context_.sample_rate = sample_rate;

        require_ok(
            AudioDeviceCreateIOProcID(device_, duplex_io_proc, &context_, &proc_id_),
            "AudioDeviceCreateIOProcID");
    }

    ~CoreAudioDeviceStream() override
    {
        if (proc_id_ != nullptr) {
            if (started_) {
                (void)AudioDeviceStop(device_, proc_id_);
            }
            (void)AudioDeviceDestroyIOProcID(device_, proc_id_);
        }
    }

    CoreAudioDeviceStream(const CoreAudioDeviceStream&) = delete;
    CoreAudioDeviceStream& operator=(const CoreAudioDeviceStream&) = delete;

    void start()
    {
        require_ok(AudioDeviceStart(device_, proc_id_), "AudioDeviceStart");
        started_ = true;
    }

    long callbacks() const override
    {
        return context_.callbacks.load(std::memory_order_relaxed);
    }

    bool playback_prefilled() const override
    {
        return context_.playback_prefilled.load(std::memory_order_relaxed);
    }

private:
    AudioObjectID device_ = kAudioObjectUnknown;
    AudioDeviceIOProcID proc_id_ = nullptr;
    bool started_ = false;
    CoreAudioDuplexContext context_;
};

} // namespace

std::vector<DeviceInfo> list_devices()
{
    const auto ids = get_device_ids();
    std::vector<DeviceInfo> devices;
    devices.reserve(ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i) {
        devices.push_back(make_device_info(static_cast<int>(i), ids[i]));
    }
    return devices;
}

DeviceProbe probe_device(int id, double requested_sample_rate)
{
    const auto selected = select_device(id);
    DeviceProbe probe;
    probe.device = selected.info;
    probe.driver_name = get_cf_string_property(selected.object, kAudioDevicePropertyDeviceUID);
    probe.driver_version = static_cast<long>(get_u32_property_or_zero(selected.object, kAudioDevicePropertyTransportType));
    probe.input_channels = static_cast<long>(device_channels(selected.object, kAudioDevicePropertyScopeInput));
    probe.output_channels = static_cast<long>(device_channels(selected.object, kAudioDevicePropertyScopeOutput));
    probe.input_latency_samples =
        static_cast<long>(get_u32_property_or_zero(selected.object, kAudioDevicePropertyLatency, kAudioDevicePropertyScopeInput)) +
        static_cast<long>(get_u32_property_or_zero(selected.object, kAudioDevicePropertySafetyOffset, kAudioDevicePropertyScopeInput));
    probe.output_latency_samples =
        static_cast<long>(get_u32_property_or_zero(selected.object, kAudioDevicePropertyLatency, kAudioDevicePropertyScopeOutput)) +
        static_cast<long>(get_u32_property_or_zero(selected.object, kAudioDevicePropertySafetyOffset, kAudioDevicePropertyScopeOutput));
    const AudioValueRange buffer_range = buffer_frame_range(selected.object);
    probe.min_buffer_size = static_cast<long>(buffer_range.mMinimum);
    probe.max_buffer_size = static_cast<long>(buffer_range.mMaximum);
    probe.preferred_buffer_size = static_cast<long>(get_u32_property_or_zero(selected.object, kAudioDevicePropertyBufferFrameSize));
    probe.buffer_granularity = 1;
    probe.current_sample_rate = get_double_property_or_zero(selected.object, kAudioDevicePropertyNominalSampleRate);
    probe.requested_sample_rate_supported = sample_rate_supported(selected.object, requested_sample_rate);
    return probe;
}

DeviceMeterResult meter_device(int id, double requested_sample_rate, long buffer_size, int duration_ms)
{
    return run_exploratory_io(id, requested_sample_rate, buffer_size, duration_ms, 0);
}

DeviceRingResult ring_device(
    int id,
    double requested_sample_rate,
    long buffer_size,
    int duration_ms,
    std::size_t ring_capacity_frames)
{
    if (duration_ms <= 0) {
        throw std::runtime_error("ring duration must be positive");
    }
    if (ring_capacity_frames == 0) {
        throw std::runtime_error("ring capacity must be positive");
    }

    const auto selected = select_device(id);
    const UInt32 input_channels = device_channels(selected.object, kAudioDevicePropertyScopeInput);
    const UInt32 output_channels = device_channels(selected.object, kAudioDevicePropertyScopeOutput);
    if (input_channels <= 0 || output_channels <= 0) {
        throw std::runtime_error("CoreAudio ring validation requires at least one input and one output channel");
    }
    if (!is_supported_float32_format(selected.object, kAudioDevicePropertyScopeInput) ||
        !is_supported_float32_format(selected.object, kAudioDevicePropertyScopeOutput)) {
        throw std::runtime_error("CoreAudio ring validation currently supports float32 linear PCM input/output only");
    }

    configure_device(selected.object, requested_sample_rate, buffer_size);
    const long active_buffer_size =
        static_cast<long>(get_u32_property_or_zero(selected.object, kAudioDevicePropertyBufferFrameSize));
    if (active_buffer_size <= 0) {
        throw std::runtime_error("CoreAudio reported an invalid buffer frame size");
    }
    if (ring_capacity_frames < static_cast<std::size_t>(active_buffer_size)) {
        throw std::runtime_error("ring capacity must be at least one CoreAudio buffer");
    }

    MonoRingBuffer ring(ring_capacity_frames);
    RingContext context;
    context.ring = &ring;
    context.capture_scratch.resize(static_cast<std::size_t>(active_buffer_size));
    context.playback_scratch.resize(static_cast<std::size_t>(active_buffer_size));
    context.input_channels = InputChannels::Mono;

    AudioDeviceIOProcID proc_id = nullptr;
    require_ok(
        AudioDeviceCreateIOProcID(selected.object, ring_io_proc, &context, &proc_id),
        "AudioDeviceCreateIOProcID");

    struct ProcGuard {
        AudioObjectID device = kAudioObjectUnknown;
        AudioDeviceIOProcID proc = nullptr;
        bool started = false;
        ~ProcGuard()
        {
            if (proc != nullptr) {
                if (started) {
                    (void)AudioDeviceStop(device, proc);
                }
                (void)AudioDeviceDestroyIOProcID(device, proc);
            }
        }
    } guard{selected.object, proc_id, false};

    require_ok(AudioDeviceStart(selected.object, proc_id), "AudioDeviceStart");
    guard.started = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));

    const auto ring_stats = ring.stats();
    DeviceRingResult result;
    result.device = selected.info;
    result.sample_rate = get_double_property_or_zero(selected.object, kAudioDevicePropertyNominalSampleRate);
    result.buffer_size = active_buffer_size;
    result.callbacks = context.callbacks.load(std::memory_order_relaxed);
    result.ring_overruns = ring_stats.overruns;
    result.ring_underruns = ring_stats.underruns;
    result.ring_readable = ring.available_read();
    return result;
}

std::unique_ptr<DeviceStream> start_duplex_stream(
    int id,
    double requested_sample_rate,
    long requested_buffer_size,
    InputChannels requested_input_channels,
    ChannelSelection channels,
    MonoRingBuffer& capture_ring,
    MonoRingBuffer& playback_ring,
    std::size_t playback_prefill_frames,
    StreamControl& control)
{
    const auto selected = select_device(id);
    const UInt32 input_channels = device_channels(selected.object, kAudioDevicePropertyScopeInput);
    const UInt32 output_channels = device_channels(selected.object, kAudioDevicePropertyScopeOutput);
    const UInt32 requested_channels = requested_input_channels == InputChannels::Stereo ? 2U : 1U;
    if (channels.input_left < 0 || static_cast<UInt32>(channels.input_left) >= input_channels ||
        (requested_channels > 1 && (channels.input_right < 0 || static_cast<UInt32>(channels.input_right) >= input_channels))) {
        throw std::runtime_error("selected CoreAudio input channel is out of range");
    }
    const UInt32 requested_output_channels = channels.output_right >= 0 ? 2U : 1U;
    if (channels.output_left < 0 || static_cast<UInt32>(channels.output_left) >= output_channels ||
        (requested_output_channels > 1 && (channels.output_right < 0 || static_cast<UInt32>(channels.output_right) >= output_channels))) {
        throw std::runtime_error("selected CoreAudio output channel is out of range");
    }
    if (!is_supported_float32_format(selected.object, kAudioDevicePropertyScopeInput) ||
        !is_supported_float32_format(selected.object, kAudioDevicePropertyScopeOutput)) {
        throw std::runtime_error("CoreAudio duplex stream currently supports float32 linear PCM input/output only");
    }

    configure_device(selected.object, requested_sample_rate, requested_buffer_size);
    const long buffer_size =
        static_cast<long>(get_u32_property_or_zero(selected.object, kAudioDevicePropertyBufferFrameSize));
    if (buffer_size <= 0) {
        throw std::runtime_error("CoreAudio reported an invalid buffer frame size");
    }
    if (playback_prefill_frames > playback_ring.capacity()) {
        throw std::runtime_error("playback prefill must fit within playback ring capacity");
    }

    const double sample_rate = get_double_property_or_zero(selected.object, kAudioDevicePropertyNominalSampleRate);
    auto stream = std::make_unique<CoreAudioDeviceStream>(
        selected.object,
        buffer_size,
        requested_input_channels,
        channels,
        capture_ring,
        playback_ring,
        playback_prefill_frames,
        control,
        sample_rate > 0.0 ? sample_rate : requested_sample_rate);
    stream->start();
    return stream;
}

} // namespace jam2::audio

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
#include <limits>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace jam2::audio {
namespace {

constexpr double kPi = 3.14159265358979323846;
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

std::string one_based_channel_text(int channel)
{
    return channel >= 0 ? std::to_string(channel + 1) : std::string("off");
}

std::string selected_channel_range_text(const ChannelSelection& channels, bool input)
{
    const auto& selected = input ? channels.input : channels.output;
    if (selected.empty()) {
        return "off";
    }
    std::ostringstream out;
    for (std::size_t i = 0; i < selected.size(); ++i) {
        if (i > 0) {
            out << ",";
        }
        out << one_based_channel_text(selected[i]);
    }
    return out.str();
}

std::string channel_range_error(
    const char* backend,
    const char* direction,
    const ChannelSelection& channels,
    UInt32 available,
    bool input)
{
    std::ostringstream out;
    out << "selected " << backend << " " << direction << " channel(s) "
        << selected_channel_range_text(channels, input)
        << " out of range; device has " << available << " " << direction
        << " channel(s)";
    return out.str();
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

int i32_peak_ppm(std::span<const std::int32_t> samples)
{
    std::uint32_t peak = 0;
    for (std::int32_t sample : samples) {
        const std::uint32_t abs_sample = sample == std::numeric_limits<std::int32_t>::min() ?
            static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) :
            static_cast<std::uint32_t>(std::abs(sample));
        peak = std::max(peak, abs_sample);
    }
    const double normalized = static_cast<double>(peak) / 2147483647.0;
    return static_cast<int>(std::clamp(normalized, 0.0, 1.0) * 1000000.0);
}

std::int32_t scale_i32_sample(std::int32_t sample, double level)
{
    const double scaled = static_cast<double>(sample) * level;
    return static_cast<std::int32_t>(std::clamp(scaled, -2147483648.0, 2147483647.0));
}

void observe_peak(std::atomic<int>& peak, std::span<const std::int32_t> samples)
{
    update_peak(peak, i32_peak_ppm(samples));
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
    OutputRecorder* recorder = nullptr;
    TrackTakeRecorder* track_take_recorder = nullptr;
    std::vector<std::int32_t> capture_scratch;
    std::vector<std::int32_t> playback_scratch;
    std::vector<std::int32_t> recorder_my_input_scratch;
    std::vector<std::int32_t> recorder_their_input_scratch;
    std::vector<std::int32_t> recorder_inputs_mix_scratch;
    std::vector<std::int32_t> recorder_metronome_scratch;
    std::size_t playback_prefill_frames = 0;
    double sample_rate = 48000.0;
    std::uint64_t test_input_sample_counter = 0;
    std::uint64_t engine_frame_counter = 0;
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
    std::atomic<std::uint64_t> last_callback_us{0};
    std::atomic<std::uint64_t> callback_interval_min_us{0};
    std::atomic<std::uint64_t> callback_interval_sum_us{0};
    std::atomic<std::uint64_t> callback_interval_max_us{0};
    std::atomic<std::uint64_t> callback_interval_samples{0};
    std::atomic<std::uint64_t> callback_gap_over_1_1x_count{0};
    std::atomic<std::uint64_t> callback_gap_over_1_5x_count{0};
    std::atomic<std::uint64_t> callback_gap_over_2x_count{0};
};

std::uint64_t callback_now_us()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

void atomic_update_max(std::atomic<std::uint64_t>& target, std::uint64_t value)
{
    std::uint64_t current = target.load(std::memory_order_relaxed);
    while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

void atomic_update_min(std::atomic<std::uint64_t>& target, std::uint64_t value)
{
    std::uint64_t current = target.load(std::memory_order_relaxed);
    while ((current == 0 || value < current) &&
           !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

void observe_callback_interval(CoreAudioDuplexContext& context)
{
    const std::uint64_t now = callback_now_us();
    const std::uint64_t previous = context.last_callback_us.exchange(now, std::memory_order_relaxed);
    if (previous == 0 || now <= previous || context.sample_rate <= 0.0 || context.playback_scratch.empty()) {
        return;
    }
    const std::uint64_t interval = now - previous;
    atomic_update_min(context.callback_interval_min_us, interval);
    context.callback_interval_sum_us.fetch_add(interval, std::memory_order_relaxed);
    atomic_update_max(context.callback_interval_max_us, interval);
    context.callback_interval_samples.fetch_add(1, std::memory_order_relaxed);
    const double expected = static_cast<double>(context.playback_scratch.size()) * 1000000.0 / context.sample_rate;
    if (interval > static_cast<std::uint64_t>(expected * 1.1)) {
        context.callback_gap_over_1_1x_count.fetch_add(1, std::memory_order_relaxed);
    }
    if (interval > static_cast<std::uint64_t>(expected * 1.5)) {
        context.callback_gap_over_1_5x_count.fetch_add(1, std::memory_order_relaxed);
    }
    if (interval > static_cast<std::uint64_t>(expected * 2.0)) {
        context.callback_gap_over_2x_count.fetch_add(1, std::memory_order_relaxed);
    }
}

std::int32_t pop_one_frame(MonoRingBuffer& ring)
{
    std::array<std::int32_t, 1> frame{};
    (void)ring.pop(frame, false);
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
    if (ratio_ppm == 1000000 && !context.resample_has_current && !context.resample_has_next) {
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

void apply_remote_level(CoreAudioDuplexContext& context, std::span<std::int32_t> output)
{
    if (context.control == nullptr) {
        return;
    }
    const int level_ppm = context.control->remote_level_ppm.load(std::memory_order_relaxed);
    if (level_ppm == 1000000) {
        return;
    }
    const double level = static_cast<double>(std::clamp(level_ppm, 0, 4000000)) / 1000000.0;
    for (std::int32_t& sample : output) {
        sample = scale_i32_sample(sample, level);
    }
}

std::int32_t mix_i32_samples(std::int32_t a, std::int32_t b)
{
    const std::int64_t mixed = static_cast<std::int64_t>(a) + static_cast<std::int64_t>(b);
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(mixed, -2147483648LL, 2147483647LL));
}

void mix_local_monitor(CoreAudioDuplexContext& context, std::span<std::int32_t> output, std::span<const std::int32_t> input)
{
    if (context.control == nullptr ||
        !context.control->local_monitor_enabled.load(std::memory_order_relaxed) ||
        input.empty()) {
        if (context.control != nullptr) {
            context.control->monitor_peak_ppm.store(0, std::memory_order_relaxed);
            context.control->gui_monitor_peak_ppm.store(0, std::memory_order_relaxed);
        }
        return;
    }
    const int level_ppm = context.control->local_monitor_level_ppm.load(std::memory_order_relaxed);
    if (level_ppm <= 0) {
        context.control->monitor_peak_ppm.store(0, std::memory_order_relaxed);
        context.control->gui_monitor_peak_ppm.store(0, std::memory_order_relaxed);
        return;
    }
    const double level = static_cast<double>(std::clamp(level_ppm, 0, 4000000)) / 1000000.0;
    std::uint32_t monitor_peak = 0;
    const std::size_t frames = std::min(output.size(), input.size());
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::int32_t monitored = scale_i32_sample(input[frame], level);
        const std::uint32_t abs_sample = monitored == std::numeric_limits<std::int32_t>::min() ?
            static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) :
            static_cast<std::uint32_t>(std::abs(monitored));
        monitor_peak = std::max(monitor_peak, abs_sample);
        output[frame] = mix_i32_samples(output[frame], monitored);
    }
    const double normalized = static_cast<double>(monitor_peak) / 2147483647.0;
    const int peak_ppm = static_cast<int>(std::clamp(normalized, 0.0, 1.0) * 1000000.0);
    context.control->monitor_peak_ppm.store(peak_ppm, std::memory_order_relaxed);
    context.control->gui_monitor_peak_ppm.store(peak_ppm, std::memory_order_relaxed);
}

void mix_prepared_source(CoreAudioDuplexContext& context, std::span<std::int32_t> output, std::uint64_t frame)
{
    if (context.control == nullptr || context.control->prepared_source == nullptr || output.empty()) {
        return;
    }
    context.control->prepared_source->mix(output.data(), output.size(), frame);
    context.control->prepared_source_frame.store(
        context.control->prepared_source->sourceFrame(),
        std::memory_order_relaxed);
    context.control->prepared_source_scheduled_start_frame.store(
        context.control->prepared_source->scheduledStartFrame(),
        std::memory_order_relaxed);
    context.control->prepared_source_actual_start_frame.store(
        context.control->prepared_source->actualStartFrame(),
        std::memory_order_relaxed);
    context.control->prepared_source_underruns.store(
        context.control->prepared_source->underruns(),
        std::memory_order_relaxed);
}

void observe_output_peak(CoreAudioDuplexContext& context, std::span<const std::int32_t> output)
{
    if (context.control == nullptr) {
        return;
    }
    observe_peak(context.control->output_peak_ppm, output);
    observe_peak(context.control->gui_output_peak_ppm, output);
    for (std::int32_t sample : output) {
        if (sample == std::numeric_limits<std::int32_t>::min() ||
            sample == std::numeric_limits<std::int32_t>::max()) {
            context.control->output_clipped_samples.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

std::int32_t render_test_input_sample(int mode, std::uint64_t sample_time, double sample_rate, double level)
{
    if (mode == 1 || sample_rate <= 0.0) {
        return 0;
    }
    if (mode == 2) {
        const double phase = std::fmod(static_cast<double>(sample_time) * 440.0 / sample_rate, 1.0);
        return static_cast<std::int32_t>(std::sin(phase * 2.0 * kPi) * level * 2147483647.0);
    }
    if (mode == 3) {
        const std::uint64_t period = static_cast<std::uint64_t>(sample_rate > 1.0 ? sample_rate : 1.0);
        const std::uint64_t width = std::max<std::uint64_t>(1, period / 100);
        return (sample_time % period) < width ?
            static_cast<std::int32_t>(level * 2147483647.0) :
            0;
    }
    return 0;
}

std::int32_t render_metronome_test_input_sample(
    const StreamControl& control,
    std::uint64_t sample_time,
    double sample_rate,
    double level)
{
    if (sample_rate <= 0.0 ||
        !control.metronome_enabled.load(std::memory_order_relaxed) ||
        !control.metronome_epoch_valid.load(std::memory_order_relaxed)) {
        return 0;
    }
    const std::uint64_t epoch = control.metronome_epoch_sample_time.load(std::memory_order_relaxed);
    if (sample_time < epoch) {
        return 0;
    }
    const jam2::metronome::PatternSnapshot pattern = jam2::metronome::sanitize({
        control.metronome_bpm.load(std::memory_order_relaxed),
        control.metronome_beats_per_bar.load(std::memory_order_relaxed),
        control.metronome_division.load(std::memory_order_relaxed),
        control.metronome_step_count.load(std::memory_order_relaxed),
        control.metronome_play_mask_low.load(std::memory_order_relaxed),
        control.metronome_play_mask_high.load(std::memory_order_relaxed),
        control.metronome_accent_mask_low.load(std::memory_order_relaxed),
        control.metronome_accent_mask_high.load(std::memory_order_relaxed),
    });
    const std::uint64_t step_interval =
        jam2::metronome::step_interval_samples(sample_rate, pattern.bpm, pattern.division);
    const double rendered = jam2::metronome::render_sample(
        pattern,
        sample_time - epoch,
        step_interval,
        sample_rate,
        level);
    return jam2::metronome::mix_i32(0, rendered);
}

void fill_test_input(CoreAudioDuplexContext& context, std::span<std::int32_t> output)
{
    if (context.control == nullptr) {
        std::fill(output.begin(), output.end(), 0);
        return;
    }
    const int mode = context.control->test_input_mode.load(std::memory_order_relaxed);
    const double level = static_cast<double>(
        std::clamp(context.control->test_input_level_ppm.load(std::memory_order_relaxed), 0, 1000000)) / 1000000.0;
    for (std::int32_t& sample : output) {
        sample = mode == 4 ?
            render_metronome_test_input_sample(
                *context.control,
                context.test_input_sample_counter,
                context.sample_rate,
                level) :
            render_test_input_sample(mode, context.test_input_sample_counter, context.sample_rate, level);
        ++context.test_input_sample_counter;
    }
}

void mix_metronome_click(CoreAudioDuplexContext& context, std::span<std::int32_t> output, std::span<std::int32_t> metronome_stem)
{
    if (context.control == nullptr ||
        context.sample_rate <= 0.0) {
        return;
    }
    if (metronome_stem.size() == output.size()) {
        std::fill(metronome_stem.begin(), metronome_stem.end(), 0);
    }
    const bool enabled = context.control->metronome_enabled.load(std::memory_order_relaxed);
    const bool local_click_suppressed =
        context.control->metronome_mode.load(std::memory_order_relaxed) == 1 &&
        !context.control->leader_audio_local_click.load(std::memory_order_relaxed);
    if (!enabled || local_click_suppressed) {
        return;
    }

    const int level_ppm = context.control->metronome_level_ppm.load(std::memory_order_relaxed);
    const double level = static_cast<double>(std::clamp(level_ppm, 0, 4000000)) / 1000000.0;
    const bool epoch_valid = context.control->metronome_epoch_valid.load(std::memory_order_relaxed);
    const std::uint64_t epoch = context.control->metronome_epoch_sample_time.load(std::memory_order_relaxed);
    const std::int64_t render_offset_frames =
        context.control->metronome_render_offset_frames.load(std::memory_order_relaxed);
    const jam2::metronome::PatternSnapshot pattern = jam2::metronome::sanitize({
        context.control->metronome_bpm.load(std::memory_order_relaxed),
        context.control->metronome_beats_per_bar.load(std::memory_order_relaxed),
        context.control->metronome_division.load(std::memory_order_relaxed),
        context.control->metronome_step_count.load(std::memory_order_relaxed),
        context.control->metronome_play_mask_low.load(std::memory_order_relaxed),
        context.control->metronome_play_mask_high.load(std::memory_order_relaxed),
        context.control->metronome_accent_mask_low.load(std::memory_order_relaxed),
        context.control->metronome_accent_mask_high.load(std::memory_order_relaxed),
    });
    const std::uint64_t step_interval =
        jam2::metronome::step_interval_samples(context.sample_rate, pattern.bpm, pattern.division);

    for (std::size_t i = 0; i < output.size(); ++i) {
        std::uint64_t render_sample_counter =
            context.engine_frame_counter + static_cast<std::uint64_t>(i);
        if (render_offset_frames < 0) {
            const std::uint64_t offset = static_cast<std::uint64_t>(-render_offset_frames);
            render_sample_counter = render_sample_counter > offset ? render_sample_counter - offset : 0ULL;
        } else {
            render_sample_counter += static_cast<std::uint64_t>(render_offset_frames);
        }
        const bool before_epoch = epoch_valid && render_sample_counter < epoch;
        const std::uint64_t position =
            before_epoch ? 0ULL : (epoch_valid ? render_sample_counter - epoch : render_sample_counter);
        if (!before_epoch) {
            const double rendered =
                jam2::metronome::render_sample(pattern, position, step_interval, context.sample_rate, level);
            if (metronome_stem.size() == output.size()) {
                metronome_stem[i] = jam2::metronome::mix_i32(0, rendered);
            }
            output[i] = jam2::metronome::mix_i32(output[i], rendered);
            if (step_interval > 0) {
                context.metronome_beat_index = (position / step_interval) + 1;
            }
        }
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
    observe_callback_interval(*context);
    const bool network_capture_enabled = context->control != nullptr &&
        prepare_network_capture_callback(
            *context->control,
            *context->capture,
            context->engine_frame_counter);
    const std::size_t output_frames_for_input = std::min(buffer_frames(output), context->recorder_my_input_scratch.size());
    if (output_frames_for_input > 0) {
        std::fill(
            context->recorder_my_input_scratch.begin(),
            context->recorder_my_input_scratch.begin() + output_frames_for_input,
            0);
    }

    const int test_input_mode = context->control != nullptr ?
        context->control->test_input_mode.load(std::memory_order_relaxed) :
        0;
    const std::size_t input_frames = std::min(buffer_frames(input), context->capture_scratch.size());
    if (test_input_mode != 0 && !context->capture_scratch.empty()) {
        const std::size_t generated_frames = std::min(buffer_frames(output), context->capture_scratch.size());
        auto generated = std::span<std::int32_t>(context->capture_scratch.data(), generated_frames);
        fill_test_input(*context, generated);
        if (context->control != nullptr) {
            observe_peak(context->control->input_peak_ppm, generated);
            observe_peak(context->control->gui_input_peak_ppm, generated);
        }
        if (network_capture_enabled) {
            context->capture->push(std::span<const std::int32_t>(generated.data(), generated.size()));
        }
        if (context->recorder_my_input_scratch.size() >= generated_frames) {
            std::copy(generated.begin(), generated.end(), context->recorder_my_input_scratch.begin());
        }
    } else if (input_frames > 0) {
        for (std::size_t frame = 0; frame < input_frames; ++frame) {
            float sum = 0.0F;
            for (int channel : context->channels.input) {
                sum += read_float_channel(input, frame, static_cast<UInt32>(channel));
            }
            const float mixed = context->channels.input.empty() ?
                0.0F :
                sum / static_cast<float>(context->channels.input.size());
            context->capture_scratch[frame] = float_to_i32(mixed);
        }
        if (network_capture_enabled) {
            context->capture->push(std::span<const std::int32_t>(context->capture_scratch.data(), input_frames));
        }
        if (context->control != nullptr) {
            observe_peak(
                context->control->input_peak_ppm,
                std::span<const std::int32_t>(context->capture_scratch.data(), input_frames));
            observe_peak(
                context->control->gui_input_peak_ppm,
                std::span<const std::int32_t>(context->capture_scratch.data(), input_frames));
        }
        if (context->recorder_my_input_scratch.size() >= input_frames) {
            std::copy(
                context->capture_scratch.begin(),
                context->capture_scratch.begin() + input_frames,
                context->recorder_my_input_scratch.begin());
        }
    }

    const std::size_t output_frames = std::min(buffer_frames(output), context->playback_scratch.size());
    if (output_frames > 0) {
        auto playback = std::span<std::int32_t>(context->playback_scratch.data(), output_frames);
        const bool network_playback_enabled = context->control != nullptr &&
            context->control->network_playback_enabled.load(std::memory_order_acquire);
        if (!network_playback_enabled) {
            context->playback_prefilled.store(false, std::memory_order_relaxed);
            context->playback->pop(std::span<std::int32_t>{}, false);
            std::fill(playback.begin(), playback.end(), 0);
        } else if (!context->playback_prefilled.load(std::memory_order_relaxed)) {
            if (context->playback->available_read() >= context->playback_prefill_frames) {
                context->playback_prefilled.store(true, std::memory_order_relaxed);
            } else {
                std::fill(playback.begin(), playback.end(), 0);
            }
        }
        if (network_playback_enabled && context->playback_prefilled.load(std::memory_order_relaxed)) {
            pop_resampled_playback(*context, playback);
            apply_remote_level(*context, playback);
        }
        if (context->control != nullptr) {
            observe_peak(context->control->remote_peak_ppm, playback);
            observe_peak(context->control->gui_remote_peak_ppm, playback);
        }
        if (context->recorder_their_input_scratch.size() >= output_frames) {
            std::copy(playback.begin(), playback.end(), context->recorder_their_input_scratch.begin());
        }
        mix_local_monitor(
            *context,
            playback,
            std::span<const std::int32_t>(
                context->capture_scratch.data(),
                std::min<std::size_t>(context->capture_scratch.size(), playback.size())));
        const std::uint64_t audio_frame_start = context->engine_frame_counter;
        if (context->track_take_recorder != nullptr &&
            context->recorder_my_input_scratch.size() >= playback.size()) {
            const std::uint64_t compensation = context->control != nullptr
                ? context->control->recording_latency_compensation_frames.load(std::memory_order_relaxed)
                : 0ULL;
            const std::uint64_t capture_frame_start = audio_frame_start > compensation
                ? audio_frame_start - compensation
                : 0ULL;
            context->track_take_recorder->record(
                capture_frame_start,
                std::span<const std::int32_t>(context->recorder_my_input_scratch.data(), playback.size()));
        }
        mix_prepared_source(*context, playback, audio_frame_start);
        mix_metronome_click(
            *context,
            playback,
            std::span<std::int32_t>(
                context->recorder_metronome_scratch.data(),
                std::min<std::size_t>(context->recorder_metronome_scratch.size(), playback.size())));
        if (context->control != nullptr) {
            observe_peak(
                context->control->metronome_peak_ppm,
                std::span<const std::int32_t>(
                    context->recorder_metronome_scratch.data(),
                    std::min<std::size_t>(context->recorder_metronome_scratch.size(), playback.size())));
            observe_peak(
                context->control->gui_metronome_peak_ppm,
                std::span<const std::int32_t>(
                    context->recorder_metronome_scratch.data(),
                    std::min<std::size_t>(context->recorder_metronome_scratch.size(), playback.size())));
            observe_output_peak(*context, playback);
        }
        if (context->recorder != nullptr &&
            context->recorder_inputs_mix_scratch.size() >= playback.size() &&
            context->recorder_my_input_scratch.size() >= playback.size() &&
            context->recorder_their_input_scratch.size() >= playback.size() &&
            context->recorder_metronome_scratch.size() >= playback.size()) {
            for (std::size_t frame = 0; frame < playback.size(); ++frame) {
                context->recorder_inputs_mix_scratch[frame] = mix_i32_samples(
                    context->recorder_my_input_scratch[frame],
                    context->recorder_their_input_scratch[frame]);
            }
            context->recorder->record(RecordBlock{
                audio_frame_start,
                playback,
                std::span<const std::int32_t>(context->recorder_my_input_scratch.data(), playback.size()),
                std::span<const std::int32_t>(context->recorder_their_input_scratch.data(), playback.size()),
                std::span<const std::int32_t>(context->recorder_inputs_mix_scratch.data(), playback.size()),
                std::span<const std::int32_t>(context->recorder_metronome_scratch.data(), playback.size()),
            });
        }
        for (std::size_t frame = 0; frame < output_frames; ++frame) {
            const float sample = i32_to_float(context->playback_scratch[frame]);
            for (int channel : context->channels.output) {
                write_float_channel(output, frame, static_cast<UInt32>(channel), sample);
            }
        }
    }

    const std::size_t callback_frames = std::max(buffer_frames(input), buffer_frames(output));
    context->engine_frame_counter += static_cast<std::uint64_t>(callback_frames);
    if (context->control != nullptr) {
        context->control->engine_frame_counter.store(context->engine_frame_counter, std::memory_order_release);
    }
    context->callbacks.fetch_add(1, std::memory_order_relaxed);
    return noErr;
}

class CoreAudioDeviceStream final : public DeviceStream {
public:
    CoreAudioDeviceStream(
        AudioObjectID device,
        DeviceInfo info,
        long buffer_size,
        InputChannels input_channels,
        ChannelSelection channels,
        MonoRingBuffer& capture_ring,
        MonoRingBuffer& playback_ring,
        std::size_t playback_prefill_frames,
        StreamControl& control,
        OutputRecorder* recorder,
        TrackTakeRecorder* track_take_recorder,
        double sample_rate)
        : device_(device),
          info_(std::move(info)),
          buffer_size_(buffer_size),
          input_channels_(input_channels),
          channels_(channels),
          sample_rate_(sample_rate)
    {
        context_.input_channels = input_channels;
        context_.channels = channels;
        context_.capture = &capture_ring;
        context_.playback = &playback_ring;
        context_.control = &control;
        context_.recorder = recorder;
        context_.track_take_recorder = track_take_recorder;
        context_.capture_scratch.resize(static_cast<std::size_t>(buffer_size));
        context_.playback_scratch.resize(static_cast<std::size_t>(buffer_size));
        context_.recorder_my_input_scratch.resize(static_cast<std::size_t>(buffer_size));
        context_.recorder_their_input_scratch.resize(static_cast<std::size_t>(buffer_size));
        context_.recorder_inputs_mix_scratch.resize(static_cast<std::size_t>(buffer_size));
        context_.recorder_metronome_scratch.resize(static_cast<std::size_t>(buffer_size));
        context_.playback_prefill_frames = playback_prefill_frames;
        context_.sample_rate = sample_rate;
        input_latency_frames_ = static_cast<long>(
            get_u32_property_or_zero(device_, kAudioDevicePropertyLatency, kAudioDevicePropertyScopeInput) +
            get_u32_property_or_zero(device_, kAudioDevicePropertySafetyOffset, kAudioDevicePropertyScopeInput));
        output_latency_frames_ = channels_.output.empty() ? 0L : static_cast<long>(
            get_u32_property_or_zero(device_, kAudioDevicePropertyLatency, kAudioDevicePropertyScopeOutput) +
            get_u32_property_or_zero(device_, kAudioDevicePropertySafetyOffset, kAudioDevicePropertyScopeOutput));
        control.input_latency_frames.store(
            static_cast<std::uint32_t>(std::max(0L, input_latency_frames_)),
            std::memory_order_relaxed);
        control.output_latency_frames.store(
            static_cast<std::uint32_t>(std::max(0L, output_latency_frames_)),
            std::memory_order_relaxed);
        control.recording_latency_compensation_frames.store(
            static_cast<std::uint64_t>(std::max(0L, input_latency_frames_)) +
                static_cast<std::uint64_t>(std::max(0L, output_latency_frames_)),
            std::memory_order_relaxed);

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

    StreamInfo info() const override
    {
        StreamInfo result;
        result.device = info_;
        result.sample_rate = sample_rate_;
        result.buffer_size = buffer_size_;
        result.input_latency_frames = input_latency_frames_;
        result.output_latency_frames = output_latency_frames_;
        result.input_channels = input_channels_;
        result.channels = channels_;
        result.sample_format = "CoreAudio Float32 packed";
        return result;
    }

    CallbackTimingStats callback_timing_stats() const override
    {
        return CallbackTimingStats{
            context_.callback_interval_min_us.load(std::memory_order_relaxed),
            context_.callback_interval_sum_us.load(std::memory_order_relaxed),
            context_.callback_interval_max_us.load(std::memory_order_relaxed),
            context_.callback_interval_samples.load(std::memory_order_relaxed),
            context_.callback_gap_over_1_1x_count.load(std::memory_order_relaxed),
            context_.callback_gap_over_1_5x_count.load(std::memory_order_relaxed),
            context_.callback_gap_over_2x_count.load(std::memory_order_relaxed),
        };
    }

private:
    AudioObjectID device_ = kAudioObjectUnknown;
    DeviceInfo info_;
    AudioDeviceIOProcID proc_id_ = nullptr;
    bool started_ = false;
    long buffer_size_ = 0;
    long input_latency_frames_ = 0;
    long output_latency_frames_ = 0;
    InputChannels input_channels_ = InputChannels::Mono;
    ChannelSelection channels_;
    double sample_rate_ = 0.0;
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
    result.ring_underrun_events = ring_stats.underrun_events;
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
    StreamControl& control,
    OutputRecorder* recorder,
    TrackTakeRecorder* track_take_recorder)
{
    const auto selected = select_device(id);
    const UInt32 input_channels = device_channels(selected.object, kAudioDevicePropertyScopeInput);
    const UInt32 output_channels = device_channels(selected.object, kAudioDevicePropertyScopeOutput);
    if (channels.input.empty()) {
        throw std::runtime_error("CoreAudio stream requires at least one selected input channel");
    }
    for (int channel : channels.input) {
        if (channel < 0 || static_cast<UInt32>(channel) >= input_channels) {
            throw std::runtime_error(channel_range_error("CoreAudio", "input", channels, input_channels, true));
        }
    }
    for (int channel : channels.output) {
        if (channel < 0 || static_cast<UInt32>(channel) >= output_channels) {
            throw std::runtime_error(channel_range_error("CoreAudio", "output", channels, output_channels, false));
        }
    }
    if (!is_supported_float32_format(selected.object, kAudioDevicePropertyScopeInput)) {
        throw std::runtime_error("CoreAudio stream currently supports float32 linear PCM input only");
    }
    if (!channels.output.empty() && !is_supported_float32_format(selected.object, kAudioDevicePropertyScopeOutput)) {
        throw std::runtime_error("CoreAudio stream currently supports float32 linear PCM output only");
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
        selected.info,
        buffer_size,
        requested_input_channels,
        channels,
        capture_ring,
        playback_ring,
        playback_prefill_frames,
        control,
        recorder,
        track_take_recorder,
        sample_rate > 0.0 ? sample_rate : requested_sample_rate);
    stream->start();
    return stream;
}

} // namespace jam2::audio

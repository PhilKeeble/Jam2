#include "audio_device.hpp"
#include "audio_device_processing.hpp"
#include "coreaudio_buffer_processing.hpp"

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
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

namespace processing = device_processing;
namespace coreaudio = coreaudio_processing;

constexpr AudioObjectPropertyElement kMainElement = kAudioObjectPropertyElementMain;
constexpr double kSampleRateToleranceHz = 1.0;
constexpr auto kDeviceConfigurationPollInterval = std::chrono::milliseconds(10);
constexpr auto kDeviceConfigurationTimeout = std::chrono::milliseconds(3000);
// CoreAudio timestamps are stable across adjacent device cycles. Keep exact
// valid/invalid counts and callback-gap/fault counters, but sample the more
// expensive host-time conversion aggregates often enough for tuning without
// paying for four conversions on every sub-millisecond callback.
constexpr std::uint64_t kTimestampTimingSampleMask = 0x0f;

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

std::vector<AudioObjectID> get_object_ids(
    AudioObjectID object,
    AudioObjectPropertySelector selector,
    AudioObjectPropertyScope scope)
{
    auto property = address(selector, scope);
    if (!has_property(object, property)) {
        return {};
    }
    const UInt32 size = property_size(object, property);
    std::vector<AudioObjectID> objects(size / sizeof(AudioObjectID));
    if (!objects.empty()) {
        UInt32 mutable_size = size;
        require_ok(
            AudioObjectGetPropertyData(
                object, &property, 0, nullptr, &mutable_size, objects.data()),
            "AudioObjectGetPropertyData object list");
    }
    return objects;
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

std::string stream_format_description(
    AudioObjectID stream,
    std::size_t stream_index)
{
    AudioStreamBasicDescription desc{};
    if (!try_get_property(
            stream,
            address(kAudioStreamPropertyVirtualFormat),
            desc)) {
        return "stream" + std::to_string(stream_index) + "_format=unknown";
    }
    std::ostringstream out;
    out << "stream" << stream_index
        << "_format_id=" << desc.mFormatID
        << " rate=" << desc.mSampleRate
        << " channels_per_frame=" << desc.mChannelsPerFrame
        << " bits_per_channel=" << desc.mBitsPerChannel
        << " bytes_per_frame=" << desc.mBytesPerFrame
        << " frames_per_packet=" << desc.mFramesPerPacket
        << " flags=" << format_flags(desc.mFormatFlags);
    return out.str();
}

std::string format_description(AudioObjectID device, AudioObjectPropertyScope scope)
{
    const auto streams = get_object_ids(device, kAudioDevicePropertyStreams, scope);
    std::ostringstream out;
    out << "streams=" << streams.size();
    for (std::size_t stream_index = 0; stream_index < streams.size(); ++stream_index) {
        out << " " << stream_format_description(streams[stream_index], stream_index);
    }
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

double wait_for_nominal_sample_rate(AudioObjectID device, double requested_sample_rate)
{
    // CoreAudio may acknowledge the setter before the device publishes the new
    // nominal rate. Keep that cold-path transition bounded before stream setup.
    const auto deadline = std::chrono::steady_clock::now() + kDeviceConfigurationTimeout;
    double active_sample_rate = get_double_property_or_zero(
        device,
        kAudioDevicePropertyNominalSampleRate);
    while (std::abs(active_sample_rate - requested_sample_rate) > kSampleRateToleranceHz &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(kDeviceConfigurationPollInterval);
        active_sample_rate = get_double_property_or_zero(
            device,
            kAudioDevicePropertyNominalSampleRate);
    }
    if (std::abs(active_sample_rate - requested_sample_rate) > kSampleRateToleranceHz) {
        std::ostringstream message;
        message << "CoreAudio sample rate change did not settle within "
                << kDeviceConfigurationTimeout.count() << " ms: requested "
                << requested_sample_rate << " Hz, last reported "
                << active_sample_rate << " Hz";
        throw std::runtime_error(message.str());
    }
    return active_sample_rate;
}

double configure_nominal_sample_rate(AudioObjectID device, double requested_sample_rate)
{
    double active_sample_rate = get_double_property_or_zero(
        device,
        kAudioDevicePropertyNominalSampleRate);
    if (std::abs(active_sample_rate - requested_sample_rate) <= kSampleRateToleranceHz) {
        return active_sample_rate;
    }

    Float64 rate = requested_sample_rate;
    auto property = address(kAudioDevicePropertyNominalSampleRate);
    require_ok(
        AudioObjectSetPropertyData(device, &property, 0, nullptr, sizeof(rate), &rate),
        "AudioObjectSetPropertyData nominal sample rate");
    return wait_for_nominal_sample_rate(device, requested_sample_rate);
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

void configure_device(AudioObjectID device, double requested_sample_rate, long requested_buffer_size)
{
    if (requested_sample_rate > 0.0) {
        if (!sample_rate_supported(device, requested_sample_rate)) {
            throw std::runtime_error("requested CoreAudio sample rate is not supported");
        }
        (void)configure_nominal_sample_rate(device, requested_sample_rate);
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
    const auto streams = get_object_ids(device, kAudioDevicePropertyStreams, scope);
    if (streams.empty()) {
        return false;
    }
    return std::all_of(streams.begin(), streams.end(), [](AudioObjectID stream) {
        AudioStreamBasicDescription desc{};
        if (!try_get_property(
                stream,
                address(kAudioStreamPropertyVirtualFormat),
                desc)) {
            return false;
        }
        const bool non_interleaved =
            (desc.mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
        const UInt32 expected_bytes_per_frame = static_cast<UInt32>(sizeof(float)) *
            (non_interleaved ? 1U : desc.mChannelsPerFrame);
        return desc.mFormatID == kAudioFormatLinearPCM &&
            (desc.mFormatFlags & kAudioFormatFlagIsFloat) != 0 &&
            (desc.mFormatFlags & kAudioFormatFlagIsPacked) != 0 &&
            desc.mBitsPerChannel == 32 &&
            desc.mChannelsPerFrame > 0 &&
            desc.mBytesPerFrame == expected_bytes_per_frame &&
            desc.mFramesPerPacket == 1 &&
            desc.mBytesPerPacket == expected_bytes_per_frame;
    });
}

long maximum_stream_latency_frames(
    AudioObjectID device,
    AudioObjectPropertyScope scope)
{
    const auto streams = get_object_ids(device, kAudioDevicePropertyStreams, scope);
    UInt32 maximum = 0;
    for (AudioObjectID stream : streams) {
        maximum = std::max(
            maximum,
            get_u32_property_or_zero(stream, kAudioStreamPropertyLatency));
    }
    return static_cast<long>(maximum);
}

struct AtomicTimingAggregate {
    std::atomic<std::uint64_t> minimum{
        (std::numeric_limits<std::uint64_t>::max)()};
    std::atomic<std::uint64_t> sum{0};
    std::atomic<std::uint64_t> maximum{0};
    std::atomic<std::uint64_t> samples{0};
};

void observe_timing(AtomicTimingAggregate& state, std::uint64_t nanoseconds) noexcept
{
    const std::uint64_t minimum = state.minimum.load(std::memory_order_relaxed);
    if (nanoseconds < minimum) {
        state.minimum.store(nanoseconds, std::memory_order_relaxed);
    }
    const std::uint64_t maximum = state.maximum.load(std::memory_order_relaxed);
    if (nanoseconds > maximum) {
        state.maximum.store(nanoseconds, std::memory_order_relaxed);
    }
    state.sum.store(
        state.sum.load(std::memory_order_relaxed) + nanoseconds,
        std::memory_order_relaxed);
    state.samples.store(
        state.samples.load(std::memory_order_relaxed) + 1,
        std::memory_order_relaxed);
}

std::uint64_t host_time_delta_ns(
    std::uint64_t later,
    std::uint64_t earlier) noexcept
{
    return later >= earlier ? AudioConvertHostTimeToNanos(later - earlier) : 0;
}

bool has_host_time(const AudioTimeStamp* timestamp) noexcept
{
    return timestamp != nullptr &&
        (timestamp->mFlags & kAudioTimeStampHostTimeValid) != 0;
}

struct CoreAudioDuplexContext {
    InputChannels input_channels = InputChannels::Mono;
    ChannelSelection channels;
    MonoRingBuffer* capture = nullptr;
    MonoRingBuffer* pitch = nullptr;
    MonoRingBuffer* playback = nullptr;
    StreamControl* control = nullptr;
    OutputRecorder* recorder = nullptr;
    TrackTakeRecorder* track_take_recorder = nullptr;
    std::vector<std::int32_t> capture_scratch;
    std::vector<std::int32_t> physical_input_scratch;
    std::vector<const std::int32_t*> input_source_pointers;
    std::vector<coreaudio::ConstFloatChannelView> input_channel_views;
    std::vector<coreaudio::FloatChannelView> output_channel_views;
    std::vector<double> input_peak_scratch;
    SmoothedMonoDownmix input_downmix;
    std::vector<std::int32_t> playback_scratch;
    std::vector<std::int32_t> playback_resampler_scratch;
    std::vector<std::int32_t> recorder_my_input_scratch;
    std::vector<std::int32_t> recorder_their_input_scratch;
    std::vector<std::int32_t> recorder_inputs_mix_scratch;
    std::vector<std::int32_t> recorder_metronome_scratch;
    std::vector<std::int32_t> recorder_prepared_scratch;
    std::size_t playback_prefill_frames = 0;
    double sample_rate = 48000.0;
    std::uint64_t test_input_sample_counter = 0;
    std::uint64_t engine_frame_counter = 0;
    std::uint64_t metronome_beat_index = 0;
    processing::PlaybackResamplerState playback_resampler;
    std::atomic<long> callbacks{0};
    std::atomic<bool> playback_prefilled{false};
    processing::CallbackIntervalState callback_intervals;
    std::atomic<std::uint64_t> callback_frame_min{
        (std::numeric_limits<std::uint64_t>::max)()};
    std::atomic<std::uint64_t> callback_frame_max{0};
    std::atomic<std::uint64_t> callback_frame_samples{0};
    std::atomic<std::uint64_t> callback_frame_capacity_exceeded{0};
    std::atomic<std::uint64_t> processor_overloads{0};
    std::atomic<std::uint64_t> abnormal_stops{0};
    std::atomic<std::uint64_t> timestamp_callbacks{0};
    std::atomic<std::uint64_t> timestamp_invalid_callbacks{0};
    AtomicTimingAggregate cycle_to_callback;
    AtomicTimingAggregate input_to_cycle;
    AtomicTimingAggregate cycle_to_output;
    AtomicTimingAggregate cycle_jitter;
    std::uint64_t last_cycle_host_time = 0;
    std::size_t last_cycle_frames = 0;
    std::uint64_t callback_generation = 0;
    processing::MetronomeWaveBank metronome_wave_bank;
};

OSStatus device_property_listener(
    AudioObjectID,
    UInt32 address_count,
    const AudioObjectPropertyAddress addresses[],
    void* client_data)
{
    auto* context = static_cast<CoreAudioDuplexContext*>(client_data);
    if (context == nullptr || addresses == nullptr) {
        return noErr;
    }
    for (UInt32 index = 0; index < address_count; ++index) {
        if (addresses[index].mSelector == kAudioDeviceProcessorOverload) {
            context->processor_overloads.fetch_add(1, std::memory_order_relaxed);
        } else if (addresses[index].mSelector == kAudioDevicePropertyIOStoppedAbnormally) {
            context->abnormal_stops.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return noErr;
}

OSStatus duplex_io_proc(
    AudioObjectID,
    const AudioTimeStamp* now,
    const AudioBufferList* input,
    const AudioTimeStamp* input_time,
    AudioBufferList* output,
    const AudioTimeStamp* output_time,
    void* client_data)
{
    auto* context = static_cast<CoreAudioDuplexContext*>(client_data);
    if (context == nullptr || context->capture == nullptr || context->playback == nullptr) {
        return noErr;
    }
    const std::uint64_t callback_start_us = processing::callback_now_us();
    const std::size_t input_buffer_frames = coreaudio::buffer_frames(input);
    const std::size_t output_buffer_frames = coreaudio::buffer_frames(output);
    const std::size_t callback_frames = std::max(input_buffer_frames, output_buffer_frames);
    if (has_host_time(now)) {
        const std::uint64_t timestamp_callbacks =
            context->timestamp_callbacks.load(std::memory_order_relaxed) + 1;
        context->timestamp_callbacks.store(
            timestamp_callbacks,
            std::memory_order_relaxed);
        const bool sample_timing =
            (timestamp_callbacks & kTimestampTimingSampleMask) == 1;
        if (sample_timing) {
            const std::uint64_t callback_host_time = AudioGetCurrentHostTime();
            observe_timing(
                context->cycle_to_callback,
                host_time_delta_ns(callback_host_time, now->mHostTime));
            if (has_host_time(input_time)) {
                observe_timing(
                    context->input_to_cycle,
                    host_time_delta_ns(now->mHostTime, input_time->mHostTime));
            }
            if (has_host_time(output_time)) {
                observe_timing(
                    context->cycle_to_output,
                    host_time_delta_ns(output_time->mHostTime, now->mHostTime));
            }
        }
        if (sample_timing &&
            context->last_cycle_host_time != 0 &&
            context->last_cycle_frames > 0 &&
            context->sample_rate > 0.0 &&
            now->mHostTime >= context->last_cycle_host_time) {
            const std::uint64_t actual_ns = host_time_delta_ns(
                now->mHostTime, context->last_cycle_host_time);
            const std::uint64_t expected_ns = static_cast<std::uint64_t>(std::llround(
                static_cast<double>(context->last_cycle_frames) * 1000000000.0 /
                context->sample_rate));
            observe_timing(
                context->cycle_jitter,
                actual_ns >= expected_ns
                    ? actual_ns - expected_ns
                    : expected_ns - actual_ns);
        }
        context->last_cycle_host_time = now->mHostTime;
        context->last_cycle_frames = callback_frames;
    } else {
        context->timestamp_invalid_callbacks.store(
            context->timestamp_invalid_callbacks.load(std::memory_order_relaxed) + 1,
            std::memory_order_relaxed);
    }
    if (callback_frames > 0) {
        const std::uint64_t minimum =
            context->callback_frame_min.load(std::memory_order_relaxed);
        if (callback_frames < minimum) {
            context->callback_frame_min.store(
                static_cast<std::uint64_t>(callback_frames),
                std::memory_order_relaxed);
        }
        const std::uint64_t maximum =
            context->callback_frame_max.load(std::memory_order_relaxed);
        if (callback_frames > maximum) {
            context->callback_frame_max.store(
                static_cast<std::uint64_t>(callback_frames),
                std::memory_order_relaxed);
        }
        context->callback_frame_samples.store(
            context->callback_frame_samples.load(std::memory_order_relaxed) + 1,
            std::memory_order_relaxed);
        if (callback_frames > context->capture_scratch.size() ||
            callback_frames > context->playback_scratch.size()) {
            context->callback_frame_capacity_exceeded.store(
                context->callback_frame_capacity_exceeded.load(
                    std::memory_order_relaxed) + 1,
                std::memory_order_relaxed);
        }
    }
    processing::publish_callback_begin(
        context->control,
        context->callback_generation);
    processing::observe_callback_interval(
        context->callback_intervals,
        callback_start_us,
        callback_frames,
        context->sample_rate);
    const bool network_capture_enabled = context->control != nullptr &&
        prepare_network_capture_callback(
            *context->control,
            *context->capture,
            context->engine_frame_counter);
    const bool track_take_armed = context->track_take_recorder != nullptr &&
        context->track_take_recorder->armed();
    const bool jam_recording_active = context->recorder != nullptr &&
        context->recorder->active();
    const std::int32_t track_take_options = track_take_armed && context->control != nullptr
        ? context->control->track_take_options.load(std::memory_order_relaxed)
        : 0;
    const auto track_take_source = static_cast<TrackTakeSource>(
        track_take_options & 0xff);
    const bool current_jam_take = track_take_armed &&
        track_take_source == TrackTakeSource::CurrentJam;
    const bool prepared_stem_required = current_jam_take &&
        (track_take_options & kTrackTakeIncludePrepared) != 0;
    const bool metronome_stem_required = jam_recording_active ||
        (current_jam_take &&
         (track_take_options & kTrackTakeIncludeMetronome) != 0);
    const bool recording_scratch_required = track_take_armed || jam_recording_active;
    const std::size_t output_frames_for_input = std::min(
        output_buffer_frames, context->recorder_my_input_scratch.size());
    if (recording_scratch_required && output_frames_for_input > 0) {
        std::fill(
            context->recorder_my_input_scratch.begin(),
            context->recorder_my_input_scratch.begin() + output_frames_for_input,
            0);
    }

    const int test_input_mode = context->control != nullptr ?
        context->control->test_input_mode.load(std::memory_order_relaxed) :
        0;
    const std::size_t input_frames = std::min(input_buffer_frames, context->capture_scratch.size());
    if (test_input_mode != 0 && !context->capture_scratch.empty()) {
        const std::size_t generated_frames = std::min(
            output_buffer_frames, context->capture_scratch.size());
        auto generated = std::span<std::int32_t>(context->capture_scratch.data(), generated_frames);
        processing::fill_test_input(
            context->control,
            context->sample_rate,
            context->test_input_sample_counter,
            generated);
        auto* source_router = context->control != nullptr ?
            context->control->input_source_router : nullptr;
        if (source_router != nullptr) {
            std::fill(
                context->input_source_pointers.begin(),
                context->input_source_pointers.end(),
                generated.data());
            const bool routed = source_router->process(
                std::span<const std::int32_t* const>(
                    context->input_source_pointers.data(),
                    context->input_source_pointers.size()),
                generated.size(),
                context->engine_frame_counter,
                context->sample_rate,
                generated);
            if (!routed) std::fill(generated.begin(), generated.end(), 0);
        }
        if (context->control != nullptr) {
            if (source_router != nullptr) {
                processing::observe_input_peak_value(
                    context->control, source_router->last_peak_ppm());
            } else {
                processing::observe_input_peaks(context->control, generated);
            }
        }
        if (network_capture_enabled) {
            push_network_capture_callback(
                *context->control, *context->capture, generated, callback_start_us);
        }
        if (context->control != nullptr && context->pitch != nullptr) {
            push_pitch_analysis_callback(*context->control, *context->pitch, generated);
        }
        if (recording_scratch_required &&
            context->recorder_my_input_scratch.size() >= generated_frames) {
            std::copy(generated.begin(), generated.end(), context->recorder_my_input_scratch.begin());
        }
    } else if (input_frames > 0) {
        for (std::size_t selected = 0; selected < context->channels.input.size(); ++selected) {
            context->input_channel_views[selected] = coreaudio::input_channel(
                input, static_cast<UInt32>(context->channels.input[selected]));
        }
        auto* source_router = context->control != nullptr ?
            context->control->input_source_router : nullptr;
        if (source_router != nullptr) {
            for (std::size_t selected = 0; selected < context->channels.input.size(); ++selected) {
                auto* destination = context->physical_input_scratch.data() + selected * context->capture_scratch.size();
                context->input_source_pointers[selected] = destination;
                coreaudio::copy_input_channel(
                    context->input_channel_views[selected],
                    std::span<std::int32_t>(destination, input_frames));
            }
            const bool routed = source_router->process(
                std::span<const std::int32_t* const>(context->input_source_pointers.data(),
                    context->input_source_pointers.size()),
                input_frames,
                context->engine_frame_counter,
                context->sample_rate,
                std::span<std::int32_t>(context->capture_scratch.data(), input_frames));
            if (!routed) std::fill_n(context->capture_scratch.begin(), input_frames, 0);
        } else {
        std::fill(
            context->input_peak_scratch.begin(),
            context->input_peak_scratch.end(),
            0.0);
        for (std::size_t selected = 0;
             selected < context->channels.input.size();
             ++selected) {
            double peak = 0.0;
            const coreaudio::ConstFloatChannelView source =
                context->input_channel_views[selected];
            for (std::size_t frame = 0; frame < input_frames; ++frame) {
                const float sample = source.available() && frame < source.frames
                    ? source.samples[frame * source.stride]
                    : 0.0F;
                peak = std::max(
                    peak,
                    std::abs(static_cast<double>(sample)));
            }
            context->input_peak_scratch[selected] = peak;
        }
        context->input_downmix.beginBlock(
            std::span<const double>(
                context->input_peak_scratch.data(),
                context->input_peak_scratch.size()));
        if (context->control != nullptr) {
            publish_downmix_diagnostics(
                *context->control, context->input_downmix);
        }
        for (std::size_t frame = 0; frame < input_frames; ++frame) {
            double sum = 0.0;
            for (std::size_t selected = 0;
                 selected < context->channels.input.size();
                 ++selected) {
                const coreaudio::ConstFloatChannelView source =
                    context->input_channel_views[selected];
                const float sample = source.available() && frame < source.frames
                    ? source.samples[frame * source.stride]
                    : 0.0F;
                sum += static_cast<double>(sample) *
                    context->input_downmix.weightAt(selected, frame);
            }
            const double mixed = sum * context->input_downmix.normalizationAt(frame);
            context->capture_scratch[frame] = coreaudio::float_to_i32(
                static_cast<float>(std::clamp(mixed, -1.0, 1.0)));
        }
        }
        if (network_capture_enabled) {
            push_network_capture_callback(
                *context->control,
                *context->capture,
                std::span<const std::int32_t>(context->capture_scratch.data(), input_frames),
                callback_start_us);
        }
        if (context->control != nullptr && context->pitch != nullptr) {
            push_pitch_analysis_callback(
                *context->control,
                *context->pitch,
                std::span<const std::int32_t>(context->capture_scratch.data(), input_frames));
        }
        if (context->control != nullptr) {
            if (source_router != nullptr) {
                processing::observe_input_peak_value(
                    context->control, source_router->last_peak_ppm());
            } else {
                processing::observe_input_peaks(
                    context->control,
                    std::span<const std::int32_t>(
                        context->capture_scratch.data(), input_frames));
            }
        }
        if (recording_scratch_required &&
            context->recorder_my_input_scratch.size() >= input_frames) {
            const bool selected = source_router != nullptr && source_router->copy_recording_source(
                input_frames,
                std::span<std::int32_t>(context->recorder_my_input_scratch.data(), input_frames));
            if (!selected) {
                std::copy(
                    context->capture_scratch.begin(),
                    context->capture_scratch.begin() + input_frames,
                    context->recorder_my_input_scratch.begin());
            }
        }
    }

    const std::size_t output_frames = std::min(
        output_buffer_frames, context->playback_scratch.size());
    if (output_frames > 0) {
        auto playback = std::span<std::int32_t>(context->playback_scratch.data(), output_frames);
        const bool network_playback_enabled = context->control != nullptr &&
            context->control->network_playback_enabled.load(std::memory_order_acquire);
        if (!network_playback_enabled) {
            context->playback_prefilled.store(false, std::memory_order_relaxed);
            context->playback->pop(std::span<std::int32_t>{}, false);
            context->playback_resampler.reset();
            context->control->playback_ratio_applied_ppm.store(1000000, std::memory_order_relaxed);
            context->control->playback_ratio_ramping.store(false, std::memory_order_relaxed);
            std::fill(playback.begin(), playback.end(), 0);
        } else if (!context->playback_prefilled.load(std::memory_order_relaxed)) {
            if (context->playback->available_read() >= context->playback_prefill_frames) {
                context->playback_prefilled.store(true, std::memory_order_relaxed);
            } else {
                std::fill(playback.begin(), playback.end(), 0);
            }
        }
        if (network_playback_enabled && context->playback_prefilled.load(std::memory_order_relaxed)) {
            processing::pop_resampled_playback(
                context->playback,
                context->control,
                context->playback_resampler,
                context->playback_resampler_scratch,
                playback);
            processing::apply_remote_level(context->control, playback);
        }
        if (context->control != nullptr) {
            context->control->network_playback_enabled_applied.store(
                network_playback_enabled,
                std::memory_order_release);
            processing::observe_shared_peak(
                context->control->remote_peak_ppm,
                context->control->gui_remote_peak_ppm,
                playback);
        }
        if (recording_scratch_required &&
            context->recorder_their_input_scratch.size() >= output_frames) {
            std::copy(playback.begin(), playback.end(), context->recorder_their_input_scratch.begin());
        }
        processing::mix_local_monitor(
            context->control,
            playback,
            std::span<const std::int32_t>(
                context->capture_scratch.data(),
                std::min<std::size_t>(context->capture_scratch.size(), playback.size())));
        const std::uint64_t audio_frame_start = context->engine_frame_counter;
        processing::mix_prepared_source(
            context->control,
            playback,
            audio_frame_start,
            prepared_stem_required
                ? std::span<std::int32_t>(
                    context->recorder_prepared_scratch.data(),
                    std::min<std::size_t>(
                        context->recorder_prepared_scratch.size(), playback.size()))
                : std::span<std::int32_t>{});
        processing::mix_metronome_click(
            context->control,
            context->sample_rate,
            context->engine_frame_counter,
            context->metronome_beat_index,
            playback,
            metronome_stem_required
                ? std::span<std::int32_t>(
                    context->recorder_metronome_scratch.data(),
                    std::min<std::size_t>(
                        context->recorder_metronome_scratch.size(), playback.size()))
                : std::span<std::int32_t>{},
            &context->metronome_wave_bank);
        processing::apply_output_level(context->control, playback);
        if (context->control != nullptr) {
            processing::observe_output_peak(context->control, playback);
        }
        if (track_take_armed &&
            context->recorder_my_input_scratch.size() >= playback.size() &&
            context->recorder_their_input_scratch.size() >= playback.size() &&
            context->recorder_inputs_mix_scratch.size() >= playback.size()) {
            if (track_take_source == TrackTakeSource::CurrentJam) {
                for (std::size_t index = 0; index < playback.size(); ++index) {
                    std::int32_t sample = processing::mix_i32_samples(
                        context->recorder_my_input_scratch[index],
                        context->recorder_their_input_scratch[index]);
                    if ((track_take_options & kTrackTakeIncludePrepared) != 0 &&
                        context->recorder_prepared_scratch.size() >= playback.size()) {
                        sample = processing::mix_i32_samples(
                            sample, context->recorder_prepared_scratch[index]);
                    }
                    if ((track_take_options & kTrackTakeIncludeMetronome) != 0 &&
                        context->recorder_metronome_scratch.size() >= playback.size()) {
                        sample = processing::mix_i32_samples(
                            sample, context->recorder_metronome_scratch[index]);
                    }
                    context->recorder_inputs_mix_scratch[index] = sample;
                }
                context->track_take_recorder->record(
                    audio_frame_start,
                    std::span<const std::int32_t>(
                        context->recorder_inputs_mix_scratch.data(), playback.size()));
            } else {
                std::uint64_t compensation = context->control != nullptr
                    ? context->control->recording_latency_compensation_frames.load(std::memory_order_relaxed)
                    : 0ULL;
                if (context->control != nullptr &&
                    context->control->input_source_router != nullptr) {
                    compensation += static_cast<std::uint64_t>(
                        context->control->input_source_router->recording_latency_frames());
                }
                const std::uint64_t capture_frame_start = audio_frame_start > compensation
                    ? audio_frame_start - compensation
                    : 0ULL;
                context->track_take_recorder->record(
                    capture_frame_start,
                    std::span<const std::int32_t>(
                        context->recorder_my_input_scratch.data(), playback.size()));
            }
        }
        if (jam_recording_active &&
            context->recorder_inputs_mix_scratch.size() >= playback.size() &&
            context->recorder_my_input_scratch.size() >= playback.size() &&
            context->recorder_their_input_scratch.size() >= playback.size() &&
            context->recorder_metronome_scratch.size() >= playback.size()) {
            for (std::size_t frame = 0; frame < playback.size(); ++frame) {
                context->recorder_inputs_mix_scratch[frame] = processing::mix_i32_samples(
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
        for (std::size_t selected = 0; selected < context->channels.output.size(); ++selected) {
            context->output_channel_views[selected] = coreaudio::output_channel(
                output, static_cast<UInt32>(context->channels.output[selected]));
        }
        coreaudio::write_mono_output(playback, context->output_channel_views);
    }

    context->engine_frame_counter += static_cast<std::uint64_t>(callback_frames);
    if (context->control != nullptr) {
        context->control->engine_frame_counter.store(context->engine_frame_counter, std::memory_order_release);
    }
    processing::publish_callback_end(
        context->control,
        context->callback_generation);
    context->callbacks.store(
        context->callbacks.load(std::memory_order_relaxed) + 1,
        std::memory_order_relaxed);
    processing::observe_callback_work(
        context->callback_intervals,
        callback_start_us,
        processing::callback_now_us());
    return noErr;
}

class CoreAudioDeviceStream final : public DeviceStream {
public:
    CoreAudioDeviceStream(
        AudioObjectID device,
        DeviceInfo info,
        long buffer_size,
        long maximum_callback_frames,
        bool variable_callback_frames,
        InputChannels input_channels,
        ChannelSelection channels,
        MonoRingBuffer& capture_ring,
        MonoRingBuffer& pitch_ring,
        MonoRingBuffer& playback_ring,
        std::size_t playback_prefill_frames,
        StreamControl& control,
        OutputRecorder* recorder,
        TrackTakeRecorder* track_take_recorder,
        double sample_rate)
        : device_(device),
          info_(std::move(info)),
          buffer_size_(buffer_size),
          maximum_callback_frames_(maximum_callback_frames),
          variable_callback_frames_(variable_callback_frames),
          input_channels_(input_channels),
          channels_(channels),
          sample_rate_(sample_rate)
    {
        context_.input_channels = input_channels;
        context_.channels = channels;
        context_.capture = &capture_ring;
        context_.pitch = &pitch_ring;
        context_.playback = &playback_ring;
        context_.control = &control;
        context_.recorder = recorder;
        context_.track_take_recorder = track_take_recorder;
        const std::size_t scratch_frames = static_cast<std::size_t>(maximum_callback_frames);
        context_.capture_scratch.resize(scratch_frames);
        context_.physical_input_scratch.resize(
            channels.input.size() * scratch_frames);
        context_.input_source_pointers.resize(channels.input.size());
        context_.input_channel_views.resize(channels.input.size());
        context_.output_channel_views.resize(channels.output.size());
        context_.input_peak_scratch.resize(channels.input.size());
        context_.input_downmix.configure(
            channels.input.size(), sample_rate, scratch_frames);
        control.input_downmix_selected_channels.store(
            static_cast<int>(channels.input.size()), std::memory_order_relaxed);
        context_.playback_scratch.resize(scratch_frames);
        context_.playback_resampler_scratch.resize(
            scratch_frames * 2U + 2U);
        context_.recorder_my_input_scratch.resize(scratch_frames);
        context_.recorder_their_input_scratch.resize(scratch_frames);
        context_.recorder_inputs_mix_scratch.resize(scratch_frames);
        context_.recorder_metronome_scratch.resize(scratch_frames);
        context_.recorder_prepared_scratch.resize(scratch_frames);
        context_.playback_prefill_frames = playback_prefill_frames;
        context_.sample_rate = sample_rate;
        context_.metronome_wave_bank.prepare(sample_rate);
        input_device_latency_frames_ = static_cast<long>(get_u32_property_or_zero(
            device_, kAudioDevicePropertyLatency, kAudioDevicePropertyScopeInput));
        input_safety_offset_frames_ = static_cast<long>(get_u32_property_or_zero(
            device_, kAudioDevicePropertySafetyOffset, kAudioDevicePropertyScopeInput));
        input_stream_latency_frames_ = maximum_stream_latency_frames(
            device_, kAudioDevicePropertyScopeInput);
        output_device_latency_frames_ = channels_.output.empty() ? 0L : static_cast<long>(
            get_u32_property_or_zero(
                device_, kAudioDevicePropertyLatency, kAudioDevicePropertyScopeOutput));
        output_safety_offset_frames_ = channels_.output.empty() ? 0L : static_cast<long>(
            get_u32_property_or_zero(
                device_, kAudioDevicePropertySafetyOffset, kAudioDevicePropertyScopeOutput));
        output_stream_latency_frames_ = channels_.output.empty() ? 0L :
            maximum_stream_latency_frames(device_, kAudioDevicePropertyScopeOutput);
        input_latency_frames_ = input_device_latency_frames_ + input_safety_offset_frames_;
        output_latency_frames_ = output_device_latency_frames_ + output_safety_offset_frames_;
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
        register_property_listener(
            kAudioDeviceProcessorOverload,
            processor_overload_listener_active_,
            processor_overload_listener_error_);
        register_property_listener(
            kAudioDevicePropertyIOStoppedAbnormally,
            abnormal_stop_listener_active_,
            abnormal_stop_listener_error_);
    }

    ~CoreAudioDeviceStream() override
    {
        if (proc_id_ != nullptr) {
            if (started_) {
                (void)AudioDeviceStop(device_, proc_id_);
            }
            remove_property_listener(
                kAudioDeviceProcessorOverload,
                processor_overload_listener_active_);
            remove_property_listener(
                kAudioDevicePropertyIOStoppedAbnormally,
                abnormal_stop_listener_active_);
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
        result.maximum_callback_frames = maximum_callback_frames_;
        result.variable_callback_frames = variable_callback_frames_;
        result.input_latency_frames = input_latency_frames_;
        result.output_latency_frames = output_latency_frames_;
        result.input_device_latency_frames = input_device_latency_frames_;
        result.input_safety_offset_frames = input_safety_offset_frames_;
        result.input_stream_latency_frames = input_stream_latency_frames_;
        result.output_device_latency_frames = output_device_latency_frames_;
        result.output_safety_offset_frames = output_safety_offset_frames_;
        result.output_stream_latency_frames = output_stream_latency_frames_;
        result.input_stream_count = get_object_ids(
            device_, kAudioDevicePropertyStreams, kAudioDevicePropertyScopeInput).size();
        result.output_stream_count = get_object_ids(
            device_, kAudioDevicePropertyStreams, kAudioDevicePropertyScopeOutput).size();
        result.processor_overload_listener_active =
            processor_overload_listener_active_;
        result.processor_overload_listener_error =
            processor_overload_listener_error_;
        result.abnormal_stop_listener_active = abnormal_stop_listener_active_;
        result.abnormal_stop_listener_error = abnormal_stop_listener_error_;
        result.input_channels = input_channels_;
        result.channels = channels_;
        result.sample_format = "CoreAudio Float32 packed";
        return result;
    }

    CallbackTimingStats callback_timing_stats() const override
    {
        CallbackTimingStats result{
            context_.callback_intervals.minimumUs.load(std::memory_order_relaxed),
            context_.callback_intervals.sumUs.load(std::memory_order_relaxed),
            context_.callback_intervals.maximumUs.load(std::memory_order_relaxed),
            context_.callback_intervals.samples.load(std::memory_order_relaxed),
            context_.callback_intervals.gapsOver1_1x.load(std::memory_order_relaxed),
            context_.callback_intervals.gapsOver1_5x.load(std::memory_order_relaxed),
            context_.callback_intervals.gapsOver2x.load(std::memory_order_relaxed),
            context_.callback_intervals.workMinimumUs.load(std::memory_order_relaxed),
            context_.callback_intervals.workSumUs.load(std::memory_order_relaxed),
            context_.callback_intervals.workMaximumUs.load(std::memory_order_relaxed),
            context_.callback_intervals.workSamples.load(std::memory_order_relaxed),
        };
        const std::uint64_t frame_samples =
            context_.callback_frame_samples.load(std::memory_order_relaxed);
        result.frame_min = frame_samples > 0
            ? context_.callback_frame_min.load(std::memory_order_relaxed)
            : 0;
        result.frame_max = context_.callback_frame_max.load(std::memory_order_relaxed);
        result.frame_samples = frame_samples;
        result.frame_capacity_exceeded =
            context_.callback_frame_capacity_exceeded.load(std::memory_order_relaxed);
        result.processor_overloads =
            context_.processor_overloads.load(std::memory_order_relaxed);
        result.abnormal_stops =
            context_.abnormal_stops.load(std::memory_order_relaxed);
        result.timestamp_callbacks =
            context_.timestamp_callbacks.load(std::memory_order_relaxed);
        result.timestamp_invalid_callbacks =
            context_.timestamp_invalid_callbacks.load(std::memory_order_relaxed);
        copy_timing(context_.cycle_to_callback,
            result.cycle_to_callback_min_ns,
            result.cycle_to_callback_sum_ns,
            result.cycle_to_callback_max_ns,
            result.cycle_to_callback_samples);
        copy_timing(context_.input_to_cycle,
            result.input_to_cycle_min_ns,
            result.input_to_cycle_sum_ns,
            result.input_to_cycle_max_ns,
            result.input_to_cycle_samples);
        copy_timing(context_.cycle_to_output,
            result.cycle_to_output_min_ns,
            result.cycle_to_output_sum_ns,
            result.cycle_to_output_max_ns,
            result.cycle_to_output_samples);
        copy_timing(context_.cycle_jitter,
            result.cycle_jitter_min_ns,
            result.cycle_jitter_sum_ns,
            result.cycle_jitter_max_ns,
            result.cycle_jitter_samples);
        return result;
    }

private:
    static void copy_timing(
        const AtomicTimingAggregate& source,
        std::uint64_t& minimum,
        std::uint64_t& sum,
        std::uint64_t& maximum,
        std::uint64_t& samples)
    {
        samples = source.samples.load(std::memory_order_relaxed);
        minimum = samples > 0
            ? source.minimum.load(std::memory_order_relaxed)
            : 0;
        sum = source.sum.load(std::memory_order_relaxed);
        maximum = source.maximum.load(std::memory_order_relaxed);
    }

    void register_property_listener(
        AudioObjectPropertySelector selector,
        bool& active,
        long& error)
    {
        const auto property = address(selector);
        if (!has_property(device_, property)) {
            active = false;
            error = 0;
            return;
        }
        const OSStatus status = AudioObjectAddPropertyListener(
            device_, &property, device_property_listener, &context_);
        active = status == noErr;
        error = static_cast<long>(status);
    }

    void remove_property_listener(
        AudioObjectPropertySelector selector,
        bool active) noexcept
    {
        if (!active) {
            return;
        }
        const auto property = address(selector);
        (void)AudioObjectRemovePropertyListener(
            device_, &property, device_property_listener, &context_);
    }

    AudioObjectID device_ = kAudioObjectUnknown;
    DeviceInfo info_;
    AudioDeviceIOProcID proc_id_ = nullptr;
    bool started_ = false;
    long buffer_size_ = 0;
    long maximum_callback_frames_ = 0;
    bool variable_callback_frames_ = false;
    bool processor_overload_listener_active_ = false;
    long processor_overload_listener_error_ = 0;
    bool abnormal_stop_listener_active_ = false;
    long abnormal_stop_listener_error_ = 0;
    long input_latency_frames_ = 0;
    long output_latency_frames_ = 0;
    long input_device_latency_frames_ = 0;
    long input_safety_offset_frames_ = 0;
    long input_stream_latency_frames_ = 0;
    long output_device_latency_frames_ = 0;
    long output_safety_offset_frames_ = 0;
    long output_stream_latency_frames_ = 0;
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

DeviceTestResult test_device(int id)
{
    const auto selected = select_device(id);
    DeviceTestResult capabilities;
    capabilities.device = selected.info;
    const AudioValueRange buffer_range = buffer_frame_range(selected.object);
    capabilities.current_sample_rate =
        get_double_property_or_zero(selected.object, kAudioDevicePropertyNominalSampleRate);
    for (std::size_t index = 0; index < kTestSampleRates.size(); ++index) {
        capabilities.sample_rate_supported[index] =
            sample_rate_supported(selected.object, kTestSampleRates[index]);
    }
    for (std::size_t index = 0; index < kTestBufferSizes.size(); ++index) {
        const double size = static_cast<double>(kTestBufferSizes[index]);
        capabilities.buffer_size_supported[index] =
            buffer_range.mMinimum > 0.0 && size >= buffer_range.mMinimum && size <= buffer_range.mMaximum;
    }
    return capabilities;
}

std::unique_ptr<DeviceStream> start_duplex_stream(
    int id,
    double requested_sample_rate,
    long requested_buffer_size,
    InputChannels requested_input_channels,
    ChannelSelection channels,
    MonoRingBuffer& capture_ring,
    MonoRingBuffer& pitch_ring,
    MonoRingBuffer& playback_ring,
    std::size_t playback_prefill_frames,
    StreamControl& control,
    OutputRecorder* recorder,
    TrackTakeRecorder* track_take_recorder)
{
    auto selected = select_device(id);
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
    selected.info = make_device_info(id, selected.object);
    const long buffer_size =
        static_cast<long>(get_u32_property_or_zero(selected.object, kAudioDevicePropertyBufferFrameSize));
    if (buffer_size <= 0) {
        throw std::runtime_error("CoreAudio reported an invalid buffer frame size");
    }
    if (playback_prefill_frames > playback_ring.capacity()) {
        throw std::runtime_error("playback prefill must fit within playback ring capacity");
    }
    const long variable_buffer_max = static_cast<long>(get_u32_property_or_zero(
        selected.object, kAudioDevicePropertyUsesVariableBufferFrameSizes));
    const bool variable_callback_frames = variable_buffer_max > buffer_size;
    const long maximum_callback_frames = std::max(buffer_size, variable_buffer_max);

    const double sample_rate = get_double_property_or_zero(selected.object, kAudioDevicePropertyNominalSampleRate);
    auto stream = std::make_unique<CoreAudioDeviceStream>(
        selected.object,
        selected.info,
        buffer_size,
        maximum_callback_frames,
        variable_callback_frames,
        requested_input_channels,
        channels,
        capture_ring,
        pitch_ring,
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

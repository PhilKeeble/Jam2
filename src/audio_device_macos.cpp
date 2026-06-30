#include "audio_device.hpp"

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef kAudioObjectPropertyElementMain
#define kAudioObjectPropertyElementMain kAudioObjectPropertyElementMaster
#endif

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
    const auto meter = run_exploratory_io(id, requested_sample_rate, buffer_size, duration_ms, ring_capacity_frames);
    DeviceRingResult result;
    result.device = meter.device;
    result.sample_rate = meter.sample_rate;
    result.buffer_size = meter.buffer_size;
    result.callbacks = meter.callbacks;
    result.ring_overruns = 0;
    result.ring_underruns = 0;
    result.ring_readable = 0;
    return result;
}

std::unique_ptr<DeviceStream> start_duplex_stream(
    int,
    double,
    long,
    InputChannels,
    MonoRingBuffer&,
    MonoRingBuffer&,
    std::size_t,
    StreamControl&)
{
    throw std::runtime_error("CoreAudio duplex streaming is not implemented in this exploratory pass; use --list-devices, probe-device, meter-device, or ring-device on macOS");
}

} // namespace jam2::audio

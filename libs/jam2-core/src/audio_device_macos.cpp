#include "audio_device.hpp"
#include "audio_device_processing.hpp"

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
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

namespace processing = device_processing;

constexpr AudioObjectPropertyElement kMainElement = kAudioObjectPropertyElementMain;
constexpr double kSampleRateToleranceHz = 1.0;
constexpr auto kDeviceConfigurationPollInterval = std::chrono::milliseconds(10);
constexpr auto kDeviceConfigurationTimeout = std::chrono::milliseconds(3000);

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
    MonoRingBuffer* pitch = nullptr;
    MonoRingBuffer* playback = nullptr;
    StreamControl* control = nullptr;
    OutputRecorder* recorder = nullptr;
    TrackTakeRecorder* track_take_recorder = nullptr;
    std::vector<std::int32_t> capture_scratch;
    std::vector<std::int32_t> physical_input_scratch;
    std::vector<const std::int32_t*> input_source_pointers;
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
    std::uint64_t callback_generation = 0;
    processing::MetronomeWaveBank metronome_wave_bank;
};

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
    const std::uint64_t callback_start_us = processing::callback_now_us();
    processing::publish_callback_begin(
        context->control,
        context->callback_generation);
    processing::observe_callback_interval(
        context->callback_intervals,
        callback_start_us,
        context->playback_scratch.size(),
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
    const std::size_t output_frames_for_input = std::min(buffer_frames(output), context->recorder_my_input_scratch.size());
    if (recording_scratch_required && output_frames_for_input > 0) {
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
        auto* source_router = context->control != nullptr ?
            context->control->input_source_router : nullptr;
        if (source_router != nullptr) {
            for (std::size_t selected = 0; selected < context->channels.input.size(); ++selected) {
                auto* destination = context->physical_input_scratch.data() + selected * context->capture_scratch.size();
                context->input_source_pointers[selected] = destination;
                const int channel = context->channels.input[selected];
                for (std::size_t frame = 0; frame < input_frames; ++frame) {
                    destination[frame] = float_to_i32(read_float_channel(
                        input, frame, static_cast<UInt32>(channel)));
                }
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
            const int channel = context->channels.input[selected];
            for (std::size_t frame = 0; frame < input_frames; ++frame) {
                peak = std::max(
                    peak,
                    std::abs(static_cast<double>(
                        read_float_channel(
                            input,
                            frame,
                            static_cast<UInt32>(channel)))));
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
                sum += static_cast<double>(read_float_channel(
                           input,
                           frame,
                           static_cast<UInt32>(context->channels.input[selected]))) *
                    context->input_downmix.weightAt(selected, frame);
            }
            const double mixed = sum * context->input_downmix.normalizationAt(frame);
            context->capture_scratch[frame] = float_to_i32(
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

    const std::size_t output_frames = std::min(buffer_frames(output), context->playback_scratch.size());
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
    processing::publish_callback_end(
        context->control,
        context->callback_generation);
    context->callbacks.fetch_add(1, std::memory_order_relaxed);
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
        context_.capture_scratch.resize(static_cast<std::size_t>(buffer_size));
        context_.physical_input_scratch.resize(
            channels.input.size() * static_cast<std::size_t>(buffer_size));
        context_.input_source_pointers.resize(channels.input.size());
        context_.input_peak_scratch.resize(channels.input.size());
        context_.input_downmix.configure(
            channels.input.size(), sample_rate, static_cast<std::size_t>(buffer_size));
        control.input_downmix_selected_channels.store(
            static_cast<int>(channels.input.size()), std::memory_order_relaxed);
        context_.playback_scratch.resize(static_cast<std::size_t>(buffer_size));
        context_.playback_resampler_scratch.resize(
            static_cast<std::size_t>(buffer_size) * 2U + 2U);
        context_.recorder_my_input_scratch.resize(static_cast<std::size_t>(buffer_size));
        context_.recorder_their_input_scratch.resize(static_cast<std::size_t>(buffer_size));
        context_.recorder_inputs_mix_scratch.resize(static_cast<std::size_t>(buffer_size));
        context_.recorder_metronome_scratch.resize(static_cast<std::size_t>(buffer_size));
        context_.recorder_prepared_scratch.resize(static_cast<std::size_t>(buffer_size));
        context_.playback_prefill_frames = playback_prefill_frames;
        context_.sample_rate = sample_rate;
        context_.metronome_wave_bank.prepare(sample_rate);
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

    const double sample_rate = get_double_property_or_zero(selected.object, kAudioDevicePropertyNominalSampleRate);
    auto stream = std::make_unique<CoreAudioDeviceStream>(
        selected.object,
        selected.info,
        buffer_size,
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

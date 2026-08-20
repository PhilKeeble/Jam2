#include "audio_device.hpp"
#include "audio_device_processing.hpp"
#include "audio_ring.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>

#include "iasiodrv.h"

namespace jam2::audio {
namespace {

namespace processing = device_processing;

class RegistryKey {
public:
    RegistryKey() = default;
    ~RegistryKey()
    {
        if (key_ != nullptr) {
            RegCloseKey(key_);
        }
    }

    RegistryKey(const RegistryKey&) = delete;
    RegistryKey& operator=(const RegistryKey&) = delete;

    HKEY* out()
    {
        if (key_ != nullptr) {
            RegCloseKey(key_);
            key_ = nullptr;
        }
        return &key_;
    }

    HKEY get() const { return key_; }
    explicit operator bool() const { return key_ != nullptr; }

private:
    HKEY key_ = nullptr;
};

std::string hresult_text(HRESULT hr)
{
    std::ostringstream out;
    out << "0x" << std::hex << std::setw(8) << std::setfill('0') << static_cast<unsigned long>(hr);
    return out.str();
}

class ComRuntime {
public:
    ComRuntime()
    {
        const HRESULT hr = CoInitialize(nullptr);
        if (FAILED(hr)) {
            throw std::runtime_error("CoInitialize failed: " + hresult_text(hr));
        }
        initialized_ = true;
    }

    ~ComRuntime()
    {
        if (initialized_) {
            CoUninitialize();
        }
    }

    ComRuntime(const ComRuntime&) = delete;
    ComRuntime& operator=(const ComRuntime&) = delete;
    ComRuntime(ComRuntime&& other) noexcept : initialized_(other.initialized_)
    {
        other.initialized_ = false;
    }
    ComRuntime& operator=(ComRuntime&& other) noexcept
    {
        if (this != &other) {
            if (initialized_) {
                CoUninitialize();
            }
            initialized_ = other.initialized_;
            other.initialized_ = false;
        }
        return *this;
    }

private:
    bool initialized_ = false;
};

class AsioDriver {
public:
    explicit AsioDriver(IASIO* driver) : driver_(driver) {}
    ~AsioDriver()
    {
        if (driver_ != nullptr) {
            driver_->Release();
        }
    }

    AsioDriver(const AsioDriver&) = delete;
    AsioDriver& operator=(const AsioDriver&) = delete;
    AsioDriver(AsioDriver&& other) noexcept : driver_(other.driver_)
    {
        other.driver_ = nullptr;
    }
    AsioDriver& operator=(AsioDriver&& other) noexcept
    {
        if (this != &other) {
            if (driver_ != nullptr) {
                driver_->Release();
            }
            driver_ = other.driver_;
            other.driver_ = nullptr;
        }
        return *this;
    }

    IASIO* get() const { return driver_; }

private:
    IASIO* driver_ = nullptr;
};

void require_asio_ok(ASIOError error, const char* operation)
{
    if (error != ASE_OK && error != ASE_SUCCESS) {
        throw std::runtime_error(std::string(operation) + " failed with ASIO error " + std::to_string(error));
    }
}

std::string asio_sample_type_name(ASIOSampleType type)
{
    switch (type) {
    case ASIOSTInt16LSB: return "ASIO Int16 LSB";
    case ASIOSTInt24LSB: return "ASIO Int24 LSB";
    case ASIOSTInt32LSB: return "ASIO Int32 LSB";
    case ASIOSTFloat32LSB: return "ASIO Float32 LSB";
    case ASIOSTFloat64LSB: return "ASIO Float64 LSB";
    case ASIOSTInt16MSB: return "ASIO Int16 MSB";
    case ASIOSTInt24MSB: return "ASIO Int24 MSB";
    case ASIOSTInt32MSB: return "ASIO Int32 MSB";
    case ASIOSTFloat32MSB: return "ASIO Float32 MSB";
    case ASIOSTFloat64MSB: return "ASIO Float64 MSB";
    case ASIOSTInt32LSB16: return "ASIO Int32 LSB 16-bit";
    case ASIOSTInt32LSB18: return "ASIO Int32 LSB 18-bit";
    case ASIOSTInt32LSB20: return "ASIO Int32 LSB 20-bit";
    case ASIOSTInt32LSB24: return "ASIO Int32 LSB 24-bit";
    case ASIOSTInt32MSB16: return "ASIO Int32 MSB 16-bit";
    case ASIOSTInt32MSB18: return "ASIO Int32 MSB 18-bit";
    case ASIOSTInt32MSB20: return "ASIO Int32 MSB 20-bit";
    case ASIOSTInt32MSB24: return "ASIO Int32 MSB 24-bit";
    default: break;
    }
    std::ostringstream out;
    out << "ASIO sample type " << static_cast<long>(type);
    return out.str();
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
    long available,
    bool input)
{
    std::ostringstream out;
    out << "selected " << backend << " " << direction << " channel(s) "
        << selected_channel_range_text(channels, input)
        << " out of range; device has " << available << " " << direction
        << " channel(s)";
    return out.str();
}

struct DuplexContext {
    std::vector<ASIOBufferInfo*> inputs;
    std::vector<ASIOBufferInfo*> outputs;
    long buffer_size = 0;
    InputChannels input_channels = InputChannels::Mono;
    MonoRingBuffer* capture = nullptr;
    MonoRingBuffer* pitch = nullptr;
    MonoRingBuffer* playback = nullptr;
    StreamControl* control = nullptr;
    OutputRecorder* recorder = nullptr;
    TrackTakeRecorder* track_take_recorder = nullptr;
    std::vector<std::int32_t> capture_scratch;
    std::vector<const std::int32_t*> input_source_pointers;
    std::vector<double> input_peak_scratch;
    SmoothedMonoDownmix input_downmix;
    std::vector<std::int32_t> playback_scratch;
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
};

DuplexContext* g_duplex_context = nullptr;

void asio_sample_rate_changed(ASIOSampleRate)
{
}

long asio_message(long selector, long, void*, double*)
{
    if (selector == kAsioSelectorSupported || selector == kAsioEngineVersion) {
        return 2;
    }
    return 0;
}

void duplex_buffer_switch(long double_buffer_index, ASIOBool)
{
    DuplexContext* context = g_duplex_context;
    if (context == nullptr || context->capture == nullptr || context->playback == nullptr) {
        return;
    }
    if (context->control != nullptr) {
        context->control->audio_callback_generation.fetch_add(
            1, std::memory_order_acq_rel);
    }
    processing::observe_callback_interval(
        context->callback_intervals,
        processing::callback_now_us(),
        static_cast<std::size_t>(context->buffer_size),
        context->sample_rate);
    const bool network_capture_enabled = context->control != nullptr &&
        prepare_network_capture_callback(
            *context->control,
            *context->capture,
            context->engine_frame_counter);
    if (context->recorder_my_input_scratch.size() >= static_cast<std::size_t>(context->buffer_size)) {
        std::fill(
            context->recorder_my_input_scratch.begin(),
            context->recorder_my_input_scratch.begin() + context->buffer_size,
            0);
    }

    const int test_input_mode = context->control != nullptr ?
        context->control->test_input_mode.load(std::memory_order_relaxed) :
        0;
    if (test_input_mode != 0 &&
        context->capture_scratch.size() >= static_cast<std::size_t>(context->buffer_size)) {
        auto generated = std::span<std::int32_t>(context->capture_scratch.data(), static_cast<std::size_t>(context->buffer_size));
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
            processing::observe_input_peaks(context->control, generated);
        }
        if (network_capture_enabled) {
            context->capture->push(std::span<const std::int32_t>(generated.data(), generated.size()));
        }
        if (context->control != nullptr && context->pitch != nullptr) {
            push_pitch_analysis_callback(*context->control, *context->pitch, generated);
        }
        if (context->recorder_my_input_scratch.size() >= static_cast<std::size_t>(context->buffer_size)) {
            std::copy(generated.begin(), generated.end(), context->recorder_my_input_scratch.begin());
        }
    } else if (!context->inputs.empty() &&
        context->inputs[0] != nullptr &&
        context->inputs[0]->buffers[double_buffer_index] != nullptr &&
        context->capture_scratch.size() >= static_cast<std::size_t>(context->buffer_size)) {
        std::span<const std::int32_t> captured_input;
        auto* source_router = context->control != nullptr ?
            context->control->input_source_router : nullptr;
        if (source_router != nullptr) {
            for (std::size_t channel = 0; channel < context->inputs.size(); ++channel) {
                ASIOBufferInfo* input_info = context->inputs[channel];
                context->input_source_pointers[channel] = input_info != nullptr &&
                    input_info->buffers[double_buffer_index] != nullptr ?
                    static_cast<const std::int32_t*>(input_info->buffers[double_buffer_index]) : nullptr;
            }
            const bool routed = source_router->process(
                std::span<const std::int32_t* const>(context->input_source_pointers.data(),
                    context->input_source_pointers.size()),
                static_cast<std::size_t>(context->buffer_size),
                context->engine_frame_counter,
                context->sample_rate,
                std::span<std::int32_t>(context->capture_scratch.data(),
                    static_cast<std::size_t>(context->buffer_size)));
            if (!routed) std::fill_n(context->capture_scratch.begin(),
                static_cast<std::size_t>(context->buffer_size), 0);
            captured_input = std::span<const std::int32_t>(context->capture_scratch.data(),
                static_cast<std::size_t>(context->buffer_size));
            if (network_capture_enabled) context->capture->push(captured_input);
        } else if (context->inputs.size() == 1) {
            const auto* input = static_cast<const std::int32_t*>(context->inputs[0]->buffers[double_buffer_index]);
            captured_input = std::span<const std::int32_t>(input, static_cast<std::size_t>(context->buffer_size));
            std::copy(captured_input.begin(), captured_input.end(), context->capture_scratch.begin());
            if (network_capture_enabled) {
                context->capture->push(captured_input);
            }
        } else {
            std::fill(
                context->input_peak_scratch.begin(),
                context->input_peak_scratch.end(),
                0.0);
            for (std::size_t channel = 0; channel < context->inputs.size(); ++channel) {
                ASIOBufferInfo* input_info = context->inputs[channel];
                if (input_info == nullptr ||
                    input_info->buffers[double_buffer_index] == nullptr) {
                    continue;
                }
                const auto* input = static_cast<const std::int32_t*>(
                    input_info->buffers[double_buffer_index]);
                double peak = 0.0;
                for (long frame = 0; frame < context->buffer_size; ++frame) {
                    peak = (std::max)(
                        peak,
                        std::abs(static_cast<double>(input[frame]) / 2147483648.0));
                }
                context->input_peak_scratch[channel] = peak;
            }
            context->input_downmix.beginBlock(
                std::span<const double>(
                    context->input_peak_scratch.data(),
                    context->input_peak_scratch.size()));
            if (context->control != nullptr) {
                publish_downmix_diagnostics(
                    *context->control, context->input_downmix);
            }
            for (long i = 0; i < context->buffer_size; ++i) {
                double sum = 0.0;
                for (std::size_t channel = 0; channel < context->inputs.size(); ++channel) {
                    ASIOBufferInfo* input_info = context->inputs[channel];
                    if (input_info == nullptr || input_info->buffers[double_buffer_index] == nullptr) {
                        continue;
                    }
                    const auto* input = static_cast<const std::int32_t*>(input_info->buffers[double_buffer_index]);
                    sum += static_cast<double>(input[i]) *
                        context->input_downmix.weightAt(
                            channel, static_cast<std::size_t>(i));
                }
                const double mixed = sum * context->input_downmix.normalizationAt(
                    static_cast<std::size_t>(i));
                context->capture_scratch[static_cast<std::size_t>(i)] =
                    static_cast<std::int32_t>(std::clamp(
                        mixed, -2147483648.0, 2147483647.0));
            }
            captured_input = std::span<const std::int32_t>(
                context->capture_scratch.data(),
                static_cast<std::size_t>(context->buffer_size));
            if (network_capture_enabled) {
                context->capture->push(captured_input);
            }
        }
        if (context->control != nullptr && context->pitch != nullptr) {
            push_pitch_analysis_callback(*context->control, *context->pitch, captured_input);
        }
        if (context->control != nullptr) {
            processing::observe_input_peaks(context->control, captured_input);
        }
        if (context->recorder_my_input_scratch.size() >= static_cast<std::size_t>(context->buffer_size)) {
            const bool selected = source_router != nullptr && source_router->copy_recording_source(
                static_cast<std::size_t>(context->buffer_size),
                std::span<std::int32_t>(context->recorder_my_input_scratch.data(),
                    static_cast<std::size_t>(context->buffer_size)));
            if (!selected)
                std::copy(captured_input.begin(), captured_input.end(), context->recorder_my_input_scratch.begin());
        }
    }

    if (!context->outputs.empty() &&
        context->outputs[0] != nullptr &&
        context->outputs[0]->buffers[double_buffer_index] != nullptr &&
        context->playback_scratch.size() >= static_cast<std::size_t>(context->buffer_size)) {
        auto* mono = context->playback_scratch.data();
        const bool network_playback_enabled = context->control != nullptr &&
            context->control->network_playback_enabled.load(std::memory_order_acquire);
        if (!network_playback_enabled) {
            context->playback_prefilled.store(false, std::memory_order_relaxed);
            context->playback->pop(std::span<std::int32_t>{}, false);
            context->playback_resampler.reset();
            context->control->playback_ratio_applied_ppm.store(1000000, std::memory_order_relaxed);
            context->control->playback_ratio_ramping.store(false, std::memory_order_relaxed);
            std::fill(mono, mono + context->buffer_size, 0);
        } else if (!context->playback_prefilled.load(std::memory_order_relaxed)) {
            if (context->playback->available_read() >= context->playback_prefill_frames) {
                context->playback_prefilled.store(true, std::memory_order_relaxed);
            } else {
                std::fill(mono, mono + context->buffer_size, 0);
            }
        }
        auto playback = std::span<std::int32_t>(mono, static_cast<std::size_t>(context->buffer_size));
        if (network_playback_enabled && context->playback_prefilled.load(std::memory_order_relaxed)) {
            processing::pop_resampled_playback(
                context->playback,
                context->control,
                context->playback_resampler,
                playback);
            processing::apply_remote_level(context->control, playback);
        }
        if (context->control != nullptr) {
            context->control->network_playback_enabled_applied.store(
                network_playback_enabled,
                std::memory_order_release);
            processing::observe_peak(context->control->remote_peak_ppm, playback);
            processing::observe_peak(context->control->gui_remote_peak_ppm, playback);
        }
        if (context->recorder_their_input_scratch.size() >= static_cast<std::size_t>(context->buffer_size)) {
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
            std::span<std::int32_t>(
                context->recorder_prepared_scratch.data(),
                std::min<std::size_t>(context->recorder_prepared_scratch.size(), playback.size())));
        processing::mix_metronome_click(
            context->control,
            context->sample_rate,
            context->engine_frame_counter,
            context->metronome_beat_index,
            playback,
            std::span<std::int32_t>(
                context->recorder_metronome_scratch.data(),
                std::min<std::size_t>(context->recorder_metronome_scratch.size(), playback.size())));
        processing::apply_output_level(context->control, playback);
        if (context->control != nullptr) {
            processing::observe_peak(
                context->control->metronome_peak_ppm,
                std::span<const std::int32_t>(
                    context->recorder_metronome_scratch.data(),
                    std::min<std::size_t>(context->recorder_metronome_scratch.size(), playback.size())));
            processing::observe_peak(
                context->control->gui_metronome_peak_ppm,
                std::span<const std::int32_t>(
                    context->recorder_metronome_scratch.data(),
                    std::min<std::size_t>(context->recorder_metronome_scratch.size(), playback.size())));
            processing::observe_output_peak(context->control, playback);
        }
        if (context->track_take_recorder != nullptr &&
            context->recorder_my_input_scratch.size() >= playback.size() &&
            context->recorder_their_input_scratch.size() >= playback.size() &&
            context->recorder_inputs_mix_scratch.size() >= playback.size()) {
            const std::int32_t options = context->control != nullptr
                ? context->control->track_take_options.load(std::memory_order_relaxed)
                : 0;
            const auto source = static_cast<TrackTakeSource>(options & 0xff);
            if (source == TrackTakeSource::CurrentJam) {
                for (std::size_t index = 0; index < playback.size(); ++index) {
                    std::int32_t sample = processing::mix_i32_samples(
                        context->recorder_my_input_scratch[index],
                        context->recorder_their_input_scratch[index]);
                    if ((options & kTrackTakeIncludePrepared) != 0 &&
                        context->recorder_prepared_scratch.size() >= playback.size()) {
                        sample = processing::mix_i32_samples(
                            sample, context->recorder_prepared_scratch[index]);
                    }
                    if ((options & kTrackTakeIncludeMetronome) != 0 &&
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
        if (context->recorder != nullptr &&
            context->recorder_inputs_mix_scratch.size() >= playback.size() &&
            context->recorder_my_input_scratch.size() >= playback.size() &&
            context->recorder_their_input_scratch.size() >= playback.size() &&
            context->recorder_metronome_scratch.size() >= playback.size()) {
            for (std::size_t i = 0; i < playback.size(); ++i) {
                context->recorder_inputs_mix_scratch[i] = processing::mix_i32_samples(
                    context->recorder_my_input_scratch[i],
                    context->recorder_their_input_scratch[i]);
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
        for (long i = 0; i < context->buffer_size; ++i) {
            for (ASIOBufferInfo* output_info : context->outputs) {
                if (output_info == nullptr || output_info->buffers[double_buffer_index] == nullptr) {
                    continue;
                }
                auto* output = static_cast<std::int32_t*>(output_info->buffers[double_buffer_index]);
                output[i] = mono[i];
            }
        }
    }

    context->engine_frame_counter += static_cast<std::uint64_t>(context->buffer_size);
    if (context->control != nullptr) {
        context->control->engine_frame_counter.store(context->engine_frame_counter, std::memory_order_release);
        context->control->audio_callback_generation.fetch_add(
            1, std::memory_order_release);
    }
    context->callbacks.fetch_add(1, std::memory_order_relaxed);
}

ASIOTime* duplex_buffer_switch_time_info(ASIOTime* params, long double_buffer_index, ASIOBool direct_process)
{
    duplex_buffer_switch(double_buffer_index, direct_process);
    return params;
}

class WindowsDeviceStream final : public DeviceStream {
public:
    WindowsDeviceStream(
        ComRuntime com,
        AsioDriver driver,
        DeviceInfo device,
        std::vector<ASIOBufferInfo> buffers,
        long buffer_size,
        InputChannels input_channels,
        ChannelSelection channels,
        int output_channel_count,
        MonoRingBuffer& capture_ring,
        MonoRingBuffer& pitch_ring,
        MonoRingBuffer& playback_ring,
        std::size_t playback_prefill_frames,
        StreamControl& control,
        OutputRecorder* recorder,
        TrackTakeRecorder* track_take_recorder,
        double sample_rate)
        : com_(std::move(com)),
          driver_(std::move(driver)),
          device_(std::move(device)),
          buffers_(std::move(buffers)),
          sample_rate_(sample_rate),
          buffer_size_(buffer_size),
          input_channels_(input_channels),
          channels_(channels)
    {
        const std::size_t input_count = channels_.input.size();
        context_.inputs.reserve(input_count);
        for (std::size_t index = 0; index < input_count; ++index) {
            context_.inputs.push_back(&buffers_[index]);
        }
        context_.outputs.reserve(static_cast<std::size_t>(output_channel_count));
        for (std::size_t index = 0; index < static_cast<std::size_t>(output_channel_count); ++index) {
            context_.outputs.push_back(&buffers_[input_count + index]);
        }
        context_.buffer_size = buffer_size;
        context_.input_channels = input_channels;
        context_.capture = &capture_ring;
        context_.pitch = &pitch_ring;
        context_.playback = &playback_ring;
        context_.control = &control;
        context_.recorder = recorder;
        context_.track_take_recorder = track_take_recorder;
        context_.capture_scratch.resize(static_cast<std::size_t>(buffer_size));
        context_.input_source_pointers.resize(input_count);
        context_.input_peak_scratch.resize(input_count);
        context_.input_downmix.configure(input_count, sample_rate, static_cast<std::size_t>(buffer_size));
        control.input_downmix_selected_channels.store(
            static_cast<int>(input_count), std::memory_order_relaxed);
        context_.playback_scratch.resize(static_cast<std::size_t>(buffer_size));
        context_.recorder_my_input_scratch.resize(static_cast<std::size_t>(buffer_size));
        context_.recorder_their_input_scratch.resize(static_cast<std::size_t>(buffer_size));
        context_.recorder_inputs_mix_scratch.resize(static_cast<std::size_t>(buffer_size));
        context_.recorder_metronome_scratch.resize(static_cast<std::size_t>(buffer_size));
        context_.recorder_prepared_scratch.resize(static_cast<std::size_t>(buffer_size));
        context_.playback_prefill_frames = playback_prefill_frames;
        context_.sample_rate = sample_rate;
    }

    ~WindowsDeviceStream() override
    {
        if (started_) {
            (void)driver_.get()->stop();
        }
        if (created_) {
            (void)driver_.get()->disposeBuffers();
        }
        if (g_duplex_context == &context_) {
            g_duplex_context = nullptr;
        }
    }

    WindowsDeviceStream(const WindowsDeviceStream&) = delete;
    WindowsDeviceStream& operator=(const WindowsDeviceStream&) = delete;

    void start()
    {
        callbacks_.bufferSwitch = duplex_buffer_switch;
        callbacks_.sampleRateDidChange = asio_sample_rate_changed;
        callbacks_.asioMessage = asio_message;
        callbacks_.bufferSwitchTimeInfo = duplex_buffer_switch_time_info;

        g_duplex_context = &context_;
        require_asio_ok(
            driver_.get()->createBuffers(buffers_.data(), static_cast<long>(buffers_.size()), buffer_size_, &callbacks_),
            "ASIO createBuffers");
        created_ = true;
        long reported_input_latency = 0;
        long reported_output_latency = 0;
        require_asio_ok(
            driver_.get()->getLatencies(&reported_input_latency, &reported_output_latency),
            "ASIO getLatencies");
        input_latency_frames_ = (std::max)(0L, reported_input_latency);
        output_latency_frames_ = context_.outputs.empty() ? 0L : (std::max)(0L, reported_output_latency);
        context_.control->input_latency_frames.store(
            static_cast<std::uint32_t>(input_latency_frames_),
            std::memory_order_relaxed);
        context_.control->output_latency_frames.store(
            static_cast<std::uint32_t>(output_latency_frames_),
            std::memory_order_relaxed);
        context_.control->recording_latency_compensation_frames.store(
            static_cast<std::uint64_t>(input_latency_frames_) + static_cast<std::uint64_t>(output_latency_frames_),
            std::memory_order_relaxed);
        require_asio_ok(driver_.get()->start(), "ASIO start");
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
        result.device = device_;
        result.sample_rate = sample_rate_;
        result.buffer_size = buffer_size_;
        result.input_latency_frames = input_latency_frames_;
        result.output_latency_frames = output_latency_frames_;
        result.input_channels = input_channels_;
        result.channels = channels_;
        result.sample_format = asio_sample_type_name(ASIOSTInt32LSB);
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
        };
    }

private:
    ComRuntime com_;
    AsioDriver driver_;
    DeviceInfo device_;
    std::vector<ASIOBufferInfo> buffers_;
    ASIOCallbacks callbacks_{};
    DuplexContext context_{};
    double sample_rate_ = 0.0;
    long buffer_size_ = 0;
    long input_latency_frames_ = 0;
    long output_latency_frames_ = 0;
    InputChannels input_channels_ = InputChannels::Mono;
    ChannelSelection channels_;
    bool created_ = false;
    bool started_ = false;
};

std::string read_string_value(HKEY key, const char* value_name)
{
    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExA(key, value_name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) || size == 0) {
        return {};
    }

    std::string value(size, '\0');
    if (RegQueryValueExA(
            key,
            value_name,
            nullptr,
            &type,
            reinterpret_cast<LPBYTE>(value.data()),
            &size) != ERROR_SUCCESS) {
        return {};
    }
    while (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

std::string clsid_to_driver_path(const std::string& clsid)
{
    if (clsid.empty()) {
        return {};
    }
    const std::string subkey = "CLSID\\" + clsid + "\\InprocServer32";
    RegistryKey key;
    if (RegOpenKeyExA(HKEY_CLASSES_ROOT, subkey.c_str(), 0, KEY_READ, key.out()) != ERROR_SUCCESS) {
        return {};
    }
    return read_string_value(key.get(), nullptr);
}

void enumerate_asio_key(const char* registry_path, std::vector<DeviceInfo>& devices)
{
    RegistryKey root;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, registry_path, 0, KEY_READ, root.out()) != ERROR_SUCCESS) {
        return;
    }

    DWORD index = 0;
    for (;;) {
        std::array<char, 256> name{};
        DWORD name_size = static_cast<DWORD>(name.size());
        const LSTATUS status = RegEnumKeyExA(
            root.get(),
            index,
            name.data(),
            &name_size,
            nullptr,
            nullptr,
            nullptr,
            nullptr);
        if (status == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (status != ERROR_SUCCESS) {
            throw std::runtime_error("failed to enumerate ASIO registry key");
        }
        ++index;

        RegistryKey driver;
        const std::string driver_key_path = std::string(registry_path) + "\\" + name.data();
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, driver_key_path.c_str(), 0, KEY_READ, driver.out()) != ERROR_SUCCESS) {
            continue;
        }

        DeviceInfo info;
        info.backend = "ASIO";
        info.name = name.data();
        info.clsid = read_string_value(driver.get(), "CLSID");
        info.driver_path = clsid_to_driver_path(info.clsid);
        const auto duplicate = std::find_if(devices.begin(), devices.end(), [&](const DeviceInfo& existing) {
            if (!info.clsid.empty() && !existing.clsid.empty()) {
                return existing.clsid == info.clsid;
            }
            return existing.name == info.name;
        });
        if (duplicate != devices.end()) {
            continue;
        }
        info.id = static_cast<int>(devices.size());
        devices.push_back(std::move(info));
    }
}

} // namespace

std::vector<DeviceInfo> list_devices()
{
    std::vector<DeviceInfo> devices;
    enumerate_asio_key("SOFTWARE\\ASIO", devices);
    enumerate_asio_key("SOFTWARE\\WOW6432Node\\ASIO", devices);
    return devices;
}

DeviceTestResult test_device(int id)
{
    const auto devices = list_devices();
    if (id < 0 || static_cast<std::size_t>(id) >= devices.size()) {
        throw std::runtime_error("audio device id is out of range");
    }
    const DeviceInfo& device = devices[static_cast<std::size_t>(id)];
    if (device.clsid.empty()) {
        throw std::runtime_error("selected ASIO device has no CLSID");
    }

    ComRuntime com;
    wchar_t wide_clsid[64]{};
    const int converted = MultiByteToWideChar(
        CP_UTF8,
        0,
        device.clsid.c_str(),
        -1,
        wide_clsid,
        static_cast<int>(std::size(wide_clsid)));
    if (converted <= 0) {
        throw std::runtime_error("failed to convert ASIO CLSID");
    }

    CLSID clsid{};
    HRESULT hr = CLSIDFromString(wide_clsid, &clsid);
    if (FAILED(hr)) {
        throw std::runtime_error("CLSIDFromString failed: " + hresult_text(hr));
    }

    IASIO* raw_driver = nullptr;
    hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, reinterpret_cast<void**>(&raw_driver));
    if (FAILED(hr) || raw_driver == nullptr) {
        throw std::runtime_error("CoCreateInstance failed for ASIO driver: " + hresult_text(hr));
    }
    AsioDriver driver(raw_driver);

    if (driver.get()->init(GetConsoleWindow()) != ASIOTrue) {
        std::array<char, 256> message{};
        driver.get()->getErrorMessage(message.data());
        const std::string text = message.data();
        throw std::runtime_error(text.empty() ? "ASIO init failed" : "ASIO init failed: " + text);
    }

    DeviceTestResult capabilities;
    capabilities.device = device;
    long input_channels = 0;
    long output_channels = 0;
    require_asio_ok(driver.get()->getChannels(&input_channels, &output_channels), "ASIO getChannels");
    long min_buffer_size = 0;
    long max_buffer_size = 0;
    long preferred_buffer_size = 0;
    long buffer_granularity = 0;
    require_asio_ok(
        driver.get()->getBufferSize(
            &min_buffer_size,
            &max_buffer_size,
            &preferred_buffer_size,
            &buffer_granularity),
        "ASIO getBufferSize");

    ASIOSampleRate current_rate = 0.0;
    const ASIOError rate_error = driver.get()->getSampleRate(&current_rate);
    if (rate_error == ASE_OK || rate_error == ASE_SUCCESS) {
        capabilities.current_sample_rate = current_rate;
    }
    for (std::size_t index = 0; index < kTestSampleRates.size(); ++index) {
        const ASIOError result = driver.get()->canSampleRate(kTestSampleRates[index]);
        capabilities.sample_rate_supported[index] = result == ASE_OK || result == ASE_SUCCESS;
    }
    for (std::size_t index = 0; index < kTestBufferSizes.size(); ++index) {
        const long size = kTestBufferSizes[index];
        bool supported = size >= min_buffer_size && size <= max_buffer_size;
        if (supported && min_buffer_size == max_buffer_size) {
            supported = size == min_buffer_size;
        } else if (supported && buffer_granularity == -1) {
            supported = size > 0 && (size & (size - 1)) == 0;
        } else if (supported && buffer_granularity == 0) {
            supported = size == preferred_buffer_size;
        } else if (supported && buffer_granularity > 0) {
            supported = (size - min_buffer_size) % buffer_granularity == 0;
        }
        capabilities.buffer_size_supported[index] = supported;
    }
    return capabilities;
}

std::unique_ptr<DeviceStream> start_duplex_stream(
    int id,
    double requested_sample_rate,
    long buffer_size,
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
    const auto devices = list_devices();
    if (id < 0 || static_cast<std::size_t>(id) >= devices.size()) {
        throw std::runtime_error("audio device id is out of range");
    }
    const DeviceInfo& device = devices[static_cast<std::size_t>(id)];
    if (device.clsid.empty()) {
        throw std::runtime_error("selected ASIO device has no CLSID");
    }

    ComRuntime com;
    wchar_t wide_clsid[64]{};
    const int converted = MultiByteToWideChar(
        CP_UTF8,
        0,
        device.clsid.c_str(),
        -1,
        wide_clsid,
        static_cast<int>(std::size(wide_clsid)));
    if (converted <= 0) {
        throw std::runtime_error("failed to convert ASIO CLSID");
    }

    CLSID clsid{};
    HRESULT hr = CLSIDFromString(wide_clsid, &clsid);
    if (FAILED(hr)) {
        throw std::runtime_error("CLSIDFromString failed: " + hresult_text(hr));
    }

    IASIO* raw_driver = nullptr;
    hr = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, clsid, reinterpret_cast<void**>(&raw_driver));
    if (FAILED(hr) || raw_driver == nullptr) {
        throw std::runtime_error("CoCreateInstance failed for ASIO driver: " + hresult_text(hr));
    }
    AsioDriver driver(raw_driver);

    if (driver.get()->init(GetConsoleWindow()) != ASIOTrue) {
        std::array<char, 256> message{};
        driver.get()->getErrorMessage(message.data());
        const std::string text = message.data();
        throw std::runtime_error(text.empty() ? "ASIO init failed" : "ASIO init failed: " + text);
    }

    long input_channels = 0;
    long output_channels = 0;
    require_asio_ok(driver.get()->getChannels(&input_channels, &output_channels), "ASIO getChannels");
    if (input_channels <= 0) {
        throw std::runtime_error("ASIO stream requires at least one input channel");
    }
    const int selected_input_channels = static_cast<int>(channels.input.size());
    const int selected_output_channels = static_cast<int>(channels.output.size());
    if (selected_input_channels <= 0) {
        throw std::runtime_error("ASIO stream requires at least one selected input channel");
    }
    if (selected_output_channels > 0 && output_channels <= 0) {
        throw std::runtime_error("ASIO stream selected output channels but device has no output channels");
    }
    for (int channel : channels.input) {
        if (channel < 0 || channel >= input_channels) {
            throw std::runtime_error(channel_range_error("ASIO", "input", channels, input_channels, true));
        }
    }
    for (int channel : channels.output) {
        if (channel < 0 || channel >= output_channels) {
            throw std::runtime_error(channel_range_error("ASIO", "output", channels, output_channels, false));
        }
    }

    long min_buffer = 0;
    long max_buffer = 0;
    long preferred_buffer = 0;
    long granularity = 0;
    require_asio_ok(driver.get()->getBufferSize(&min_buffer, &max_buffer, &preferred_buffer, &granularity), "ASIO getBufferSize");
    if (buffer_size <= 0) {
        buffer_size = preferred_buffer;
    }
    if (buffer_size < min_buffer || buffer_size > max_buffer) {
        throw std::runtime_error("requested ASIO buffer size is outside the device min/max range");
    }
    if (playback_prefill_frames > playback_ring.capacity()) {
        throw std::runtime_error("playback prefill must fit within playback ring capacity");
    }

    ASIOSampleRate current_rate = 0.0;
    (void)driver.get()->getSampleRate(&current_rate);
    if (requested_sample_rate > 0.0 && current_rate != requested_sample_rate) {
        const ASIOError can_rate = driver.get()->canSampleRate(requested_sample_rate);
        if (can_rate != ASE_OK && can_rate != ASE_SUCCESS) {
            throw std::runtime_error("requested ASIO sample rate is not supported");
        }
        require_asio_ok(driver.get()->setSampleRate(requested_sample_rate), "ASIO setSampleRate");
        (void)driver.get()->getSampleRate(&current_rate);
    }

    for (int index = 0; index < selected_input_channels; ++index) {
        ASIOChannelInfo input_info{};
        input_info.channel = channels.input[static_cast<std::size_t>(index)];
        input_info.isInput = ASIOTrue;
        require_asio_ok(driver.get()->getChannelInfo(&input_info), "ASIO getChannelInfo input");
        if (input_info.type != ASIOSTInt32LSB) {
            throw std::runtime_error("ASIO duplex stream currently supports Int32LSB input only");
        }
    }
    for (int index = 0; index < selected_output_channels; ++index) {
        ASIOChannelInfo output_info{};
        output_info.channel = channels.output[static_cast<std::size_t>(index)];
        output_info.isInput = ASIOFalse;
        require_asio_ok(driver.get()->getChannelInfo(&output_info), "ASIO getChannelInfo output");
        if (output_info.type != ASIOSTInt32LSB) {
            throw std::runtime_error("ASIO duplex stream currently supports Int32LSB output only");
        }
    }

    std::vector<ASIOBufferInfo> buffers(static_cast<std::size_t>(selected_input_channels + selected_output_channels));
    for (int index = 0; index < selected_input_channels; ++index) {
        buffers[static_cast<std::size_t>(index)].isInput = ASIOTrue;
        buffers[static_cast<std::size_t>(index)].channelNum = channels.input[static_cast<std::size_t>(index)];
    }
    for (int index = 0; index < selected_output_channels; ++index) {
        const std::size_t buffer_index = static_cast<std::size_t>(selected_input_channels + index);
        buffers[buffer_index].isInput = ASIOFalse;
        buffers[buffer_index].channelNum = channels.output[static_cast<std::size_t>(index)];
    }

    auto stream = std::make_unique<WindowsDeviceStream>(
        std::move(com),
        std::move(driver),
        device,
        std::move(buffers),
        buffer_size,
        requested_input_channels,
        channels,
        selected_output_channels,
        capture_ring,
        pitch_ring,
        playback_ring,
        playback_prefill_frames,
        control,
        recorder,
        track_take_recorder,
        current_rate);
    stream->start();
    return stream;
}

} // namespace jam2::audio

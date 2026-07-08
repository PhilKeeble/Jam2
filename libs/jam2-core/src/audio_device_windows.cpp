#include "audio_device.hpp"
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

constexpr double kPi = 3.14159265358979323846;

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

int sample_bytes(ASIOSampleType type)
{
    switch (type) {
    case ASIOSTInt16LSB:
    case ASIOSTInt16MSB:
        return 2;
    case ASIOSTInt24LSB:
    case ASIOSTInt24MSB:
        return 3;
    case ASIOSTInt32LSB:
    case ASIOSTInt32MSB:
    case ASIOSTInt32LSB16:
    case ASIOSTInt32LSB18:
    case ASIOSTInt32LSB20:
    case ASIOSTInt32LSB24:
    case ASIOSTInt32MSB16:
    case ASIOSTInt32MSB18:
    case ASIOSTInt32MSB20:
    case ASIOSTInt32MSB24:
    case ASIOSTFloat32LSB:
    case ASIOSTFloat32MSB:
        return 4;
    case ASIOSTFloat64LSB:
    case ASIOSTFloat64MSB:
        return 8;
    default:
        return 0;
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

struct MeterContext {
    ASIOBufferInfo* input = nullptr;
    ASIOBufferInfo* output = nullptr;
    long buffer_size = 0;
    ASIOSampleType input_type = ASIOSTInt32LSB;
    ASIOSampleType output_type = ASIOSTInt32LSB;
    std::atomic<long> callbacks{0};
    std::atomic<int> peak_ppm{0};
};

MeterContext* g_meter_context = nullptr;

struct RingContext {
    ASIOBufferInfo* input = nullptr;
    ASIOBufferInfo* output = nullptr;
    long buffer_size = 0;
    ASIOSampleType input_type = ASIOSTInt32LSB;
    ASIOSampleType output_type = ASIOSTInt32LSB;
    MonoRingBuffer* ring = nullptr;
    std::atomic<long> callbacks{0};
};

RingContext* g_ring_context = nullptr;

struct DuplexContext {
    std::vector<ASIOBufferInfo*> inputs;
    std::vector<ASIOBufferInfo*> outputs;
    long buffer_size = 0;
    InputChannels input_channels = InputChannels::Mono;
    MonoRingBuffer* capture = nullptr;
    MonoRingBuffer* playback = nullptr;
    StreamControl* control = nullptr;
    OutputRecorder* recorder = nullptr;
    std::vector<std::int32_t> capture_scratch;
    std::vector<std::int32_t> playback_scratch;
    std::vector<std::int32_t> recorder_my_input_scratch;
    std::vector<std::int32_t> recorder_their_input_scratch;
    std::vector<std::int32_t> recorder_inputs_mix_scratch;
    std::vector<std::int32_t> recorder_metronome_scratch;
    std::size_t playback_prefill_frames = 0;
    double sample_rate = 48000.0;
    std::uint64_t test_input_sample_counter = 0;
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
    std::atomic<std::uint64_t> last_callback_us{0};
    std::atomic<std::uint64_t> callback_interval_min_us{0};
    std::atomic<std::uint64_t> callback_interval_sum_us{0};
    std::atomic<std::uint64_t> callback_interval_max_us{0};
    std::atomic<std::uint64_t> callback_interval_samples{0};
    std::atomic<std::uint64_t> callback_gap_over_1_1x_count{0};
    std::atomic<std::uint64_t> callback_gap_over_1_5x_count{0};
    std::atomic<std::uint64_t> callback_gap_over_2x_count{0};
};

DuplexContext* g_duplex_context = nullptr;

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

void observe_callback_interval(DuplexContext& context)
{
    const std::uint64_t now = callback_now_us();
    const std::uint64_t previous = context.last_callback_us.exchange(now, std::memory_order_relaxed);
    if (previous == 0 || now <= previous || context.sample_rate <= 0.0 || context.buffer_size <= 0) {
        return;
    }
    const std::uint64_t interval = now - previous;
    atomic_update_min(context.callback_interval_min_us, interval);
    context.callback_interval_sum_us.fetch_add(interval, std::memory_order_relaxed);
    atomic_update_max(context.callback_interval_max_us, interval);
    context.callback_interval_samples.fetch_add(1, std::memory_order_relaxed);
    const double expected = static_cast<double>(context.buffer_size) * 1000000.0 / context.sample_rate;
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

int read_signed24_lsb(const std::uint8_t* bytes)
{
    int value = static_cast<int>(bytes[0]) |
        (static_cast<int>(bytes[1]) << 8) |
        (static_cast<int>(bytes[2]) << 16);
    if ((value & 0x00800000) != 0) {
        value |= static_cast<int>(0xff000000);
    }
    return value;
}

int read_signed24_msb(const std::uint8_t* bytes)
{
    int value = (static_cast<int>(bytes[0]) << 16) |
        (static_cast<int>(bytes[1]) << 8) |
        static_cast<int>(bytes[2]);
    if ((value & 0x00800000) != 0) {
        value |= static_cast<int>(0xff000000);
    }
    return value;
}

double sample_abs(const void* data, long index, ASIOSampleType type)
{
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    switch (type) {
    case ASIOSTInt16LSB: {
        std::int16_t value = 0;
        std::memcpy(&value, bytes + index * 2, sizeof(value));
        return std::abs(static_cast<double>(value)) / 32768.0;
    }
    case ASIOSTInt24LSB:
        return std::abs(static_cast<double>(read_signed24_lsb(bytes + index * 3))) / 8388608.0;
    case ASIOSTInt24MSB:
        return std::abs(static_cast<double>(read_signed24_msb(bytes + index * 3))) / 8388608.0;
    case ASIOSTInt32LSB:
    case ASIOSTInt32LSB16:
    case ASIOSTInt32LSB18:
    case ASIOSTInt32LSB20:
    case ASIOSTInt32LSB24: {
        std::int32_t value = 0;
        std::memcpy(&value, bytes + index * 4, sizeof(value));
        return std::abs(static_cast<double>(value)) / 2147483648.0;
    }
    case ASIOSTFloat32LSB: {
        float value = 0.0F;
        std::memcpy(&value, bytes + index * 4, sizeof(value));
        return std::abs(static_cast<double>(value));
    }
    case ASIOSTFloat64LSB: {
        double value = 0.0;
        std::memcpy(&value, bytes + index * 8, sizeof(value));
        return std::abs(value);
    }
    default:
        return 0.0;
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
        const std::uint32_t abs_sample = sample == (std::numeric_limits<std::int32_t>::min)() ?
            static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)()) :
            static_cast<std::uint32_t>(std::abs(sample));
        peak = (std::max)(peak, abs_sample);
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

void meter_buffer_switch(long double_buffer_index, ASIOBool)
{
    MeterContext* context = g_meter_context;
    if (context == nullptr) {
        return;
    }

    if (context->output != nullptr && context->output->buffers[double_buffer_index] != nullptr) {
        const int bytes_per_sample = sample_bytes(context->output_type);
        if (bytes_per_sample > 0) {
            std::memset(
                context->output->buffers[double_buffer_index],
                0,
                static_cast<std::size_t>(bytes_per_sample) * static_cast<std::size_t>(context->buffer_size));
        }
    }

    double peak = 0.0;
    if (context->input != nullptr && context->input->buffers[double_buffer_index] != nullptr) {
        for (long i = 0; i < context->buffer_size; ++i) {
            const double value = sample_abs(context->input->buffers[double_buffer_index], i, context->input_type);
            if (value > peak) {
                peak = value;
            }
        }
    }
    const int peak_ppm = static_cast<int>((peak > 1.0 ? 1.0 : peak) * 1000000.0);
    update_peak(context->peak_ppm, peak_ppm);
    context->callbacks.fetch_add(1, std::memory_order_relaxed);
}

void meter_sample_rate_changed(ASIOSampleRate)
{
}

long meter_asio_message(long selector, long, void*, double*)
{
    if (selector == kAsioSelectorSupported || selector == kAsioEngineVersion) {
        return 2;
    }
    return 0;
}

ASIOTime* meter_buffer_switch_time_info(ASIOTime* params, long double_buffer_index, ASIOBool direct_process)
{
    meter_buffer_switch(double_buffer_index, direct_process);
    return params;
}

void ring_buffer_switch(long double_buffer_index, ASIOBool)
{
    RingContext* context = g_ring_context;
    if (context == nullptr || context->ring == nullptr) {
        return;
    }

    if (context->input != nullptr &&
        context->input->buffers[double_buffer_index] != nullptr &&
        context->input_type == ASIOSTInt32LSB) {
        const auto* input = static_cast<const std::int32_t*>(context->input->buffers[double_buffer_index]);
        context->ring->push(std::span<const std::int32_t>(input, static_cast<std::size_t>(context->buffer_size)));
    }

    if (context->output != nullptr &&
        context->output->buffers[double_buffer_index] != nullptr &&
        context->output_type == ASIOSTInt32LSB) {
        auto* output = static_cast<std::int32_t*>(context->output->buffers[double_buffer_index]);
        context->ring->pop(std::span<std::int32_t>(output, static_cast<std::size_t>(context->buffer_size)));
    }

    context->callbacks.fetch_add(1, std::memory_order_relaxed);
}

long ring_asio_message(long selector, long, void*, double*)
{
    if (selector == kAsioSelectorSupported || selector == kAsioEngineVersion) {
        return 2;
    }
    return 0;
}

ASIOTime* ring_buffer_switch_time_info(ASIOTime* params, long double_buffer_index, ASIOBool direct_process)
{
    ring_buffer_switch(double_buffer_index, direct_process);
    return params;
}

void mix_metronome_click(DuplexContext& context, std::span<std::int32_t> output, std::span<std::int32_t> metronome_stem)
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
        context.metronome_sample_counter += static_cast<std::uint64_t>(output.size());
        return;
    }

    const int level_ppm = context.control->metronome_level_ppm.load(std::memory_order_relaxed);
    const double level = static_cast<double>(std::clamp(level_ppm, 0, 1000000)) / 1000000.0;
    const bool epoch_valid = context.control->metronome_epoch_valid.load(std::memory_order_relaxed);
    const std::uint64_t epoch = context.control->metronome_epoch_sample_time.load(std::memory_order_relaxed);
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
        const bool before_epoch = epoch_valid && context.metronome_sample_counter < epoch;
        const std::uint64_t position =
            before_epoch ? 0ULL : (epoch_valid ? context.metronome_sample_counter - epoch : context.metronome_sample_counter);
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
        ++context.metronome_sample_counter;
    }
}

std::int32_t pop_one_frame(MonoRingBuffer& ring)
{
    std::array<std::int32_t, 1> frame{};
    (void)ring.pop(frame, false);
    return frame[0];
}

void pop_resampled_playback(DuplexContext& context, std::span<std::int32_t> output)
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

void apply_remote_level(DuplexContext& context, std::span<std::int32_t> output)
{
    if (context.control == nullptr) {
        return;
    }
    const int level_ppm = context.control->remote_level_ppm.load(std::memory_order_relaxed);
    if (level_ppm == 1000000) {
        return;
    }
    const double level = static_cast<double>(std::clamp(level_ppm, 0, 1000000)) / 1000000.0;
    for (std::int32_t& sample : output) {
        sample = scale_i32_sample(sample, level);
    }
}

std::int32_t mix_i32_samples(std::int32_t a, std::int32_t b);

void mix_local_monitor(DuplexContext& context, std::span<std::int32_t> output, std::span<const std::int32_t> input)
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
    const double level = static_cast<double>(std::clamp(level_ppm, 0, 1000000)) / 1000000.0;
    std::uint32_t monitor_peak = 0;
    const std::size_t frames = (std::min)(output.size(), input.size());
    for (std::size_t i = 0; i < frames; ++i) {
        const std::int32_t monitored = scale_i32_sample(input[i], level);
        const std::uint32_t abs_sample = monitored == (std::numeric_limits<std::int32_t>::min)() ?
            static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)()) :
            static_cast<std::uint32_t>(std::abs(monitored));
        monitor_peak = (std::max)(monitor_peak, abs_sample);
        output[i] = mix_i32_samples(output[i], monitored);
    }
    const double normalized = static_cast<double>(monitor_peak) / 2147483647.0;
    const int peak_ppm = static_cast<int>(std::clamp(normalized, 0.0, 1.0) * 1000000.0);
    context.control->monitor_peak_ppm.store(peak_ppm, std::memory_order_relaxed);
    context.control->gui_monitor_peak_ppm.store(peak_ppm, std::memory_order_relaxed);
}

void observe_output_peak(DuplexContext& context, std::span<const std::int32_t> output)
{
    if (context.control == nullptr) {
        return;
    }
    observe_peak(context.control->output_peak_ppm, output);
    observe_peak(context.control->gui_output_peak_ppm, output);
    for (std::int32_t sample : output) {
        if (sample == (std::numeric_limits<std::int32_t>::min)() ||
            sample == (std::numeric_limits<std::int32_t>::max)()) {
            context.control->output_clipped_samples.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

std::int32_t mix_i32_samples(std::int32_t a, std::int32_t b)
{
    const std::int64_t mixed = static_cast<std::int64_t>(a) + static_cast<std::int64_t>(b);
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(mixed, -2147483648LL, 2147483647LL));
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

void fill_test_input(DuplexContext& context, std::span<std::int32_t> output)
{
    if (context.control == nullptr) {
        std::fill(output.begin(), output.end(), 0);
        return;
    }
    const int mode = context.control->test_input_mode.load(std::memory_order_relaxed);
    const double level = static_cast<double>(
        std::clamp(context.control->test_input_level_ppm.load(std::memory_order_relaxed), 0, 1000000)) / 1000000.0;
    for (std::int32_t& sample : output) {
        sample = render_test_input_sample(mode, context.test_input_sample_counter, context.sample_rate, level);
        ++context.test_input_sample_counter;
    }
}

void duplex_buffer_switch(long double_buffer_index, ASIOBool)
{
    DuplexContext* context = g_duplex_context;
    if (context == nullptr || context->capture == nullptr || context->playback == nullptr) {
        return;
    }
    observe_callback_interval(*context);
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
        fill_test_input(*context, generated);
        if (context->control != nullptr) {
            observe_peak(context->control->input_peak_ppm, generated);
            observe_peak(context->control->gui_input_peak_ppm, generated);
        }
        context->capture->push(std::span<const std::int32_t>(generated.data(), generated.size()));
        if (context->recorder_my_input_scratch.size() >= static_cast<std::size_t>(context->buffer_size)) {
            std::copy(generated.begin(), generated.end(), context->recorder_my_input_scratch.begin());
        }
    } else if (!context->inputs.empty() &&
        context->inputs[0] != nullptr &&
        context->inputs[0]->buffers[double_buffer_index] != nullptr &&
        context->capture_scratch.size() >= static_cast<std::size_t>(context->buffer_size)) {
        std::span<const std::int32_t> captured_input;
        if (context->inputs.size() == 1) {
            const auto* input = static_cast<const std::int32_t*>(context->inputs[0]->buffers[double_buffer_index]);
            captured_input = std::span<const std::int32_t>(input, static_cast<std::size_t>(context->buffer_size));
            std::copy(captured_input.begin(), captured_input.end(), context->capture_scratch.begin());
            context->capture->push(captured_input);
        } else {
            for (long i = 0; i < context->buffer_size; ++i) {
                std::int64_t sum = 0;
                int mixed_channels = 0;
                for (ASIOBufferInfo* input_info : context->inputs) {
                    if (input_info == nullptr || input_info->buffers[double_buffer_index] == nullptr) {
                        continue;
                    }
                    const auto* input = static_cast<const std::int32_t*>(input_info->buffers[double_buffer_index]);
                    sum += input[i];
                    ++mixed_channels;
                }
                context->capture_scratch[static_cast<std::size_t>(i)] =
                    mixed_channels > 0 ? static_cast<std::int32_t>(sum / mixed_channels) : 0;
            }
            captured_input = std::span<const std::int32_t>(
                context->capture_scratch.data(),
                static_cast<std::size_t>(context->buffer_size));
            context->capture->push(captured_input);
        }
        if (context->control != nullptr) {
            observe_peak(context->control->input_peak_ppm, captured_input);
            observe_peak(context->control->gui_input_peak_ppm, captured_input);
        }
        if (context->recorder_my_input_scratch.size() >= static_cast<std::size_t>(context->buffer_size)) {
            std::copy(captured_input.begin(), captured_input.end(), context->recorder_my_input_scratch.begin());
        }
    }

    if (!context->outputs.empty() &&
        context->outputs[0] != nullptr &&
        context->outputs[0]->buffers[double_buffer_index] != nullptr &&
        context->playback_scratch.size() >= static_cast<std::size_t>(context->buffer_size)) {
        auto* mono = context->playback_scratch.data();
        if (!context->playback_prefilled.load(std::memory_order_relaxed)) {
            if (context->playback->available_read() >= context->playback_prefill_frames) {
                context->playback_prefilled.store(true, std::memory_order_relaxed);
            } else {
                std::fill(mono, mono + context->buffer_size, 0);
            }
        }
        auto playback = std::span<std::int32_t>(mono, static_cast<std::size_t>(context->buffer_size));
        if (context->playback_prefilled.load(std::memory_order_relaxed)) {
            pop_resampled_playback(*context, playback);
            apply_remote_level(*context, playback);
        }
        if (context->control != nullptr) {
            observe_peak(context->control->remote_peak_ppm, playback);
            observe_peak(context->control->gui_remote_peak_ppm, playback);
        }
        if (context->recorder_their_input_scratch.size() >= static_cast<std::size_t>(context->buffer_size)) {
            std::copy(playback.begin(), playback.end(), context->recorder_their_input_scratch.begin());
        }
        mix_local_monitor(
            *context,
            playback,
            std::span<const std::int32_t>(
                context->capture_scratch.data(),
                std::min<std::size_t>(context->capture_scratch.size(), playback.size())));
        const std::uint64_t audio_frame_start = context->metronome_sample_counter;
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
            for (std::size_t i = 0; i < playback.size(); ++i) {
                context->recorder_inputs_mix_scratch[i] = mix_i32_samples(
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

    context->callbacks.fetch_add(1, std::memory_order_relaxed);
}

ASIOTime* duplex_buffer_switch_time_info(ASIOTime* params, long double_buffer_index, ASIOBool direct_process)
{
    duplex_buffer_switch(double_buffer_index, direct_process);
    return params;
}

class AsioBuffers {
public:
    explicit AsioBuffers(IASIO* driver) : driver_(driver) {}
    ~AsioBuffers()
    {
        if (started_) {
            (void)driver_->stop();
        }
        if (created_) {
            (void)driver_->disposeBuffers();
        }
        g_meter_context = nullptr;
        g_ring_context = nullptr;
        g_duplex_context = nullptr;
    }

    AsioBuffers(const AsioBuffers&) = delete;
    AsioBuffers& operator=(const AsioBuffers&) = delete;

    void mark_created() { created_ = true; }
    void mark_started() { started_ = true; }

private:
    IASIO* driver_ = nullptr;
    bool created_ = false;
    bool started_ = false;
};

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
        MonoRingBuffer& playback_ring,
        std::size_t playback_prefill_frames,
        StreamControl& control,
        OutputRecorder* recorder,
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
        context_.playback = &playback_ring;
        context_.control = &control;
        context_.recorder = recorder;
        context_.capture_scratch.resize(static_cast<std::size_t>(buffer_size));
        context_.playback_scratch.resize(static_cast<std::size_t>(buffer_size));
        context_.recorder_my_input_scratch.resize(static_cast<std::size_t>(buffer_size));
        context_.recorder_their_input_scratch.resize(static_cast<std::size_t>(buffer_size));
        context_.recorder_inputs_mix_scratch.resize(static_cast<std::size_t>(buffer_size));
        context_.recorder_metronome_scratch.resize(static_cast<std::size_t>(buffer_size));
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
        callbacks_.sampleRateDidChange = meter_sample_rate_changed;
        callbacks_.asioMessage = ring_asio_message;
        callbacks_.bufferSwitchTimeInfo = duplex_buffer_switch_time_info;

        g_duplex_context = &context_;
        require_asio_ok(
            driver_.get()->createBuffers(buffers_.data(), static_cast<long>(buffers_.size()), buffer_size_, &callbacks_),
            "ASIO createBuffers");
        created_ = true;
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
        result.input_channels = input_channels_;
        result.channels = channels_;
        result.sample_format = asio_sample_type_name(ASIOSTInt32LSB);
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
    ComRuntime com_;
    AsioDriver driver_;
    DeviceInfo device_;
    std::vector<ASIOBufferInfo> buffers_;
    ASIOCallbacks callbacks_{};
    DuplexContext context_{};
    double sample_rate_ = 0.0;
    long buffer_size_ = 0;
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

DeviceProbe probe_device(int id, double requested_sample_rate)
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

    DeviceProbe probe;
    probe.device = device;
    std::array<char, 128> driver_name{};
    driver.get()->getDriverName(driver_name.data());
    probe.driver_name = driver_name.data();
    probe.driver_version = driver.get()->getDriverVersion();
    require_asio_ok(driver.get()->getChannels(&probe.input_channels, &probe.output_channels), "ASIO getChannels");
    require_asio_ok(driver.get()->getLatencies(&probe.input_latency_samples, &probe.output_latency_samples), "ASIO getLatencies");
    require_asio_ok(
        driver.get()->getBufferSize(
            &probe.min_buffer_size,
            &probe.max_buffer_size,
            &probe.preferred_buffer_size,
            &probe.buffer_granularity),
        "ASIO getBufferSize");

    ASIOSampleRate current_rate = 0.0;
    const ASIOError rate_error = driver.get()->getSampleRate(&current_rate);
    if (rate_error == ASE_OK || rate_error == ASE_SUCCESS) {
        probe.current_sample_rate = current_rate;
    }
    const ASIOError can_rate = driver.get()->canSampleRate(requested_sample_rate);
    probe.requested_sample_rate_supported = can_rate == ASE_OK || can_rate == ASE_SUCCESS;
    return probe;
}

DeviceMeterResult meter_device(int id, double requested_sample_rate, long buffer_size, int duration_ms)
{
    if (duration_ms <= 0) {
        throw std::runtime_error("meter duration must be positive");
    }
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
    if (input_channels <= 0 || output_channels <= 0) {
        throw std::runtime_error("ASIO meter requires at least one input and one output channel");
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

    ASIOChannelInfo input_info{};
    input_info.channel = 0;
    input_info.isInput = ASIOTrue;
    require_asio_ok(driver.get()->getChannelInfo(&input_info), "ASIO getChannelInfo input");

    ASIOChannelInfo output_info{};
    output_info.channel = 0;
    output_info.isInput = ASIOFalse;
    require_asio_ok(driver.get()->getChannelInfo(&output_info), "ASIO getChannelInfo output");
    if (sample_bytes(input_info.type) == 0 || sample_bytes(output_info.type) == 0) {
        throw std::runtime_error("ASIO sample type is not supported by the meter probe");
    }

    std::array<ASIOBufferInfo, 2> buffers{};
    buffers[0].isInput = ASIOTrue;
    buffers[0].channelNum = 0;
    buffers[1].isInput = ASIOFalse;
    buffers[1].channelNum = 0;

    MeterContext context;
    context.input = &buffers[0];
    context.output = &buffers[1];
    context.buffer_size = buffer_size;
    context.input_type = input_info.type;
    context.output_type = output_info.type;

    ASIOCallbacks callbacks{};
    callbacks.bufferSwitch = meter_buffer_switch;
    callbacks.sampleRateDidChange = meter_sample_rate_changed;
    callbacks.asioMessage = meter_asio_message;
    callbacks.bufferSwitchTimeInfo = meter_buffer_switch_time_info;

    AsioBuffers session(driver.get());
    g_meter_context = &context;
    require_asio_ok(driver.get()->createBuffers(buffers.data(), static_cast<long>(buffers.size()), buffer_size, &callbacks), "ASIO createBuffers");
    session.mark_created();
    require_asio_ok(driver.get()->start(), "ASIO start");
    session.mark_started();
    Sleep(static_cast<DWORD>(duration_ms));

    DeviceMeterResult result;
    result.device = device;
    result.sample_rate = current_rate;
    result.buffer_size = buffer_size;
    result.callbacks = context.callbacks.load(std::memory_order_relaxed);
    result.input_sample_type = input_info.type;
    result.output_sample_type = output_info.type;
    result.input_peak = static_cast<double>(context.peak_ppm.load(std::memory_order_relaxed)) / 1000000.0;
    return result;
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
    if (input_channels <= 0 || output_channels <= 0) {
        throw std::runtime_error("ASIO ring validation requires at least one input and one output channel");
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
    if (ring_capacity_frames < static_cast<std::size_t>(buffer_size)) {
        throw std::runtime_error("ring capacity must be at least one ASIO buffer");
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

    ASIOChannelInfo input_info{};
    input_info.channel = 0;
    input_info.isInput = ASIOTrue;
    require_asio_ok(driver.get()->getChannelInfo(&input_info), "ASIO getChannelInfo input");

    ASIOChannelInfo output_info{};
    output_info.channel = 0;
    output_info.isInput = ASIOFalse;
    require_asio_ok(driver.get()->getChannelInfo(&output_info), "ASIO getChannelInfo output");
    if (input_info.type != ASIOSTInt32LSB || output_info.type != ASIOSTInt32LSB) {
        throw std::runtime_error("ASIO ring validation currently supports Int32LSB input/output only");
    }

    std::array<ASIOBufferInfo, 2> buffers{};
    buffers[0].isInput = ASIOTrue;
    buffers[0].channelNum = 0;
    buffers[1].isInput = ASIOFalse;
    buffers[1].channelNum = 0;

    MonoRingBuffer ring(ring_capacity_frames);
    RingContext context;
    context.input = &buffers[0];
    context.output = &buffers[1];
    context.buffer_size = buffer_size;
    context.input_type = input_info.type;
    context.output_type = output_info.type;
    context.ring = &ring;

    ASIOCallbacks callbacks{};
    callbacks.bufferSwitch = ring_buffer_switch;
    callbacks.sampleRateDidChange = meter_sample_rate_changed;
    callbacks.asioMessage = ring_asio_message;
    callbacks.bufferSwitchTimeInfo = ring_buffer_switch_time_info;

    AsioBuffers session(driver.get());
    g_ring_context = &context;
    require_asio_ok(driver.get()->createBuffers(buffers.data(), static_cast<long>(buffers.size()), buffer_size, &callbacks), "ASIO createBuffers");
    session.mark_created();
    require_asio_ok(driver.get()->start(), "ASIO start");
    session.mark_started();
    Sleep(static_cast<DWORD>(duration_ms));

    const auto ring_stats = ring.stats();
    DeviceRingResult result;
    result.device = device;
    result.sample_rate = current_rate;
    result.buffer_size = buffer_size;
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
    long buffer_size,
    InputChannels requested_input_channels,
    ChannelSelection channels,
    MonoRingBuffer& capture_ring,
    MonoRingBuffer& playback_ring,
    std::size_t playback_prefill_frames,
    StreamControl& control,
    OutputRecorder* recorder)
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
        playback_ring,
        playback_prefill_frames,
        control,
        recorder,
        current_rate);
    stream->start();
    return stream;
}

} // namespace jam2::audio

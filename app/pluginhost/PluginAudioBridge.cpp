#include "PluginAudioBridge.hpp"

#include "common.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace jam2::pluginhost {
namespace {

float i32_to_float(std::int32_t value) noexcept
{
    return static_cast<float>(static_cast<double>(value) / 2147483648.0);
}

std::int32_t float_to_i32(float value) noexcept
{
    const double clamped = std::clamp(static_cast<double>(value), -1.0, 1.0);
    if (clamped <= -1.0) return (std::numeric_limits<std::int32_t>::min)();
    return static_cast<std::int32_t>(std::llround(clamped * 2147483647.0));
}

} // namespace

PluginAudioBridge::PluginAudioBridge(SharedState& shared, std::size_t maximum_frames,
    jam2::audio::InputSourceKind kind)
    : shared_(shared), maximum_frames_(std::min(maximum_frames, kMaximumFrames)),
      kind_(kind), delayed_dry_(maximum_frames_, 0)
{
    dry_delay_ring_.assign(kMaximumPluginLatencyFrames +
        kIsolationPipelineBlocks * maximum_frames_ + 1U, 0);
}

void PluginAudioBridge::set_bypassed(bool value) noexcept
{
    bypassed_.store(value, std::memory_order_release);
}

bool PluginAudioBridge::bypassed() const noexcept
{
    return bypassed_.load(std::memory_order_acquire);
}

std::size_t PluginAudioBridge::latency_frames(std::size_t block_frames) const noexcept
{
    const std::size_t plugin_latency = (std::min<std::size_t>)(
        shared_.plugin_latency_frames.load(std::memory_order_relaxed),
        kMaximumPluginLatencyFrames);
    return kIsolationPipelineBlocks * block_frames + plugin_latency;
}

void PluginAudioBridge::set_midi_queue(jam2::midi::EventQueue* queue) noexcept
{
    midi_queue_.store(queue, std::memory_order_release);
    last_midi_dropped_ = queue ? queue->dropped() : 0;
    midi_builder_.reset();
}

void PluginAudioBridge::set_muted(bool value) noexcept
{
    muted_.store(value, std::memory_order_release);
}

void PluginAudioBridge::request_midi_reset() noexcept
{
    midi_reset_requested_.store(true, std::memory_order_release);
}

bool PluginAudioBridge::consume(std::uint64_t expected_sequence, std::size_t frames,
    std::span<std::int32_t> output) noexcept
{
    if (expected_sequence == 0) return false;
    TransportSlot* selected = nullptr;
    for (auto& slot : shared_.transport_blocks) {
        const std::uint64_t generation = slot.response_generation.load(std::memory_order_acquire);
        if (generation == expected_sequence * 2U &&
            slot.response_frames == frames) {
            selected = &slot;
            break;
        }
    }
    if (!selected) return false;

    const std::uint64_t before = selected->response_generation.load(std::memory_order_acquire);
    if ((before & 1U) != 0U || selected->process_ok == 0U) {
        if (selected->process_ok == 0U) failed_blocks_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const std::size_t channels = selected->output_channels;
    if (channels == 0 || channels > 2) return false;
    double wet_peak = 0.0;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        float sample = selected->output[0][frame];
        if (channels == 2) sample = 0.5f * (sample + selected->output[1][frame]);
        if (!std::isfinite(sample)) {
            failed_blocks_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        wet_peak = std::max(wet_peak, std::abs(static_cast<double>(sample)));
        output[frame] = float_to_i32(sample);
    }
    const std::uint64_t after = selected->response_generation.load(std::memory_order_acquire);
    if (before != after) {
        stale_responses_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto wet_peak_ppm = static_cast<std::uint32_t>(std::llround(
        std::min(1.0, wet_peak) * 1000000.0));
    std::uint32_t maximum = maximum_wet_output_peak_ppm_.load(
        std::memory_order_relaxed);
    while (wet_peak_ppm > maximum &&
           !maximum_wet_output_peak_ppm_.compare_exchange_weak(
               maximum, wet_peak_ppm, std::memory_order_relaxed)) {}
    completed_blocks_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void PluginAudioBridge::publish(const jam2::audio::InputSourceRenderRequest& request) noexcept
{
    const std::uint64_t sequence = ++sequence_;
    auto& slot = shared_.transport_blocks[sequence % shared_.transport_blocks.size()];
    slot.request_generation.store(sequence * 2U - 1U, std::memory_order_release);
    slot.engine_frame = request.engine_frame;
    slot.frames = static_cast<std::uint32_t>(request.frames);
    slot.input_channels = static_cast<std::uint32_t>(request.input_channels);
    for (std::size_t channel = 0; channel < 2; ++channel) {
        const auto* source = channel < request.input_channels ? request.inputs[channel] : nullptr;
        for (std::size_t frame = 0; frame < request.frames; ++frame)
            slot.input[channel][frame] = source ? i32_to_float(source[frame]) : 0.0f;
    }

    slot.midi_count = 0;
    slot.midi_live_count = 0;
    auto* queue = midi_queue_.load(std::memory_order_acquire);
    const std::uint64_t dropped_before = queue ? queue->dropped() : last_midi_dropped_;
    const bool reset_midi = midi_reset_requested_.exchange(false, std::memory_order_acq_rel) ||
        dropped_before != last_midi_dropped_;
    if (reset_midi) {
        for (std::uint8_t channel = 0; channel < 16; ++channel) {
            slot.midi[slot.midi_count++] = {
                0U, static_cast<std::uint8_t>(0xb0U | channel), 121U, 0U, {}};
            slot.midi[slot.midi_count++] = {
                0U, static_cast<std::uint8_t>(0xb0U | channel), 123U, 0U, {}};
        }
        last_midi_dropped_ = dropped_before;
    }
    if (queue) {
        const std::uint64_t start = jam2::monotonic_us();
        const auto duration = static_cast<std::uint64_t>(std::llround(
            static_cast<double>(request.frames) * 1000000.0 /
            std::max(1.0, request.sample_rate)));
        const std::size_t reset_count = slot.midi_count;
        const auto result = midi_builder_.build(*queue, start, start + duration,
            static_cast<std::uint32_t>(request.frames),
            std::span<jam2::midi::Event>(midi_scratch_).subspan(reset_count));
        slot.midi_count += static_cast<std::uint32_t>(result.count);
        slot.midi_live_count = static_cast<std::uint32_t>(result.count);
        midi_late_.fetch_add(result.late, std::memory_order_relaxed);
        midi_deferred_.fetch_add(result.deferred, std::memory_order_relaxed);
        for (std::size_t index = 0; index < result.count; ++index) {
            const auto& source = midi_scratch_[reset_count + index];
            slot.midi[reset_count + index] = {
                source.sample_offset, source.status, source.data1, source.data2, {}};
        }
    }
    slot.request_generation.store(sequence * 2U, std::memory_order_release);
    submitted_blocks_.fetch_add(1, std::memory_order_relaxed);
}

void PluginAudioBridge::render_delayed_dry(
    std::size_t frames, std::span<std::int32_t> output) noexcept
{
    if (kind_ == jam2::audio::InputSourceKind::Audio)
        std::copy_n(delayed_dry_.begin(), frames, output.begin());
    else
        std::fill_n(output.begin(), frames, 0);
}

void PluginAudioBridge::capture_dry(const jam2::audio::InputSourceRenderRequest& request) noexcept
{
    const std::size_t delay = latency_frames(request.frames);
    for (std::size_t frame = 0; frame < request.frames; ++frame) {
        std::int32_t dry = 0;
        if (request.input_channels == 1 && request.inputs[0]) dry = request.inputs[0][frame];
        else if (request.input_channels >= 2 && request.inputs[0] && request.inputs[1]) dry = static_cast<std::int32_t>(
            (static_cast<std::int64_t>(request.inputs[0][frame]) + request.inputs[1][frame]) / 2);
        const std::size_t read = (dry_delay_write_ + dry_delay_ring_.size() - delay) %
            dry_delay_ring_.size();
        delayed_dry_[frame] = dry_delay_ring_[read];
        dry_delay_ring_[dry_delay_write_] = dry;
        dry_delay_write_ = (dry_delay_write_ + 1U) % dry_delay_ring_.size();
    }
}

bool PluginAudioBridge::render_mono(
    const jam2::audio::InputSourceRenderRequest& request,
    std::span<std::int32_t> output) noexcept
{
    if (request.frames == 0 || request.frames > maximum_frames_ || output.size() < request.frames)
        return false;
    capture_dry(request);
    // Consume the response at the fixed isolation deadline. A late
    // response is never inserted into a newer point in the stream: doing that
    // alternated old wet audio with the aligned fallback and was heard as
    // harsh, glitchy noise when a plugin missed a deadline.
    const std::uint64_t expected = sequence_ >= kIsolationPipelineBlocks
        ? sequence_ - (kIsolationPipelineBlocks - 1U) : 0U;
    const bool got_plugin = consume(expected, request.frames, output.first(request.frames));
    const bool bypass = bypassed();
    const bool muted = kind_ == jam2::audio::InputSourceKind::MidiInstrument &&
        muted_.load(std::memory_order_acquire);
    const bool conceal_miss = !got_plugin && expected != 0U && !bypass && !muted &&
        previous_output_wet_ && !concealed_previous_miss_;
    if (conceal_miss) {
        std::fill_n(output.begin(), request.frames, previous_output_sample_);
        deadline_concealments_.fetch_add(1, std::memory_order_relaxed);
    } else if (bypass || !got_plugin) {
        render_delayed_dry(request.frames, output.first(request.frames));
    }
    if (!got_plugin && expected != 0U)
        deadline_misses_.fetch_add(1, std::memory_order_relaxed);
    if (muted) {
        std::fill_n(output.begin(), request.frames, 0);
    }
    const bool wet = got_plugin && !bypass && !muted;
    const bool continuous_wet = wet || conceal_miss;
    if (have_previous_output_ && continuous_wet != previous_output_wet_) {
        const std::size_t fade_frames = (std::min<std::size_t>)(32U, request.frames);
        for (std::size_t frame = 0; frame < fade_frames; ++frame) {
            const std::int64_t old_weight = static_cast<std::int64_t>(fade_frames - frame);
            const std::int64_t new_weight = static_cast<std::int64_t>(frame + 1U);
            output[frame] = static_cast<std::int32_t>((
                static_cast<std::int64_t>(previous_output_sample_) * old_weight +
                static_cast<std::int64_t>(output[frame]) * new_weight) /
                static_cast<std::int64_t>(fade_frames + 1U));
        }
    }
    previous_output_sample_ = output[request.frames - 1U];
    previous_output_wet_ = continuous_wet;
    have_previous_output_ = true;
    if (got_plugin || bypass || muted) concealed_previous_miss_ = false;
    else if (conceal_miss) concealed_previous_miss_ = true;
    publish(request);
    return true;
}

PluginBridgeStats PluginAudioBridge::stats() const noexcept
{
    auto* queue = midi_queue_.load(std::memory_order_acquire);
    const std::uint64_t processed = shared_.processed_blocks.load(std::memory_order_relaxed);
    return {
        submitted_blocks_.load(std::memory_order_relaxed),
        completed_blocks_.load(std::memory_order_relaxed),
        deadline_misses_.load(std::memory_order_relaxed),
        deadline_concealments_.load(std::memory_order_relaxed),
        failed_blocks_.load(std::memory_order_relaxed),
        stale_responses_.load(std::memory_order_relaxed),
        midi_late_.load(std::memory_order_relaxed),
        midi_deferred_.load(std::memory_order_relaxed),
        queue ? queue->dropped() : 0,
        shared_.midi_events_consumed.load(std::memory_order_relaxed),
        shared_.plugin_latency_frames.load(std::memory_order_relaxed),
        shared_.negotiated_input_channels.load(std::memory_order_relaxed),
        shared_.negotiated_output_channels.load(std::memory_order_relaxed),
        maximum_frames_ * kIsolationPipelineBlocks,
        shared_.process_time_last_us.load(std::memory_order_relaxed),
        processed > 0 ? shared_.process_time_sum_us.load(std::memory_order_relaxed) / processed : 0,
        shared_.process_time_max_us.load(std::memory_order_relaxed),
        queue ? queue->depth() : 0,
        queue ? queue->high_water() : 0,
        shared_.worker_input_peak_ppm.load(std::memory_order_relaxed),
        maximum_wet_output_peak_ppm_.load(std::memory_order_relaxed),
    };
}

} // namespace jam2::pluginhost

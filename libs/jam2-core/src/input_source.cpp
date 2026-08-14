#include "input_source.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace jam2::audio {

InputSourceRouter::InputSourceRouter(
    std::size_t maximum_frames,
    std::size_t physical_channels)
    : maximum_frames_(maximum_frames),
      physical_channels_(physical_channels),
      source_scratch_(maximum_frames, 0),
      mix_scratch_(maximum_frames, 0),
      recording_scratch_(maximum_frames, 0)
{
}

void InputSourceRouter::set_recording_source(std::size_t slot) noexcept
{
    recording_slot_.store(slot < slots_.size() ? slot : kCombinedInputSources,
        std::memory_order_release);
}

bool InputSourceRouter::copy_recording_source(
    std::size_t frames, std::span<std::int32_t> output) const noexcept
{
    if (frames == 0 || frames > recording_scratch_.size() || output.size() < frames ||
        !recording_source_ready_.load(std::memory_order_acquire)) return false;
    std::copy_n(recording_scratch_.begin(), frames, output.begin());
    return true;
}

bool InputSourceRouter::valid(const InputSourceConfiguration& source) const noexcept
{
    if (source.level_ppm < 0 || source.level_ppm > 4000000) return false;
    if (source.kind == InputSourceKind::MidiInstrument) {
        return source.first_channel == kNoInputChannel &&
            source.second_channel == kNoInputChannel && source.renderer != nullptr;
    }
    if (source.first_channel >= physical_channels_) return false;
    return source.second_channel == kNoInputChannel ||
        (source.second_channel < physical_channels_ &&
         source.second_channel != source.first_channel);
}

bool InputSourceRouter::configure(
    std::size_t slot,
    const InputSourceConfiguration& source) noexcept
{
    if (slot >= slots_.size() || !valid(source)) {
        invalid_configurations_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    Slot& destination = slots_[slot];
    destination.topology_revision.fetch_add(1, std::memory_order_acq_rel);
    destination.enabled.store(false, std::memory_order_release);
    destination.configured.store(false, std::memory_order_release);
    destination.kind.store(source.kind, std::memory_order_relaxed);
    destination.first_channel.store(source.first_channel, std::memory_order_relaxed);
    destination.second_channel.store(source.second_channel, std::memory_order_relaxed);
    destination.level_ppm.store(source.level_ppm, std::memory_order_relaxed);
    destination.renderer.store(source.renderer, std::memory_order_release);
    destination.configured.store(true, std::memory_order_release);
    destination.enabled.store(source.enabled, std::memory_order_release);
    destination.topology_revision.fetch_add(1, std::memory_order_release);
    return true;
}

bool InputSourceRouter::set_enabled(std::size_t slot, bool enabled) noexcept
{
    if (slot >= slots_.size() ||
        !slots_[slot].configured.load(std::memory_order_acquire)) {
        return false;
    }
    slots_[slot].enabled.store(enabled, std::memory_order_release);
    return true;
}

bool InputSourceRouter::set_level(std::size_t slot, int level_ppm) noexcept
{
    if (slot >= slots_.size() || level_ppm < 0 || level_ppm > 4000000 ||
        !slots_[slot].configured.load(std::memory_order_acquire)) {
        return false;
    }
    slots_[slot].level_ppm.store(level_ppm, std::memory_order_release);
    return true;
}

void InputSourceRouter::clear(std::size_t slot) noexcept
{
    if (slot >= slots_.size()) return;
    Slot& source = slots_[slot];
    source.topology_revision.fetch_add(1, std::memory_order_acq_rel);
    source.enabled.store(false, std::memory_order_release);
    source.renderer.store(nullptr, std::memory_order_release);
    source.first_channel.store(kNoInputChannel, std::memory_order_relaxed);
    source.second_channel.store(kNoInputChannel, std::memory_order_relaxed);
    source.configured.store(false, std::memory_order_release);
    source.topology_revision.fetch_add(1, std::memory_order_release);
}

bool InputSourceRouter::process(
    std::span<const std::int32_t* const> physical_inputs,
    std::size_t frames,
    std::uint64_t engine_frame,
    double sample_rate,
    std::span<std::int32_t> mono_output) noexcept
{
    if (frames == 0 || frames > maximum_frames_ || mono_output.size() < frames ||
        physical_inputs.size() < physical_channels_) {
        invalid_configurations_.fetch_add(1, std::memory_order_relaxed);
        if (!mono_output.empty()) std::fill(mono_output.begin(), mono_output.end(), 0);
        return false;
    }
    std::fill(mix_scratch_.begin(), mix_scratch_.begin() + frames, 0);
    recording_source_ready_.store(false, std::memory_order_relaxed);
    const std::size_t recording_slot = recording_slot_.load(std::memory_order_acquire);
    std::size_t selected_recording_latency = 0;
    std::size_t combined_recording_latency = 0;
    std::size_t rendered_sources = 0;
    for (std::size_t slot_index = 0; slot_index < slots_.size(); ++slot_index) {
        Slot& source = slots_[slot_index];
        const std::uint64_t revision =
            source.topology_revision.load(std::memory_order_acquire);
        if ((revision & 1U) != 0U) continue;
        const bool configured = source.configured.load(std::memory_order_acquire);
        const InputSourceKind kind = source.kind.load(std::memory_order_relaxed);
        const bool included = source.enabled.load(std::memory_order_acquire);
        const std::size_t first = source.first_channel.load(std::memory_order_relaxed);
        const std::size_t second = source.second_channel.load(std::memory_order_relaxed);
        InputSourceRenderer* renderer = source.renderer.load(std::memory_order_acquire);
        const std::int64_t level = std::clamp(
            source.level_ppm.load(std::memory_order_relaxed), 0, 4000000);
        if (source.topology_revision.load(std::memory_order_acquire) != revision ||
            !configured) continue;
        // MIDI renderers must continue draining timestamped controller events
        // while excluded from My Send, otherwise mute/send-off fills the queue
        // and replays stale notes when the source is enabled again.
        if (!included && slot_index != recording_slot &&
            kind != InputSourceKind::MidiInstrument) continue;
        std::array<const std::int32_t*, kMaximumSourceInputChannels> inputs{};
        std::size_t input_channels = 0;
        if (kind == InputSourceKind::Audio) {
            if (first >= physical_inputs.size()) continue;
            inputs[input_channels++] = physical_inputs[first];
            if (second != kNoInputChannel && second < physical_inputs.size()) {
                inputs[input_channels++] = physical_inputs[second];
            }
        }

        bool rendered = false;
        const std::size_t source_latency = renderer != nullptr
            ? renderer->latency_frames(frames)
            : 0;
        if (renderer != nullptr) {
            std::fill(source_scratch_.begin(), source_scratch_.begin() + frames, 0);
            rendered = renderer->render_mono(
                {inputs, input_channels, frames, engine_frame, sample_rate},
                std::span<std::int32_t>(source_scratch_.data(), frames));
            if (!rendered) {
                renderer_failures_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (!rendered) {
            if (kind == InputSourceKind::MidiInstrument) {
                std::fill(source_scratch_.begin(), source_scratch_.begin() + frames, 0);
            } else if (input_channels == 1 && inputs[0] != nullptr) {
                std::copy(inputs[0], inputs[0] + frames, source_scratch_.begin());
            } else if (input_channels == 2 && inputs[0] != nullptr && inputs[1] != nullptr) {
                for (std::size_t frame = 0; frame < frames; ++frame) {
                    source_scratch_[frame] = static_cast<std::int32_t>(
                        (static_cast<std::int64_t>(inputs[0][frame]) +
                         static_cast<std::int64_t>(inputs[1][frame])) / 2);
                }
            } else {
                std::fill(source_scratch_.begin(), source_scratch_.begin() + frames, 0);
            }
        }
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const std::int64_t levelled =
                static_cast<std::int64_t>(source_scratch_[frame]) * level / 1000000LL;
            if (included) mix_scratch_[frame] += levelled;
            if (slot_index == recording_slot) {
                recording_scratch_[frame] = static_cast<std::int32_t>(std::clamp<std::int64_t>(
                    levelled, std::numeric_limits<std::int32_t>::min(),
                    std::numeric_limits<std::int32_t>::max()));
            }
        }
        if (slot_index == recording_slot)
            recording_source_ready_.store(true, std::memory_order_release);
        if (slot_index == recording_slot) selected_recording_latency = source_latency;
        if (included) {
            combined_recording_latency = std::max(combined_recording_latency, source_latency);
            ++rendered_sources;
        }
    }

    recording_latency_frames_.store(
        recording_slot == kCombinedInputSources
            ? combined_recording_latency
            : selected_recording_latency,
        std::memory_order_relaxed);

    std::uint64_t absolute_peak = 0;
    const std::int64_t divisor = static_cast<std::int64_t>(std::max<std::size_t>(1, rendered_sources));
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::int64_t mixed = mix_scratch_[frame] / divisor;
        const auto sample = static_cast<std::int32_t>(std::clamp<std::int64_t>(
            mixed,
            std::numeric_limits<std::int32_t>::min(),
            std::numeric_limits<std::int32_t>::max()));
        mono_output[frame] = sample;
        const std::uint64_t magnitude = sample == std::numeric_limits<std::int32_t>::min()
            ? static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())
            : static_cast<std::uint64_t>(std::abs(sample));
        absolute_peak = std::max(absolute_peak, magnitude);
    }
    peak_ppm_.store(static_cast<int>(absolute_peak * 1000000ULL / 2147483647ULL),
        std::memory_order_relaxed);
    rendered_blocks_.fetch_add(1, std::memory_order_relaxed);
    return rendered_sources > 0;
}

InputSourceRouterStats InputSourceRouter::stats() const noexcept
{
    InputSourceRouterStats result;
    for (const Slot& slot : slots_) {
        if (slot.configured.load(std::memory_order_acquire)) ++result.configured_sources;
    }
    result.rendered_blocks = rendered_blocks_.load(std::memory_order_relaxed);
    result.renderer_failures = renderer_failures_.load(std::memory_order_relaxed);
    result.invalid_configurations = invalid_configurations_.load(std::memory_order_relaxed);
    result.peak_ppm = peak_ppm_.load(std::memory_order_relaxed);
    return result;
}

InputSourceSlotSnapshot InputSourceRouter::slot_snapshot(
    std::size_t slot) const noexcept
{
    if (slot >= slots_.size()) return {};
    const Slot& source = slots_[slot];
    for (int attempt = 0; attempt < 3; ++attempt) {
        const std::uint64_t revision =
            source.topology_revision.load(std::memory_order_acquire);
        if ((revision & 1U) != 0U) continue;
        const InputSourceSlotSnapshot result{
            source.kind.load(std::memory_order_relaxed),
            source.first_channel.load(std::memory_order_relaxed),
            source.second_channel.load(std::memory_order_relaxed),
            source.level_ppm.load(std::memory_order_relaxed),
            source.configured.load(std::memory_order_acquire),
            source.enabled.load(std::memory_order_acquire),
            source.renderer.load(std::memory_order_acquire) != nullptr,
        };
        if (source.topology_revision.load(std::memory_order_acquire) == revision)
            return result;
    }
    return {};
}

} // namespace jam2::audio

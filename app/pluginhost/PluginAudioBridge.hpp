#pragma once

#include "PluginProtocol.hpp"
#include "input_source.hpp"
#include "midi.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace jam2::pluginhost {

struct PluginBridgeStats {
    std::uint64_t submitted_blocks = 0;
    std::uint64_t completed_blocks = 0;
    std::uint64_t deadline_misses = 0;
    std::uint64_t failed_blocks = 0;
    std::uint64_t stale_responses = 0;
    std::uint64_t midi_late = 0;
    std::uint64_t midi_deferred = 0;
    std::uint64_t midi_dropped = 0;
    std::uint32_t worker_latency_frames = 0;
    std::uint32_t negotiated_input_channels = 0;
    std::uint32_t negotiated_output_channels = 0;
    std::size_t isolation_latency_frames = 0;
    std::uint64_t worker_process_last_us = 0;
    std::uint64_t worker_process_average_us = 0;
    std::uint64_t worker_process_max_us = 0;
    std::size_t midi_queue_depth = 0;
    std::size_t midi_queue_high_water = 0;
};

class PluginAudioBridge final : public jam2::audio::InputSourceRenderer {
public:
    PluginAudioBridge(SharedState& shared, std::size_t maximum_frames,
        jam2::audio::InputSourceKind kind);

    bool render_mono(const jam2::audio::InputSourceRenderRequest& request,
        std::span<std::int32_t> output) noexcept override;

    void set_bypassed(bool bypassed) noexcept;
    bool bypassed() const noexcept;
    void set_midi_queue(jam2::midi::EventQueue* queue) noexcept;
    void set_muted(bool muted) noexcept;
    void request_midi_reset() noexcept;
    PluginBridgeStats stats() const noexcept;

private:
    bool consume(std::uint64_t expected_sequence, std::size_t frames,
        std::span<std::int32_t> output) noexcept;
    void publish(const jam2::audio::InputSourceRenderRequest& request) noexcept;
    void render_delayed_dry(std::size_t frames, std::span<std::int32_t> output) noexcept;
    void capture_dry(const jam2::audio::InputSourceRenderRequest& request) noexcept;

    SharedState& shared_;
    std::size_t maximum_frames_ = 0;
    jam2::audio::InputSourceKind kind_ = jam2::audio::InputSourceKind::Audio;
    std::vector<std::int32_t> delayed_dry_;
    std::vector<std::int32_t> dry_delay_ring_;
    std::array<jam2::midi::Event, jam2::midi::kMaximumEventsPerBlock> midi_scratch_{};
    jam2::midi::BlockBuilder midi_builder_;
    std::atomic<jam2::midi::EventQueue*> midi_queue_{nullptr};
    std::atomic<bool> bypassed_{false};
    std::atomic<bool> muted_{false};
    std::atomic<bool> midi_reset_requested_{false};
    std::uint64_t sequence_ = 0;
    std::size_t dry_delay_write_ = 0;
    std::int32_t previous_output_sample_ = 0;
    bool previous_output_wet_ = false;
    bool have_previous_output_ = false;
    std::uint64_t last_midi_dropped_ = 0;
    std::atomic<std::uint64_t> submitted_blocks_{0};
    std::atomic<std::uint64_t> completed_blocks_{0};
    std::atomic<std::uint64_t> deadline_misses_{0};
    std::atomic<std::uint64_t> failed_blocks_{0};
    std::atomic<std::uint64_t> stale_responses_{0};
    std::atomic<std::uint64_t> midi_late_{0};
    std::atomic<std::uint64_t> midi_deferred_{0};
};

} // namespace jam2::pluginhost

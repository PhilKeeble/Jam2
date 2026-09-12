#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "peer_stream.hpp"

namespace jam2 {

struct PeerMixerConfig {
    int sample_rate = 48000;
    int frames_per_block = 128;
    std::size_t deadline_frames = 0;
    std::size_t output_max_frames = 0;
    std::size_t max_blocks_per_advance = 64;
    std::size_t fixed_target_frames = 0;
    bool adaptive_playback_cushion = false;
    std::size_t adaptive_target_frames = 0;
    std::size_t adaptive_min_frames = 0;
    std::size_t adaptive_max_frames = 0;
    int adaptive_release_ppm = 5000;
    // Continuous callback-safe surplus required before release at minimum;
    // zero disables minimum-target surplus release (not raised-cushion release).
    std::uint64_t adaptive_surplus_stable_us = 1000000;
    std::size_t receive_recovery_trigger_frames = 256;
    // Fixed source-only edge allowance; never padding or an expanding target.
    std::uint64_t live_tail_rounding_tolerance_us = 2000;
};

struct PeerMixerPeerStats {
    std::uint64_t peer_id = 0;
    bool active = false;
    bool contributing = false;
    bool muted = false;
    int gain_ppm = 1000000;
    int recent_peak_ppm = 0;
    std::uint64_t queue_capacity_frames = 0;
    std::uint64_t queue_depth_frames = 0;
    std::uint64_t queue_high_water_frames = 0;
    std::uint64_t queue_capacity_drops = 0;
    std::uint64_t queue_capacity_dropped_frames = 0;
    std::uint64_t requested_drop_frames = 0;
    std::uint64_t late_after_release_frames = 0;
    std::uint64_t live_tail_trim_events = 0;
    std::uint64_t live_tail_trimmed_frames = 0;
    std::uint64_t live_tail_trim_max_frames = 0;
    std::uint64_t resampled_output_frames = 0;
    std::uint64_t unity_resampler_fast_frames = 0;
    double resampler_ratio = 1.0;
};

struct PeerMixerStats {
    std::uint64_t active_peers = 0;
    std::uint64_t contributing_peers = 0;
    std::uint64_t active_slots = 0;
    std::uint64_t max_slots = 0;
    std::uint64_t active_slots_high_water = 0;
    std::uint64_t released_slots = 0;
    std::uint64_t complete_slots = 0;
    std::uint64_t deadline_slots = 0;
    std::uint64_t missing_peer_contributions = 0;
    std::uint64_t missing_peer_frames = 0;
    std::uint64_t late_after_release_frames = 0;
    std::uint64_t live_tail_trim_events = 0;
    std::uint64_t live_tail_trimmed_frames = 0;
    std::uint64_t live_tail_trim_max_frames = 0;
    std::uint64_t capacity_drops = 0;
    std::uint64_t capacity_dropped_frames = 0;
    std::uint64_t clipped_samples = 0;
    std::uint64_t output_frames = 0;
    std::uint64_t output_drop_requested_frames = 0;
    std::uint64_t output_drop_request_events = 0;
    std::uint64_t output_dropped_frames = 0;
    std::uint64_t work_budget_yields = 0;
    bool receive_recovery_active = false;
    std::uint64_t receive_recovery_events = 0;
    std::uint64_t receive_recovery_completions = 0;
    std::uint64_t receive_recovery_debt_frames = 0;
    std::uint64_t receive_recovery_debt_max_frames = 0;
    std::uint64_t receive_recovery_duration_us = 0;
    std::uint64_t receive_recovery_duration_max_us = 0;
    std::uint64_t receive_recovery_trigger_frames = 0;
    bool adaptive_playback_cushion_enabled = false;
    std::uint64_t adaptive_target_frames = 0;
    std::uint64_t adaptive_raise_events = 0;
    std::uint64_t adaptive_release_events = 0;
    std::uint64_t adaptive_padding_frames = 0;
    bool adaptive_delivery_gap_hold_active = false;
    std::uint64_t adaptive_delivery_gap_raise_events = 0;
    std::uint64_t adaptive_delivery_gap_hold_events = 0;
    std::uint64_t adaptive_delivery_gap_hold_remaining_us = 0;
    std::uint64_t adaptive_delivery_gap_required_frames = 0;
    std::uint64_t adaptive_delivery_gap_required_max_frames = 0;
    std::uint64_t live_tail_batch_tolerance_frames = 0;
    std::uint64_t live_tail_rounding_tolerance_frames = 0;
    std::uint64_t adaptive_release_alignment_margin_frames = 0;
    // Ordinary servicing limits, excluding peer jitter storage. Hard receive
    // recovery uses its own smaller tails; receive ingestion can transiently
    // exceed this path limit before the bounded batch is serviced.
    std::uint64_t normal_output_limit_frames = 0;
    std::uint64_t normal_path_limit_frames = 0;
};

// Network-thread mixer. Each peer has a fixed-capacity local-timeline queue and
// an independent streaming resampler. Mixing uses one preallocated wide
// accumulator and performs one final int32 saturation per output sample.
class PeerMixer {
public:
    PeerMixer(const PeerMixerConfig& config, PeerStreamPlayback* output);
    ~PeerMixer();

    PeerMixer(const PeerMixer&) = delete;
    PeerMixer& operator=(const PeerMixer&) = delete;
    PeerMixer(PeerMixer&&) noexcept;
    PeerMixer& operator=(PeerMixer&&) noexcept;

    PeerStreamPlayback* addPeer(std::uint64_t peer_id, std::size_t queue_capacity_frames);
    bool removePeer(std::uint64_t peer_id) noexcept;
    bool setPeerActive(std::uint64_t peer_id, bool active) noexcept;
    bool setPeerGain(std::uint64_t peer_id, int gain_ppm) noexcept;
    bool setPeerMuted(std::uint64_t peer_id, bool muted) noexcept;

    void advance(std::uint64_t now_us) noexcept;
    void finishReceiveBatch(bool receive_budget_exhausted) noexcept;
    void finish(std::uint64_t now_us) noexcept;

    const PeerMixerStats& stats() const noexcept;
    const PeerMixerPeerStats* peerStats(std::uint64_t peer_id) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace jam2

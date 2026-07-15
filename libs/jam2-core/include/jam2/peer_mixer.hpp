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
    bool adaptive_playback_cushion = false;
    std::size_t adaptive_target_frames = 0;
    std::size_t adaptive_min_frames = 0;
    std::size_t adaptive_max_frames = 0;
    int adaptive_release_ppm = 5000;
};

struct PeerMixerPeerStats {
    std::uint64_t peer_id = 0;
    bool active = false;
    bool contributing = false;
    bool muted = false;
    int gain_ppm = 1000000;
    std::uint64_t queue_capacity_frames = 0;
    std::uint64_t queue_depth_frames = 0;
    std::uint64_t queue_high_water_frames = 0;
    std::uint64_t queue_capacity_drops = 0;
    std::uint64_t queue_capacity_dropped_frames = 0;
    std::uint64_t requested_drop_frames = 0;
    std::uint64_t late_after_release_frames = 0;
    std::uint64_t resampled_output_frames = 0;
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
    std::uint64_t capacity_drops = 0;
    std::uint64_t capacity_dropped_frames = 0;
    std::uint64_t clipped_samples = 0;
    std::uint64_t output_frames = 0;
    std::uint64_t output_drop_requested_frames = 0;
    std::uint64_t output_drop_request_events = 0;
    std::uint64_t output_dropped_frames = 0;
    std::uint64_t work_budget_yields = 0;
    bool adaptive_playback_cushion_enabled = false;
    std::uint64_t adaptive_target_frames = 0;
    std::uint64_t adaptive_raise_events = 0;
    std::uint64_t adaptive_release_events = 0;
    std::uint64_t adaptive_padding_frames = 0;
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
    void finish(std::uint64_t now_us) noexcept;

    const PeerMixerStats& stats() const noexcept;
    const PeerMixerPeerStats* peerStats(std::uint64_t peer_id) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace jam2

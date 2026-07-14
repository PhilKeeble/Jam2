#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "protocol.hpp"

namespace jam2 {

// Immutable local tuning for one remote audio clock. Wire/session values are
// validated by NetworkSession before a PeerStream is constructed.
struct PeerStreamConfig {
    int sample_rate = 48000;
    int frames_per_packet = 128;
    bool sample_time_playout = true;
    std::size_t playout_delay_frames = 0;
    std::size_t playback_max_frames = 0;
    std::size_t playback_queue_capacity_frames = 0;
    std::size_t jitter_buffer_frames = 0;
    std::size_t jitter_buffer_max_frames = 0;
    bool adaptive_playback_cushion = false;
    std::size_t adaptive_playback_target_frames = 0;
    std::size_t adaptive_playback_min_frames = 0;
    std::size_t adaptive_playback_max_frames = 0;
    int adaptive_playback_release_ppm = 1000;
    bool drift_correction = true;
    double drift_smoothing = 0.02;
    int drift_deadband_ppm = 25;
    int drift_max_correction_ppm = 500;
    std::uint64_t stats_warmup_us = 3000000;
    bool collect_diagnostics = false;
};

// Network-thread sink used by the one-peer compatibility path. Phase 4 can
// replace this with a per-peer local-timeline/mix sink without changing the
// packet acceptance and clock-correction implementation.
class PeerStreamPlayback {
public:
    virtual ~PeerStreamPlayback() = default;
    virtual std::size_t depthFrames() const noexcept = 0;
    virtual std::size_t pushFrames(std::span<const std::int32_t> frames) noexcept = 0;
    virtual void requestDropFrames(std::size_t frames) noexcept = 0;
    virtual void setResamplerRatio(double ratio) noexcept = 0;
};

struct PeerStreamStats {
    std::uint64_t replay_rejects = 0;
    std::uint64_t forward_gap_rejects = 0;
    std::uint64_t forward_gap_resyncs = 0;
    std::uint64_t sequence_ambiguous_rejects = 0;
    std::uint64_t sample_time_stale_rejects = 0;
    std::uint64_t sample_time_future_rejects = 0;
    std::uint64_t reorder_pending_high_water = 0;
    std::uint64_t reorder_capacity_drops = 0;
    std::uint64_t jitter_pending_high_water = 0;
    std::uint64_t jitter_capacity_drops = 0;
    std::uint64_t reorder_work_budget_yields = 0;
    std::uint64_t playback_dropped_frames = 0;
    std::uint64_t playback_drop_events = 0;
    std::uint64_t playback_drop_event_max_frames = 0;
    std::uint64_t playback_depth_min_frames = 0;
    std::uint64_t playback_depth_sum_frames = 0;
    std::uint64_t playback_depth_max_frames = 0;
    std::uint64_t playback_depth_samples = 0;
    std::uint64_t stats_warmup_skipped_packets = 0;
    protocol::SequenceStats sequence;
    std::uint64_t reordered_recovered = 0;
    std::uint64_t reordered_lost = 0;
    std::uint64_t reordered_lost_events = 0;
    std::uint64_t reordered_max_distance_packets = 0;
    std::uint64_t jitter_min_us = 0;
    std::uint64_t jitter_sum_us = 0;
    std::uint64_t jitter_max_us = 0;
    std::uint64_t jitter_samples = 0;
    std::uint64_t audio_packet_gap_min_us = 0;
    std::uint64_t audio_packet_gap_sum_us = 0;
    std::uint64_t audio_packet_gap_max_us = 0;
    std::uint64_t audio_packet_gap_samples = 0;
    std::uint64_t audio_packet_gap_over_2x_count = 0;
    std::uint64_t audio_packet_gap_over_4x_count = 0;
    std::uint64_t playback_push_min_frames = 0;
    std::uint64_t playback_push_sum_frames = 0;
    std::uint64_t playback_push_max_frames = 0;
    std::uint64_t playback_push_batches = 0;
    std::uint64_t playback_depth_under_2ms_events = 0;
    std::uint64_t playback_depth_under_2ms_max_duration_us = 0;
    std::uint64_t playback_depth_under_5ms_events = 0;
    std::uint64_t playback_depth_under_5ms_max_duration_us = 0;
    std::uint64_t playback_depth_under_10ms_events = 0;
    std::uint64_t playback_depth_under_10ms_max_duration_us = 0;
    std::uint64_t rtt_min_us = 0;
    std::uint64_t rtt_sum_us = 0;
    std::uint64_t rtt_max_us = 0;
    std::uint64_t rtt_samples = 0;
    bool drift_valid = false;
    double raw_drift_ppm = 0.0;
    double drift_ppm = 0.0;
    double resampler_ratio = 1.0;
    double resampler_ratio_min = 0.0;
    double resampler_ratio_sum = 0.0;
    double resampler_ratio_max = 0.0;
    std::uint64_t resampler_ratio_samples = 0;
    std::uint64_t drift_correction_active_samples = 0;
    std::uint64_t drift_correction_clamped_samples = 0;
    double resampler_ratio_change_max_ppm_per_second = 0.0;
    bool sample_time_playout_enabled = false;
    std::uint64_t expected_remote_sample_time = 0;
    std::uint64_t last_received_sample_time = 0;
    std::uint64_t last_played_remote_sample_time = 0;
    std::uint64_t remote_sample_lag_frames = 0;
    std::uint64_t missing_sample_ranges = 0;
    std::uint64_t missing_audio_frames_inserted = 0;
    std::uint64_t late_audio_frames_dropped = 0;
    std::uint64_t playout_delay_frames = 0;
    std::int64_t playout_delay_error_frames = 0;
    bool jitter_buffer_enabled = false;
    std::uint64_t jitter_buffer_target_frames = 0;
    std::uint64_t jitter_buffer_max_frames = 0;
    std::uint64_t jitter_buffer_depth_frames = 0;
    std::uint64_t jitter_buffer_depth_max_frames = 0;
    std::uint64_t jitter_buffer_queued_packets = 0;
    std::uint64_t jitter_buffer_released_packets = 0;
    std::uint64_t jitter_buffer_late_packets = 0;
    std::uint64_t jitter_buffer_dropped_packets = 0;
    std::uint64_t jitter_buffer_dropped_frames = 0;
    std::uint64_t jitter_buffer_forced_releases = 0;
    bool adaptive_playback_cushion_enabled = false;
    std::uint64_t adaptive_playback_target_frames = 0;
    std::uint64_t adaptive_playback_min_frames = 0;
    std::uint64_t adaptive_playback_max_frames = 0;
    std::uint64_t adaptive_playback_raise_events = 0;
    std::uint64_t adaptive_playback_release_events = 0;
    std::uint64_t adaptive_playback_burst_events = 0;
    std::uint64_t adaptive_playback_padding_frames = 0;
    std::uint64_t adaptive_playback_time_above_target_us = 0;
    std::uint64_t adaptive_playback_time_under_target_us = 0;
    std::uint64_t adaptive_playback_longest_above_target_us = 0;
    std::uint64_t adaptive_playback_longest_under_target_us = 0;
};

enum class PeerAudioResult : std::uint8_t {
    Accepted,
    InvalidPayload,
    Duplicate,
    Late,
    AmbiguousSequence,
    ForwardGapRejected,
    StaleSampleTime,
    FutureSampleTime,
    ReorderCapacity,
};

enum class PeerReplayChannel : std::uint8_t {
    Ping,
    Metronome,
    Transport,
    Bye,
};

class PeerStream {
public:
    PeerStream(
        const PeerStreamConfig& config,
        std::uint64_t start_time_us,
        PeerStreamPlayback* playback);
    ~PeerStream();

    PeerStream(const PeerStream&) = delete;
    PeerStream& operator=(const PeerStream&) = delete;
    PeerStream(PeerStream&&) noexcept;
    PeerStream& operator=(PeerStream&&) noexcept;

    PeerAudioResult receiveAudio(
        const protocol::Header& header,
        std::span<const std::uint8_t> payload,
        std::uint64_t receive_time_us) noexcept;
    void advance(std::uint64_t now_us) noexcept;
    void finish(std::uint64_t now_us) noexcept;

    bool acceptReplay(PeerReplayChannel channel, std::uint32_t sequence) noexcept;
    void observeRtt(std::uint64_t rtt_us) noexcept;

    const PeerStreamConfig& config() const noexcept;
    const PeerStreamStats& stats() const noexcept;
    std::uint64_t effectivePlayoutDelayFrames() const noexcept;
    bool playoutSampleTimeInitialized() const noexcept;
    std::uint64_t nextPlayoutRemoteSampleTime() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace jam2

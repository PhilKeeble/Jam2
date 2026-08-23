#include "peer_stream.hpp"
#include "runtime_limits.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace jam2 {

namespace {

constexpr std::uint64_t kDriftBaselineCalibrationUs = 250000;
constexpr std::uint64_t kDriftMinimumObservationUs = 250000;
constexpr std::uint64_t kDriftObservationWindowUs = 250000;
constexpr double kDriftRatioSlewPpmPerSecond = 100.0;

void observe_timing(
    std::uint64_t value,
    std::uint64_t& minimum,
    std::uint64_t& sum,
    std::uint64_t& maximum) noexcept
{
    if (minimum == 0 || value < minimum) {
        minimum = value;
    }
    sum += value;
    maximum = std::max(maximum, value);
}

struct DurationTracker {
    bool active = false;
    std::uint64_t start_us = 0;
};

void observe_duration(
    bool active,
    std::uint64_t now_us,
    DurationTracker& tracker,
    std::uint64_t& events,
    std::uint64_t& total_us,
    std::uint64_t& longest_us) noexcept
{
    if (active) {
        if (!tracker.active) {
            tracker.active = true;
            tracker.start_us = now_us;
            ++events;
        }
        return;
    }
    if (!tracker.active) {
        return;
    }
    const std::uint64_t duration = now_us >= tracker.start_us ? now_us - tracker.start_us : 0;
    total_us += duration;
    longest_us = std::max(longest_us, duration);
    tracker.active = false;
}

} // namespace

struct PeerStream::Impl {
    struct PendingAudioPacket {
        std::uint32_t sequence = 0;
        std::uint64_t sample_time = 0;
        std::uint64_t receive_time = 0;
        bool reordered = false;
        std::size_t sample_count = 0;
        std::array<std::int32_t, protocol::kMaxAudioFramesPerPacket> samples{};

        std::span<const std::int32_t> sampleSpan() const noexcept
        {
            return {samples.data(), sample_count};
        }
    };

    struct PacketSlot {
        bool occupied = false;
        PendingAudioPacket packet;
    };

    static void transferPacket(
        PendingAudioPacket& destination,
        PendingAudioPacket&& source) noexcept
    {
        destination.sequence = source.sequence;
        destination.sample_time = source.sample_time;
        destination.receive_time = source.receive_time;
        destination.reordered = source.reordered;
        destination.sample_count = source.sample_count;
        std::copy_n(
            source.samples.data(),
            source.sample_count,
            destination.samples.data());
    }

    bool decodePacket(
        PendingAudioPacket& packet,
        const protocol::Header& header,
        std::span<const std::uint8_t> payload,
        std::uint64_t receive_time_us,
        bool reordered,
        std::size_t packet_frames) noexcept
    {
        packet.sequence = header.sequence;
        packet.sample_time = header.timing_value;
        packet.receive_time = receive_time_us;
        packet.reordered = reordered;
        packet.sample_count = packet_frames;
        return protocol::unpack_audio_q31_into(
            config.audio_format,
            payload,
            std::span<std::int32_t>(
                packet.samples.data(), packet.sample_count));
    }

    PeerStreamConfig config;
    PeerStreamPlayback* playback = nullptr;
    PeerStreamStats stats;
    std::uint64_t start_time_us = 0;
    std::uint64_t packet_interval_us = 0;
    std::uint32_t reorder_window_packets = 4;
    std::uint32_t max_forward_sequence_gap = 128;
    std::uint64_t sample_time_horizon_frames = 0;
    std::vector<PacketSlot> reorder_slots;
    std::vector<PacketSlot> jitter_slots;
    std::size_t reorder_count = 0;
    std::size_t jitter_count = 0;
    std::size_t jitter_read_index = 0;
    std::size_t jitter_write_index = 0;
    bool expected_sequence_set = false;
    std::uint32_t expected_sequence = 0;
    std::uint32_t highest_sequence = 0;
    bool forward_resync_candidate_set = false;
    std::uint32_t forward_resync_candidate_last = 0;
    std::uint32_t forward_resync_candidate_count = 0;
    bool forward_gap_recovery_pending = false;
    std::uint32_t forward_gap_recovery_expected = 0;
    bool highest_remote_sample_time_set = false;
    std::uint64_t highest_remote_sample_time = 0;
    bool playout_sample_time_initialized = false;
    std::uint64_t next_playout_remote_sample_time = 0;
    bool drift_started = false;
    bool drift_calibrating = false;
    bool drift_smoothed = false;
    double smoothed_drift_ppm = 0.0;
    double previous_resampler_ratio = 1.0;
    std::uint64_t previous_resampler_ratio_time_us = 0;
    std::uint64_t first_remote_sample_time = 0;
    std::uint64_t first_receive_time_us = 0;
    std::uint64_t drift_calibration_start_receive_time_us = 0;
    std::uint64_t drift_calibration_best_remote_sample_time = 0;
    std::uint64_t drift_calibration_best_receive_time_us = 0;
    long double drift_calibration_first_offset_us = 0.0L;
    long double drift_calibration_best_offset_us = 0.0L;
    bool drift_observation_active = false;
    std::uint64_t drift_observation_start_receive_time_us = 0;
    std::uint64_t drift_observation_best_remote_sample_time = 0;
    std::uint64_t drift_observation_best_receive_time_us = 0;
    long double drift_observation_best_offset_us = 0.0L;
    std::uint64_t drift_observation_packets = 0;
    std::uint64_t last_audio_receive_us = 0;
    std::uint64_t last_audio_gap_receive_us = 0;
    std::uint64_t adaptive_target_frames = 0;
    std::uint64_t adaptive_last_update_us = 0;
    double adaptive_release_accumulator_frames = 0.0;
    DurationTracker adaptive_under;
    DurationTracker adaptive_above;
    DurationTracker low_depth_2ms;
    DurationTracker low_depth_5ms;
    DurationTracker low_depth_10ms;
    std::uint64_t low_depth_2ms_frames = 1;
    std::uint64_t low_depth_5ms_frames = 1;
    std::uint64_t low_depth_10ms_frames = 1;
    std::vector<std::int32_t> silence;
    protocol::ReplayWindow ping_replay;
    protocol::ReplayWindow metronome_replay;
    protocol::ReplayWindow transport_replay;
    protocol::ReplayWindow bye_replay;

    Impl(const PeerStreamConfig& requested, std::uint64_t start, PeerStreamPlayback* sink)
        : config(requested),
          playback(sink),
          start_time_us(start),
          adaptive_target_frames(requested.adaptive_playback_target_frames)
    {
        if (!limits::valid_sample_rate(config.sample_rate) || config.frames_per_packet <= 0 ||
            config.frames_per_packet > static_cast<int>(protocol::kMaxAudioFramesPerPacket)) {
            throw std::runtime_error("invalid PeerStream sample rate or frames-per-packet contract");
        }
        if (config.jitter_buffer_max_frames < config.jitter_buffer_frames) {
            throw std::runtime_error("PeerStream jitter maximum is below its target");
        }
        if (config.adaptive_playback_cushion &&
            (config.adaptive_playback_min_frames > config.adaptive_playback_target_frames ||
             config.adaptive_playback_target_frames > config.adaptive_playback_max_frames)) {
            throw std::runtime_error("PeerStream adaptive playback bounds are inconsistent");
        }
        if (config.adaptive_playback_release_ppm < 0 ||
            config.adaptive_playback_release_ppm > 1000000) {
            throw std::runtime_error("PeerStream adaptive playback release is outside 0..1000000 ppm");
        }

        packet_interval_us = static_cast<std::uint64_t>(config.frames_per_packet) * 1000000ULL /
            static_cast<std::uint64_t>(config.sample_rate);
        reorder_window_packets = config.jitter_buffer_max_frames > 0
            ? std::max<std::uint32_t>(
                4,
                static_cast<std::uint32_t>(
                    (config.jitter_buffer_max_frames + static_cast<std::size_t>(config.frames_per_packet) - 1U) /
                    static_cast<std::size_t>(config.frames_per_packet)) + 1U)
            : 4U;
        const std::size_t reorder_capacity = std::min<std::size_t>(
            4096,
            std::max<std::size_t>(8, static_cast<std::size_t>(reorder_window_packets) + 2U));
        const std::size_t jitter_capacity = std::min<std::size_t>(
            4096,
            std::max<std::size_t>(
                8,
                config.jitter_buffer_max_frames > 0
                    ? config.jitter_buffer_max_frames / static_cast<std::size_t>(config.frames_per_packet) + 4U
                    : reorder_capacity));
        reorder_slots.resize(reorder_capacity);
        jitter_slots.resize(jitter_capacity);
        silence.resize(static_cast<std::size_t>(config.frames_per_packet), 0);
        max_forward_sequence_gap = std::min<std::uint32_t>(
            4096,
            std::max<std::uint32_t>(128, reorder_window_packets * 2U));
        sample_time_horizon_frames = static_cast<std::uint64_t>(config.sample_rate) * 10ULL;
        low_depth_2ms_frames = std::max<std::uint64_t>(
            1,
            static_cast<std::uint64_t>(std::ceil(static_cast<double>(config.sample_rate) * 2.0 / 1000.0)));
        low_depth_5ms_frames = std::max<std::uint64_t>(
            1,
            static_cast<std::uint64_t>(std::ceil(static_cast<double>(config.sample_rate) * 5.0 / 1000.0)));
        low_depth_10ms_frames = std::max<std::uint64_t>(
            1,
            static_cast<std::uint64_t>(std::ceil(static_cast<double>(config.sample_rate) * 10.0 / 1000.0)));

        stats.sample_time_playout_enabled = config.sample_time_playout;
        stats.playout_delay_frames = config.playout_delay_frames;
        stats.jitter_buffer_enabled = config.jitter_buffer_frames > 0;
        stats.jitter_buffer_target_frames = config.jitter_buffer_frames;
        stats.jitter_buffer_max_frames = config.jitter_buffer_max_frames;
        stats.adaptive_playback_cushion_enabled = config.adaptive_playback_cushion;
        stats.adaptive_playback_target_frames = adaptive_target_frames;
        stats.adaptive_playback_min_frames = config.adaptive_playback_min_frames;
        stats.adaptive_playback_max_frames = config.adaptive_playback_max_frames;
    }

    std::uint64_t currentPlayoutDelayFrames() const noexcept
    {
        return config.adaptive_playback_cushion ? adaptive_target_frames : config.playout_delay_frames;
    }

    std::uint64_t effectivePlayoutDelayFrames() const noexcept
    {
        return currentPlayoutDelayFrames() + config.jitter_buffer_frames;
    }

    void updateLowDepth(
        bool below,
        std::uint64_t now_us,
        DurationTracker& tracker,
        std::uint64_t& event_count,
        std::uint64_t& longest_us) noexcept
    {
        std::uint64_t unused_total = 0;
        observe_duration(below, now_us, tracker, event_count, unused_total, longest_us);
    }

    void beginDriftObservation(
        const PendingAudioPacket& packet,
        long double arrival_offset_us) noexcept
    {
        drift_observation_active = true;
        drift_observation_start_receive_time_us = packet.receive_time;
        drift_observation_best_remote_sample_time = packet.sample_time;
        drift_observation_best_receive_time_us = packet.receive_time;
        drift_observation_best_offset_us = arrival_offset_us;
        drift_observation_packets = 1;
    }

    void applyDriftObservation() noexcept
    {
        if (!drift_observation_active ||
            drift_observation_best_receive_time_us <= first_receive_time_us ||
            drift_observation_best_remote_sample_time <= first_remote_sample_time ||
            drift_observation_best_receive_time_us - first_receive_time_us <
                kDriftMinimumObservationUs) {
            return;
        }
        const double remote_elapsed_samples = static_cast<double>(
            drift_observation_best_remote_sample_time - first_remote_sample_time);
        const double remote_elapsed_us = remote_elapsed_samples * 1000000.0 /
            static_cast<double>(config.sample_rate);
        const double local_elapsed_us = static_cast<double>(
            drift_observation_best_receive_time_us - first_receive_time_us);
        stats.raw_drift_ppm =
            ((remote_elapsed_us / local_elapsed_us) - 1.0) * 1000000.0;
        if (!drift_smoothed || config.drift_smoothing >= 1.0) {
            smoothed_drift_ppm = stats.raw_drift_ppm;
            drift_smoothed = true;
        } else if (config.drift_smoothing > 0.0) {
            // The public smoothing control historically described a
            // per-packet alpha. Apply the equivalent accumulated alpha to one
            // low-delay observation per window so changing packet size does
            // not change its time response.
            const double retained = std::pow(
                1.0 - config.drift_smoothing,
                static_cast<double>(drift_observation_packets));
            const double alpha = 1.0 - retained;
            smoothed_drift_ppm +=
                (stats.raw_drift_ppm - smoothed_drift_ppm) * alpha;
        }
        stats.drift_ppm = smoothed_drift_ppm;

        const double max_ratio_delta =
            static_cast<double>(config.drift_max_correction_ppm) / 1000000.0;
        const bool inside_deadband =
            std::abs(stats.drift_ppm) <= static_cast<double>(config.drift_deadband_ppm);
        const double requested_ratio = config.drift_correction && !inside_deadband
            ? 1.0 + stats.drift_ppm / 1000000.0
            : 1.0;
        const double bounded_ratio = std::clamp(
            requested_ratio, 1.0 - max_ratio_delta, 1.0 + max_ratio_delta);
        const bool correction_clamped = config.drift_correction && !inside_deadband &&
            bounded_ratio != requested_ratio;

        double applied_ratio = bounded_ratio;
        if (previous_resampler_ratio_time_us != 0 &&
            drift_observation_best_receive_time_us > previous_resampler_ratio_time_us) {
            const double elapsed_seconds = static_cast<double>(
                drift_observation_best_receive_time_us - previous_resampler_ratio_time_us) /
                1000000.0;
            const double maximum_change =
                kDriftRatioSlewPpmPerSecond * elapsed_seconds / 1000000.0;
            applied_ratio = std::clamp(
                bounded_ratio,
                previous_resampler_ratio - maximum_change,
                previous_resampler_ratio + maximum_change);
            if (elapsed_seconds > 0.0) {
                const double change_ppm =
                    std::abs(applied_ratio - previous_resampler_ratio) * 1000000.0;
                stats.resampler_ratio_change_max_ppm_per_second = std::max(
                    stats.resampler_ratio_change_max_ppm_per_second,
                    change_ppm / elapsed_seconds);
            }
        }
        stats.resampler_ratio = applied_ratio;
        if (config.collect_diagnostics) {
            if (stats.resampler_ratio_samples == 0) {
                stats.resampler_ratio_min = applied_ratio;
                stats.resampler_ratio_max = applied_ratio;
            } else {
                stats.resampler_ratio_min = std::min(
                    stats.resampler_ratio_min, applied_ratio);
                stats.resampler_ratio_max = std::max(
                    stats.resampler_ratio_max, applied_ratio);
            }
            stats.resampler_ratio_sum += applied_ratio;
            ++stats.resampler_ratio_samples;
            if (applied_ratio != 1.0) {
                ++stats.drift_correction_active_samples;
            }
            if (correction_clamped) {
                ++stats.drift_correction_clamped_samples;
            }
        }
        previous_resampler_ratio = applied_ratio;
        previous_resampler_ratio_time_us =
            drift_observation_best_receive_time_us;
        if (playback != nullptr) {
            playback->setResamplerRatio(applied_ratio);
        }
        stats.drift_valid = true;
    }

    void observeDriftWindow(
        const PendingAudioPacket& packet,
        long double arrival_offset_us) noexcept
    {
        if (!drift_observation_active) {
            beginDriftObservation(packet, arrival_offset_us);
            return;
        }
        if (packet.receive_time >= drift_observation_start_receive_time_us &&
            packet.receive_time - drift_observation_start_receive_time_us >=
                kDriftObservationWindowUs) {
            applyDriftObservation();
            beginDriftObservation(packet, arrival_offset_us);
            return;
        }
        ++drift_observation_packets;
        if (arrival_offset_us < drift_observation_best_offset_us) {
            drift_observation_best_offset_us = arrival_offset_us;
            drift_observation_best_remote_sample_time = packet.sample_time;
            drift_observation_best_receive_time_us = packet.receive_time;
        }
    }

    void processPacket(const PendingAudioPacket& packet) noexcept
    {
        const bool past_warmup = packet.receive_time >= start_time_us + config.stats_warmup_us;
        if (!past_warmup) {
            ++stats.stats_warmup_skipped_packets;
        }
        bool inserted_missing = false;
        if (playback != nullptr) {
            stats.last_received_sample_time = packet.sample_time;
            std::size_t pushed_frames = 0;
            if (config.sample_time_playout) {
                if (!playout_sample_time_initialized) {
                    playout_sample_time_initialized = true;
                    next_playout_remote_sample_time = packet.sample_time;
                    stats.expected_remote_sample_time = next_playout_remote_sample_time;
                }
                if (packet.sample_time > next_playout_remote_sample_time) {
                    std::uint64_t missing = packet.sample_time - next_playout_remote_sample_time;
                    inserted_missing = true;
                    ++stats.missing_sample_ranges;
                    stats.missing_audio_frames_inserted += missing;
                    while (missing > 0) {
                        const std::size_t chunk = static_cast<std::size_t>(std::min<std::uint64_t>(missing, silence.size()));
                        pushed_frames += playback->pushFrames(
                            std::span<const std::int32_t>(silence.data(), chunk));
                        missing -= chunk;
                    }
                    next_playout_remote_sample_time = packet.sample_time;
                }
                const std::uint64_t packet_end = packet.sample_time + packet.sample_count;
                if (packet_end <= next_playout_remote_sample_time) {
                    stats.late_audio_frames_dropped += packet.sample_count;
                    return;
                }
                std::span<const std::int32_t> packet_samples = packet.sampleSpan();
                if (packet.sample_time < next_playout_remote_sample_time) {
                    const std::size_t skip = static_cast<std::size_t>(std::min<std::uint64_t>(
                        next_playout_remote_sample_time - packet.sample_time,
                        packet.sample_count));
                    stats.late_audio_frames_dropped += skip;
                    packet_samples = packet_samples.subspan(skip);
                }
                pushed_frames += playback->pushFrames(packet_samples);
                next_playout_remote_sample_time += packet_samples.size();
                stats.expected_remote_sample_time = next_playout_remote_sample_time;
                stats.last_played_remote_sample_time = next_playout_remote_sample_time;
                stats.remote_sample_lag_frames = next_playout_remote_sample_time >= packet.sample_time
                    ? next_playout_remote_sample_time - packet.sample_time
                    : 0;
            } else {
                pushed_frames = playback->pushFrames(packet.sampleSpan());
                stats.expected_remote_sample_time = packet.sample_time + packet.sample_count;
                stats.last_played_remote_sample_time = stats.expected_remote_sample_time;
            }

            stats.playout_delay_frames = currentPlayoutDelayFrames();
            std::uint64_t depth = playback->depthFrames();
            stats.playout_delay_error_frames =
                static_cast<std::int64_t>(depth) - static_cast<std::int64_t>(stats.playout_delay_frames);
            if (config.collect_diagnostics && past_warmup) {
                observe_timing(
                    pushed_frames,
                    stats.playback_push_min_frames,
                    stats.playback_push_sum_frames,
                    stats.playback_push_max_frames);
                ++stats.playback_push_batches;
            }
            if (config.playback_max_frames > 0 && depth > config.playback_max_frames) {
                const std::uint64_t requested = depth - config.playback_max_frames;
                playback->requestDropFrames(static_cast<std::size_t>(requested));
                stats.playback_dropped_frames += requested;
                ++stats.playback_drop_events;
                stats.playback_drop_event_max_frames = std::max(
                    stats.playback_drop_event_max_frames,
                    requested);
            }

            if (config.adaptive_playback_cushion && past_warmup) {
                const bool burst_evidence = depth < config.adaptive_playback_min_frames || inserted_missing;
                if (burst_evidence) {
                    adaptive_release_accumulator_frames = 0.0;
                    if (adaptive_target_frames < config.adaptive_playback_max_frames) {
                        const std::uint64_t previous = adaptive_target_frames;
                        adaptive_target_frames = std::min<std::uint64_t>(
                            config.adaptive_playback_max_frames,
                            std::max<std::uint64_t>(
                                adaptive_target_frames + static_cast<std::uint64_t>(config.frames_per_packet),
                                config.adaptive_playback_min_frames));
                        if (adaptive_target_frames > previous) {
                            ++stats.adaptive_playback_raise_events;
                            ++stats.adaptive_playback_burst_events;
                        }
                    }
                } else if (!burst_evidence && adaptive_target_frames > config.adaptive_playback_min_frames &&
                           config.adaptive_playback_release_ppm > 0) {
                    const std::uint64_t elapsed_us = adaptive_last_update_us != 0 && packet.receive_time > adaptive_last_update_us
                        ? packet.receive_time - adaptive_last_update_us
                        : 0;
                    const int effective_release_ppm = config.adaptive_playback_release_ppm;
                    adaptive_release_accumulator_frames +=
                        static_cast<double>(config.sample_rate) *
                        static_cast<double>(effective_release_ppm) *
                        static_cast<double>(elapsed_us) /
                        1000000000000.0;
                    const std::uint64_t available_release =
                        static_cast<std::uint64_t>(adaptive_release_accumulator_frames);
                    const std::uint64_t release = std::min<std::uint64_t>(
                        available_release,
                        adaptive_target_frames - config.adaptive_playback_min_frames);
                    if (release > 0) {
                        adaptive_target_frames -= release;
                        adaptive_release_accumulator_frames -= static_cast<double>(release);
                        ++stats.adaptive_playback_release_events;
                    }
                }
                adaptive_last_update_us = packet.receive_time;
                depth = playback->depthFrames();
                if (depth < adaptive_target_frames) {
                    std::uint64_t padding = adaptive_target_frames - depth;
                    stats.adaptive_playback_padding_frames += padding;
                    while (padding > 0) {
                        const std::size_t chunk = static_cast<std::size_t>(std::min<std::uint64_t>(padding, silence.size()));
                        (void)playback->pushFrames(std::span<const std::int32_t>(silence.data(), chunk));
                        padding -= chunk;
                    }
                }
                depth = playback->depthFrames();
                std::uint64_t unused_events = 0;
                observe_duration(
                    depth < adaptive_target_frames,
                    packet.receive_time,
                    adaptive_under,
                    unused_events,
                    stats.adaptive_playback_time_under_target_us,
                    stats.adaptive_playback_longest_under_target_us);
                observe_duration(
                    depth > adaptive_target_frames,
                    packet.receive_time,
                    adaptive_above,
                    unused_events,
                    stats.adaptive_playback_time_above_target_us,
                    stats.adaptive_playback_longest_above_target_us);
                stats.adaptive_playback_target_frames = adaptive_target_frames;
                stats.playout_delay_frames = adaptive_target_frames;
                stats.playout_delay_error_frames =
                    static_cast<std::int64_t>(depth) - static_cast<std::int64_t>(adaptive_target_frames);
            }

            if (past_warmup) {
                depth = playback->depthFrames();
                if (config.collect_diagnostics) {
                    updateLowDepth(
                        depth < low_depth_2ms_frames,
                        packet.receive_time,
                        low_depth_2ms,
                        stats.playback_depth_under_2ms_events,
                        stats.playback_depth_under_2ms_max_duration_us);
                    updateLowDepth(
                        depth < low_depth_5ms_frames,
                        packet.receive_time,
                        low_depth_5ms,
                        stats.playback_depth_under_5ms_events,
                        stats.playback_depth_under_5ms_max_duration_us);
                    updateLowDepth(
                        depth < low_depth_10ms_frames,
                        packet.receive_time,
                        low_depth_10ms,
                        stats.playback_depth_under_10ms_events,
                        stats.playback_depth_under_10ms_max_duration_us);
                }
                observe_timing(
                    depth,
                    stats.playback_depth_min_frames,
                    stats.playback_depth_sum_frames,
                    stats.playback_depth_max_frames);
                ++stats.playback_depth_samples;
            }
        }

        if (past_warmup) {
            const long double remote_time_us =
                static_cast<long double>(packet.sample_time) * 1000000.0L /
                static_cast<long double>(config.sample_rate);
            const long double arrival_offset_us =
                static_cast<long double>(packet.receive_time) - remote_time_us;
            if (!drift_started) {
                if (!drift_calibrating) {
                    drift_calibrating = true;
                    stats.drift_baseline_calibrating = true;
                    drift_calibration_start_receive_time_us = packet.receive_time;
                    drift_calibration_best_remote_sample_time = packet.sample_time;
                    drift_calibration_best_receive_time_us = packet.receive_time;
                    drift_calibration_first_offset_us = arrival_offset_us;
                    drift_calibration_best_offset_us = arrival_offset_us;
                }
                ++stats.drift_baseline_calibration_packets;
                if (arrival_offset_us < drift_calibration_best_offset_us) {
                    drift_calibration_best_offset_us = arrival_offset_us;
                    drift_calibration_best_remote_sample_time = packet.sample_time;
                    drift_calibration_best_receive_time_us = packet.receive_time;
                    const long double improvement =
                        drift_calibration_first_offset_us - drift_calibration_best_offset_us;
                    stats.drift_baseline_delay_improvement_us =
                        static_cast<std::uint64_t>(std::max(0.0L, improvement));
                }
                if (packet.receive_time >= drift_calibration_start_receive_time_us &&
                    packet.receive_time - drift_calibration_start_receive_time_us >=
                        kDriftBaselineCalibrationUs) {
                    // Socket dequeue time includes any delay accumulated before
                    // the network worker reads a packet. Select the lowest-delay
                    // point from a short bounded calibration window so a queued
                    // burst at the stats warmup boundary cannot become the
                    // lifetime clock baseline.
                    drift_started = true;
                    drift_calibrating = false;
                    stats.drift_baseline_calibrating = false;
                    first_remote_sample_time =
                        drift_calibration_best_remote_sample_time;
                    first_receive_time_us =
                        drift_calibration_best_receive_time_us;
                    previous_resampler_ratio = 1.0;
                    previous_resampler_ratio_time_us = first_receive_time_us;
                }
            }
            if (drift_started) {
                // Socket dequeue time includes network and scheduler delay.
                // Feed the estimator only the lowest-delay packet in each
                // bounded window so a queued burst cannot be interpreted as
                // an oscillator-rate change.
                observeDriftWindow(packet, arrival_offset_us);
            }
        }

        if (past_warmup && last_audio_receive_us != 0) {
            const std::uint64_t interval = packet.receive_time >= last_audio_receive_us
                ? packet.receive_time - last_audio_receive_us
                : 0;
            const std::uint64_t jitter = interval > packet_interval_us
                ? interval - packet_interval_us
                : packet_interval_us - interval;
            observe_timing(jitter, stats.jitter_min_us, stats.jitter_sum_us, stats.jitter_max_us);
            ++stats.jitter_samples;
        }
        if (config.collect_diagnostics && past_warmup && last_audio_gap_receive_us != 0) {
            const std::uint64_t interval = packet.receive_time >= last_audio_gap_receive_us
                ? packet.receive_time - last_audio_gap_receive_us
                : 0;
            observe_timing(
                interval,
                stats.audio_packet_gap_min_us,
                stats.audio_packet_gap_sum_us,
                stats.audio_packet_gap_max_us);
            ++stats.audio_packet_gap_samples;
            if (packet_interval_us > 0 && interval > packet_interval_us * 2U) {
                ++stats.audio_packet_gap_over_2x_count;
            }
            if (packet_interval_us > 0 && interval > packet_interval_us * 4U) {
                ++stats.audio_packet_gap_over_4x_count;
            }
        }
        if (past_warmup) {
            last_audio_receive_us = packet.receive_time;
            if (config.collect_diagnostics) {
                last_audio_gap_receive_us = packet.receive_time;
            }
        } else {
            last_audio_receive_us = 0;
            last_audio_gap_receive_us = 0;
        }
    }

    std::uint64_t jitterDepthFrames() const noexcept
    {
        if (jitter_count == 0) {
            return 0;
        }
        const PacketSlot& first = jitter_slots[jitter_read_index];
        const std::size_t last_index =
            (jitter_write_index + jitter_slots.size() - 1U) % jitter_slots.size();
        const PacketSlot& last = jitter_slots[last_index];
        const std::uint64_t last_end = last.packet.sample_time >
                (std::numeric_limits<std::uint64_t>::max)() - last.packet.sample_count
            ? (std::numeric_limits<std::uint64_t>::max)()
            : last.packet.sample_time + last.packet.sample_count;
        return last_end > first.packet.sample_time
            ? last_end - first.packet.sample_time
            : 0;
    }

    void updateJitterDepth() noexcept
    {
        stats.jitter_buffer_depth_frames = jitterDepthFrames();
        stats.jitter_buffer_depth_max_frames = std::max(
            stats.jitter_buffer_depth_max_frames,
            stats.jitter_buffer_depth_frames);
    }

    PacketSlot* oldestJitterSlot() noexcept
    {
        return jitter_count > 0 ? &jitter_slots[jitter_read_index] : nullptr;
    }

    void releaseOldestJitterPacket() noexcept
    {
        PacketSlot& oldest = jitter_slots[jitter_read_index];
        oldest.occupied = false;
        jitter_read_index = (jitter_read_index + 1U) % jitter_slots.size();
        --jitter_count;
        ++stats.jitter_buffer_released_packets;
        processPacket(oldest.packet);
    }

    void resetJitterAfterDiscontinuity() noexcept
    {
        if (config.jitter_buffer_frames == 0) {
            return;
        }
        while (jitter_count > 0) {
            PacketSlot& slot = jitter_slots[jitter_read_index];
            ++stats.jitter_buffer_dropped_packets;
            stats.jitter_buffer_dropped_frames += slot.packet.sample_count;
            slot.occupied = false;
            jitter_read_index = (jitter_read_index + 1U) % jitter_slots.size();
            --jitter_count;
        }
        jitter_read_index = 0;
        jitter_write_index = 0;
        stats.jitter_buffer_depth_frames = 0;
        ++stats.jitter_buffer_rebases;
    }

    void drainJitter(std::uint64_t now_us) noexcept
    {
        if (config.jitter_buffer_frames == 0) {
            return;
        }
        const std::uint64_t target_delay_us =
            config.jitter_buffer_frames * 1000000ULL /
            static_cast<std::uint64_t>(config.sample_rate);
        for (;;) {
            updateJitterDepth();
            PacketSlot* oldest = oldestJitterSlot();
            if (oldest == nullptr) {
                return;
            }
            const bool maximum_exceeded = config.jitter_buffer_max_frames > 0 &&
                stats.jitter_buffer_depth_frames > config.jitter_buffer_max_frames;
            const bool target_reached =
                stats.jitter_buffer_depth_frames >= config.jitter_buffer_frames;
            const bool residence_expired = now_us >= oldest->packet.receive_time &&
                now_us - oldest->packet.receive_time >= target_delay_us;
            if (!maximum_exceeded && !target_reached && !residence_expired) {
                return;
            }
            if (maximum_exceeded) {
                ++stats.jitter_buffer_forced_releases;
            } else if (target_reached) {
                ++stats.jitter_buffer_target_releases;
            } else {
                ++stats.jitter_buffer_timeout_releases;
                ++stats.jitter_buffer_late_packets;
            }
            releaseOldestJitterPacket();
        }
    }

    void queueOrProcess(PendingAudioPacket&& packet) noexcept
    {
        if (config.jitter_buffer_frames == 0) {
            processPacket(packet);
            return;
        }
        if (jitter_count >= jitter_slots.size()) {
            ++stats.jitter_capacity_drops;
            ++stats.jitter_buffer_dropped_packets;
            stats.jitter_buffer_dropped_frames += packet.sample_count;
            return;
        }
        const std::uint64_t receive_time = packet.receive_time;
        PacketSlot& slot = jitter_slots[jitter_write_index];
        transferPacket(slot.packet, std::move(packet));
        commitJitterSlot(receive_time);
    }

    void commitJitterSlot(std::uint64_t receive_time) noexcept
    {
        PacketSlot& slot = jitter_slots[jitter_write_index];
        slot.occupied = true;
        jitter_write_index = (jitter_write_index + 1U) % jitter_slots.size();
        ++jitter_count;
        stats.jitter_pending_high_water = std::max<std::uint64_t>(
            stats.jitter_pending_high_water,
            jitter_count);
        ++stats.jitter_buffer_queued_packets;
        updateJitterDepth();
        drainJitter(receive_time);
    }

    void drainReorder() noexcept
    {
        std::size_t work = 0;
        while (expected_sequence_set && work < 64U) {
            PacketSlot& next = reorder_slots[expected_sequence % reorder_slots.size()];
            if (next.occupied && next.packet.sequence == expected_sequence) {
                if (next.packet.reordered) {
                    ++stats.reordered_recovered;
                }
                next.occupied = false;
                --reorder_count;
                ++expected_sequence;
                ++work;
                queueOrProcess(std::move(next.packet));
                continue;
            }
            if (protocol::sequence_after(highest_sequence, expected_sequence) &&
                protocol::sequence_forward_distance(highest_sequence, expected_sequence) > reorder_window_packets) {
                ++stats.sequence.lost;
                ++stats.reordered_lost;
                ++stats.reordered_lost_events;
                ++expected_sequence;
                ++work;
                continue;
            }
            break;
        }
        if (work == 64U) {
            ++stats.reorder_work_budget_yields;
        }
    }

    void markExpectedLost() noexcept
    {
        if (!expected_sequence_set) {
            return;
        }
        ++stats.sequence.lost;
        ++stats.sequence.loss_events;
        stats.sequence.loss_max_gap = std::max<std::uint64_t>(stats.sequence.loss_max_gap, 1);
        ++stats.reordered_lost;
        ++stats.reordered_lost_events;
        ++expected_sequence;
        drainReorder();
    }

    PeerAudioResult receiveAudio(
        const protocol::Header& header,
        std::span<const std::uint8_t> payload,
        std::uint64_t receive_time_us) noexcept
    {
        const std::size_t bytes_per_sample = protocol::audio_bytes_per_sample(config.audio_format);
        if (bytes_per_sample == 0 ||
            payload.size() != static_cast<std::size_t>(config.frames_per_packet) * bytes_per_sample ||
            payload.size() > protocol::kMaxAudioFramesPerPacket * bytes_per_sample) {
            return PeerAudioResult::InvalidPayload;
        }
        if (!expected_sequence_set) {
            expected_sequence_set = true;
            expected_sequence = header.sequence;
            highest_sequence = header.sequence;
        }
        if (forward_gap_recovery_pending && expected_sequence == forward_gap_recovery_expected) {
            if (header.sequence == expected_sequence) {
                forward_gap_recovery_pending = false;
            } else if (protocol::sequence_after(header.sequence, expected_sequence) &&
                       protocol::sequence_forward_distance(header.sequence, expected_sequence) == 1U) {
                forward_gap_recovery_pending = false;
                markExpectedLost();
            }
        }
        if (protocol::sequence_before(header.sequence, expected_sequence)) {
            ++stats.sequence.late;
            return PeerAudioResult::Late;
        }
        const bool expected = header.sequence == expected_sequence;
        const bool forward = protocol::sequence_after(header.sequence, expected_sequence);
        if (!expected && !forward) {
            ++stats.sequence_ambiguous_rejects;
            return PeerAudioResult::AmbiguousSequence;
        }
        PacketSlot& reorder_slot = reorder_slots[header.sequence % reorder_slots.size()];
        if (reorder_slot.occupied && reorder_slot.packet.sequence == header.sequence) {
            ++stats.sequence.duplicate;
            return PeerAudioResult::Duplicate;
        }

        bool reordered = forward;
        if (reordered) {
            const std::uint32_t distance = protocol::sequence_forward_distance(header.sequence, expected_sequence);
            if (distance > max_forward_sequence_gap) {
                if (forward_resync_candidate_set &&
                    header.sequence == forward_resync_candidate_last + 1U) {
                    ++forward_resync_candidate_count;
                } else {
                    forward_resync_candidate_set = true;
                    forward_resync_candidate_count = 1;
                }
                forward_resync_candidate_last = header.sequence;
                if (forward_resync_candidate_count < 3U) {
                    forward_gap_recovery_pending = true;
                    forward_gap_recovery_expected = expected_sequence;
                    ++stats.forward_gap_rejects;
                    return PeerAudioResult::ForwardGapRejected;
                }
                for (PacketSlot& slot : reorder_slots) {
                    slot.occupied = false;
                }
                reorder_count = 0;
                resetJitterAfterDiscontinuity();
                expected_sequence = header.sequence;
                highest_sequence = header.sequence;
                forward_resync_candidate_set = false;
                forward_resync_candidate_count = 0;
                forward_gap_recovery_pending = false;
                reordered = false;
                ++stats.forward_gap_resyncs;
            }
        } else {
            forward_resync_candidate_set = false;
            forward_resync_candidate_count = 0;
        }
        if (reordered) {
            ++stats.sequence.out_of_order;
            if (config.collect_diagnostics) {
                stats.reordered_max_distance_packets = std::max<std::uint64_t>(
                    stats.reordered_max_distance_packets,
                    protocol::sequence_forward_distance(header.sequence, expected_sequence));
            }
        }
        if (protocol::sequence_after(header.sequence, highest_sequence)) {
            highest_sequence = header.sequence;
        }

        const std::uint64_t packet_frames = payload.size() / bytes_per_sample;
        if (header.timing_value > (std::numeric_limits<std::uint64_t>::max)() - packet_frames) {
            if (header.sequence == expected_sequence) {
                markExpectedLost();
            }
            ++stats.sample_time_future_rejects;
            return PeerAudioResult::FutureSampleTime;
        }
        const std::uint64_t packet_end = header.timing_value + packet_frames;
        if (!highest_remote_sample_time_set) {
            // A peer may join an already-running sender whose session sample
            // counter is well beyond zero.  The first authenticated packet is
            // the bounded stream baseline; the horizon applies to subsequent
            // discontinuities, not to its absolute value.
            highest_remote_sample_time_set = true;
            highest_remote_sample_time = packet_end;
        } else {
            if (packet_end < highest_remote_sample_time &&
                highest_remote_sample_time - packet_end > sample_time_horizon_frames) {
                if (header.sequence == expected_sequence) {
                    markExpectedLost();
                }
                ++stats.sample_time_stale_rejects;
                return PeerAudioResult::StaleSampleTime;
            }
            if (header.timing_value > highest_remote_sample_time &&
                header.timing_value - highest_remote_sample_time > sample_time_horizon_frames) {
                if (header.sequence == expected_sequence) {
                    markExpectedLost();
                }
                ++stats.sample_time_future_rejects;
                return PeerAudioResult::FutureSampleTime;
            }
            highest_remote_sample_time = std::max(highest_remote_sample_time, packet_end);
        }

        if (header.sequence == expected_sequence) {
            if (config.jitter_buffer_frames == 0) {
                PendingAudioPacket packet;
                if (!decodePacket(
                        packet,
                        header,
                        payload,
                        receive_time_us,
                        reordered,
                        static_cast<std::size_t>(packet_frames))) {
                    return PeerAudioResult::InvalidPayload;
                }
                processPacket(packet);
            } else if (jitter_count >= jitter_slots.size()) {
                ++stats.jitter_capacity_drops;
                ++stats.jitter_buffer_dropped_packets;
                stats.jitter_buffer_dropped_frames += packet_frames;
            } else {
                PacketSlot& jitter_slot = jitter_slots[jitter_write_index];
                if (!decodePacket(
                        jitter_slot.packet,
                        header,
                        payload,
                        receive_time_us,
                        reordered,
                        static_cast<std::size_t>(packet_frames))) {
                    return PeerAudioResult::InvalidPayload;
                }
                commitJitterSlot(receive_time_us);
            }
            ++expected_sequence;
            drainReorder();
            return PeerAudioResult::Accepted;
        }
        if (reorder_slot.occupied || reorder_count >= reorder_slots.size()) {
            ++stats.reorder_capacity_drops;
            return PeerAudioResult::ReorderCapacity;
        }
        if (!decodePacket(
                reorder_slot.packet,
                header,
                payload,
                receive_time_us,
                reordered,
                static_cast<std::size_t>(packet_frames))) {
            return PeerAudioResult::InvalidPayload;
        }
        reorder_slot.occupied = true;
        ++reorder_count;
        stats.reorder_pending_high_water = std::max<std::uint64_t>(
            stats.reorder_pending_high_water,
            reorder_count);
        drainReorder();
        return PeerAudioResult::Accepted;
    }

    void finish(std::uint64_t now_us) noexcept
    {
        updateLowDepth(false, now_us, low_depth_2ms, stats.playback_depth_under_2ms_events,
            stats.playback_depth_under_2ms_max_duration_us);
        updateLowDepth(false, now_us, low_depth_5ms, stats.playback_depth_under_5ms_events,
            stats.playback_depth_under_5ms_max_duration_us);
        updateLowDepth(false, now_us, low_depth_10ms, stats.playback_depth_under_10ms_events,
            stats.playback_depth_under_10ms_max_duration_us);
        std::uint64_t unused_events = 0;
        observe_duration(
            false,
            now_us,
            adaptive_under,
            unused_events,
            stats.adaptive_playback_time_under_target_us,
            stats.adaptive_playback_longest_under_target_us);
        observe_duration(
            false,
            now_us,
            adaptive_above,
            unused_events,
            stats.adaptive_playback_time_above_target_us,
            stats.adaptive_playback_longest_above_target_us);
    }
};

PeerStream::PeerStream(
    const PeerStreamConfig& config,
    std::uint64_t start_time_us,
    PeerStreamPlayback* playback)
    : impl_(std::make_unique<Impl>(config, start_time_us, playback))
{
}

PeerStream::~PeerStream() = default;
PeerStream::PeerStream(PeerStream&&) noexcept = default;
PeerStream& PeerStream::operator=(PeerStream&&) noexcept = default;

PeerAudioResult PeerStream::receiveAudio(
    const protocol::Header& header,
    std::span<const std::uint8_t> payload,
    std::uint64_t receive_time_us) noexcept
{
    return impl_->receiveAudio(header, payload, receive_time_us);
}

void PeerStream::advance(std::uint64_t now_us) noexcept
{
    const bool reorder_work_pending = impl_->reorder_count != 0 ||
        (impl_->expected_sequence_set &&
         protocol::sequence_after(impl_->highest_sequence, impl_->expected_sequence) &&
         protocol::sequence_forward_distance(
             impl_->highest_sequence,
             impl_->expected_sequence) > impl_->reorder_window_packets);
    if (reorder_work_pending) {
        impl_->drainReorder();
    }
    if (impl_->jitter_count != 0) {
        impl_->drainJitter(now_us);
    }
}

void PeerStream::finish(std::uint64_t now_us) noexcept
{
    impl_->finish(now_us);
}

bool PeerStream::acceptReplay(PeerReplayChannel channel, std::uint32_t sequence) noexcept
{
    protocol::ReplayWindow* window = nullptr;
    switch (channel) {
    case PeerReplayChannel::Ping: window = &impl_->ping_replay; break;
    case PeerReplayChannel::Metronome: window = &impl_->metronome_replay; break;
    case PeerReplayChannel::Transport: window = &impl_->transport_replay; break;
    case PeerReplayChannel::Bye: window = &impl_->bye_replay; break;
    }
    if (window != nullptr && window->observe(sequence) == protocol::ReplayResult::New) {
        return true;
    }
    ++impl_->stats.replay_rejects;
    return false;
}

void PeerStream::observeRtt(std::uint64_t rtt_us) noexcept
{
    observe_timing(
        rtt_us,
        impl_->stats.rtt_min_us,
        impl_->stats.rtt_sum_us,
        impl_->stats.rtt_max_us);
    ++impl_->stats.rtt_samples;
}

const PeerStreamConfig& PeerStream::config() const noexcept
{
    return impl_->config;
}

const PeerStreamStats& PeerStream::stats() const noexcept
{
    return impl_->stats;
}

std::uint64_t PeerStream::effectivePlayoutDelayFrames() const noexcept
{
    return impl_->effectivePlayoutDelayFrames();
}

bool PeerStream::playoutSampleTimeInitialized() const noexcept
{
    return impl_->playout_sample_time_initialized;
}

std::uint64_t PeerStream::nextPlayoutRemoteSampleTime() const noexcept
{
    return impl_->next_playout_remote_sample_time;
}

} // namespace jam2

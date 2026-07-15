#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include "CliStats.hpp"

#include "common.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace jam2::cli::stats {

double unit_from_ppm(int value)
{
    return static_cast<double>(std::clamp(value, 0, 1000000)) / 1000000.0;
}

double frames_to_ms(std::size_t frames, double sample_rate)
{
    return sample_rate > 0.0 ? (static_cast<double>(frames) * 1000.0 / sample_rate) : 0.0;
}

double signed_frames_to_ms(std::int64_t frames, double sample_rate)
{
    return sample_rate > 0.0 ? (static_cast<double>(frames) * 1000.0 / sample_rate) : 0.0;
}

std::size_t audio_payload_bytes(int frame_size)
{
    return static_cast<std::size_t>(frame_size > 0 ? frame_size : 0) * 3U;
}

std::size_t audio_packet_bytes(int frame_size)
{
    return jam2::protocol::kHeaderSize + audio_payload_bytes(frame_size);
}

std::string platform_name()
{
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";

#endif
}

void print_udp_parse_stats(const UdpParseStats& stats, std::ostream& out)
{
    out << "UDP parse rejects: total=" << stats.total()
        << " short=" << stats.short_packet
        << " magic=" << stats.wrong_magic
        << " version=" << stats.wrong_version
        << " type=" << stats.unknown_type
        << " flags=" << stats.invalid_flags
        << " reserved=" << stats.invalid_reserved
        << " session=" << stats.wrong_session
        << " size=" << stats.invalid_payload_size
        << " auth=" << stats.authentication_failed << "\n";
}

void copy_peer_stream_stats(
    AudioPacketStats& target,
    const jam2::PeerStreamStats& source)
{
    target.udp_replay_rejects = source.replay_rejects;
    target.udp_forward_gap_rejects = source.forward_gap_rejects;
    target.udp_forward_gap_resyncs = source.forward_gap_resyncs;
    target.udp_sequence_ambiguous_rejects = source.sequence_ambiguous_rejects;
    target.udp_sample_time_stale_rejects = source.sample_time_stale_rejects;
    target.udp_sample_time_future_rejects = source.sample_time_future_rejects;
#define JAM2_COPY_PEER_STAT(name) target.name = source.name
    JAM2_COPY_PEER_STAT(reorder_pending_high_water);
    JAM2_COPY_PEER_STAT(reorder_capacity_drops);
    JAM2_COPY_PEER_STAT(jitter_pending_high_water);
    JAM2_COPY_PEER_STAT(jitter_capacity_drops);
    JAM2_COPY_PEER_STAT(playback_dropped_frames);
    JAM2_COPY_PEER_STAT(playback_drop_events);
    JAM2_COPY_PEER_STAT(playback_drop_event_max_frames);
    JAM2_COPY_PEER_STAT(playback_depth_min_frames);
    JAM2_COPY_PEER_STAT(playback_depth_sum_frames);
    JAM2_COPY_PEER_STAT(playback_depth_max_frames);
    JAM2_COPY_PEER_STAT(playback_depth_samples);
    JAM2_COPY_PEER_STAT(stats_warmup_skipped_packets);
    JAM2_COPY_PEER_STAT(sequence);
    JAM2_COPY_PEER_STAT(reordered_recovered);
    JAM2_COPY_PEER_STAT(reordered_lost);
    JAM2_COPY_PEER_STAT(reordered_lost_events);
    JAM2_COPY_PEER_STAT(reordered_max_distance_packets);
    JAM2_COPY_PEER_STAT(jitter_min_us);
    JAM2_COPY_PEER_STAT(jitter_sum_us);
    JAM2_COPY_PEER_STAT(jitter_max_us);
    JAM2_COPY_PEER_STAT(jitter_samples);
    JAM2_COPY_PEER_STAT(audio_packet_gap_min_us);
    JAM2_COPY_PEER_STAT(audio_packet_gap_sum_us);
    JAM2_COPY_PEER_STAT(audio_packet_gap_max_us);
    JAM2_COPY_PEER_STAT(audio_packet_gap_samples);
    JAM2_COPY_PEER_STAT(audio_packet_gap_over_2x_count);
    JAM2_COPY_PEER_STAT(audio_packet_gap_over_4x_count);
    JAM2_COPY_PEER_STAT(playback_push_min_frames);
    JAM2_COPY_PEER_STAT(playback_push_sum_frames);
    JAM2_COPY_PEER_STAT(playback_push_max_frames);
    JAM2_COPY_PEER_STAT(playback_push_batches);
    JAM2_COPY_PEER_STAT(playback_depth_under_2ms_events);
    JAM2_COPY_PEER_STAT(playback_depth_under_2ms_max_duration_us);
    JAM2_COPY_PEER_STAT(playback_depth_under_5ms_events);
    JAM2_COPY_PEER_STAT(playback_depth_under_5ms_max_duration_us);
    JAM2_COPY_PEER_STAT(playback_depth_under_10ms_events);
    JAM2_COPY_PEER_STAT(playback_depth_under_10ms_max_duration_us);
    JAM2_COPY_PEER_STAT(rtt_min_us);
    JAM2_COPY_PEER_STAT(rtt_sum_us);
    JAM2_COPY_PEER_STAT(rtt_max_us);
    JAM2_COPY_PEER_STAT(drift_valid);
    JAM2_COPY_PEER_STAT(raw_drift_ppm);
    JAM2_COPY_PEER_STAT(drift_ppm);
    JAM2_COPY_PEER_STAT(resampler_ratio);
    JAM2_COPY_PEER_STAT(resampler_ratio_min);
    JAM2_COPY_PEER_STAT(resampler_ratio_sum);
    JAM2_COPY_PEER_STAT(resampler_ratio_max);
    JAM2_COPY_PEER_STAT(resampler_ratio_samples);
    JAM2_COPY_PEER_STAT(drift_correction_active_samples);
    JAM2_COPY_PEER_STAT(drift_correction_clamped_samples);
    JAM2_COPY_PEER_STAT(resampler_ratio_change_max_ppm_per_second);
    JAM2_COPY_PEER_STAT(sample_time_playout_enabled);
    JAM2_COPY_PEER_STAT(expected_remote_sample_time);
    JAM2_COPY_PEER_STAT(last_received_sample_time);
    JAM2_COPY_PEER_STAT(last_played_remote_sample_time);
    JAM2_COPY_PEER_STAT(remote_sample_lag_frames);
    JAM2_COPY_PEER_STAT(missing_sample_ranges);
    JAM2_COPY_PEER_STAT(missing_audio_frames_inserted);
    JAM2_COPY_PEER_STAT(late_audio_frames_dropped);
    JAM2_COPY_PEER_STAT(playout_delay_frames);
    JAM2_COPY_PEER_STAT(playout_delay_error_frames);
    JAM2_COPY_PEER_STAT(jitter_buffer_enabled);
    JAM2_COPY_PEER_STAT(jitter_buffer_target_frames);
    JAM2_COPY_PEER_STAT(jitter_buffer_max_frames);
    JAM2_COPY_PEER_STAT(jitter_buffer_depth_frames);
    JAM2_COPY_PEER_STAT(jitter_buffer_depth_max_frames);
    JAM2_COPY_PEER_STAT(jitter_buffer_queued_packets);
    JAM2_COPY_PEER_STAT(jitter_buffer_released_packets);
    JAM2_COPY_PEER_STAT(jitter_buffer_late_packets);
    JAM2_COPY_PEER_STAT(jitter_buffer_dropped_packets);
    JAM2_COPY_PEER_STAT(jitter_buffer_dropped_frames);
    JAM2_COPY_PEER_STAT(jitter_buffer_forced_releases);
    JAM2_COPY_PEER_STAT(adaptive_playback_cushion_enabled);
    JAM2_COPY_PEER_STAT(adaptive_playback_target_frames);
    JAM2_COPY_PEER_STAT(adaptive_playback_min_frames);
    JAM2_COPY_PEER_STAT(adaptive_playback_max_frames);
    JAM2_COPY_PEER_STAT(adaptive_playback_raise_events);
    JAM2_COPY_PEER_STAT(adaptive_playback_release_events);
    JAM2_COPY_PEER_STAT(adaptive_playback_burst_events);
    JAM2_COPY_PEER_STAT(adaptive_playback_padding_frames);
    JAM2_COPY_PEER_STAT(adaptive_playback_time_above_target_us);
    JAM2_COPY_PEER_STAT(adaptive_playback_time_under_target_us);
    JAM2_COPY_PEER_STAT(adaptive_playback_longest_above_target_us);
    JAM2_COPY_PEER_STAT(adaptive_playback_longest_under_target_us);
#undef JAM2_COPY_PEER_STAT
}

void add_peer_stream_stats(
    AudioPacketStats& target,
    const jam2::PeerStreamStats& source)
{
    target.udp_replay_rejects += source.replay_rejects;
    target.udp_forward_gap_rejects += source.forward_gap_rejects;
    target.udp_forward_gap_resyncs += source.forward_gap_resyncs;
    target.udp_sequence_ambiguous_rejects += source.sequence_ambiguous_rejects;
    target.udp_sample_time_stale_rejects += source.sample_time_stale_rejects;
    target.udp_sample_time_future_rejects += source.sample_time_future_rejects;
    target.reorder_pending_high_water = std::max(target.reorder_pending_high_water, source.reorder_pending_high_water);
    target.reorder_capacity_drops += source.reorder_capacity_drops;
    target.jitter_pending_high_water = std::max(target.jitter_pending_high_water, source.jitter_pending_high_water);
    target.jitter_capacity_drops += source.jitter_capacity_drops;
    target.playback_dropped_frames += source.playback_dropped_frames;
    target.playback_drop_events += source.playback_drop_events;
    target.playback_drop_event_max_frames = std::max(
        target.playback_drop_event_max_frames,
        source.playback_drop_event_max_frames);
    if (source.playback_depth_samples > 0) {
        if (target.playback_depth_samples == 0 || source.playback_depth_min_frames < target.playback_depth_min_frames) {
            target.playback_depth_min_frames = source.playback_depth_min_frames;
        }
        target.playback_depth_sum_frames += source.playback_depth_sum_frames;
        target.playback_depth_max_frames = std::max(target.playback_depth_max_frames, source.playback_depth_max_frames);
        target.playback_depth_samples += source.playback_depth_samples;
    }
    target.stats_warmup_skipped_packets += source.stats_warmup_skipped_packets;
    target.sequence.lost += source.sequence.lost;
    target.sequence.loss_events += source.sequence.loss_events;
    target.sequence.loss_max_gap = std::max(target.sequence.loss_max_gap, source.sequence.loss_max_gap);
    target.sequence.duplicate += source.sequence.duplicate;
    target.sequence.out_of_order += source.sequence.out_of_order;
    target.sequence.late += source.sequence.late;
    target.reordered_recovered += source.reordered_recovered;
    target.reordered_lost += source.reordered_lost;
    target.reordered_lost_events += source.reordered_lost_events;
    target.reordered_max_distance_packets = std::max(
        target.reordered_max_distance_packets,
        source.reordered_max_distance_packets);
    if (source.jitter_samples > 0) {
        if (target.jitter_samples == 0 || source.jitter_min_us < target.jitter_min_us) {
            target.jitter_min_us = source.jitter_min_us;
        }
        target.jitter_sum_us += source.jitter_sum_us;
        target.jitter_max_us = std::max(target.jitter_max_us, source.jitter_max_us);
        target.jitter_samples += source.jitter_samples;
    }
    if (source.rtt_samples > 0) {
        if (target.recv_pongs == 0 || source.rtt_min_us < target.rtt_min_us) {
            target.rtt_min_us = source.rtt_min_us;
        }
        target.rtt_sum_us += source.rtt_sum_us;
        target.rtt_max_us = std::max(target.rtt_max_us, source.rtt_max_us);
    }
    if (std::abs(source.drift_ppm) > std::abs(target.drift_ppm)) {
        target.drift_valid = source.drift_valid;
        target.raw_drift_ppm = source.raw_drift_ppm;
        target.drift_ppm = source.drift_ppm;
        target.resampler_ratio = source.resampler_ratio;
    }
    target.missing_sample_ranges += source.missing_sample_ranges;
    target.missing_audio_frames_inserted += source.missing_audio_frames_inserted;
    target.late_audio_frames_dropped += source.late_audio_frames_dropped;
    target.playout_delay_frames = std::max(target.playout_delay_frames, source.playout_delay_frames);
    target.jitter_buffer_enabled = target.jitter_buffer_enabled || source.jitter_buffer_enabled;
    target.jitter_buffer_target_frames = std::max(target.jitter_buffer_target_frames, source.jitter_buffer_target_frames);
    target.jitter_buffer_max_frames = std::max(target.jitter_buffer_max_frames, source.jitter_buffer_max_frames);
    target.jitter_buffer_depth_frames += source.jitter_buffer_depth_frames;
    target.jitter_buffer_depth_max_frames = std::max(
        target.jitter_buffer_depth_max_frames,
        source.jitter_buffer_depth_max_frames);
    target.jitter_buffer_queued_packets += source.jitter_buffer_queued_packets;
    target.jitter_buffer_released_packets += source.jitter_buffer_released_packets;
    target.jitter_buffer_late_packets += source.jitter_buffer_late_packets;
    target.jitter_buffer_dropped_packets += source.jitter_buffer_dropped_packets;
    target.jitter_buffer_dropped_frames += source.jitter_buffer_dropped_frames;
    target.jitter_buffer_forced_releases += source.jitter_buffer_forced_releases;
    target.adaptive_playback_cushion_enabled =
        target.adaptive_playback_cushion_enabled || source.adaptive_playback_cushion_enabled;
    target.adaptive_playback_target_frames = std::max(
        target.adaptive_playback_target_frames,
        source.adaptive_playback_target_frames);
    target.adaptive_playback_raise_events += source.adaptive_playback_raise_events;
    target.adaptive_playback_release_events += source.adaptive_playback_release_events;
    target.adaptive_playback_burst_events += source.adaptive_playback_burst_events;
    target.adaptive_playback_padding_frames += source.adaptive_playback_padding_frames;
}

void copy_peer_mixer_stats(
    AudioPacketStats& target,
    const jam2::PeerMixerStats& source)
{
    target.network_active_peer_count = source.active_peers;
    target.mix_contributing_peers = source.contributing_peers;
    target.mix_active_slots = source.active_slots;
    target.mix_max_slots = source.max_slots;
    target.mix_active_slots_high_water = source.active_slots_high_water;
    target.mix_released_slots = source.released_slots;
    target.mix_complete_slots = source.complete_slots;
    target.mix_deadline_slots = source.deadline_slots;
    target.mix_missing_peer_contributions = source.missing_peer_contributions;
    target.mix_missing_peer_frames = source.missing_peer_frames;
    target.mix_late_after_release_frames = source.late_after_release_frames;
    target.mix_capacity_drops = source.capacity_drops;
    target.mix_capacity_dropped_frames = source.capacity_dropped_frames;
    target.mix_clipped_samples = source.clipped_samples;
    target.mix_output_frames = source.output_frames;
    target.mix_output_drop_requested_frames = source.output_drop_requested_frames;
    target.mix_output_drop_request_events = source.output_drop_request_events;

    target.mix_output_dropped_frames = source.output_dropped_frames;

    target.mix_work_budget_yields = source.work_budget_yields;
    target.adaptive_playback_cushion_enabled = source.adaptive_playback_cushion_enabled;
    target.adaptive_playback_target_frames = source.adaptive_target_frames;
    target.adaptive_playback_raise_events = source.adaptive_raise_events;
    target.adaptive_playback_release_events = source.adaptive_release_events;
    target.adaptive_playback_burst_events = source.adaptive_raise_events;
    target.adaptive_playback_padding_frames = source.adaptive_padding_frames;
}

std::string csv_escape(std::string_view value)
{
    bool quote = false;
    for (const char c : value) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            quote = true;
            break;
        }
    }
    if (!quote) {
        return std::string(value);
    }
    std::string out;
    out.reserve(value.size() + 2);
    out.push_back('"');
    for (const char c : value) {
        if (c == '"') {
            out.push_back('"');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

void append_os_scheduling_csv(std::ostream& out, const OsSchedulingStatus& status)
{
    out << ',' << csv_escape(os_priority_text(status.requested))
        << ',' << csv_escape(status.platform)
        << ',' << status.cpu_count
        << ',' << csv_escape(status.process_priority)
        << ',' << csv_escape(status.thread_priority)
        << ',' << csv_escape(status.mmcss_requested)
        << ',' << csv_escape(status.mmcss_active)
        << ',' << csv_escape(status.mmcss_profile)
        << ',' << csv_escape(status.mmcss_error)
        << ',' << csv_escape(status.timer_resolution_requested)
        << ',' << csv_escape(status.timer_resolution_active)
        << ',' << csv_escape(status.timer_resolution_error)
        << ',' << csv_escape(status.qos_requested)
        << ',' << csv_escape(status.qos_active)
        << ',' << csv_escape(status.qos_error)
        << ',' << csv_escape(status.realtime_requested)
        << ',' << csv_escape(status.realtime_active)
        << ',' << csv_escape(status.realtime_error);
}

std::string command_line_text(int argc, char** argv)
{
    std::ostringstream out;
    bool redact_next = false;
    for (int i = 0; i < argc; ++i) {
        if (i > 0) {
            out << ' ';
        }
        std::string value = argv[i];
        if (redact_next) {

            value = "<redacted>";
            redact_next = false;
        } else if (value == "--session-key") {
            redact_next = true;

        } else if (value.rfind("--session-key=", 0) == 0) {
            value = "--session-key=<redacted>";
        } else if (value.rfind("jam2://v1?", 0) == 0) {
            std::size_t key = value.find("?key=");
            if (key == std::string::npos) {
                key = value.find("&key=");
            }
            if (key != std::string::npos) {
                const std::size_t key_value = key + 5;
                const std::size_t key_end = value.find('&', key_value);
                value.replace(
                    key_value,

                    key_end == std::string::npos ? value.size() - key_value : key_end - key_value,
                    "<redacted>");
            }
        }
        out << value;
    }
    return out.str();
}

double playback_depth_avg_frames(const AudioPacketStats& stats)
{
    return stats.playback_depth_samples > 0 ?
        static_cast<double>(stats.playback_depth_sum_frames) / static_cast<double>(stats.playback_depth_samples) :
        0.0;
}

double playback_depth_avg_ms(const AudioPacketStats& stats, const Options& options)
{
    return options.sample_rate > 0 ?
        playback_depth_avg_frames(stats) * 1000.0 / static_cast<double>(options.sample_rate) :
        0.0;
}

double frames_percent(std::uint64_t frames, std::uint64_t total_frames)
{
    return total_frames > 0 ?
        static_cast<double>(frames) * 100.0 / static_cast<double>(total_frames) :
        0.0;
}

double packet_path_frame_percent(std::uint64_t frames, std::uint64_t audio_frame_seconds)
{
    return audio_frame_seconds > 0 ?
        static_cast<double>(frames) * 100.0 / static_cast<double>(audio_frame_seconds) :
        0.0;
}

double avg_us_to_ms(std::uint64_t sum_us, std::uint64_t samples)
{
    return samples > 0 ?
        static_cast<double>(sum_us) / static_cast<double>(samples) / 1000.0 :
        0.0;
}

double avg_u64(std::uint64_t sum, std::uint64_t samples)
{
    return samples > 0 ?
        static_cast<double>(sum) / static_cast<double>(samples) :
        0.0;
}

double avg_double(double sum, std::uint64_t samples)
{
    return samples > 0 ? sum / static_cast<double>(samples) : 0.0;
}

double rtt_avg_ms(const AudioPacketStats& stats)
{
    return stats.recv_pongs > 0 ?
        static_cast<double>(stats.rtt_sum_us) / static_cast<double>(stats.recv_pongs) / 1000.0 :
        0.0;
}

double sequence_loss_percent(const AudioPacketStats& stats)
{
    const std::uint64_t denominator = stats.recv_packets + stats.sequence.lost;
    return denominator > 0 ? static_cast<double>(stats.sequence.lost) * 100.0 / static_cast<double>(denominator) : 0.0;
}

double estimated_one_way_ms(const AudioPacketStats& stats, const Options& options)
{
    const double network_ms = stats.recv_pongs > 0 ? rtt_avg_ms(stats) * 0.5 : 0.0;
    const double audio_buffer_ms = options.audio_buffer_size > 0 ?
        frames_to_ms(static_cast<std::size_t>(options.audio_buffer_size), options.sample_rate) :
        frames_to_ms(static_cast<std::size_t>(options.frame_size), options.sample_rate);
    const double jitter_buffer_ms = options.jitter_buffer_frames > 0 ?
        frames_to_ms(options.jitter_buffer_frames, options.sample_rate) :
        0.0;
    return network_ms + jitter_buffer_ms + playback_depth_avg_ms(stats, options) + audio_buffer_ms;
}

std::tm local_time_from(std::time_t value)
{
    std::tm out{};
#if defined(_WIN32)
    localtime_s(&out, &value);
#else
    localtime_r(&value, &out);
#endif
    return out;
}

unsigned long current_process_id()
{
#if defined(_WIN32)
    return static_cast<unsigned long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long>(getpid());
#endif
}

std::filesystem::path make_stats_csv_path(const std::filesystem::path& folder)
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    const std::tm local = local_time_from(now_time);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count() % 1000;
    std::ostringstream name;
    name << "jam2_stats_"
         << std::put_time(&local, "%Y%m%d_%H%M%S")
         << '_'
         << std::setw(3) << std::setfill('0') << millis
         << "_pid" << current_process_id()
         << ".csv";
    return folder / name.str();
}



CsvStatsLog::AudioSnapshot make_audio_snapshot(
    const jam2::Engine* engine)
{
    CsvStatsLog::AudioSnapshot snapshot;
    if (engine == nullptr) {
        return snapshot;
    }
    const jam2::EngineSnapshot engine_snapshot = engine->snapshot();
    if (!engine_snapshot.frame_clock_active) return snapshot;
    snapshot.has_audio = true;
    snapshot.stream = engine->coldSnapshot().stream;
    snapshot.callbacks = engine_snapshot.callbacks;
    snapshot.callback_timing = engine_snapshot.callback_timing;
    snapshot.playback_prefilled = engine_snapshot.playback_prefilled;
    const auto& capture = engine_snapshot.capture_ring;
    snapshot.capture_ring_overruns = capture.overruns;
    snapshot.capture_ring_overrun_events = capture.overrun_events;
    snapshot.capture_ring_overrun_event_max_frames = capture.overrun_event_max_frames;
    snapshot.capture_ring_underruns = capture.underruns;
    snapshot.capture_ring_underrun_events = capture.underrun_events;
    snapshot.capture_ring_readable = engine_snapshot.capture_ring_depth_frames;
    const auto& playback = engine_snapshot.playback_ring;
    snapshot.playback_ring_overruns = playback.overruns;
    snapshot.playback_ring_underruns = playback.underruns;
    snapshot.playback_ring_underrun_events = playback.underrun_events;
    snapshot.playback_ring_underrun_event_max_frames = playback.underrun_event_max_frames;
    snapshot.playback_ring_underrun_burst_events = playback.underrun_burst_events;
    snapshot.playback_ring_underrun_burst_max_frames = playback.underrun_burst_max_frames;
    snapshot.playback_ring_underrun_burst_current_frames = playback.underrun_burst_current_frames;
    snapshot.playback_depth_under_2ms_frames = playback.depth_under_2ms_frames;
    snapshot.playback_depth_under_5ms_frames = playback.depth_under_5ms_frames;
    snapshot.playback_depth_under_10ms_frames = playback.depth_under_10ms_frames;
    snapshot.playback_depth_10ms_plus_frames = playback.depth_10ms_plus_frames;
    snapshot.playback_depth_observed_frames = playback.depth_observed_frames;
    snapshot.playback_drop_requested_frames = playback.drop_requested_frames;
    snapshot.playback_drop_applied_frames = playback.drop_applied_frames;
    snapshot.playback_drop_coalesced_requests = playback.drop_coalesced_requests;
    snapshot.playback_drop_pending_frames = playback.drop_pending_frames;
    snapshot.playback_drop_max_batch_frames = playback.drop_max_batch_frames;
    snapshot.playback_ring_readable = engine_snapshot.playback_ring_depth_frames;
    snapshot.input_peak = unit_from_ppm(engine_snapshot.input_peak_ppm);
    snapshot.send_peak = unit_from_ppm(engine_snapshot.send_peak_ppm);
    snapshot.monitor_peak = unit_from_ppm(engine_snapshot.monitor_peak_ppm);
    snapshot.remote_peak = unit_from_ppm(engine_snapshot.remote_peak_ppm);
    snapshot.metronome_peak = unit_from_ppm(engine_snapshot.metronome_peak_ppm);
    snapshot.output_peak = unit_from_ppm(engine_snapshot.output_peak_ppm);
    snapshot.output_clipped_samples = engine_snapshot.output_clipped_samples;
    snapshot.prepared_source_frame = engine_snapshot.prepared_source_frame;
    snapshot.prepared_source_scheduled_start_frame = engine_snapshot.prepared_source_scheduled_start_frame;
    snapshot.prepared_source_actual_start_frame = engine_snapshot.prepared_source_actual_start_frame;
    snapshot.prepared_source_underruns = engine_snapshot.prepared_source_underruns;
    snapshot.prepared_source_busy_events = engine_snapshot.prepared_source_busy_events;
    snapshot.network_capture_enabled = engine_snapshot.network_capture_enabled;
    snapshot.network_capture_generation = engine_snapshot.network_capture_generation;
    snapshot.network_capture_ready = engine_snapshot.network_capture_ready;
    snapshot.network_capture_epoch_frame = engine_snapshot.network_capture_epoch_frame;
    snapshot.network_capture_stale_frames_discarded = engine_snapshot.network_capture_stale_frames_discarded;
    snapshot.network_playback_enabled = engine_snapshot.network_playback_enabled;
    return snapshot;
}

CsvStatsLog::Context make_csv_context(
    std::string command_line,
    std::string_view mode,
    const Options& options,
    const jam2::UdpSocket& socket,
    const jam2::Endpoint& local,
    const jam2::Endpoint& peer,
    std::string_view endpoint_mode)

{
    CsvStatsLog::Context context;
    context.command_line = std::move(command_line);
    context.mode = std::string(mode);
    context.platform = platform_name();
    context.local_endpoint = jam2::endpoint_to_string(local);
    context.peer_endpoint = jam2::endpoint_to_string(peer);
    context.endpoint_mode = std::string(endpoint_mode);

    context.socket_send_buffer_bytes = socket.send_buffer_size();
    context.socket_recv_buffer_bytes = socket.recv_buffer_size();
    context.requested_socket_send_buffer_bytes = options.socket_send_buffer.value_or(0);
    context.requested_socket_recv_buffer_bytes = options.socket_recv_buffer.value_or(0);
    context.requested_sample_rate = options.sample_rate;
    context.frame_size = options.frame_size;
    context.requested_audio_buffer_frames = options.audio_buffer_size;
    context.capture_ring_frames = options.capture_ring_frames;
    context.playback_ring_frames = options.playback_ring_frames;
    context.playback_prefill_frames = options.playback_prefill_frames;
    context.playback_max_frames = options.playback_max_frames;
    context.stats_warmup_ms = options.stats_warmup_ms;
    context.drift_correction = options.drift_correction;
    context.drift_smoothing = options.drift_smoothing;
    context.drift_deadband_ppm = options.drift_deadband_ppm;
    context.drift_max_correction_ppm = options.drift_max_correction_ppm;

    context.metronome = options.metronome;
    context.bpm = options.bpm;
    context.metronome_level = options.metronome_level;
    context.metronome_mode = std::string(metronome_mode_text(options.metronome_mode));
    context.sample_time_playout = options.sample_time_playout;
    context.playout_delay_frames = options.playout_delay_frames;
    context.jitter_buffer_frames = options.jitter_buffer_frames;
    context.jitter_buffer_max_frames = options.jitter_buffer_max_frames;
    context.remote_level = options.remote_level;
    context.audio_device_id = options.audio_device_id.value_or(-1);
    context.requested_input_mode = mono_mix_mode_text(options.channel_selection.input.size());
    context.requested_channels = channel_selection_text(options.channel_selection);
    return context;
}

void print_periodic_stream_stats(
    const AudioPacketStats& stats,
    const Options& options,
    const CsvStatsLog::AudioSnapshot& audio,
    std::uint64_t elapsed_ms)
{
    const double underrun_event_max_ms =
        frames_to_ms(static_cast<std::size_t>(audio.playback_ring_underrun_event_max_frames), options.sample_rate);
    const double underrun_burst_max_ms =
        frames_to_ms(static_cast<std::size_t>(audio.playback_ring_underrun_burst_max_frames), options.sample_rate);
    std::cout << "stats elapsed_ms=" << elapsed_ms
              << " frame_size=" << options.frame_size
              << " sent_packets=" << stats.sent_packets
              << " recv_packets=" << stats.recv_packets
              << " sequence_lost=" << stats.sequence.lost
              << " sequence_loss_events=" << stats.sequence.loss_events
              << " sequence_loss_max_gap=" << stats.sequence.loss_max_gap
              << " sequence_loss_percent=" << sequence_loss_percent(stats)
              << " sequence_duplicate=" << stats.sequence.duplicate
              << " sequence_out_of_order=" << stats.sequence.out_of_order
              << " reordered_recovered=" << stats.reordered_recovered
              << " reordered_lost=" << stats.reordered_lost
              << " reordered_lost_events=" << stats.reordered_lost_events
              << " reordered_max_distance_packets=" << stats.reordered_max_distance_packets
              << " sequence_late=" << stats.sequence.late
              << " playback_depth_avg_ms=" << playback_depth_avg_ms(stats, options)
              << " playback_ring_readable_ms=" << frames_to_ms(audio.playback_ring_readable, options.sample_rate)
              << " playback_ring_underruns=" << audio.playback_ring_underruns
              << " playback_ring_underrun_events=" << audio.playback_ring_underrun_events
              << " playback_ring_underrun_event_max_ms=" << underrun_event_max_ms
              << " playback_ring_underrun_burst_events=" << audio.playback_ring_underrun_burst_events
              << " playback_ring_underrun_burst_max_ms=" << underrun_burst_max_ms
              << " playback_ring_underrun_time_ms="
              << frames_to_ms(static_cast<std::size_t>(audio.playback_ring_underruns), options.sample_rate)
              << " playback_ring_underrun_time_percent="
              << frames_percent(audio.playback_ring_underruns, audio.playback_depth_observed_frames)
              << " playback_depth_observed_frames=" << audio.playback_depth_observed_frames
              << " jitter_avg_ms="
              << (stats.jitter_samples > 0 ?
                  static_cast<double>(stats.jitter_sum_us) / static_cast<double>(stats.jitter_samples) / 1000.0 :
                  0.0)
              << " jitter_max_ms=" << (stats.jitter_samples > 0 ? static_cast<double>(stats.jitter_max_us) / 1000.0 : 0.0)
              << " audio_packet_gap_max_ms="
              << (stats.audio_packet_gap_samples > 0 ? static_cast<double>(stats.audio_packet_gap_max_us) / 1000.0 : 0.0)
              << " audio_packet_gap_over_2x_count=" << stats.audio_packet_gap_over_2x_count
              << " audio_packet_gap_over_4x_count=" << stats.audio_packet_gap_over_4x_count
              << " audio_packet_gap_samples=" << stats.audio_packet_gap_samples
              << " receive_loop_gap_max_ms="
              << (stats.receive_loop_gap_samples > 0 ? static_cast<double>(stats.receive_loop_gap_max_us) / 1000.0 : 0.0)
              << " receive_burst_packets_max=" << stats.receive_burst_packets_max
              << " estimated_one_way_ms=" << estimated_one_way_ms(stats, options)
              << " rtt_avg_ms=" << rtt_avg_ms(stats)
              << " drift_ppm=" << stats.drift_ppm
              << " resampler_ratio=" << stats.resampler_ratio
              << " playback_dropped_frames=" << stats.playback_dropped_frames
              << " missing_audio_frames_inserted=" << stats.missing_audio_frames_inserted
              << " late_audio_frames_dropped=" << stats.late_audio_frames_dropped
              << " jitter_buffer=" << (stats.jitter_buffer_enabled ? "on" : "off")
              << " jitter_buffer_target_frames=" << stats.jitter_buffer_target_frames
              << " jitter_buffer_depth_frames=" << stats.jitter_buffer_depth_frames
              << " jitter_buffer_released_packets=" << stats.jitter_buffer_released_packets
              << " jitter_buffer_late_packets=" << stats.jitter_buffer_late_packets
              << " jitter_buffer_dropped_packets=" << stats.jitter_buffer_dropped_packets
              << " jitter_buffer_forced_releases=" << stats.jitter_buffer_forced_releases
              << " metronome_compensation_active=" << (stats.metronome_compensation_active ? "yes" : "no")
              << " metronome_compensation_offset_ms="
              << signed_frames_to_ms(stats.metronome_compensation_offset_frames, options.sample_rate)
              << " metronome_compensation_target_ms="
              << signed_frames_to_ms(stats.metronome_compensation_target_frames, options.sample_rate)
              << " input_peak=" << audio.input_peak
              << " send_peak=" << audio.send_peak
              << " monitor_peak=" << audio.monitor_peak
              << " remote_peak=" << audio.remote_peak
              << " metronome_peak=" << audio.metronome_peak
              << " output_peak=" << audio.output_peak
              << " output_clipped_samples=" << audio.output_clipped_samples
              << " os_priority_requested=" << os_priority_text(stats.os_scheduling.requested)
              << " os_cpu_count=" << stats.os_scheduling.cpu_count
              << " os_process_priority_active=" << stats.os_scheduling.process_priority
              << " os_thread_priority_active=" << stats.os_scheduling.thread_priority
              << " os_mmcss_active=" << stats.os_scheduling.mmcss_active
              << " os_mmcss_profile=" << stats.os_scheduling.mmcss_profile
              << " os_timer_resolution_active=" << stats.os_scheduling.timer_resolution_active
              << " os_qos_active=" << stats.os_scheduling.qos_active
              << " os_realtime_active=" << stats.os_scheduling.realtime_active
              << "\n";
}

void print_audio_packet_stats(const AudioPacketStats& stats, const Options& options)
{
    const std::uint64_t stream_ms = stats.elapsed_ms > 0 ? stats.elapsed_ms : static_cast<std::uint64_t>(options.stream_ms);
    const double seconds = static_cast<double>(stream_ms) / 1000.0;
    const double send_bitrate = seconds > 0.0 ? (static_cast<double>(stats.sent_bytes) * 8.0 / seconds) : 0.0;
    const double recv_bitrate = seconds > 0.0 ? (static_cast<double>(stats.recv_bytes) * 8.0 / seconds) : 0.0;
    const double sent_rate = seconds > 0.0 ? static_cast<double>(stats.sent_packets) / seconds : 0.0;
    const double recv_rate = seconds > 0.0 ? static_cast<double>(stats.recv_packets) / seconds : 0.0;
    const double frame_interval_ms =
        options.sample_rate > 0 ? (static_cast<double>(options.frame_size) * 1000.0 / static_cast<double>(options.sample_rate)) : 0.0;
    const std::uint64_t expected_packets =
        options.stream_ms > 0 && options.frame_size > 0 ?
            ((static_cast<std::uint64_t>(stream_ms) * static_cast<std::uint64_t>(options.sample_rate)) +
             (static_cast<std::uint64_t>(options.frame_size) * 1000ULL) - 1ULL) /
                (static_cast<std::uint64_t>(options.frame_size) * 1000ULL) :
            0;
    std::cout << "Network session: local_peer_id=" << stats.local_peer_id
              << " remote_peer_id=" << stats.remote_peer_id
              << " bootstrap=" << stats.bootstrap_role
              << " protocol=" << stats.session_protocol_version
              << " format=" << stats.session_audio_format
              << " sample_rate=" << stats.session_sample_rate
              << " frames_per_packet=" << stats.session_frames_per_packet << "\n";
    std::cout << "Authority: bootstrap_coordinator_peer_id=" << stats.bootstrap_coordinator_peer_id
              << " arrangement_authority_peer_id=" << stats.arrangement_authority_peer_id
              << " grid_authority_peer_id=" << stats.grid_authority_peer_id
              << " grid_revision=" << stats.grid_revision
              << " grid_run_state=" << stats.grid_run_state
              << " grid_mode=" << stats.grid_mode
              << " authority_epoch_frame=" << stats.grid_authority_epoch_frame
              << " mapped_epoch_frame=" << stats.grid_mapped_epoch_frame
              << " mapping_error_frames=" << stats.grid_mapping_error_frames << "\n";
    std::cout << "Leader audio: source_peer_id=" << stats.leader_audio_source_peer_id
              << " injected_packets=" << stats.leader_audio_injected_packets << "\n";
    std::cout << "Transport authority: source_peer_id=" << stats.transport_source_peer_id
              << " event_counter=" << stats.transport_event_counter
              << " grid_revision=" << stats.transport_grid_revision
              << " action=" << stats.transport_action
              << " source_frame=" << stats.transport_source_frame
              << " requested_target_frame=" << stats.transport_requested_target_frame
              << " applied_target_frame=" << stats.transport_applied_target_frame << "\n";
    std::cout << "Stream duration ms: " << stream_ms << "\n";
    std::cout << "Sample rate: " << options.sample_rate << "\n";
    std::cout << "Frame size samples: " << options.frame_size << "\n";
    std::cout << "Frame interval ms: " << frame_interval_ms << "\n";
    std::cout << "OS priority requested: " << os_priority_text(options.os_priority) << "\n";
    std::cout << "OS priority active request: " << os_priority_text(stats.os_scheduling.requested) << "\n";
    std::cout << "OS platform: " << stats.os_scheduling.platform << "\n";
    std::cout << "OS CPU count: " << stats.os_scheduling.cpu_count << "\n";
    std::cout << "OS process priority active: " << stats.os_scheduling.process_priority << "\n";
    std::cout << "OS thread priority active: " << stats.os_scheduling.thread_priority << "\n";
    std::cout << "OS MMCSS requested: " << stats.os_scheduling.mmcss_requested << "\n";
    std::cout << "OS MMCSS active: " << stats.os_scheduling.mmcss_active << "\n";
    std::cout << "OS MMCSS profile: " << stats.os_scheduling.mmcss_profile << "\n";
    std::cout << "OS MMCSS error: " << stats.os_scheduling.mmcss_error << "\n";
    std::cout << "OS timer resolution requested: " << stats.os_scheduling.timer_resolution_requested << "\n";
    std::cout << "OS timer resolution active: " << stats.os_scheduling.timer_resolution_active << "\n";
    std::cout << "OS timer resolution error: " << stats.os_scheduling.timer_resolution_error << "\n";
    std::cout << "OS QoS requested: " << stats.os_scheduling.qos_requested << "\n";
    std::cout << "OS QoS active: " << stats.os_scheduling.qos_active << "\n";
    std::cout << "OS QoS error: " << stats.os_scheduling.qos_error << "\n";
    std::cout << "OS realtime requested: " << stats.os_scheduling.realtime_requested << "\n";
    std::cout << "OS realtime active: " << stats.os_scheduling.realtime_active << "\n";
    std::cout << "OS realtime error: " << stats.os_scheduling.realtime_error << "\n";
    std::cout << "Audio UDP payload bytes: " << audio_payload_bytes(options.frame_size) << "\n";
    std::cout << "Audio UDP packet bytes: " << audio_packet_bytes(options.frame_size) << "\n";
    std::cout << "Audio buffer size frames: " << options.audio_buffer_size << "\n";
    std::cout << "Audio buffer size ms: "
              << frames_to_ms(static_cast<std::size_t>(options.audio_buffer_size > 0 ? options.audio_buffer_size : 0), options.sample_rate)
              << "\n";
    std::cout << "Drift correction: " << (options.drift_correction ? "on" : "off") << "\n";
    std::cout << "Drift smoothing alpha: " << options.drift_smoothing << "\n";
    std::cout << "Drift deadband ppm: " << options.drift_deadband_ppm << "\n";
    std::cout << "Drift max correction ppm: " << options.drift_max_correction_ppm << "\n";
    std::cout << "Metronome: " << (options.metronome ? "on" : "off") << "\n";
    std::cout << "BPM: " << options.bpm << "\n";

    std::cout << "Metronome level: " << options.metronome_level << "\n";
    std::cout << "Metronome mode: " << metronome_mode_text(options.metronome_mode) << "\n";
    std::cout << "Metronome compensation max ms: " << options.metronome_compensation_max_ms << "\n";
    std::cout << "Metronome compensation smoothing ms: " << options.metronome_compensation_smoothing_ms << "\n";
    std::cout << "Metronome compensation deadband ms: " << options.metronome_compensation_deadband_ms << "\n";
    std::cout << "Metronome compensation slew ms per sec: " << options.metronome_compensation_slew_ms_per_sec << "\n";
    std::cout << "Remote playback level: " << options.remote_level << "\n";
    std::cout << "Send level: " << options.send_level << "\n";
    std::cout << "Local monitor: " << (options.local_monitor ? "on" : "off") << "\n";

    std::cout << "Local monitor level: " << options.local_monitor_level << "\n";
    std::cout << "Sample-time playout: " << (options.sample_time_playout ? "on" : "off") << "\n";
    std::cout << "Playout delay frames: " << options.playout_delay_frames << "\n";
    std::cout << "Playout delay ms: " << frames_to_ms(options.playout_delay_frames, options.sample_rate) << "\n";
    std::cout << "Jitter buffer requested: " << (options.jitter_buffer_frames > 0 ? "on" : "off") << "\n";
    std::cout << "Jitter buffer target frames requested: " << options.jitter_buffer_frames << "\n";
    std::cout << "Jitter buffer target ms requested: " << frames_to_ms(options.jitter_buffer_frames, options.sample_rate) << "\n";
    std::cout << "Jitter buffer max frames requested: " << options.jitter_buffer_max_frames << "\n";
    std::cout << "Jitter buffer max ms requested: " << frames_to_ms(options.jitter_buffer_max_frames, options.sample_rate) << "\n";
    std::cout << "Adaptive playback cushion requested: " << (options.adaptive_playback_cushion ? "on" : "off") << "\n";
    std::cout << "Adaptive playback target frames requested: " << options.adaptive_playback_target_frames << "\n";
    std::cout << "Adaptive playback min frames requested: " << options.adaptive_playback_min_frames << "\n";
    std::cout << "Adaptive playback max frames requested: " << options.adaptive_playback_max_frames << "\n";
    std::cout << "Adaptive playback release ppm: " << options.adaptive_playback_release_ppm << "\n";
    std::cout << "Final metronome: " << (stats.final_metronome_enabled ? "on" : "off") << "\n";
    std::cout << "Final BPM: " << stats.final_bpm << "\n";
    std::cout << "Final metronome level: " << stats.final_metronome_level << "\n";

    std::cout << "Final remote playback level: " << stats.final_remote_level << "\n";
    std::cout << "Final send level: " << stats.final_send_level << "\n";
    std::cout << "Final local monitor: " << (stats.final_local_monitor_enabled ? "on" : "off") << "\n";
    std::cout << "Final local monitor level: " << stats.final_local_monitor_level << "\n";
    if (options.stream_ms > 0) {
        std::cout << "Expected audio packets: " << expected_packets << "\n";
    }
    std::cout << "Audio packets sent: " << stats.sent_packets << "\n";
    std::cout << "Audio packets received: " << stats.recv_packets << "\n";
    std::cout << "Startup UDP packets drained: " << stats.startup_drained_packets << "\n";
    std::cout << "Audio packets received plus startup drained: "
              << (stats.recv_packets + stats.startup_drained_packets) << "\n";
    std::cout << "Stats warmup ms: " << options.stats_warmup_ms << "\n";
    std::cout << "Stats warmup skipped audio packets: " << stats.stats_warmup_skipped_packets << "\n";
    std::cout << "Audio send packet rate pps: " << sent_rate << "\n";
    std::cout << "Audio receive packet rate pps: " << recv_rate << "\n";
    std::cout << "Send bitrate bps: " << send_bitrate << "\n";
    std::cout << "Send bitrate kbps: " << (send_bitrate / 1000.0) << "\n";
    std::cout << "Receive bitrate bps: " << recv_bitrate << "\n";
    std::cout << "Receive bitrate kbps: " << (recv_bitrate / 1000.0) << "\n";
    std::cout << "Ignored audio packets: " << stats.ignored_packets << "\n";
    print_udp_parse_stats(stats.udp_parse);
    if (stats.playback_depth_samples > 0) {
        std::cout << "Playback dropped frames: " << stats.playback_dropped_frames << "\n";
        std::cout << "Playback drop events: " << stats.playback_drop_events << "\n";
        std::cout << "Playback drop event max frames: " << stats.playback_drop_event_max_frames << "\n";
        std::cout << "Playback drop event max ms: "
                  << frames_to_ms(static_cast<std::size_t>(stats.playback_drop_event_max_frames), options.sample_rate) << "\n";
        std::cout << "Playback dropped time ms: "
                  << frames_to_ms(static_cast<std::size_t>(stats.playback_dropped_frames), options.sample_rate) << "\n";
        std::cout << "Playback depth frames min: " << stats.playback_depth_min_frames << "\n";
        std::cout << "Playback depth frames avg: "
                  << (static_cast<double>(stats.playback_depth_sum_frames) / static_cast<double>(stats.playback_depth_samples)) << "\n";
        std::cout << "Playback depth frames max: " << stats.playback_depth_max_frames << "\n";
        std::cout << "Playback depth ms min: " << frames_to_ms(static_cast<std::size_t>(stats.playback_depth_min_frames), options.sample_rate) << "\n";
        std::cout << "Playback depth ms avg: "
                  << (static_cast<double>(stats.playback_depth_sum_frames) * 1000.0 /
                      static_cast<double>(stats.playback_depth_samples) / static_cast<double>(options.sample_rate)) << "\n";
        std::cout << "Playback depth ms max: " << frames_to_ms(static_cast<std::size_t>(stats.playback_depth_max_frames), options.sample_rate) << "\n";
    }
    if (stats.audio_delay_samples > 0) {
        std::cout << "Audio receive delay ms min: " << (static_cast<double>(stats.audio_delay_min_us) / 1000.0) << "\n";
        std::cout << "Audio receive delay ms avg: "
                  << (static_cast<double>(stats.audio_delay_sum_us) / static_cast<double>(stats.audio_delay_samples) / 1000.0) << "\n";
        std::cout << "Audio receive delay ms max: " << (static_cast<double>(stats.audio_delay_max_us) / 1000.0) << "\n";
    } else {
        std::cout << "Audio receive delay ms: unavailable across peer clocks\n";
    }
    if (stats.jitter_samples > 0) {
        std::cout << "Audio interarrival jitter ms min: " << (static_cast<double>(stats.jitter_min_us) / 1000.0) << "\n";
        std::cout << "Audio interarrival jitter ms avg: "
                  << (static_cast<double>(stats.jitter_sum_us) / static_cast<double>(stats.jitter_samples) / 1000.0) << "\n";
        std::cout << "Audio interarrival jitter ms max: " << (static_cast<double>(stats.jitter_max_us) / 1000.0) << "\n";
    }
    std::cout << "PING sent: " << stats.sent_pings << "\n";
    std::cout << "PONG sent: " << stats.sent_pongs << "\n";
    std::cout << "PONG received: " << stats.recv_pongs << "\n";
    std::cout << "BYE sent: " << (stats.sent_bye ? "yes" : "no") << "\n";
    std::cout << "BYE received: " << (stats.received_bye ? "yes" : "no") << "\n";
    std::cout << "Metronome states sent: " << stats.metronome_sent << "\n";
    std::cout << "Metronome states received: " << stats.metronome_received << "\n";
    std::cout << "Metronome epoch sample time: " << stats.metronome_epoch_sample_time << "\n";
    std::cout << "Metronome local beat: " << stats.local_metronome_beat << "\n";
    std::cout << "Metronome remote beat: " << stats.remote_metronome_beat << "\n";
    std::cout << "Metronome alignment valid: " << (stats.metronome_alignment_valid ? "yes" : "no") << "\n";
    std::cout << "Metronome compensation active: " << (stats.metronome_compensation_active ? "yes" : "no") << "\n";
    std::cout << "Metronome compensation offset frames: " << stats.metronome_compensation_offset_frames << "\n";
    std::cout << "Metronome compensation offset ms: "
              << signed_frames_to_ms(stats.metronome_compensation_offset_frames, options.sample_rate) << "\n";
    std::cout << "Metronome compensation target frames: " << stats.metronome_compensation_target_frames << "\n";
    std::cout << "Metronome compensation target ms: "
              << signed_frames_to_ms(stats.metronome_compensation_target_frames, options.sample_rate) << "\n";
    std::cout << "Metronome compensation clamp events: " << stats.metronome_compensation_clamp_events << "\n";
    std::cout << "Metronome compensation stale events: " << stats.metronome_compensation_stale_events << "\n";
    if (stats.metronome_received > 0) {
        std::cout << "Metronome last remote beat: " << stats.last_remote_beat << "\n";
    }
    std::cout << "Expected remote sample time: " << stats.expected_remote_sample_time << "\n";
    std::cout << "Last received sample time: " << stats.last_received_sample_time << "\n";
    std::cout << "Last played remote sample time: " << stats.last_played_remote_sample_time << "\n";
    std::cout << "Remote sample lag frames: " << stats.remote_sample_lag_frames << "\n";
    std::cout << "Remote sample lag ms: " << frames_to_ms(static_cast<std::size_t>(stats.remote_sample_lag_frames), options.sample_rate) << "\n";
    std::cout << "Missing sample ranges: " << stats.missing_sample_ranges << "\n";
    std::cout << "Missing audio frames inserted: " << stats.missing_audio_frames_inserted << "\n";
    std::cout << "Late audio frames dropped: " << stats.late_audio_frames_dropped << "\n";
    std::cout << "Playout delay error frames: " << stats.playout_delay_error_frames << "\n";
    std::cout << "Playout delay error ms: "
              << frames_to_ms(
                     static_cast<std::size_t>(
                         stats.playout_delay_error_frames < 0 ? -stats.playout_delay_error_frames : stats.playout_delay_error_frames),
                     options.sample_rate)
              << "\n";
    std::cout << "Jitter buffer: " << (stats.jitter_buffer_enabled ? "on" : "off") << "\n";
    std::cout << "Jitter buffer target frames: " << stats.jitter_buffer_target_frames << "\n";
    std::cout << "Jitter buffer target ms: "
              << frames_to_ms(static_cast<std::size_t>(stats.jitter_buffer_target_frames), options.sample_rate) << "\n";
    std::cout << "Jitter buffer max frames: " << stats.jitter_buffer_max_frames << "\n";
    std::cout << "Jitter buffer max ms: "
              << frames_to_ms(static_cast<std::size_t>(stats.jitter_buffer_max_frames), options.sample_rate) << "\n";
    std::cout << "Jitter buffer final depth frames: " << stats.jitter_buffer_depth_frames << "\n";
    std::cout << "Jitter buffer max depth frames: " << stats.jitter_buffer_depth_max_frames << "\n";
    std::cout << "Jitter buffer queued packets: " << stats.jitter_buffer_queued_packets << "\n";
    std::cout << "Jitter buffer released packets: " << stats.jitter_buffer_released_packets << "\n";
    std::cout << "Jitter buffer late packets: " << stats.jitter_buffer_late_packets << "\n";
    std::cout << "Jitter buffer dropped packets: " << stats.jitter_buffer_dropped_packets << "\n";
    std::cout << "Jitter buffer dropped frames: " << stats.jitter_buffer_dropped_frames << "\n";
    std::cout << "Jitter buffer forced releases: " << stats.jitter_buffer_forced_releases << "\n";
    if (stats.send_interval_samples > 0) {
        std::cout << "Send interval ms min: " << static_cast<double>(stats.send_interval_min_us) / 1000.0 << "\n";
        std::cout << "Send interval ms avg: " << avg_us_to_ms(stats.send_interval_sum_us, stats.send_interval_samples) << "\n";
        std::cout << "Send interval ms max: " << static_cast<double>(stats.send_interval_max_us) / 1000.0 << "\n";
    }
    if (stats.send_schedule_error_samples > 0) {
        std::cout << "Send schedule error ms min: " << static_cast<double>(stats.send_schedule_error_min_us) / 1000.0 << "\n";
        std::cout << "Send schedule error ms avg: "
                  << avg_us_to_ms(stats.send_schedule_error_sum_us, stats.send_schedule_error_samples) << "\n";
        std::cout << "Send schedule error ms max: " << static_cast<double>(stats.send_schedule_error_max_us) / 1000.0 << "\n";
    }
    std::cout << "Send catchup events: " << stats.send_catchup_events << "\n";
    std::cout << "Send catchup max packets: " << stats.send_catchup_max_packets << "\n";
    if (stats.receive_loop_gap_samples > 0) {
        std::cout << "Receive loop gap ms min: " << static_cast<double>(stats.receive_loop_gap_min_us) / 1000.0 << "\n";
        std::cout << "Receive loop gap ms avg: " << avg_us_to_ms(stats.receive_loop_gap_sum_us, stats.receive_loop_gap_samples) << "\n";
        std::cout << "Receive loop gap ms max: " << static_cast<double>(stats.receive_loop_gap_max_us) / 1000.0 << "\n";
    }
    std::cout << "Receive burst packets max: " << stats.receive_burst_packets_max << "\n";
    std::cout << "Receive packets per loop max: " << stats.receive_packets_per_loop_max << "\n";
    std::cout << "Adaptive playback cushion: " << (stats.adaptive_playback_cushion_enabled ? "on" : "off") << "\n";
    std::cout << "Adaptive playback target frames: " << stats.adaptive_playback_target_frames << "\n";
    std::cout << "Adaptive playback target ms: "
              << frames_to_ms(static_cast<std::size_t>(stats.adaptive_playback_target_frames), options.sample_rate) << "\n";
    std::cout << "Adaptive playback min frames: " << stats.adaptive_playback_min_frames << "\n";
    std::cout << "Adaptive playback max frames: " << stats.adaptive_playback_max_frames << "\n";
    std::cout << "Adaptive playback raise events: " << stats.adaptive_playback_raise_events << "\n";
    std::cout << "Adaptive playback release events: " << stats.adaptive_playback_release_events << "\n";
    std::cout << "Adaptive playback burst events: " << stats.adaptive_playback_burst_events << "\n";
    std::cout << "Adaptive playback padding frames: " << stats.adaptive_playback_padding_frames << "\n";
    std::cout << "Adaptive playback padding ms: "
              << frames_to_ms(static_cast<std::size_t>(stats.adaptive_playback_padding_frames), options.sample_rate) << "\n";
    std::cout << "Adaptive playback time above target ms: "
              << static_cast<double>(stats.adaptive_playback_time_above_target_us) / 1000.0 << "\n";
    std::cout << "Adaptive playback time under target ms: "
              << static_cast<double>(stats.adaptive_playback_time_under_target_us) / 1000.0 << "\n";
    if (stats.recv_pongs > 0) {
        std::cout << "RTT ms min: " << (static_cast<double>(stats.rtt_min_us) / 1000.0) << "\n";
        std::cout << "RTT ms avg: "
                  << (static_cast<double>(stats.rtt_sum_us) / static_cast<double>(stats.recv_pongs) / 1000.0) << "\n";
        std::cout << "RTT ms max: " << (static_cast<double>(stats.rtt_max_us) / 1000.0) << "\n";
    }
    std::cout << "Sequence lost: " << stats.sequence.lost << "\n";
    std::cout << "Sequence loss events: " << stats.sequence.loss_events << "\n";
    std::cout << "Sequence loss max gap: " << stats.sequence.loss_max_gap << "\n";
    std::cout << "Sequence loss percent: " << sequence_loss_percent(stats) << "\n";
    std::cout << "Sequence duplicate: " << stats.sequence.duplicate << "\n";
    std::cout << "Sequence out_of_order: " << stats.sequence.out_of_order << "\n";
    std::cout << "Sequence late: " << stats.sequence.late << "\n";
    std::cout << "Reordered packets recovered: " << stats.reordered_recovered << "\n";
    std::cout << "Reordered packets lost: " << stats.reordered_lost << "\n";
    std::cout << "Reordered packet loss events: " << stats.reordered_lost_events << "\n";
    std::cout << "Reorder pending high water packets: " << stats.reorder_pending_high_water << "\n";
    std::cout << "Reorder capacity drops: " << stats.reorder_capacity_drops << "\n";
    std::cout << "Jitter pending high water packets: " << stats.jitter_pending_high_water << "\n";
    std::cout << "Jitter capacity drops: " << stats.jitter_capacity_drops << "\n";
    if (stats.recv_pongs > 0 && stats.playback_depth_samples > 0) {
        std::cout << "Estimated one-way latency ms: " << estimated_one_way_ms(stats, options) << "\n";
        std::cout << "Estimated one-way latency note: RTT/2 + jitter buffer target + playback depth avg + audio buffer\n";
    }
    if (stats.drift_valid) {
        std::cout << "Raw drift ppm estimate: " << stats.raw_drift_ppm << "\n";
        std::cout << "Smoothed drift ppm estimate: " << stats.drift_ppm << "\n";
        std::cout << "Resampler ratio: " << stats.resampler_ratio << "\n";
    }
}

} // namespace jam2::cli::stats

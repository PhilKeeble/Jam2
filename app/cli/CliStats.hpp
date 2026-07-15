#pragma once

#include "CliOptions.hpp"
#include "engine.hpp"
#include "peer_stream.hpp"
#include "protocol.hpp"
#include "udp_socket.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace jam2::cli::stats {

using Options = Jam2RuntimeOptions;
using OsPriorityMode = Jam2OsPriorityMode;

struct OsSchedulingStatus {
    OsPriorityMode requested = OsPriorityMode::High;
    std::string platform;
    unsigned int cpu_count = 0;
    std::string process_priority;
    std::string thread_priority;
    std::string mmcss_requested;
    std::string mmcss_active;
    std::string mmcss_profile;
    std::string mmcss_error;
    std::string timer_resolution_requested;
    std::string timer_resolution_active;

    std::string timer_resolution_error;
    std::string qos_requested;
    std::string qos_active;
    std::string qos_error;
    std::string realtime_requested;
    std::string realtime_active;
    std::string realtime_error;
};

struct UdpParseStats {
    std::uint64_t short_packet = 0;
    std::uint64_t wrong_magic = 0;
    std::uint64_t wrong_version = 0;
    std::uint64_t unknown_type = 0;
    std::uint64_t wrong_session = 0;
    std::uint64_t invalid_payload_size = 0;
    std::uint64_t authentication_failed = 0;

    void observe(jam2::protocol::ParseError error)
    {
        switch (error) {
        case jam2::protocol::ParseError::None: break;
        case jam2::protocol::ParseError::ShortPacket: ++short_packet; break;
        case jam2::protocol::ParseError::WrongMagic: ++wrong_magic; break;
        case jam2::protocol::ParseError::WrongVersion: ++wrong_version; break;
        case jam2::protocol::ParseError::UnknownType: ++unknown_type; break;
        case jam2::protocol::ParseError::WrongSession: ++wrong_session; break;
        case jam2::protocol::ParseError::InvalidPayloadSize: ++invalid_payload_size; break;
        case jam2::protocol::ParseError::AuthenticationFailed: ++authentication_failed; break;
        }
    }

    void add(const UdpParseStats& other)
    {
        short_packet += other.short_packet;
        wrong_magic += other.wrong_magic;
        wrong_version += other.wrong_version;
        unknown_type += other.unknown_type;
        wrong_session += other.wrong_session;
        invalid_payload_size += other.invalid_payload_size;
        authentication_failed += other.authentication_failed;
    }

    std::uint64_t total() const
    {
        return short_packet + wrong_magic + wrong_version + unknown_type + wrong_session +
            invalid_payload_size + authentication_failed;
    }
};

struct AudioPacketStats {
    std::uint64_t local_peer_id = 0;
    std::uint64_t remote_peer_id = 0;
    std::string bootstrap_role;
    int session_protocol_version = 0;
    std::string session_audio_format;
    int session_sample_rate = 0;
    int session_frames_per_packet = 0;
    std::uint64_t network_peer_count = 0;
    std::uint64_t network_active_peer_count = 0;
    std::uint64_t mix_contributing_peers = 0;
    std::uint64_t mix_active_slots = 0;
    std::uint64_t mix_max_slots = 0;
    std::uint64_t mix_active_slots_high_water = 0;
    std::uint64_t mix_released_slots = 0;
    std::uint64_t mix_complete_slots = 0;
    std::uint64_t mix_deadline_slots = 0;
    std::uint64_t mix_missing_peer_contributions = 0;
    std::uint64_t mix_missing_peer_frames = 0;
    std::uint64_t mix_late_after_release_frames = 0;
    std::uint64_t mix_capacity_drops = 0;
    std::uint64_t mix_capacity_dropped_frames = 0;
    std::uint64_t mix_clipped_samples = 0;
    std::uint64_t mix_output_frames = 0;
    std::uint64_t mix_output_drop_requested_frames = 0;
    std::uint64_t mix_output_drop_request_events = 0;
    std::uint64_t mix_output_dropped_frames = 0;
    std::uint64_t mix_work_budget_yields = 0;
    std::uint64_t bootstrap_coordinator_peer_id = 0;
    std::uint64_t arrangement_authority_peer_id = 0;
    std::uint64_t grid_authority_peer_id = 0;
    std::uint64_t grid_revision = 0;
    std::uint64_t grid_run_state = 0;
    std::uint64_t grid_mode = 0;
    std::uint64_t grid_authority_epoch_frame = 0;
    std::uint64_t grid_mapped_epoch_frame = 0;
    std::uint64_t grid_authority_packet_frame = 0;
    std::int64_t grid_mapping_error_frames = 0;
    std::uint64_t grid_proposals_sent = 0;
    std::uint64_t grid_proposals_accepted = 0;
    std::uint64_t grid_proposals_rejected = 0;
    std::uint64_t grid_assignments_sent = 0;
    std::uint64_t grid_assignments_accepted = 0;
    std::uint64_t grid_assignments_rejected = 0;
    std::uint64_t grid_authority_states_sent = 0;
    std::uint64_t grid_authority_states_accepted = 0;
    std::uint64_t grid_authority_states_rejected = 0;
    std::uint64_t grid_authority_missing_events = 0;
    std::uint64_t transport_source_peer_id = 0;
    std::uint64_t transport_event_counter = 0;
    std::uint64_t transport_grid_revision = 0;
    std::uint64_t transport_action = 0;
    std::uint64_t transport_events_accepted = 0;
    std::uint64_t transport_events_rejected = 0;
    std::uint64_t leader_audio_source_peer_id = 0;
    std::uint64_t leader_audio_injected_packets = 0;
    std::uint64_t transport_source_frame = 0;
    std::uint64_t transport_requested_target_frame = 0;
    std::uint64_t transport_applied_target_frame = 0;
    std::uint64_t sent_packets = 0;
    std::uint64_t sent_bytes = 0;
    std::uint64_t recv_packets = 0;
    std::uint64_t recv_bytes = 0;
    std::uint64_t ignored_packets = 0;
    UdpParseStats udp_parse;
    std::uint64_t udp_replay_rejects = 0;
    std::uint64_t udp_forward_gap_rejects = 0;
    std::uint64_t udp_forward_gap_resyncs = 0;
    std::uint64_t udp_sequence_ambiguous_rejects = 0;
    std::uint64_t udp_sample_time_stale_rejects = 0;
    std::uint64_t udp_sample_time_future_rejects = 0;
    std::uint64_t udp_unmatched_pongs = 0;
    std::uint64_t udp_ping_slot_overwrites = 0;
    std::uint64_t udp_work_budget_yields = 0;
    std::uint64_t udp_receive_batch_max = 0;
    std::uint64_t reorder_pending_high_water = 0;
    std::uint64_t reorder_capacity_drops = 0;
    std::uint64_t jitter_pending_high_water = 0;
    std::uint64_t jitter_capacity_drops = 0;
    std::uint64_t sent_pings = 0;
    std::uint64_t recv_pongs = 0;
    std::uint64_t sent_pongs = 0;
    bool received_bye = false;
    bool sent_bye = false;
    std::uint64_t startup_drained_packets = 0;
    std::uint64_t playback_dropped_frames = 0;
    std::uint64_t playback_drop_events = 0;
    std::uint64_t playback_drop_event_max_frames = 0;
    std::uint64_t playback_depth_min_frames = 0;
    std::uint64_t playback_depth_sum_frames = 0;
    std::uint64_t playback_depth_max_frames = 0;
    std::uint64_t playback_depth_samples = 0;
    std::uint64_t stats_warmup_skipped_packets = 0;
    jam2::protocol::SequenceStats sequence;
    std::uint64_t audio_delay_min_us = 0;
    std::uint64_t audio_delay_sum_us = 0;
    std::uint64_t audio_delay_max_us = 0;
    std::uint64_t audio_delay_samples = 0;
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
    std::uint64_t send_interval_min_us = 0;
    std::uint64_t send_interval_sum_us = 0;
    std::uint64_t send_interval_max_us = 0;
    std::uint64_t send_interval_samples = 0;
    std::uint64_t send_schedule_error_min_us = 0;
    std::uint64_t send_schedule_error_sum_us = 0;
    std::uint64_t send_schedule_error_max_us = 0;
    std::uint64_t send_schedule_error_samples = 0;
    std::uint64_t send_catchup_events = 0;
    std::uint64_t send_catchup_max_packets = 0;


    std::uint64_t receive_loop_gap_min_us = 0;
    std::uint64_t receive_loop_gap_sum_us = 0;
    std::uint64_t receive_loop_gap_max_us = 0;
    std::uint64_t receive_loop_gap_samples = 0;
    std::uint64_t receive_burst_packets_max = 0;
    std::uint64_t receive_packets_per_loop_max = 0;
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
    std::uint64_t recv_loop_iterations = 0;
    std::uint64_t recv_loop_idle_count = 0;
    std::uint64_t recv_loop_batch_sum = 0;
    std::uint64_t recv_loop_batch_max = 0;
    std::uint64_t rtt_min_us = 0;
    std::uint64_t rtt_sum_us = 0;
    std::uint64_t rtt_max_us = 0;
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
    std::uint64_t metronome_sent = 0;
    std::uint64_t metronome_received = 0;
    std::uint64_t last_remote_beat = 0;
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
    std::uint64_t metronome_epoch_sample_time = 0;
    std::uint64_t local_metronome_beat = 0;
    std::uint64_t remote_metronome_beat = 0;
    bool metronome_alignment_valid = false;
    bool metronome_compensation_active = false;
    std::int64_t metronome_compensation_offset_frames = 0;
    std::int64_t metronome_compensation_target_frames = 0;
    std::uint64_t metronome_compensation_clamp_events = 0;
    std::uint64_t metronome_compensation_stale_events = 0;
    std::uint64_t elapsed_ms = 0;
    bool final_metronome_enabled = false;
    int final_bpm = 120;
    double final_metronome_level = 1.0;
    double final_remote_level = 1.0;
    double final_send_level = 1.0;
    bool final_local_monitor_enabled = false;
    double final_local_monitor_level = 0.0;
    OsSchedulingStatus os_scheduling;
};

void print_udp_parse_stats(const UdpParseStats& stats, std::ostream& out = std::cout);
void copy_peer_stream_stats(AudioPacketStats& target, const jam2::PeerStreamStats& source);
void add_peer_stream_stats(AudioPacketStats& target, const jam2::PeerStreamStats& source);
void copy_peer_mixer_stats(AudioPacketStats& target, const jam2::PeerMixerStats& source);

double unit_from_ppm(int value);
double frames_to_ms(std::size_t frames, double sample_rate);
double signed_frames_to_ms(std::int64_t frames, double sample_rate);
std::size_t audio_payload_bytes(int frame_size, jam2::NetworkAudioFormat format);
std::size_t audio_packet_bytes(int frame_size, jam2::NetworkAudioFormat format);
std::string platform_name();
std::string csv_escape(std::string_view value);
void append_os_scheduling_csv(std::ostream& out, const OsSchedulingStatus& status);
std::string command_line_text(int argc, char** argv);
double playback_depth_avg_frames(const AudioPacketStats& stats);
double playback_depth_avg_ms(const AudioPacketStats& stats, const Options& options);
double frames_percent(std::uint64_t frames, std::uint64_t total_frames);
double packet_path_frame_percent(std::uint64_t frames, std::uint64_t audio_frame_seconds);
double avg_us_to_ms(std::uint64_t sum_us, std::uint64_t samples);
double avg_u64(std::uint64_t sum, std::uint64_t samples);
double avg_double(double sum, std::uint64_t samples);
double rtt_avg_ms(const AudioPacketStats& stats);
double sequence_loss_percent(const AudioPacketStats& stats);
double estimated_one_way_ms(const AudioPacketStats& stats, const Options& options);
std::tm local_time_from(std::time_t value);
unsigned long current_process_id();
std::filesystem::path make_stats_csv_path(const std::filesystem::path& folder);

class CsvStatsLog {
public:
    CsvStatsLog() = default;

    struct Context {
        std::string command_line;
        std::string mode;
        std::string platform;
        std::string local_endpoint;
        std::string peer_endpoint;
        std::string endpoint_mode;
        int socket_send_buffer_bytes = 0;
        int socket_recv_buffer_bytes = 0;
        int requested_socket_send_buffer_bytes = 0;
        int requested_socket_recv_buffer_bytes = 0;
        int requested_sample_rate = 0;
        int frame_size = 0;
        long requested_audio_buffer_frames = 0;
        std::size_t capture_ring_frames = 0;
        std::size_t playback_ring_frames = 0;
        std::size_t playback_prefill_frames = 0;
        std::size_t playback_max_frames = 0;
        int stats_warmup_ms = 0;
        bool drift_correction = false;
        double drift_smoothing = 0.0;
        int drift_deadband_ppm = 0;
        int drift_max_correction_ppm = 0;
        bool metronome = false;
        int bpm = 0;
        double metronome_level = 0.0;
        std::string metronome_mode;
        bool sample_time_playout = false;
        std::size_t playout_delay_frames = 0;
        std::size_t jitter_buffer_frames = 0;
        std::size_t jitter_buffer_max_frames = 0;
        double remote_level = 1.0;
        int audio_device_id = -1;
        std::string requested_input_mode;
        std::string requested_channels;
    };

    struct AudioSnapshot {
        bool has_audio = false;
        jam2::audio::StreamInfo stream;
        long callbacks = 0;
        bool playback_prefilled = false;
        std::uint64_t capture_ring_overruns = 0;
        std::uint64_t capture_ring_overrun_events = 0;
        std::uint64_t capture_ring_overrun_event_max_frames = 0;
        std::uint64_t capture_ring_underruns = 0;
        std::uint64_t capture_ring_underrun_events = 0;
        std::size_t capture_ring_readable = 0;
        std::uint64_t playback_ring_overruns = 0;
        std::uint64_t playback_ring_underruns = 0;
        std::uint64_t playback_ring_underrun_events = 0;
        std::uint64_t playback_ring_underrun_event_max_frames = 0;
        std::uint64_t playback_ring_underrun_burst_events = 0;
        std::uint64_t playback_ring_underrun_burst_max_frames = 0;
        std::uint64_t playback_ring_underrun_burst_current_frames = 0;
        std::uint64_t playback_depth_under_2ms_frames = 0;
        std::uint64_t playback_depth_under_5ms_frames = 0;
        std::uint64_t playback_depth_under_10ms_frames = 0;
        std::uint64_t playback_depth_10ms_plus_frames = 0;
        std::uint64_t playback_depth_observed_frames = 0;
        std::uint64_t playback_drop_requested_frames = 0;
        std::uint64_t playback_drop_applied_frames = 0;
        std::uint64_t playback_drop_coalesced_requests = 0;
        std::uint64_t playback_drop_pending_frames = 0;
        std::uint64_t playback_drop_max_batch_frames = 0;
        std::size_t playback_ring_readable = 0;
        jam2::audio::CallbackTimingStats callback_timing;
        double input_peak = 0.0;
        double send_peak = 0.0;
        double monitor_peak = 0.0;
        double remote_peak = 0.0;
        double metronome_peak = 0.0;
        double output_peak = 0.0;
        std::uint64_t output_clipped_samples = 0;
        std::uint64_t prepared_source_frame = 0;
        std::uint64_t prepared_source_scheduled_start_frame = 0;

        std::uint64_t prepared_source_actual_start_frame = 0;
        std::uint64_t prepared_source_underruns = 0;
        std::uint64_t prepared_source_busy_events = 0;
        bool network_capture_enabled = false;
        bool network_capture_ready = false;

        std::uint64_t network_capture_generation = 0;
        std::uint64_t network_capture_epoch_frame = 0;
        std::uint64_t network_capture_stale_frames_discarded = 0;
        bool network_playback_enabled = false;
    };

    CsvStatsLog(const std::filesystem::path& folder, Context context)
        : context_(std::move(context))
    {
        std::filesystem::create_directories(folder);
        path_ = make_stats_csv_path(folder);
        out_.open(path_);
        if (!out_) {

            throw std::runtime_error("failed to open stats CSV: " + path_.string());
        }
        out_ << std::setprecision(12);
        out_ << "row_type,elapsed_ms,command_line,mode,platform,local_endpoint,peer_endpoint,endpoint_mode,"
                "socket_send_buffer_bytes,socket_recv_buffer_bytes,requested_socket_send_buffer_bytes,requested_socket_recv_buffer_bytes,"
                "audio_device_id,device_backend,device_name,device_driver_id,device_path,backend_sample_format,"
                "requested_sample_rate,active_sample_rate,frame_size,requested_audio_buffer_frames,active_audio_buffer_frames,"
                "capture_ring_frames,playback_ring_frames,playback_prefill_frames,playback_max_frames,stats_warmup_ms,"
                "requested_input_mode,active_input_mode,requested_channels,active_channels,"
                "drift_correction,drift_smoothing,drift_deadband_ppm,drift_max_correction_ppm,metronome,bpm,metronome_level,"
                "sent_packets,recv_packets,sent_bytes,recv_bytes,send_bitrate_bps,recv_bitrate_bps,"
                "sequence_lost,sequence_loss_events,sequence_loss_max_gap,sequence_loss_percent,sequence_duplicate,sequence_out_of_order,sequence_late,"
                "reordered_recovered,reordered_lost,reordered_lost_events,startup_drained_packets,recv_packets_plus_startup_drained,"
                "stats_warmup_skipped_packets,ignored_packets,"
                "playback_dropped_frames,playback_depth_min_frames,playback_depth_avg_frames,playback_depth_max_frames,"
                "playback_depth_min_ms,playback_depth_avg_ms,playback_depth_max_ms,"
                "jitter_min_ms,jitter_avg_ms,jitter_max_ms,rtt_min_ms,rtt_avg_ms,rtt_max_ms,"
                "estimated_one_way_ms,raw_drift_ppm,drift_ppm,resampler_ratio,"
                "audio_callbacks,playback_prefilled,capture_ring_overruns,capture_ring_underruns,capture_ring_underrun_events,capture_ring_readable_frames,"
                "capture_ring_readable_ms,playback_ring_overruns,playback_ring_underruns,playback_ring_underrun_events,playback_ring_readable_frames,"
                "playback_ring_readable_ms,"
                "pings_sent,pongs_sent,pongs_received,bye_sent,bye_received,metronome_states_sent,metronome_states_received,"
                "final_metronome,final_bpm,"
                "playback_drop_events,playback_drop_event_max_frames,playback_drop_event_max_ms,playback_dropped_time_ms,playback_dropped_frame_percent,"
                "playback_ring_underrun_event_max_frames,playback_ring_underrun_event_max_ms,"
                "playback_ring_underrun_burst_events,playback_ring_underrun_burst_max_frames,playback_ring_underrun_burst_max_ms,"
                "playback_ring_underrun_time_ms,playback_ring_underrun_time_percent,"
                "playback_depth_under_2ms_frames,playback_depth_under_2ms_percent,"
                "playback_depth_under_5ms_frames,playback_depth_under_5ms_percent,"
                "playback_depth_under_10ms_frames,playback_depth_under_10ms_percent,"
                "playback_depth_10ms_plus_frames,playback_depth_10ms_plus_percent,playback_depth_observed_frames,"
                "capture_ring_overrun_events,capture_ring_overrun_event_max_frames,capture_ring_overrun_event_max_ms,"
                "audio_packet_gap_min_ms,audio_packet_gap_avg_ms,audio_packet_gap_max_ms,audio_packet_gap_samples,"
                "audio_packet_gap_over_2x_count,audio_packet_gap_over_4x_count,"
                "playback_push_min_frames,playback_push_avg_frames,playback_push_max_frames,playback_push_batches,"
                "playback_depth_under_2ms_events,playback_depth_under_2ms_max_duration_ms,"
                "playback_depth_under_5ms_events,playback_depth_under_5ms_max_duration_ms,"
                "playback_depth_under_10ms_events,playback_depth_under_10ms_max_duration_ms,"
                "recv_loop_iterations,recv_loop_idle_count,recv_loop_batch_avg,recv_loop_batch_max,"
                "reordered_max_distance_packets,"
                "resampler_ratio_min,resampler_ratio_avg,resampler_ratio_max,resampler_ratio_samples,"
                "drift_correction_active_samples,drift_correction_active_percent,"
                "drift_correction_clamped_samples,drift_correction_clamped_percent,"
                "resampler_ratio_change_max_ppm_per_second,remote_level,final_metronome_level,final_remote_level,"
                "metronome_mode,sample_time_playout,playout_delay_frames,playout_delay_ms,"
                "expected_remote_sample_time,last_received_sample_time,last_played_remote_sample_time,remote_sample_lag_frames,remote_sample_lag_ms,"
                "missing_sample_ranges,missing_audio_frames_inserted,late_audio_frames_dropped,playout_delay_error_frames,playout_delay_error_ms,"
                "metronome_epoch_sample_time,local_metronome_beat,remote_metronome_beat,metronome_alignment_valid,"
                "send_interval_min_ms,send_interval_avg_ms,send_interval_max_ms,send_interval_samples,"
                "send_schedule_error_min_ms,send_schedule_error_avg_ms,send_schedule_error_max_ms,send_schedule_error_samples,"
                "send_catchup_events,send_catchup_max_packets,"
                "receive_loop_gap_min_ms,receive_loop_gap_avg_ms,receive_loop_gap_max_ms,receive_loop_gap_samples,"
                "receive_burst_packets_max,receive_packets_per_loop_max,"
                "audio_callback_interval_min_ms,audio_callback_interval_avg_ms,audio_callback_interval_max_ms,audio_callback_interval_samples,"
                "audio_callback_gap_over_1_1x_count,audio_callback_gap_over_1_5x_count,audio_callback_gap_over_2x_count,"
                "adaptive_playback_cushion,adaptive_playback_target_frames,adaptive_playback_target_ms,"
                "adaptive_playback_min_frames,adaptive_playback_max_frames,adaptive_playback_raise_events,adaptive_playback_release_events,"
                "adaptive_playback_burst_events,adaptive_playback_padding_frames,adaptive_playback_padding_ms,"
                "adaptive_playback_time_above_target_ms,adaptive_playback_time_under_target_ms,"
                "adaptive_playback_longest_above_target_ms,adaptive_playback_longest_under_target_ms,"
                "jitter_buffer,jitter_buffer_target_frames,jitter_buffer_target_ms,jitter_buffer_max_frames,jitter_buffer_max_ms,"
                "jitter_buffer_depth_frames,jitter_buffer_depth_ms,jitter_buffer_depth_max_frames,jitter_buffer_depth_max_ms,"
                "jitter_buffer_queued_packets,jitter_buffer_released_packets,jitter_buffer_late_packets,"
                "jitter_buffer_dropped_packets,jitter_buffer_dropped_frames,jitter_buffer_dropped_ms,"
                "os_priority_requested,os_platform,os_cpu_count,os_process_priority_active,os_thread_priority_active,"
                "os_mmcss_requested,os_mmcss_active,os_mmcss_profile,os_mmcss_error,"
                "os_timer_resolution_requested,os_timer_resolution_active,os_timer_resolution_error,"
                "os_qos_requested,os_qos_active,os_qos_error,"
                "os_realtime_requested,os_realtime_active,os_realtime_error,"
                "metronome_compensation_active,metronome_compensation_offset_frames,metronome_compensation_offset_ms,"
                "metronome_compensation_target_frames,metronome_compensation_target_ms,"
                "metronome_compensation_clamp_events,metronome_compensation_stale_events,"
                "prepared_source_frame,prepared_source_scheduled_start_frame,prepared_source_actual_start_frame,"
                "prepared_source_underruns,prepared_source_busy_events,"
                "playback_drop_requested_frames,playback_drop_applied_frames,playback_drop_coalesced_requests,"
                "playback_drop_pending_frames,playback_drop_max_batch_frames,"
                "udp_short_packets,udp_wrong_magic,udp_wrong_version,udp_unknown_type,"
                "udp_wrong_session,udp_invalid_payload_size,udp_authentication_failed,"
                "udp_replay_rejects,udp_forward_gap_rejects,udp_forward_gap_resyncs,udp_sequence_ambiguous_rejects,"
                "udp_sample_time_stale_rejects,udp_sample_time_future_rejects,udp_unmatched_pongs,"
                "udp_ping_slot_overwrites,udp_work_budget_yields,reorder_pending_high_water,reorder_capacity_drops,"
                "jitter_pending_high_water,jitter_capacity_drops,jitter_buffer_forced_releases,"
                "network_capture_enabled,network_capture_ready,network_capture_generation,"
                "network_capture_epoch_frame,network_capture_stale_frames_discarded,network_playback_enabled,"
                "local_peer_id,remote_peer_id,bootstrap_role,session_protocol_version,session_audio_format,"
                "session_sample_rate,session_frames_per_packet,"
                "network_peer_count,network_active_peer_count,mix_contributing_peers,mix_active_slots,mix_max_slots,"
                "mix_active_slots_high_water,mix_released_slots,mix_complete_slots,mix_deadline_slots,"
                "mix_missing_peer_contributions,mix_missing_peer_frames,mix_late_after_release_frames,"
                "mix_capacity_drops,mix_capacity_dropped_frames,mix_clipped_samples,mix_output_frames,"
                "mix_output_drop_requested_frames,mix_output_drop_request_events,"
                "mix_output_dropped_frames,mix_work_budget_yields,"
                "bootstrap_coordinator_peer_id,arrangement_authority_peer_id,grid_authority_peer_id,grid_revision,"
                "grid_run_state,grid_mode,grid_authority_epoch_frame,grid_mapped_epoch_frame,"
                "grid_authority_packet_frame,grid_mapping_error_frames,grid_proposals_sent,"
                "grid_proposals_accepted,grid_proposals_rejected,grid_assignments_sent,"
                "grid_assignments_accepted,grid_assignments_rejected,grid_authority_states_sent,"
                "grid_authority_states_accepted,grid_authority_states_rejected,grid_authority_missing_events,"
                "transport_source_peer_id,transport_event_counter,transport_grid_revision,"
                "transport_events_accepted,transport_events_rejected,leader_audio_source_peer_id,"
                "leader_audio_injected_packets,transport_source_frame,transport_requested_target_frame,"
                "transport_applied_target_frame,transport_action,udp_receive_batch_max,"
                "network_audio_bytes_per_sample,udp_header_bytes,audio_payload_bytes,audio_packet_bytes,"
                "send_packet_rate_pps,recv_packet_rate_pps\n";
    }

    explicit operator bool() const { return out_.is_open(); }
    const std::filesystem::path& path() const { return path_; }

    void write(
        std::string_view row_type,
        std::uint64_t elapsed_ms,
        const AudioPacketStats& stats,
        const Options& options,
        const AudioSnapshot& audio)
    {
        if (!out_) {
            return;
        }
        const double seconds = elapsed_ms > 0 ? static_cast<double>(elapsed_ms) / 1000.0 : 0.0;
        const double send_bitrate = seconds > 0.0 ? (static_cast<double>(stats.sent_bytes) * 8.0 / seconds) : 0.0;
        const double recv_bitrate = seconds > 0.0 ? (static_cast<double>(stats.recv_bytes) * 8.0 / seconds) : 0.0;
        const double send_packet_rate = seconds > 0.0
            ? static_cast<double>(stats.sent_packets) / seconds : 0.0;
        const double recv_packet_rate = seconds > 0.0
            ? static_cast<double>(stats.recv_packets) / seconds : 0.0;
        const double jitter_min_ms = stats.jitter_samples > 0 ? static_cast<double>(stats.jitter_min_us) / 1000.0 : 0.0;
        const double jitter_avg_ms = stats.jitter_samples > 0 ?
            static_cast<double>(stats.jitter_sum_us) / static_cast<double>(stats.jitter_samples) / 1000.0 :
            0.0;
        const double jitter_max_ms = stats.jitter_samples > 0 ? static_cast<double>(stats.jitter_max_us) / 1000.0 : 0.0;
        const double rtt_min = stats.recv_pongs > 0 ? static_cast<double>(stats.rtt_min_us) / 1000.0 : 0.0;
        const double rtt_max = stats.recv_pongs > 0 ? static_cast<double>(stats.rtt_max_us) / 1000.0 : 0.0;
        const double active_sample_rate = audio.stream.sample_rate > 0.0 ?
            audio.stream.sample_rate :
            static_cast<double>(options.sample_rate);
        const std::uint64_t elapsed_audio_frames = static_cast<std::uint64_t>(
            active_sample_rate * seconds);
        const double playback_dropped_percent =
            packet_path_frame_percent(stats.playback_dropped_frames, elapsed_audio_frames);
        const double playback_underrun_percent =
            frames_percent(audio.playback_ring_underruns, audio.playback_depth_observed_frames);
        const double drift_active_percent =
            frames_percent(stats.drift_correction_active_samples, stats.resampler_ratio_samples);
        const double drift_clamped_percent =
            frames_percent(stats.drift_correction_clamped_samples, stats.resampler_ratio_samples);
        out_ << csv_escape(row_type) << ','
             << elapsed_ms << ','
             << csv_escape(context_.command_line) << ','
             << csv_escape(context_.mode) << ','
             << csv_escape(context_.platform) << ','
             << csv_escape(context_.local_endpoint) << ','
             << csv_escape(context_.peer_endpoint) << ','
             << csv_escape(context_.endpoint_mode) << ','
             << context_.socket_send_buffer_bytes << ','
             << context_.socket_recv_buffer_bytes << ','
             << context_.requested_socket_send_buffer_bytes << ','
             << context_.requested_socket_recv_buffer_bytes << ','
             << context_.audio_device_id << ','
             << csv_escape(audio.stream.device.backend) << ','
             << csv_escape(audio.stream.device.name) << ','
             << csv_escape(audio.stream.device.clsid) << ','
             << csv_escape(audio.stream.device.driver_path) << ','
             << csv_escape(audio.stream.sample_format) << ','
             << context_.requested_sample_rate << ','
             << audio.stream.sample_rate << ','
             << context_.frame_size << ','
             << context_.requested_audio_buffer_frames << ','
             << audio.stream.buffer_size << ','
             << context_.capture_ring_frames << ','
             << context_.playback_ring_frames << ','
             << context_.playback_prefill_frames << ','
             << context_.playback_max_frames << ','
             << context_.stats_warmup_ms << ','
             << csv_escape(context_.requested_input_mode) << ','
             << csv_escape(mono_mix_mode_text(audio.stream.channels.input.size())) << ','
             << csv_escape(context_.requested_channels) << ','
             << csv_escape(channel_selection_text(audio.stream.channels)) << ','
             << (context_.drift_correction ? "on" : "off") << ','
             << context_.drift_smoothing << ','
             << context_.drift_deadband_ppm << ','
             << context_.drift_max_correction_ppm << ','
             << (context_.metronome ? "on" : "off") << ','
             << context_.bpm << ','
             << context_.metronome_level << ','
             << stats.sent_packets << ','
             << stats.recv_packets << ','
             << stats.sent_bytes << ','
             << stats.recv_bytes << ','
             << send_bitrate << ','
             << recv_bitrate << ','
             << stats.sequence.lost << ','
             << stats.sequence.loss_events << ','
             << stats.sequence.loss_max_gap << ','
             << sequence_loss_percent(stats) << ','
             << stats.sequence.duplicate << ','
             << stats.sequence.out_of_order << ','
             << stats.sequence.late << ','
             << stats.reordered_recovered << ','
             << stats.reordered_lost << ','
             << stats.reordered_lost_events << ','
             << stats.startup_drained_packets << ','
             << (stats.recv_packets + stats.startup_drained_packets) << ','
             << stats.stats_warmup_skipped_packets << ','
             << stats.ignored_packets << ','
             << stats.playback_dropped_frames << ','
             << stats.playback_depth_min_frames << ','
             << playback_depth_avg_frames(stats) << ','
             << stats.playback_depth_max_frames << ','
             << frames_to_ms(static_cast<std::size_t>(stats.playback_depth_min_frames), active_sample_rate) << ','
             << playback_depth_avg_ms(stats, options) << ','
             << frames_to_ms(static_cast<std::size_t>(stats.playback_depth_max_frames), active_sample_rate) << ','
             << jitter_min_ms << ','
             << jitter_avg_ms << ','
             << jitter_max_ms << ','
             << rtt_min << ','
             << rtt_avg_ms(stats) << ','
             << rtt_max << ','
             << estimated_one_way_ms(stats, options) << ','
             << stats.raw_drift_ppm << ','
             << stats.drift_ppm << ','
             << stats.resampler_ratio << ','
             << audio.callbacks << ','
             << (audio.playback_prefilled ? "yes" : "no") << ','
             << audio.capture_ring_overruns << ','
             << audio.capture_ring_underruns << ','
             << audio.capture_ring_underrun_events << ','
             << audio.capture_ring_readable << ','
             << frames_to_ms(audio.capture_ring_readable, active_sample_rate) << ','
             << audio.playback_ring_overruns << ','
             << audio.playback_ring_underruns << ','
             << audio.playback_ring_underrun_events << ','
             << audio.playback_ring_readable << ','
             << frames_to_ms(audio.playback_ring_readable, active_sample_rate) << ','
             << stats.sent_pings << ','
             << stats.sent_pongs << ','
             << stats.recv_pongs << ','
             << (stats.sent_bye ? "yes" : "no") << ','
             << (stats.received_bye ? "yes" : "no") << ','
             << stats.metronome_sent << ','
             << stats.metronome_received << ','
             << (stats.final_metronome_enabled ? "on" : "off") << ','
             << stats.final_bpm << ','
             << stats.playback_drop_events << ','
             << stats.playback_drop_event_max_frames << ','
             << frames_to_ms(static_cast<std::size_t>(stats.playback_drop_event_max_frames), active_sample_rate) << ','
             << frames_to_ms(static_cast<std::size_t>(stats.playback_dropped_frames), active_sample_rate) << ','
             << playback_dropped_percent << ','
             << audio.playback_ring_underrun_event_max_frames << ','
             << frames_to_ms(static_cast<std::size_t>(audio.playback_ring_underrun_event_max_frames), active_sample_rate) << ','
             << audio.playback_ring_underrun_burst_events << ','
             << audio.playback_ring_underrun_burst_max_frames << ','
             << frames_to_ms(static_cast<std::size_t>(audio.playback_ring_underrun_burst_max_frames), active_sample_rate) << ','
             << frames_to_ms(static_cast<std::size_t>(audio.playback_ring_underruns), active_sample_rate) << ','
             << playback_underrun_percent << ','
             << audio.playback_depth_under_2ms_frames << ','
             << frames_percent(audio.playback_depth_under_2ms_frames, audio.playback_depth_observed_frames) << ','
             << audio.playback_depth_under_5ms_frames << ','
             << frames_percent(audio.playback_depth_under_5ms_frames, audio.playback_depth_observed_frames) << ','
             << audio.playback_depth_under_10ms_frames << ','
             << frames_percent(audio.playback_depth_under_10ms_frames, audio.playback_depth_observed_frames) << ','
             << audio.playback_depth_10ms_plus_frames << ','
             << frames_percent(audio.playback_depth_10ms_plus_frames, audio.playback_depth_observed_frames) << ','
             << audio.playback_depth_observed_frames << ','
             << audio.capture_ring_overrun_events << ','
             << audio.capture_ring_overrun_event_max_frames << ','
             << frames_to_ms(static_cast<std::size_t>(audio.capture_ring_overrun_event_max_frames), active_sample_rate) << ','
             << (stats.audio_packet_gap_samples > 0 ? static_cast<double>(stats.audio_packet_gap_min_us) / 1000.0 : 0.0) << ','
             << avg_us_to_ms(stats.audio_packet_gap_sum_us, stats.audio_packet_gap_samples) << ','
             << (stats.audio_packet_gap_samples > 0 ? static_cast<double>(stats.audio_packet_gap_max_us) / 1000.0 : 0.0) << ','
             << stats.audio_packet_gap_samples << ','
             << stats.audio_packet_gap_over_2x_count << ','
             << stats.audio_packet_gap_over_4x_count << ','
             << stats.playback_push_min_frames << ','
             << avg_u64(stats.playback_push_sum_frames, stats.playback_push_batches) << ','
             << stats.playback_push_max_frames << ','
             << stats.playback_push_batches << ','
             << stats.playback_depth_under_2ms_events << ','
             << static_cast<double>(stats.playback_depth_under_2ms_max_duration_us) / 1000.0 << ','
             << stats.playback_depth_under_5ms_events << ','
             << static_cast<double>(stats.playback_depth_under_5ms_max_duration_us) / 1000.0 << ','
             << stats.playback_depth_under_10ms_events << ','
             << static_cast<double>(stats.playback_depth_under_10ms_max_duration_us) / 1000.0 << ','
             << stats.recv_loop_iterations << ','
             << stats.recv_loop_idle_count << ','
             << avg_u64(stats.recv_loop_batch_sum, stats.recv_loop_iterations) << ','
             << stats.recv_loop_batch_max << ','

             << stats.reordered_max_distance_packets << ','
             << stats.resampler_ratio_min << ','
             << avg_double(stats.resampler_ratio_sum, stats.resampler_ratio_samples) << ','
             << stats.resampler_ratio_max << ','
             << stats.resampler_ratio_samples << ','
             << stats.drift_correction_active_samples << ','

             << drift_active_percent << ','
             << stats.drift_correction_clamped_samples << ','
             << drift_clamped_percent << ','
             << stats.resampler_ratio_change_max_ppm_per_second << ','
             << context_.remote_level << ','
             << stats.final_metronome_level << ','
             << stats.final_remote_level << ','
             << csv_escape(context_.metronome_mode) << ','
             << (context_.sample_time_playout ? "on" : "off") << ','
             << context_.playout_delay_frames << ','
             << frames_to_ms(context_.playout_delay_frames, active_sample_rate) << ','
             << stats.expected_remote_sample_time << ','
             << stats.last_received_sample_time << ','
             << stats.last_played_remote_sample_time << ','

             << stats.remote_sample_lag_frames << ','
             << frames_to_ms(static_cast<std::size_t>(stats.remote_sample_lag_frames), active_sample_rate) << ','
             << stats.missing_sample_ranges << ','
             << stats.missing_audio_frames_inserted << ','
             << stats.late_audio_frames_dropped << ','
             << stats.playout_delay_error_frames << ','
             << frames_to_ms(
                    static_cast<std::size_t>(
                        stats.playout_delay_error_frames < 0 ? -stats.playout_delay_error_frames : stats.playout_delay_error_frames),
                    active_sample_rate) << ','
             << stats.metronome_epoch_sample_time << ','
             << stats.local_metronome_beat << ','
             << stats.remote_metronome_beat << ','
             << (stats.metronome_alignment_valid ? "yes" : "no") << ','
             << (stats.send_interval_samples > 0 ? static_cast<double>(stats.send_interval_min_us) / 1000.0 : 0.0) << ','
             << avg_us_to_ms(stats.send_interval_sum_us, stats.send_interval_samples) << ','
             << (stats.send_interval_samples > 0 ? static_cast<double>(stats.send_interval_max_us) / 1000.0 : 0.0) << ','
             << stats.send_interval_samples << ','
             << (stats.send_schedule_error_samples > 0 ? static_cast<double>(stats.send_schedule_error_min_us) / 1000.0 : 0.0) << ','
             << avg_us_to_ms(stats.send_schedule_error_sum_us, stats.send_schedule_error_samples) << ','
             << (stats.send_schedule_error_samples > 0 ? static_cast<double>(stats.send_schedule_error_max_us) / 1000.0 : 0.0) << ','
             << stats.send_schedule_error_samples << ','
             << stats.send_catchup_events << ','
             << stats.send_catchup_max_packets << ','
             << (stats.receive_loop_gap_samples > 0 ? static_cast<double>(stats.receive_loop_gap_min_us) / 1000.0 : 0.0) << ','
             << avg_us_to_ms(stats.receive_loop_gap_sum_us, stats.receive_loop_gap_samples) << ','
             << (stats.receive_loop_gap_samples > 0 ? static_cast<double>(stats.receive_loop_gap_max_us) / 1000.0 : 0.0) << ','
             << stats.receive_loop_gap_samples << ','
             << stats.receive_burst_packets_max << ','
             << stats.receive_packets_per_loop_max << ','
             << (audio.callback_timing.interval_samples > 0 ? static_cast<double>(audio.callback_timing.interval_min_us) / 1000.0 : 0.0) << ','
             << avg_us_to_ms(audio.callback_timing.interval_sum_us, audio.callback_timing.interval_samples) << ','
             << (audio.callback_timing.interval_samples > 0 ? static_cast<double>(audio.callback_timing.interval_max_us) / 1000.0 : 0.0) << ','
             << audio.callback_timing.interval_samples << ','
             << audio.callback_timing.gap_over_1_1x_count << ','
             << audio.callback_timing.gap_over_1_5x_count << ','
             << audio.callback_timing.gap_over_2x_count << ','
             << (stats.adaptive_playback_cushion_enabled ? "on" : "off") << ','
             << stats.adaptive_playback_target_frames << ','
             << frames_to_ms(static_cast<std::size_t>(stats.adaptive_playback_target_frames), active_sample_rate) << ','
             << stats.adaptive_playback_min_frames << ','
             << stats.adaptive_playback_max_frames << ','
             << stats.adaptive_playback_raise_events << ','
             << stats.adaptive_playback_release_events << ','
             << stats.adaptive_playback_burst_events << ','
             << stats.adaptive_playback_padding_frames << ','
             << frames_to_ms(static_cast<std::size_t>(stats.adaptive_playback_padding_frames), active_sample_rate) << ','
             << static_cast<double>(stats.adaptive_playback_time_above_target_us) / 1000.0 << ','
             << static_cast<double>(stats.adaptive_playback_time_under_target_us) / 1000.0 << ','
             << static_cast<double>(stats.adaptive_playback_longest_above_target_us) / 1000.0 << ','
             << static_cast<double>(stats.adaptive_playback_longest_under_target_us) / 1000.0 << ','
             << (stats.jitter_buffer_enabled ? "on" : "off") << ','
             << stats.jitter_buffer_target_frames << ','
             << frames_to_ms(static_cast<std::size_t>(stats.jitter_buffer_target_frames), active_sample_rate) << ','
             << stats.jitter_buffer_max_frames << ','
             << frames_to_ms(static_cast<std::size_t>(stats.jitter_buffer_max_frames), active_sample_rate) << ','
             << stats.jitter_buffer_depth_frames << ','
             << frames_to_ms(static_cast<std::size_t>(stats.jitter_buffer_depth_frames), active_sample_rate) << ','
             << stats.jitter_buffer_depth_max_frames << ','
             << frames_to_ms(static_cast<std::size_t>(stats.jitter_buffer_depth_max_frames), active_sample_rate) << ','
             << stats.jitter_buffer_queued_packets << ','
             << stats.jitter_buffer_released_packets << ','
             << stats.jitter_buffer_late_packets << ','
             << stats.jitter_buffer_dropped_packets << ','
             << stats.jitter_buffer_dropped_frames << ','
             << frames_to_ms(static_cast<std::size_t>(stats.jitter_buffer_dropped_frames), active_sample_rate);
        append_os_scheduling_csv(out_, stats.os_scheduling);
        out_ << ','
             << (stats.metronome_compensation_active ? "yes" : "no") << ','
             << stats.metronome_compensation_offset_frames << ','
             << signed_frames_to_ms(stats.metronome_compensation_offset_frames, active_sample_rate) << ','
             << stats.metronome_compensation_target_frames << ','
             << signed_frames_to_ms(stats.metronome_compensation_target_frames, active_sample_rate) << ','
             << stats.metronome_compensation_clamp_events << ','
             << stats.metronome_compensation_stale_events << ','
             << audio.prepared_source_frame << ','
             << audio.prepared_source_scheduled_start_frame << ','
             << audio.prepared_source_actual_start_frame << ','
             << audio.prepared_source_underruns << ','
             << audio.prepared_source_busy_events << ','
             << audio.playback_drop_requested_frames << ','
             << audio.playback_drop_applied_frames << ','
             << audio.playback_drop_coalesced_requests << ','
             << audio.playback_drop_pending_frames << ','
             << audio.playback_drop_max_batch_frames << ','
             << stats.udp_parse.short_packet << ','
             << stats.udp_parse.wrong_magic << ','
             << stats.udp_parse.wrong_version << ','
             << stats.udp_parse.unknown_type << ','
             << stats.udp_parse.wrong_session << ','
             << stats.udp_parse.invalid_payload_size << ','
             << stats.udp_parse.authentication_failed << ','
             << stats.udp_replay_rejects << ','
             << stats.udp_forward_gap_rejects << ','
             << stats.udp_forward_gap_resyncs << ','
             << stats.udp_sequence_ambiguous_rejects << ','
             << stats.udp_sample_time_stale_rejects << ','
             << stats.udp_sample_time_future_rejects << ','
             << stats.udp_unmatched_pongs << ','
             << stats.udp_ping_slot_overwrites << ','
             << stats.udp_work_budget_yields << ','
             << stats.reorder_pending_high_water << ','
             << stats.reorder_capacity_drops << ','
             << stats.jitter_pending_high_water << ','
             << stats.jitter_capacity_drops << ','
             << stats.jitter_buffer_forced_releases << ','
             << (audio.network_capture_enabled ? "yes" : "no") << ','
             << (audio.network_capture_ready ? "yes" : "no") << ','
             << audio.network_capture_generation << ','
             << audio.network_capture_epoch_frame << ','
             << audio.network_capture_stale_frames_discarded << ','
             << (audio.network_playback_enabled ? "yes" : "no") << ','
             << stats.local_peer_id << ','
             << stats.remote_peer_id << ','
             << csv_escape(stats.bootstrap_role) << ','
             << stats.session_protocol_version << ','
             << csv_escape(stats.session_audio_format) << ','
             << stats.session_sample_rate << ','
             << stats.session_frames_per_packet << ','
             << stats.network_peer_count << ','
             << stats.network_active_peer_count << ','
             << stats.mix_contributing_peers << ','
             << stats.mix_active_slots << ','
             << stats.mix_max_slots << ','
             << stats.mix_active_slots_high_water << ','
             << stats.mix_released_slots << ','
             << stats.mix_complete_slots << ','
             << stats.mix_deadline_slots << ','
             << stats.mix_missing_peer_contributions << ','
             << stats.mix_missing_peer_frames << ','
             << stats.mix_late_after_release_frames << ','
             << stats.mix_capacity_drops << ','
             << stats.mix_capacity_dropped_frames << ','
             << stats.mix_clipped_samples << ','
             << stats.mix_output_frames << ','
             << stats.mix_output_drop_requested_frames << ','
             << stats.mix_output_drop_request_events << ','
             << stats.mix_output_dropped_frames << ','
             << stats.mix_work_budget_yields << ','
             << stats.bootstrap_coordinator_peer_id << ','
             << stats.arrangement_authority_peer_id << ','
             << stats.grid_authority_peer_id << ','
             << stats.grid_revision << ','
             << stats.grid_run_state << ','
             << stats.grid_mode << ','
             << stats.grid_authority_epoch_frame << ','
             << stats.grid_mapped_epoch_frame << ','
             << stats.grid_authority_packet_frame << ','
             << stats.grid_mapping_error_frames << ','
             << stats.grid_proposals_sent << ','
             << stats.grid_proposals_accepted << ','
             << stats.grid_proposals_rejected << ','
             << stats.grid_assignments_sent << ','
             << stats.grid_assignments_accepted << ','
             << stats.grid_assignments_rejected << ','
             << stats.grid_authority_states_sent << ','
             << stats.grid_authority_states_accepted << ','
             << stats.grid_authority_states_rejected << ','
             << stats.grid_authority_missing_events << ','
             << stats.transport_source_peer_id << ','
             << stats.transport_event_counter << ','
             << stats.transport_grid_revision << ','
             << stats.transport_events_accepted << ','
             << stats.transport_events_rejected << ','
             << stats.leader_audio_source_peer_id << ','
             << stats.leader_audio_injected_packets << ','
             << stats.transport_source_frame << ','
             << stats.transport_requested_target_frame << ','
             << stats.transport_applied_target_frame << ','
             << stats.transport_action << ','
             << stats.udp_receive_batch_max << ','
             << jam2::protocol::audio_bytes_per_sample(options.network_audio_format) << ','
             << jam2::protocol::kHeaderSize << ','
             << audio_payload_bytes(options.frame_size, options.network_audio_format) << ','
             << audio_packet_bytes(options.frame_size, options.network_audio_format) << ','
             << send_packet_rate << ','
             << recv_packet_rate;
        out_ << '\n';
        if (row_type == "final") {
            out_.flush();
        }
    }

    void write_periodic(
        std::uint64_t elapsed_ms,
        const AudioPacketStats& stats,
        const Options& options,
        const AudioSnapshot& audio)
    {
        if (!out_) {
            return;
        }
        std::vector<std::string> fields(351);
        auto set = [&](std::size_t index, auto value) {
            std::ostringstream text;
            text << value;
            fields[index] = text.str();
        };
        fields[0] = "periodic";
        set(1, elapsed_ms);
        set(39, stats.sent_packets);
        set(40, stats.recv_packets);
        set(45, stats.sequence.lost);
        set(46, stats.sequence.loss_events);
        set(47, stats.sequence.loss_max_gap);
        set(48, sequence_loss_percent(stats));
        set(49, stats.sequence.duplicate);
        set(50, stats.sequence.out_of_order);
        set(51, stats.sequence.late);
        set(52, stats.reordered_recovered);
        set(53, stats.reordered_lost);
        set(54, stats.reordered_lost_events);
        set(58, stats.ignored_packets);
        set(59, stats.playback_dropped_frames);
        set(60, stats.playback_depth_min_frames);
        set(61, playback_depth_avg_frames(stats));
        set(62, stats.playback_depth_max_frames);
        set(63, frames_to_ms(static_cast<std::size_t>(stats.playback_depth_min_frames), options.sample_rate));
        set(64, playback_depth_avg_ms(stats, options));
        set(65, frames_to_ms(static_cast<std::size_t>(stats.playback_depth_max_frames), options.sample_rate));
        set(66, stats.jitter_samples > 0 ? static_cast<double>(stats.jitter_min_us) / 1000.0 : 0.0);
        set(67, stats.jitter_samples > 0 ?
            static_cast<double>(stats.jitter_sum_us) / static_cast<double>(stats.jitter_samples) / 1000.0 :
            0.0);
        set(68, stats.jitter_samples > 0 ? static_cast<double>(stats.jitter_max_us) / 1000.0 : 0.0);
        set(69, stats.recv_pongs > 0 ? static_cast<double>(stats.rtt_min_us) / 1000.0 : 0.0);
        set(70, rtt_avg_ms(stats));
        set(71, stats.recv_pongs > 0 ? static_cast<double>(stats.rtt_max_us) / 1000.0 : 0.0);
        set(72, estimated_one_way_ms(stats, options));
        set(73, stats.raw_drift_ppm);
        set(74, stats.drift_ppm);
        set(75, stats.resampler_ratio);
        set(76, audio.callbacks);
        set(79, audio.capture_ring_underruns);
        set(80, audio.capture_ring_underrun_events);
        set(81, audio.capture_ring_readable);
        set(82, frames_to_ms(audio.capture_ring_readable, options.sample_rate));
        set(84, audio.playback_ring_underruns);
        set(85, audio.playback_ring_underrun_events);
        set(86, audio.playback_ring_readable);
        set(87, frames_to_ms(audio.playback_ring_readable, options.sample_rate));
        const double seconds = elapsed_ms > 0 ? static_cast<double>(elapsed_ms) / 1000.0 : 0.0;
        const std::uint64_t elapsed_audio_frames = static_cast<std::uint64_t>(
            static_cast<double>(options.sample_rate) * seconds);
        set(97, stats.playback_drop_events);
        set(98, stats.playback_drop_event_max_frames);
        set(99, frames_to_ms(static_cast<std::size_t>(stats.playback_drop_event_max_frames), options.sample_rate));
        set(100, frames_to_ms(static_cast<std::size_t>(stats.playback_dropped_frames), options.sample_rate));
        set(101, packet_path_frame_percent(stats.playback_dropped_frames, elapsed_audio_frames));
        set(102, audio.playback_ring_underrun_event_max_frames);
        set(103, frames_to_ms(static_cast<std::size_t>(audio.playback_ring_underrun_event_max_frames), options.sample_rate));
        set(104, audio.playback_ring_underrun_burst_events);
        set(105, audio.playback_ring_underrun_burst_max_frames);
        set(106, frames_to_ms(static_cast<std::size_t>(audio.playback_ring_underrun_burst_max_frames), options.sample_rate));
        set(107, frames_to_ms(static_cast<std::size_t>(audio.playback_ring_underruns), options.sample_rate));
        set(108, frames_percent(audio.playback_ring_underruns, audio.playback_depth_observed_frames));
        set(109, audio.playback_depth_under_2ms_frames);
        set(110, frames_percent(audio.playback_depth_under_2ms_frames, audio.playback_depth_observed_frames));
        set(111, audio.playback_depth_under_5ms_frames);
        set(112, frames_percent(audio.playback_depth_under_5ms_frames, audio.playback_depth_observed_frames));
        set(113, audio.playback_depth_under_10ms_frames);
        set(114, frames_percent(audio.playback_depth_under_10ms_frames, audio.playback_depth_observed_frames));
        set(115, audio.playback_depth_10ms_plus_frames);
        set(116, frames_percent(audio.playback_depth_10ms_plus_frames, audio.playback_depth_observed_frames));
        set(117, audio.playback_depth_observed_frames);
        set(118, audio.capture_ring_overrun_events);
        set(119, audio.capture_ring_overrun_event_max_frames);
        set(120, frames_to_ms(static_cast<std::size_t>(audio.capture_ring_overrun_event_max_frames), options.sample_rate));
        set(121, stats.audio_packet_gap_samples > 0 ? static_cast<double>(stats.audio_packet_gap_min_us) / 1000.0 : 0.0);
        set(122, avg_us_to_ms(stats.audio_packet_gap_sum_us, stats.audio_packet_gap_samples));
        set(123, stats.audio_packet_gap_samples > 0 ? static_cast<double>(stats.audio_packet_gap_max_us) / 1000.0 : 0.0);
        set(124, stats.audio_packet_gap_samples);
        set(125, stats.audio_packet_gap_over_2x_count);
        set(126, stats.audio_packet_gap_over_4x_count);
        set(127, stats.playback_push_min_frames);
        set(128, avg_u64(stats.playback_push_sum_frames, stats.playback_push_batches));
        set(129, stats.playback_push_max_frames);
        set(130, stats.playback_push_batches);
        set(131, stats.playback_depth_under_2ms_events);
        set(132, static_cast<double>(stats.playback_depth_under_2ms_max_duration_us) / 1000.0);
        set(133, stats.playback_depth_under_5ms_events);
        set(134, static_cast<double>(stats.playback_depth_under_5ms_max_duration_us) / 1000.0);
        set(135, stats.playback_depth_under_10ms_events);

        set(136, static_cast<double>(stats.playback_depth_under_10ms_max_duration_us) / 1000.0);
        set(137, stats.recv_loop_iterations);
        set(138, stats.recv_loop_idle_count);
        set(139, avg_u64(stats.recv_loop_batch_sum, stats.recv_loop_iterations));
        set(140, stats.recv_loop_batch_max);
        set(141, stats.reordered_max_distance_packets);
        set(142, stats.resampler_ratio_min);

        set(143, avg_double(stats.resampler_ratio_sum, stats.resampler_ratio_samples));
        set(144, stats.resampler_ratio_max);
        set(145, stats.resampler_ratio_samples);
        set(146, stats.drift_correction_active_samples);
        set(147, frames_percent(stats.drift_correction_active_samples, stats.resampler_ratio_samples));
        set(148, stats.drift_correction_clamped_samples);
        set(149, frames_percent(stats.drift_correction_clamped_samples, stats.resampler_ratio_samples));
        set(150, stats.resampler_ratio_change_max_ppm_per_second);
        set(151, context_.remote_level);
        set(152, stats.final_metronome_level);
        set(153, stats.final_remote_level);
        fields[154] = context_.metronome_mode;
        fields[155] = context_.sample_time_playout ? "on" : "off";
        set(156, context_.playout_delay_frames);
        set(157, frames_to_ms(context_.playout_delay_frames, options.sample_rate));

        set(158, stats.expected_remote_sample_time);
        set(159, stats.last_received_sample_time);
        set(160, stats.last_played_remote_sample_time);
        set(161, stats.remote_sample_lag_frames);
        set(162, frames_to_ms(static_cast<std::size_t>(stats.remote_sample_lag_frames), options.sample_rate));
        set(163, stats.missing_sample_ranges);
        set(164, stats.missing_audio_frames_inserted);
        set(165, stats.late_audio_frames_dropped);
        set(166, stats.playout_delay_error_frames);
        set(167, frames_to_ms(
            static_cast<std::size_t>(
                stats.playout_delay_error_frames < 0 ? -stats.playout_delay_error_frames : stats.playout_delay_error_frames),
            options.sample_rate));
        set(168, stats.metronome_epoch_sample_time);
        set(169, stats.local_metronome_beat);
        set(170, stats.remote_metronome_beat);
        fields[171] = stats.metronome_alignment_valid ? "yes" : "no";
        set(172, stats.send_interval_samples > 0 ? static_cast<double>(stats.send_interval_min_us) / 1000.0 : 0.0);
        set(173, avg_us_to_ms(stats.send_interval_sum_us, stats.send_interval_samples));
        set(174, stats.send_interval_samples > 0 ? static_cast<double>(stats.send_interval_max_us) / 1000.0 : 0.0);
        set(175, stats.send_interval_samples);
        set(176, stats.send_schedule_error_samples > 0 ? static_cast<double>(stats.send_schedule_error_min_us) / 1000.0 : 0.0);
        set(177, avg_us_to_ms(stats.send_schedule_error_sum_us, stats.send_schedule_error_samples));
        set(178, stats.send_schedule_error_samples > 0 ? static_cast<double>(stats.send_schedule_error_max_us) / 1000.0 : 0.0);
        set(179, stats.send_schedule_error_samples);
        set(180, stats.send_catchup_events);
        set(181, stats.send_catchup_max_packets);
        set(182, stats.receive_loop_gap_samples > 0 ? static_cast<double>(stats.receive_loop_gap_min_us) / 1000.0 : 0.0);
        set(183, avg_us_to_ms(stats.receive_loop_gap_sum_us, stats.receive_loop_gap_samples));
        set(184, stats.receive_loop_gap_samples > 0 ? static_cast<double>(stats.receive_loop_gap_max_us) / 1000.0 : 0.0);
        set(185, stats.receive_loop_gap_samples);
        set(186, stats.receive_burst_packets_max);
        set(187, stats.receive_packets_per_loop_max);
        set(188, audio.callback_timing.interval_samples > 0 ? static_cast<double>(audio.callback_timing.interval_min_us) / 1000.0 : 0.0);
        set(189, avg_us_to_ms(audio.callback_timing.interval_sum_us, audio.callback_timing.interval_samples));
        set(190, audio.callback_timing.interval_samples > 0 ? static_cast<double>(audio.callback_timing.interval_max_us) / 1000.0 : 0.0);
        set(191, audio.callback_timing.interval_samples);
        set(192, audio.callback_timing.gap_over_1_1x_count);
        set(193, audio.callback_timing.gap_over_1_5x_count);
        set(194, audio.callback_timing.gap_over_2x_count);
        fields[195] = stats.adaptive_playback_cushion_enabled ? "on" : "off";
        set(196, stats.adaptive_playback_target_frames);
        set(197, frames_to_ms(static_cast<std::size_t>(stats.adaptive_playback_target_frames), options.sample_rate));
        set(198, stats.adaptive_playback_min_frames);
        set(199, stats.adaptive_playback_max_frames);
        set(200, stats.adaptive_playback_raise_events);
        set(201, stats.adaptive_playback_release_events);
        set(202, stats.adaptive_playback_burst_events);
        set(203, stats.adaptive_playback_padding_frames);
        set(204, frames_to_ms(static_cast<std::size_t>(stats.adaptive_playback_padding_frames), options.sample_rate));
        set(205, static_cast<double>(stats.adaptive_playback_time_above_target_us) / 1000.0);
        set(206, static_cast<double>(stats.adaptive_playback_time_under_target_us) / 1000.0);
        set(207, static_cast<double>(stats.adaptive_playback_longest_above_target_us) / 1000.0);
        set(208, static_cast<double>(stats.adaptive_playback_longest_under_target_us) / 1000.0);
        fields[209] = stats.jitter_buffer_enabled ? "on" : "off";
        set(210, stats.jitter_buffer_target_frames);
        set(211, frames_to_ms(static_cast<std::size_t>(stats.jitter_buffer_target_frames), options.sample_rate));
        set(212, stats.jitter_buffer_max_frames);
        set(213, frames_to_ms(static_cast<std::size_t>(stats.jitter_buffer_max_frames), options.sample_rate));
        set(214, stats.jitter_buffer_depth_frames);
        set(215, frames_to_ms(static_cast<std::size_t>(stats.jitter_buffer_depth_frames), options.sample_rate));
        set(216, stats.jitter_buffer_depth_max_frames);
        set(217, frames_to_ms(static_cast<std::size_t>(stats.jitter_buffer_depth_max_frames), options.sample_rate));
        set(218, stats.jitter_buffer_queued_packets);
        set(219, stats.jitter_buffer_released_packets);
        set(220, stats.jitter_buffer_late_packets);
        set(221, stats.jitter_buffer_dropped_packets);
        set(222, stats.jitter_buffer_dropped_frames);
        set(223, frames_to_ms(static_cast<std::size_t>(stats.jitter_buffer_dropped_frames), options.sample_rate));
        fields[224] = std::string(os_priority_text(stats.os_scheduling.requested));
        fields[225] = stats.os_scheduling.platform;
        set(226, stats.os_scheduling.cpu_count);
        fields[227] = stats.os_scheduling.process_priority;
        fields[228] = stats.os_scheduling.thread_priority;
        fields[229] = stats.os_scheduling.mmcss_requested;
        fields[230] = stats.os_scheduling.mmcss_active;
        fields[231] = stats.os_scheduling.mmcss_profile;
        fields[232] = stats.os_scheduling.mmcss_error;
        fields[233] = stats.os_scheduling.timer_resolution_requested;
        fields[234] = stats.os_scheduling.timer_resolution_active;
        fields[235] = stats.os_scheduling.timer_resolution_error;
        fields[236] = stats.os_scheduling.qos_requested;
        fields[237] = stats.os_scheduling.qos_active;
        fields[238] = stats.os_scheduling.qos_error;
        fields[239] = stats.os_scheduling.realtime_requested;
        fields[240] = stats.os_scheduling.realtime_active;
        fields[241] = stats.os_scheduling.realtime_error;
        fields[242] = stats.metronome_compensation_active ? "yes" : "no";
        set(243, stats.metronome_compensation_offset_frames);
        set(244, signed_frames_to_ms(stats.metronome_compensation_offset_frames, options.sample_rate));
        set(245, stats.metronome_compensation_target_frames);
        set(246, signed_frames_to_ms(stats.metronome_compensation_target_frames, options.sample_rate));
        set(247, stats.metronome_compensation_clamp_events);
        set(248, stats.metronome_compensation_stale_events);
        set(249, audio.prepared_source_frame);
        set(250, audio.prepared_source_scheduled_start_frame);
        set(251, audio.prepared_source_actual_start_frame);
        set(252, audio.prepared_source_underruns);
        set(253, audio.prepared_source_busy_events);
        set(254, audio.playback_drop_requested_frames);
        set(255, audio.playback_drop_applied_frames);
        set(256, audio.playback_drop_coalesced_requests);
        set(257, audio.playback_drop_pending_frames);
        set(258, audio.playback_drop_max_batch_frames);
        set(259, stats.udp_parse.short_packet);
        set(260, stats.udp_parse.wrong_magic);
        set(261, stats.udp_parse.wrong_version);
        set(262, stats.udp_parse.unknown_type);
        set(263, stats.udp_parse.wrong_session);
        set(264, stats.udp_parse.invalid_payload_size);
        set(265, stats.udp_parse.authentication_failed);
        set(266, stats.udp_replay_rejects);
        set(267, stats.udp_forward_gap_rejects);
        set(268, stats.udp_forward_gap_resyncs);
        set(269, stats.udp_sequence_ambiguous_rejects);
        set(270, stats.udp_sample_time_stale_rejects);
        set(271, stats.udp_sample_time_future_rejects);
        set(272, stats.udp_unmatched_pongs);
        set(273, stats.udp_ping_slot_overwrites);
        set(274, stats.udp_work_budget_yields);
        set(275, stats.reorder_pending_high_water);
        set(276, stats.reorder_capacity_drops);
        set(277, stats.jitter_pending_high_water);
        set(278, stats.jitter_capacity_drops);
        set(279, stats.jitter_buffer_forced_releases);
        fields[280] = audio.network_capture_enabled ? "yes" : "no";
        fields[281] = audio.network_capture_ready ? "yes" : "no";
        set(282, audio.network_capture_generation);
        set(283, audio.network_capture_epoch_frame);
        set(284, audio.network_capture_stale_frames_discarded);
        fields[285] = audio.network_playback_enabled ? "yes" : "no";
        set(286, stats.local_peer_id);
        set(287, stats.remote_peer_id);
        fields[288] = stats.bootstrap_role;
        set(289, stats.session_protocol_version);
        fields[290] = stats.session_audio_format;
        set(291, stats.session_sample_rate);
        set(292, stats.session_frames_per_packet);
        set(293, stats.network_peer_count);
        set(294, stats.network_active_peer_count);
        set(295, stats.mix_contributing_peers);
        set(296, stats.mix_active_slots);
        set(297, stats.mix_max_slots);
        set(298, stats.mix_active_slots_high_water);
        set(299, stats.mix_released_slots);
        set(300, stats.mix_complete_slots);
        set(301, stats.mix_deadline_slots);
        set(302, stats.mix_missing_peer_contributions);
        set(303, stats.mix_missing_peer_frames);
        set(304, stats.mix_late_after_release_frames);
        set(305, stats.mix_capacity_drops);
        set(306, stats.mix_capacity_dropped_frames);
        set(307, stats.mix_clipped_samples);
        set(308, stats.mix_output_frames);
        set(309, stats.mix_output_drop_requested_frames);
        set(310, stats.mix_output_drop_request_events);
        set(311, stats.mix_output_dropped_frames);
        set(312, stats.mix_work_budget_yields);
        set(313, stats.bootstrap_coordinator_peer_id);
        set(314, stats.arrangement_authority_peer_id);
        set(315, stats.grid_authority_peer_id);
        set(316, stats.grid_revision);
        set(317, stats.grid_run_state);
        set(318, stats.grid_mode);
        set(319, stats.grid_authority_epoch_frame);
        set(320, stats.grid_mapped_epoch_frame);
        set(321, stats.grid_authority_packet_frame);
        set(322, stats.grid_mapping_error_frames);
        set(323, stats.grid_proposals_sent);
        set(324, stats.grid_proposals_accepted);
        set(325, stats.grid_proposals_rejected);
        set(326, stats.grid_assignments_sent);
        set(327, stats.grid_assignments_accepted);
        set(328, stats.grid_assignments_rejected);
        set(329, stats.grid_authority_states_sent);
        set(330, stats.grid_authority_states_accepted);
        set(331, stats.grid_authority_states_rejected);
        set(332, stats.grid_authority_missing_events);
        set(333, stats.transport_source_peer_id);
        set(334, stats.transport_event_counter);
        set(335, stats.transport_grid_revision);
        set(336, stats.transport_events_accepted);
        set(337, stats.transport_events_rejected);
        set(338, stats.leader_audio_source_peer_id);
        set(339, stats.leader_audio_injected_packets);
        set(340, stats.transport_source_frame);
        set(341, stats.transport_requested_target_frame);
        set(342, stats.transport_applied_target_frame);
        set(343, stats.transport_action);
        set(344, stats.udp_receive_batch_max);
        set(345, jam2::protocol::audio_bytes_per_sample(options.network_audio_format));
        set(346, jam2::protocol::kHeaderSize);
        set(347, audio_payload_bytes(options.frame_size, options.network_audio_format));
        set(348, audio_packet_bytes(options.frame_size, options.network_audio_format));
        set(349, seconds > 0.0 ? static_cast<double>(stats.sent_packets) / seconds : 0.0);
        set(350, seconds > 0.0 ? static_cast<double>(stats.recv_packets) / seconds : 0.0);

        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (i != 0) {
                out_ << ',';
            }
            out_ << csv_escape(fields[i]);
        }
        out_ << '\n';
    }

private:
    Context context_;
    std::filesystem::path path_;
    std::ofstream out_;
};

CsvStatsLog::AudioSnapshot make_audio_snapshot(const jam2::Engine* engine);
CsvStatsLog::Context make_csv_context(
    std::string command_line,
    std::string_view mode,
    const Options& options,
    const jam2::UdpSocket& socket,
    const jam2::Endpoint& local,
    const jam2::Endpoint& peer,
    std::string_view endpoint_mode);
void print_periodic_stream_stats(
    const AudioPacketStats& stats,
    const Options& options,
    const CsvStatsLog::AudioSnapshot& audio,
    std::uint64_t elapsed_ms);
void print_audio_packet_stats(const AudioPacketStats& stats, const Options& options);

} // namespace jam2::cli::stats

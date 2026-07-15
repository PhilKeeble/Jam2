#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "audio_device.hpp"
#include "audio_ring.hpp"
#include "metronome.hpp"

namespace jam2 {

enum class EngineLifecycle : std::uint8_t {
    Stopped,
    Starting,
    Local,
    Stopping,
    Failed,
};

enum class EngineAudioBackend : std::uint8_t {
    Device,
    Headless,
};

enum class EngineMetronomeMode : std::uint8_t {
    SharedGrid,
    LeaderAudio,
    ListenerCompensated,
};

enum class EngineTestInput : std::uint8_t {
    Off,
    Silence,
    Tone440,
    Pulse1s,
    MetronomePulse,
};

enum class EngineTransportAction : std::uint8_t {
    None,
    TrackRestart,
    RecordStart,
    RecordStop,
    TrackStop,
    TrackPlay,
};

inline constexpr bool is_track_sync_transport_action(
    EngineTransportAction action) noexcept
{
    return action == EngineTransportAction::TrackRestart ||
        action == EngineTransportAction::TrackStop ||
        action == EngineTransportAction::TrackPlay ||
        action == EngineTransportAction::RecordStart;
}

// Validated once by Engine::start and then retained unchanged for the lifetime
// of the running local engine. Session/network configuration intentionally does
// not belong here.
struct EngineConfig {
    EngineAudioBackend backend = EngineAudioBackend::Device;
    int audio_device_id = -1;
    int sample_rate = 48000;
    long audio_buffer_frames = 0;
    int headless_clock_drift_ppm = 0;
    audio::InputChannels input_channels = audio::InputChannels::Mono;
    audio::ChannelSelection channels;
    std::size_t capture_ring_frames = 4096;
    std::size_t playback_ring_frames = 4096;
    std::size_t playback_prefill_frames = 0;
    bool diagnostics_enabled = false;
    bool metronome_enabled = false;
    metronome::PatternSnapshot metronome_pattern{};
    int metronome_level_ppm = 1000000;
    int remote_level_ppm = 1000000;
    int send_level_ppm = 1000000;
    bool local_monitor_enabled = false;
    int local_monitor_level_ppm = 250000;
    EngineMetronomeMode metronome_mode = EngineMetronomeMode::SharedGrid;
    bool leader_audio_local_click = false;
    EngineTestInput test_input = EngineTestInput::Off;
    int test_input_level_ppm = 125000;
    std::size_t prepared_track_max_frames = 0;
};

enum class EngineCommandType : std::uint8_t {
    Stop,
    SetMetronomeEnabled,
    SetMetronomePattern,
    SetMetronomeLevel,
    SetRemoteLevel,
    SetSendLevel,
    SetLocalMonitorEnabled,
    SetLocalMonitorLevel,
    SetPlaybackRatio,
    SetMetronomeMode,
    SetLeaderAudioLocalClick,
    SetMetronomeEpoch,
    SetMetronomeRenderOffset,
    SetRecordingLatencyAdjustment,
    ScheduleTransport,
    CancelTransport,
    LoadPreparedTrack,
    PreparedPlay,
    PreparedStop,
    PreparedSeek,
    PreparedSetLoop,
    PreparedSetLevel,
    StartJamRecording,
    StopJamRecording,
    ArmTrackTake,
    StartTrackTake,
    StopTrackTake,
    CancelTrackTake,
};

inline constexpr std::size_t kEngineCommandTextBytes = 1024;
inline constexpr std::size_t kEngineCommandIdBytes = 128;

// Fixed-shape local command. apply_frame == 0 means the next supervisor turn;
// otherwise application waits for the authoritative callback frame clock.
struct EngineCommand {
    EngineCommandType type = EngineCommandType::Stop;
    std::uint64_t cookie = 0;
    std::uint64_t apply_frame = 0;
    std::uint64_t frame = 0;
    std::uint64_t frame_end = 0;
    std::int64_t signed_value = 0;
    std::int32_t value = 0;
    bool enabled = false;
    EngineTransportAction transport_action = EngineTransportAction::None;
    std::uint64_t transport_target_frame = 0;
    std::uint64_t transport_musical_frame = 0;
    std::uint64_t transport_countdown_start_frame = 0;
    metronome::PatternSnapshot pattern{};
    std::array<char, kEngineCommandTextBytes> text{};
    std::array<char, kEngineCommandIdBytes> id{};
};

enum class EngineEventType : std::uint8_t {
    Lifecycle,
    CommandApplied,
    CommandRejected,
    PreparedTrackLoaded,
    JamRecordingStarted,
    JamRecordingStopped,
    TrackTakeCompleted,
    TransportCommitted,
    Error,
};

inline constexpr std::size_t kEngineEventTextBytes = 512;

struct EngineEvent {
    EngineEventType type = EngineEventType::Lifecycle;
    EngineLifecycle lifecycle = EngineLifecycle::Stopped;
    std::uint64_t cookie = 0;
    std::uint64_t requested_frame = 0;
    std::uint64_t applied_frame = 0;
    std::uint64_t value = 0;
    bool ok = true;
    std::array<char, kEngineEventTextBytes> text{};
};

struct EngineRecordingSnapshot {
    bool active = false;
    std::uint64_t start_frame = 0;
    std::uint64_t stop_frame = 0;
    std::uint64_t frames_queued = 0;
    std::uint64_t frames_written = 0;
    std::uint64_t dropped_frames = 0;
    std::uint64_t drop_events = 0;
    std::uint64_t writer_errors = 0;
    std::size_t queue_depth_frames = 0;
    std::size_t queue_capacity_frames = 0;
};

struct EngineTrackTakeSnapshot {
    bool armed = false;
    bool recording = false;
    bool finalized = false;
    std::uint64_t start_frame = 0;
    std::uint64_t stop_frame = 0;
    std::uint64_t frames_queued = 0;
    std::uint64_t frames_written = 0;
    std::uint64_t dropped_frames = 0;
    std::uint64_t drop_events = 0;
    std::uint64_t writer_errors = 0;
    std::size_t queue_depth_frames = 0;
    std::size_t queue_capacity_frames = 0;
};

struct EngineSnapshot {
    EngineLifecycle lifecycle = EngineLifecycle::Stopped;
    EngineAudioBackend backend = EngineAudioBackend::Device;
    bool frame_clock_active = false;
    std::uint64_t engine_frame = 0;
    double sample_rate = 0.0;
    long audio_buffer_frames = 0;
    long callbacks = 0;
    long input_latency_frames = 0;
    long output_latency_frames = 0;
    std::int64_t recording_latency_adjustment_frames = 0;
    std::uint64_t recording_latency_compensation_frames = 0;
    bool playback_prefilled = false;
    std::size_t capture_ring_capacity_frames = 0;
    std::size_t capture_ring_depth_frames = 0;
    std::size_t playback_ring_capacity_frames = 0;
    std::size_t playback_ring_depth_frames = 0;
    audio::RingStats capture_ring;
    audio::RingStats playback_ring;
    audio::CallbackTimingStats callback_timing;
    bool network_capture_enabled = false;
    bool network_capture_ready = false;
    std::uint64_t network_capture_generation = 0;
    std::uint64_t network_capture_epoch_frame = 0;
    std::uint64_t network_capture_stale_frames_discarded = 0;
    std::uint64_t network_capture_attach_count = 0;
    std::uint64_t network_capture_detach_count = 0;
    bool network_playback_enabled = false;
    bool metronome_enabled = false;
    metronome::PatternSnapshot metronome_pattern{};
    EngineMetronomeMode metronome_mode = EngineMetronomeMode::SharedGrid;
    int metronome_level_ppm = 0;
    int remote_level_ppm = 0;
    int send_level_ppm = 0;
    bool local_monitor_enabled = false;
    int local_monitor_level_ppm = 0;
    int playback_ratio_ppm = 1000000;
    std::uint64_t metronome_epoch_frame = 0;
    bool metronome_epoch_valid = false;
    std::int64_t metronome_render_offset_frames = 0;
    std::uint64_t transport_revision = 0;
    EngineTransportAction transport_action = EngineTransportAction::None;
    std::uint64_t transport_target_frame = 0;
    std::uint64_t transport_musical_frame = 0;
    std::uint64_t transport_countdown_start_frame = 0;
    bool transport_pending = false;
    std::uint64_t transport_commit_count = 0;
    int input_peak_ppm = 0;
    int send_peak_ppm = 0;
    int monitor_peak_ppm = 0;
    int remote_peak_ppm = 0;
    int metronome_peak_ppm = 0;
    int output_peak_ppm = 0;
    std::uint64_t output_clipped_samples = 0;
    std::uint64_t prepared_source_frame = 0;
    std::uint64_t prepared_source_scheduled_start_frame = 0;
    std::uint64_t prepared_source_actual_start_frame = 0;
    std::uint64_t prepared_source_underruns = 0;
    std::uint64_t prepared_source_busy_events = 0;
    bool prepared_source_playing = false;
    EngineRecordingSnapshot jam_recording;
    EngineTrackTakeSnapshot track_take;
    std::size_t command_queue_capacity = 0;
    std::size_t command_queue_depth = 0;
    std::size_t command_queue_high_water = 0;
    std::uint64_t command_queue_rejections = 0;
    std::size_t scheduled_command_capacity = 0;
    std::size_t scheduled_command_depth = 0;
    std::size_t scheduled_command_high_water = 0;
    std::uint64_t scheduled_command_rejections = 0;
    std::size_t event_queue_capacity = 0;
    std::size_t event_queue_depth = 0;
    std::size_t event_queue_high_water = 0;
    std::uint64_t event_queue_drops = 0;
};

// Peaks accumulated specifically for one non-real-time GUI polling interval.
// Consuming them resets only the GUI accumulators; lifetime diagnostic peaks
// in EngineSnapshot remain unchanged for logs and benchmark comparisons.
struct EngineGuiPeakSnapshot {
    int input_peak_ppm = 0;
    int monitor_peak_ppm = 0;
    int remote_peak_ppm = 0;
    int metronome_peak_ppm = 0;
    int output_peak_ppm = 0;
};

// Cold metadata may own strings and channel lists. Keep it separate from the
// fixed-shape snapshot used by frequent UI and diagnostics polling.
struct EngineColdSnapshot {
    audio::StreamInfo stream;
    std::string recording_folder;
    int recording_sample_rate = 0;
};

struct NetworkCaptureAttachment {
    std::uint64_t generation = 0;
};

struct CapturedAudioBlock {
    std::uint64_t first_frame = 0;
    std::size_t frames = 0;
};

class Engine {
public:
    static constexpr std::size_t kCommandCapacity = 128;
    static constexpr std::size_t kScheduledCommandCapacity = 128;
    static constexpr std::size_t kEventCapacity = 128;

    Engine();
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void start(const EngineConfig& config);
    void requestStop() noexcept;
    void join() noexcept;

    bool submit(const EngineCommand& command) noexcept;
    EngineSnapshot snapshot() const noexcept;
    EngineGuiPeakSnapshot consumeGuiPeaks() noexcept;
    EngineColdSnapshot coldSnapshot() const;
    bool pollEvent(EngineEvent& event) noexcept;

    // One non-real-time capture consumer may use an attachment at a time.
    // Wait for networkCaptureReady before popping, stop the consumer before
    // detaching, and use CapturedAudioBlock::first_frame as the local engine
    // timeline tag rather than as a wire/session timestamp.
    NetworkCaptureAttachment attachNetworkCapture() noexcept;
    void detachNetworkCapture(NetworkCaptureAttachment attachment) noexcept;
    bool networkCaptureReady(NetworkCaptureAttachment attachment) const noexcept;
    CapturedAudioBlock popNetworkCapture(
        NetworkCaptureAttachment attachment,
        std::span<std::int32_t> output) noexcept;
    std::size_t networkPlaybackDepth() const noexcept;
    std::size_t pushNetworkPlayback(std::span<const std::int32_t> input) noexcept;
    void requestNetworkPlaybackDrop(std::size_t frames) noexcept;
    void setNetworkPlaybackRatio(double ratio) noexcept;

    const EngineConfig* config() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool engine_command_set_text(EngineCommand& command, std::string_view text) noexcept;
bool engine_command_set_id(EngineCommand& command, std::string_view id) noexcept;
std::string_view engine_event_text(const EngineEvent& event) noexcept;

} // namespace jam2

#pragma once

#include "audio_device.hpp"
#include "common.hpp"
#include "engine.hpp"
#include "network_session.hpp"
#include "stun.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

enum class Jam2MetronomeMode {
    SharedGrid,
    LeaderAudio,
    ListenerCompensated,
};

enum class Jam2TestInputMode {
    Off,
    Silence,
    Tone440,
    Pulse1s,
    MetroPulse,
};

enum class Jam2OsPriorityMode {
    Off,
    High,
    Realtime,
};

// Validated cold-path application configuration shared by the CLI parser,
// native GUI runtime, and explicit debug adapter. It is never read directly
// by the real-time callback.
struct Jam2RuntimeOptions {
    jam2::Endpoint bind{"0.0.0.0", 49000};
    jam2::Endpoint stun_server{"stun.l.google.com", jam2::stun::kDefaultPort};
    std::optional<jam2::Endpoint> public_endpoint;
    bool no_stun = false;
    int stun_timeout_ms = 1000;
    int stun_retries = 3;
    int wait_ms = 0;
    int stream_ms = 0;
    int stream_linger_ms = 100;
    bool arm_stream_on_first_peer = false;
    bool stats_enabled = false;
    int stats_interval_ms = 0;
    int stats_warmup_ms = 3000;
    std::optional<std::filesystem::path> log_stats_dir;
    std::optional<std::filesystem::path> record_jam_folder;
    std::optional<int> socket_send_buffer;
    std::optional<int> socket_recv_buffer;
    int sample_rate = 48000;
    int frame_size = 128;
    bool drift_correction = true;
    double drift_smoothing = 0.02;
    int drift_deadband_ppm = 25;
    int drift_max_correction_ppm = 500;
    bool metronome = false;
    int bpm = 120;
    double metronome_level = 1.0;
    Jam2MetronomeMode metronome_mode = Jam2MetronomeMode::SharedGrid;
    double metronome_compensation_max_ms = 250.0;
    double metronome_compensation_smoothing_ms = 750.0;
    double metronome_compensation_deadband_ms = 1.0;
    double metronome_compensation_slew_ms_per_sec = 40.0;
    double remote_level = 1.0;
    double send_level = 1.0;
    bool local_monitor = false;
    double local_monitor_level = 0.25;
    bool sample_time_playout = true;
    std::size_t playout_delay_frames = 0;
    std::size_t jitter_buffer_frames = 0;
    std::size_t jitter_buffer_max_frames = 0;
    bool adaptive_playback_cushion = false;
    std::size_t adaptive_playback_target_frames = 0;
    std::size_t adaptive_playback_min_frames = 0;
    std::size_t adaptive_playback_max_frames = 0;
    int adaptive_playback_release_ppm = 1000;
    std::optional<std::uint64_t> session_id;
    std::optional<std::array<std::uint8_t, 16>> session_key;
    std::vector<jam2::Endpoint> mesh_peers;
    std::vector<std::uint64_t> mesh_peer_ids;
    bool mesh_peers_configured = false;
    std::optional<std::uint64_t> local_peer_id;
    std::optional<std::uint64_t> bootstrap_coordinator_peer_id;
    jam2::SessionBootstrapRole bootstrap_role = jam2::SessionBootstrapRole::Creator;
    std::optional<int> audio_device_id;
    bool headless_audio = false;
    int headless_clock_drift_ppm = 0;
    std::string profile_name;
    long audio_buffer_size = 0;
    jam2::audio::InputChannels input_channels = jam2::audio::InputChannels::Mono;
    jam2::audio::ChannelSelection channel_selection;
    std::size_t capture_ring_frames = 4096;
    std::size_t playback_ring_frames = 4096;
    std::size_t playback_prefill_frames = 0;
    std::size_t playback_max_frames = 0;
    Jam2TestInputMode test_input = Jam2TestInputMode::Off;
    Jam2OsPriorityMode os_priority = Jam2OsPriorityMode::High;
};

struct Jam2RuntimeStartup {
    jam2::Endpoint local_endpoint;
    std::optional<jam2::Endpoint> public_candidate;
    std::optional<std::filesystem::path> stats_csv;
};

struct Jam2RuntimePeer {
    std::uint64_t peer_id = 0;
    jam2::Endpoint endpoint;

    bool operator==(const Jam2RuntimePeer& other) const noexcept
    {
        return peer_id == other.peer_id &&
            endpoint.host == other.endpoint.host &&
            endpoint.port == other.endpoint.port;
    }
};

struct Jam2RuntimeHost {
    static constexpr std::size_t kCommandCapacity = 128;

    jam2::Engine* engine = nullptr;
    std::atomic<bool> stop_requested{false};
    // GUI preference read only on the network/control path.  Audio callbacks
    // never consult it.  Headless runtimes accept shared track transport by
    // default.
    std::atomic<bool> track_sync_enabled{true};
    std::function<void(const Jam2RuntimeStartup&)> startup;
    std::function<void(const jam2::NetworkSessionSnapshot&)> network_snapshot;
    std::function<void(std::string_view)> log;
    std::function<void(std::string_view)> error;

    bool submitCommand(const jam2::EngineCommand& command) noexcept;
    bool submitPeerUpdate(const std::vector<Jam2RuntimePeer>& peers);
    std::optional<std::vector<Jam2RuntimePeer>> takePeerUpdate();
    std::optional<jam2::EngineCommand> takeCommand(std::uint64_t current_frame);
    std::uint64_t nextGridRequestId() noexcept;
    std::uint64_t nextTransportEventId() noexcept;
    void reset() noexcept;

private:
    std::mutex peer_mutex_;
    std::optional<std::vector<Jam2RuntimePeer>> peer_update_;
    std::mutex command_mutex_;
    std::deque<jam2::EngineCommand> commands_;
    // The same authenticated peer identity survives Leave/Join while the
    // application stays open. Keep proposal identities monotonic across those
    // network-worker lifetimes so a coordinator cannot mistake a new request
    // for a replay from the previous attachment.
    std::atomic<std::uint64_t> grid_request_id_{0};
    std::atomic<std::uint64_t> transport_event_id_{0};
};

Jam2RuntimeOptions jam2_parse_runtime_options(int argc, char** argv, int start_index);
jam2::EngineConfig jam2_make_engine_config(
    const Jam2RuntimeOptions& options,
    bool leader_audio_local_click);
bool jam2_engine_restart_required(
    const jam2::EngineConfig& active,
    const jam2::EngineConfig& requested) noexcept;

int jam2_run_network_runtime(Jam2RuntimeOptions options, Jam2RuntimeHost& host);

#include <exception>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <memory>
#include <optional>
#include <sstream>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <avrt.h>
#include <mmsystem.h>
#elif defined(__APPLE__)
#include <arpa/inet.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <netdb.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "audio_device.hpp"
#include "common.hpp"
#include "engine.hpp"
#include "gui_control_protocol.hpp"
#include "metronome.hpp"
#include "network_session.hpp"
#include "pcm16_wav.hpp"
#include "peer_stream.hpp"
#include "protocol.hpp"
#include "session_authority.hpp"
#include "stun.hpp"
#include "tuning_profile.hpp"
#include "udp_socket.hpp"

namespace {

constexpr std::string_view kUsage = R"(jam2 - two-person low-latency music streaming tool

Usage:
  jam2 --help
  jam2 list-devices
  jam2 test-device <id> [--sample-rate n]
  jam2 meter-device <id> [--sample-rate n] [--buffer-size n] [--duration-ms n]
  jam2 ring-device <id> [--sample-rate n] [--buffer-size n] [--duration-ms n] [--ring-frames n]
   jam2 local [audio options] [--metronome on|off] [--bpm n] [--status-format text|jsonl]
   jam2 listen [--profile fast|moderate|safe] [--bind ip:port] [--stun host:port] [--no-stun] [--public-endpoint ip:port] [--wait-ms n] [--stream-ms n] [--stream-linger-ms n] [--stats enabled|disabled] [--stats-interval-ms n] [--stats-warmup-ms n] [--log-stats folder] [--record-jam-folder folder] [--test-input off|silence|tone-440|pulse-1s|metro-pulse] [--os-priority off|high|realtime] [--metronome on|off] [--bpm n] [--metronome-level n] [--remote-level n] [--send-level n] [--local-monitor on|off] [--local-monitor-level n] [--gui-control ip:port] [--metronome-mode shared-grid|leader-audio|listener-compensated] [--metronome-compensation-max-ms n] [--metronome-compensation-smoothing-ms n] [--metronome-compensation-deadband-ms n] [--metronome-compensation-slew-ms-per-sec n] [--sample-time-playout on|off] [--playout-delay-frames n] [--jitter-buffer-frames n] [--jitter-buffer-max-frames n] [--adaptive-playback-cushion on|off] [--adaptive-playback-target-frames n] [--adaptive-playback-min-frames n] [--adaptive-playback-max-frames n] [--adaptive-playback-release-ppm n] [--session-id hex] [--session-key hex32] [--machine-readable-startup on|off] [--status-format text|jsonl] [--socket-send-buffer n] [--socket-recv-buffer n] [--input-channels n[,n...]] [--output-channels n[,n...]] [--playback-prefill-frames n] [--playback-max-frames n] [--drift-smoothing n] [--drift-deadband-ppm n] [--drift-max-correction-ppm n]
  jam2 connect <jam2-url> [--profile fast|moderate|safe] [options]
  jam2 mesh --session-id <hex> --session-key <hex32> --bind ip:port --peers ip:port[,ip:port...] [options] [--grid-coordinator on|off] [--headless-audio on|off]

Stage status:
  UDP HELLO/HELLO_ACK session setup, jam2 URL parsing, and STUN endpoint discovery are implemented.
  UDP audio streaming, Windows ASIO, drift stats/correction, metronome controls, and fixed-peer experimental mesh mode are implemented for the MVP slice.
)";

enum class MetronomeMode {
    SharedGrid,
    LeaderAudio,
    ListenerCompensated,
};

enum class TestInputMode {
    Off,
    Silence,
    Tone440,
    Pulse1s,
    MetroPulse,
};

enum class OsPriorityMode {
    Off,
    High,
    Realtime,
};

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

struct Options {
    jam2::Endpoint bind{"0.0.0.0", 49000};
    jam2::Endpoint stun_server{"stun.l.google.com", jam2::stun::kDefaultPort};
    std::optional<jam2::Endpoint> public_endpoint;
    bool no_stun = false;
    int stun_timeout_ms = 1000;
    int stun_retries = 3;
    int wait_ms = 0;
    int stream_ms = 0;
    int stream_linger_ms = 100;
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
    MetronomeMode metronome_mode = MetronomeMode::SharedGrid;
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
    bool mesh_peers_configured = false;
    bool grid_coordinator = false;
    std::optional<jam2::Endpoint> gui_control;
    bool machine_readable_startup = false;
    bool status_jsonl = false;
    std::optional<int> audio_device_id;
    bool headless_audio = false;
    std::string profile_name;
    long audio_buffer_size = 0;
    jam2::audio::InputChannels input_channels = jam2::audio::InputChannels::Mono;
    jam2::audio::ChannelSelection channel_selection;
    std::size_t capture_ring_frames = 4096;
    std::size_t playback_ring_frames = 4096;
    std::size_t playback_prefill_frames = 0;
    std::size_t playback_max_frames = 0;
    TestInputMode test_input = TestInputMode::Off;
    OsPriorityMode os_priority = OsPriorityMode::High;
};

void apply_tuning_profile(Options& options, const jam2::TuningProfile& profile)
{
    options.profile_name.assign(profile.name.data(), profile.name.size());
    options.sample_rate = profile.sample_rate;
    options.audio_buffer_size = profile.audio_buffer_size;
    options.frame_size = profile.frame_size;
    options.playback_prefill_frames = profile.playback_prefill_frames;
    options.playback_ring_frames = profile.playback_ring_frames;
    options.playback_max_frames = profile.playback_max_frames;
    options.capture_ring_frames = profile.capture_ring_frames;
    options.drift_correction = profile.drift_correction;
    options.drift_smoothing = profile.drift_smoothing;
    options.drift_deadband_ppm = profile.drift_deadband_ppm;
    options.drift_max_correction_ppm = profile.drift_max_correction_ppm;
    options.sample_time_playout = profile.sample_time_playout;
    options.playout_delay_frames = profile.playout_delay_frames;
    options.jitter_buffer_frames = profile.jitter_buffer_frames;
    options.jitter_buffer_max_frames = profile.jitter_buffer_max_frames;
    options.adaptive_playback_cushion = profile.adaptive_playback_cushion;
    options.adaptive_playback_target_frames = profile.adaptive_playback_target_frames;
    options.adaptive_playback_min_frames = profile.adaptive_playback_min_frames;
    options.adaptive_playback_max_frames = profile.adaptive_playback_max_frames;
    options.adaptive_playback_release_ppm = profile.adaptive_playback_release_ppm;
}

std::string_view require_value(int argc, char** argv, int& i, std::string_view name)
{
    if (i + 1 >= argc) {
        throw std::runtime_error("missing value for " + std::string(name));
    }
    ++i;
    return argv[i];
}

MetronomeMode parse_metronome_mode(std::string_view value)
{
    if (value == "shared-grid") {
        return MetronomeMode::SharedGrid;
    }
    if (value == "leader-audio") {
        return MetronomeMode::LeaderAudio;
    }
    if (value == "listener-compensated") {
        return MetronomeMode::ListenerCompensated;
    }
    throw std::runtime_error("--metronome-mode must be shared-grid, leader-audio, or listener-compensated");
}

std::string_view metronome_mode_text(MetronomeMode mode)
{
    switch (mode) {
    case MetronomeMode::SharedGrid:
        return "shared-grid";
    case MetronomeMode::LeaderAudio:
        return "leader-audio";
    case MetronomeMode::ListenerCompensated:
        return "listener-compensated";
    }
    return "shared-grid";
}

std::string_view metronome_mode_text(int mode)
{
    switch (mode) {
    case 0:
        return "shared-grid";
    case 1:
        return "leader-audio";
    case 2:
        return "listener-compensated";
    default:
        return "shared-grid";
    }
}

int metronome_mode_id(MetronomeMode mode)
{
    switch (mode) {
    case MetronomeMode::SharedGrid:
        return 0;
    case MetronomeMode::LeaderAudio:
        return 1;
    case MetronomeMode::ListenerCompensated:
        return 2;
    }
    return 0;
}

TestInputMode parse_test_input_mode(std::string_view value)
{
    if (value == "off") {
        return TestInputMode::Off;
    }
    if (value == "silence") {
        return TestInputMode::Silence;
    }
    if (value == "tone-440") {
        return TestInputMode::Tone440;
    }
    if (value == "pulse-1s") {
        return TestInputMode::Pulse1s;
    }
    if (value == "metro-pulse") {
        return TestInputMode::MetroPulse;
    }
    throw std::runtime_error("--test-input must be off, silence, tone-440, pulse-1s, or metro-pulse");
}

std::string_view test_input_mode_text(TestInputMode mode)
{
    switch (mode) {
    case TestInputMode::Off:
        return "off";
    case TestInputMode::Silence:
        return "silence";
    case TestInputMode::Tone440:
        return "tone-440";
    case TestInputMode::Pulse1s:
        return "pulse-1s";
    case TestInputMode::MetroPulse:
        return "metro-pulse";
    }
    return "off";
}

int test_input_mode_id(TestInputMode mode)
{
    switch (mode) {
    case TestInputMode::Off:
        return 0;
    case TestInputMode::Silence:
        return 1;
    case TestInputMode::Tone440:
        return 2;
    case TestInputMode::Pulse1s:
        return 3;
    case TestInputMode::MetroPulse:
        return 4;
    }
    return 0;
}

std::vector<int> parse_channel_list(std::string_view value, std::size_t min_count, std::size_t max_count, std::string_view option)
{
    std::vector<int> channels;
    std::size_t pos = 0;
    while (pos <= value.size()) {
        const std::size_t comma = value.find(',', pos);
        const std::string_view part = value.substr(pos, comma == std::string_view::npos ? value.size() - pos : comma - pos);
        if (part.empty()) {
            throw std::runtime_error(std::string(option) + " contains an empty channel");
        }
        std::size_t consumed = 0;
        int channel = 0;
        try {
            channel = std::stoi(std::string(part), &consumed);
        } catch (const std::exception&) {
            throw std::runtime_error(std::string(option) + " channels must be positive 1-based numbers");
        }
        if (consumed != part.size() || channel <= 0) {
            throw std::runtime_error(std::string(option) + " channels must be positive 1-based numbers");
        }
        if (std::find(channels.begin(), channels.end(), channel - 1) != channels.end()) {
            throw std::runtime_error(std::string(option) + " contains a duplicate channel");
        }
        channels.push_back(channel - 1);
        if (comma == std::string_view::npos) {
            break;
        }
        pos = comma + 1;
    }
    if (channels.size() < min_count || (max_count > 0 && channels.size() > max_count)) {
        std::ostringstream message;
        message << option << " expects " << min_count;
        if (max_count == 0) {
            message << "+";
        } else if (min_count != max_count) {
            message << ".." << max_count;
        }
        message << " channel value(s)";
        throw std::runtime_error(message.str());
    }
    return channels;
}

std::vector<jam2::Endpoint> parse_peer_list(std::string_view value, const jam2::Endpoint& self)
{
    std::vector<jam2::Endpoint> peers;
    if (value.empty()) {
        return peers;
    }
    std::size_t pos = 0;
    while (pos <= value.size()) {
        const std::size_t comma = value.find(',', pos);
        const std::string_view part = value.substr(pos, comma == std::string_view::npos ? value.size() - pos : comma - pos);
        if (part.empty()) {
            throw std::runtime_error("--peers contains an empty endpoint");
        }
        const jam2::Endpoint endpoint = jam2::parse_endpoint(part);
        if (endpoint.host == self.host && endpoint.port == self.port) {
            throw std::runtime_error("--peers must not include the local --bind endpoint");
        }
        const auto duplicate = std::find_if(peers.begin(), peers.end(), [&](const jam2::Endpoint& existing) {
            return existing.host == endpoint.host && existing.port == endpoint.port;
        });
        if (duplicate != peers.end()) {
            throw std::runtime_error("--peers contains a duplicate endpoint: " + jam2::endpoint_to_string(endpoint));
        }
        peers.push_back(endpoint);
        if (comma == std::string_view::npos) {
            break;
        }
        pos = comma + 1;
    }
    return peers;
}

std::string channel_selection_text(const jam2::audio::ChannelSelection& channels)
{
    auto channel_list = [](const std::vector<int>& selected) {
        if (selected.empty()) {
            return std::string("off");
        }
        std::ostringstream out;
        for (std::size_t i = 0; i < selected.size(); ++i) {
            if (i > 0) {
                out << ",";
            }
            out << (selected[i] + 1);
        }
        return out.str();
    };
    return "input=" + channel_list(channels.input) + " output=" + channel_list(channels.output);
}

std::string mono_mix_mode_text(std::size_t channel_count)
{
    return std::to_string(channel_count) + "-to-mono";
}

std::string_view os_priority_text(OsPriorityMode mode)
{
    switch (mode) {
    case OsPriorityMode::Off:
        return "off";
    case OsPriorityMode::High:
        return "high";
    case OsPriorityMode::Realtime:
        return "realtime";
    }
    return "unknown";
}

OsPriorityMode parse_os_priority(std::string_view value)
{
    if (value == "off") {
        return OsPriorityMode::Off;
    }
    if (value == "high") {
        return OsPriorityMode::High;
    }
    if (value == "realtime") {
        return OsPriorityMode::Realtime;
    }
    throw std::runtime_error("--os-priority must be off, high, or realtime");
}

Options parse_options(int argc, char** argv, int start)
{
    Options options;
    const jam2::TuningProfile* selected_profile = &jam2::default_tuning_profile();
    for (int i = start; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--profile") {
            const auto value = require_value(argc, argv, i, arg);
            selected_profile = jam2::find_tuning_profile(value);
            if (selected_profile == nullptr) {
                throw std::runtime_error("--profile must be one of: " + jam2::tuning_profile_names());
            }
        }
    }
    apply_tuning_profile(options, *selected_profile);
    for (int i = start; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--profile") {
            (void)require_value(argc, argv, i, arg);
        } else if (arg == "--bind") {
            options.bind = jam2::parse_bind_endpoint(require_value(argc, argv, i, arg));
        } else if (arg == "--peers") {
            options.mesh_peers_configured = true;
            options.mesh_peers = parse_peer_list(require_value(argc, argv, i, arg), options.bind);
        } else if (arg == "--grid-coordinator") {
            const std::string value{require_value(argc, argv, i, arg)};
            if (value == "on") {
                options.grid_coordinator = true;
            } else if (value == "off") {
                options.grid_coordinator = false;
            } else {
                throw std::runtime_error("--grid-coordinator must be on or off");
            }
        } else if (arg == "--stun") {
            options.stun_server = jam2::parse_endpoint(require_value(argc, argv, i, arg));
            options.no_stun = false;
        } else if (arg == "--public-endpoint") {
            options.public_endpoint = jam2::parse_endpoint(require_value(argc, argv, i, arg));
        } else if (arg == "--no-stun") {
            options.no_stun = true;
        } else if (arg == "--stun-timeout-ms") {
            options.stun_timeout_ms = std::stoi(std::string(require_value(argc, argv, i, arg)));
            if (options.stun_timeout_ms <= 0) {
                throw std::runtime_error("--stun-timeout-ms must be positive");
            }
        } else if (arg == "--stun-retries") {
            options.stun_retries = std::stoi(std::string(require_value(argc, argv, i, arg)));
            if (options.stun_retries <= 0) {
                throw std::runtime_error("--stun-retries must be positive");
            }
        } else if (arg == "--wait-ms") {
            options.wait_ms = std::stoi(std::string(require_value(argc, argv, i, arg)));
            if (options.wait_ms < 0) {
                throw std::runtime_error("--wait-ms must be non-negative");
            }
        } else if (arg == "--stream-ms") {
            options.stream_ms = std::stoi(std::string(require_value(argc, argv, i, arg)));
            if (options.stream_ms < 0) {
                throw std::runtime_error("--stream-ms must be non-negative");
            }
        } else if (arg == "--stream-linger-ms") {
            options.stream_linger_ms = std::stoi(std::string(require_value(argc, argv, i, arg)));
            if (options.stream_linger_ms < 0) {
                throw std::runtime_error("--stream-linger-ms must be non-negative");
            }
        } else if (arg == "--stats") {
            const auto value = require_value(argc, argv, i, arg);
            if (value == "enabled" || value == "on" || value == "true" || value == "1") {
                options.stats_enabled = true;
            } else if (value == "disabled" || value == "off" || value == "false" || value == "0") {
                options.stats_enabled = false;
            } else {
                throw std::runtime_error("--stats must be enabled or disabled");
            }
        } else if (arg == "--stats-interval-ms") {
            options.stats_interval_ms = std::stoi(std::string(require_value(argc, argv, i, arg)));
            if (options.stats_interval_ms < 0) {
                throw std::runtime_error("--stats-interval-ms must be non-negative");
            }
        } else if (arg == "--stats-warmup-ms") {
            options.stats_warmup_ms = std::stoi(std::string(require_value(argc, argv, i, arg)));
            if (options.stats_warmup_ms < 0) {
                throw std::runtime_error("--stats-warmup-ms must be non-negative");
            }
        } else if (arg == "--log-stats") {
            options.log_stats_dir = std::filesystem::path(std::string(require_value(argc, argv, i, arg)));
        } else if (arg == "--record-jam-folder") {
            options.record_jam_folder = std::filesystem::path(std::string(require_value(argc, argv, i, arg)));
        } else if (arg == "--test-input") {
            options.test_input = parse_test_input_mode(require_value(argc, argv, i, arg));
        } else if (arg == "--os-priority") {
            options.os_priority = parse_os_priority(require_value(argc, argv, i, arg));
        } else if (arg == "--sample-rate") {
            options.sample_rate = std::stoi(std::string(require_value(argc, argv, i, arg)));
            if (options.sample_rate <= 0) {
                throw std::runtime_error("--sample-rate must be positive");
            }
        } else if (arg == "--socket-send-buffer") {
            options.socket_send_buffer = std::stoi(std::string(require_value(argc, argv, i, arg)));
            if (*options.socket_send_buffer <= 0) {
                throw std::runtime_error("--socket-send-buffer must be positive");
            }
        } else if (arg == "--socket-recv-buffer") {
            options.socket_recv_buffer = std::stoi(std::string(require_value(argc, argv, i, arg)));
            if (*options.socket_recv_buffer <= 0) {
                throw std::runtime_error("--socket-recv-buffer must be positive");
            }
        } else if (arg == "--frame-size") {
            options.frame_size = std::stoi(std::string(require_value(argc, argv, i, arg)));
            if (options.frame_size != 32 && options.frame_size != 64 && options.frame_size != 128 &&
                options.frame_size != 256) {
                throw std::runtime_error("--frame-size must be 32, 64, 128, or 256");
            }
        } else if (arg == "--drift-correction") {
            const std::string value{require_value(argc, argv, i, arg)};
            if (value == "on") {
                options.drift_correction = true;
            } else if (value == "off") {
                options.drift_correction = false;
            } else {
                throw std::runtime_error("--drift-correction must be on or off");
            }
        } else if (arg == "--drift-smoothing") {
            options.drift_smoothing = std::stod(std::string(require_value(argc, argv, i, arg)));
            if (options.drift_smoothing < 0.0 || options.drift_smoothing > 1.0) {
                throw std::runtime_error("--drift-smoothing must be 0..1");
            }
        } else if (arg == "--drift-deadband-ppm") {
            options.drift_deadband_ppm = std::stoi(std::string(require_value(argc, argv, i, arg)));
            if (options.drift_deadband_ppm < 0 || options.drift_deadband_ppm > 50000) {
                throw std::runtime_error("--drift-deadband-ppm must be 0..50000");
            }
        } else if (arg == "--drift-max-correction-ppm") {
            options.drift_max_correction_ppm = std::stoi(std::string(require_value(argc, argv, i, arg)));
            if (options.drift_max_correction_ppm < 0 || options.drift_max_correction_ppm > 50000) {
                throw std::runtime_error("--drift-max-correction-ppm must be 0..50000");
            }
        } else if (arg == "--metronome") {
            const std::string value{require_value(argc, argv, i, arg)};
            if (value == "on") {
                options.metronome = true;
            } else if (value == "off") {
                options.metronome = false;
            } else {
                throw std::runtime_error("--metronome must be on or off");
            }
        } else if (arg == "--bpm") {
            options.bpm = std::stoi(std::string(require_value(argc, argv, i, arg)));
            if (options.bpm <= 0 || options.bpm > 400) {
                throw std::runtime_error("--bpm must be 1..400");
            }
        } else if (arg == "--metronome-level") {
            options.metronome_level = std::stod(std::string(require_value(argc, argv, i, arg)));
            if (options.metronome_level < 0.0 || options.metronome_level > 4.0) {
                throw std::runtime_error("--metronome-level must be 0..4");
            }
        } else if (arg == "--metronome-mode") {
            options.metronome_mode = parse_metronome_mode(require_value(argc, argv, i, arg));
        } else if (arg == "--metronome-compensation-max-ms") {
            options.metronome_compensation_max_ms = std::stod(std::string(require_value(argc, argv, i, arg)));
            if (options.metronome_compensation_max_ms < 0.0 || options.metronome_compensation_max_ms > 1000.0) {
                throw std::runtime_error("--metronome-compensation-max-ms must be 0..1000");
            }
        } else if (arg == "--metronome-compensation-smoothing-ms") {
            options.metronome_compensation_smoothing_ms = std::stod(std::string(require_value(argc, argv, i, arg)));
            if (options.metronome_compensation_smoothing_ms < 0.0 || options.metronome_compensation_smoothing_ms > 10000.0) {
                throw std::runtime_error("--metronome-compensation-smoothing-ms must be 0..10000");
            }
        } else if (arg == "--metronome-compensation-deadband-ms") {
            options.metronome_compensation_deadband_ms = std::stod(std::string(require_value(argc, argv, i, arg)));
            if (options.metronome_compensation_deadband_ms < 0.0 || options.metronome_compensation_deadband_ms > 1000.0) {
                throw std::runtime_error("--metronome-compensation-deadband-ms must be 0..1000");
            }
        } else if (arg == "--metronome-compensation-slew-ms-per-sec") {
            options.metronome_compensation_slew_ms_per_sec = std::stod(std::string(require_value(argc, argv, i, arg)));
            if (options.metronome_compensation_slew_ms_per_sec < 0.0 || options.metronome_compensation_slew_ms_per_sec > 10000.0) {
                throw std::runtime_error("--metronome-compensation-slew-ms-per-sec must be 0..10000");
            }
        } else if (arg == "--remote-level") {
            options.remote_level = std::stod(std::string(require_value(argc, argv, i, arg)));
            if (options.remote_level < 0.0 || options.remote_level > 4.0) {
                throw std::runtime_error("--remote-level must be 0..4");
            }
        } else if (arg == "--send-level") {
            options.send_level = std::stod(std::string(require_value(argc, argv, i, arg)));
            if (options.send_level < 0.0 || options.send_level > 4.0) {
                throw std::runtime_error("--send-level must be 0..4");
            }
        } else if (arg == "--local-monitor") {
            const auto value = require_value(argc, argv, i, arg);
            if (value == "on") {
                options.local_monitor = true;
            } else if (value == "off") {
                options.local_monitor = false;
            } else {
                throw std::runtime_error("--local-monitor must be on or off");
            }
        } else if (arg == "--local-monitor-level") {
            options.local_monitor_level = std::stod(std::string(require_value(argc, argv, i, arg)));
            if (options.local_monitor_level < 0.0 || options.local_monitor_level > 4.0) {
                throw std::runtime_error("--local-monitor-level must be 0..4");
            }
        } else if (arg == "--gui-control") {
            options.gui_control = jam2::parse_endpoint(require_value(argc, argv, i, arg));
        } else if (arg == "--sample-time-playout") {
            const std::string value{require_value(argc, argv, i, arg)};
            if (value == "on" || value == "true" || value == "1") {
                options.sample_time_playout = true;
            } else if (value == "off" || value == "false" || value == "0") {
                options.sample_time_playout = false;
            } else {
                throw std::runtime_error("--sample-time-playout must be on or off");
            }
        } else if (arg == "--playout-delay-frames") {
            const auto parsed = std::stoull(std::string(require_value(argc, argv, i, arg)));
            options.playout_delay_frames = static_cast<std::size_t>(parsed);
        } else if (arg == "--jitter-buffer-frames") {
            const auto parsed = std::stoull(std::string(require_value(argc, argv, i, arg)));
            options.jitter_buffer_frames = static_cast<std::size_t>(parsed);
        } else if (arg == "--jitter-buffer-max-frames") {
            const auto parsed = std::stoull(std::string(require_value(argc, argv, i, arg)));
            options.jitter_buffer_max_frames = static_cast<std::size_t>(parsed);
        } else if (arg == "--adaptive-playback-cushion") {
            const std::string value{require_value(argc, argv, i, arg)};
            if (value == "on" || value == "true" || value == "1") {
                options.adaptive_playback_cushion = true;
            } else if (value == "off" || value == "false" || value == "0") {
                options.adaptive_playback_cushion = false;
            } else {
                throw std::runtime_error("--adaptive-playback-cushion must be on or off");
            }
        } else if (arg == "--adaptive-playback-target-frames") {
            const auto parsed = std::stoull(std::string(require_value(argc, argv, i, arg)));
            options.adaptive_playback_target_frames = static_cast<std::size_t>(parsed);
        } else if (arg == "--adaptive-playback-min-frames") {
            const auto parsed = std::stoull(std::string(require_value(argc, argv, i, arg)));
            options.adaptive_playback_min_frames = static_cast<std::size_t>(parsed);
        } else if (arg == "--adaptive-playback-max-frames") {
            const auto parsed = std::stoull(std::string(require_value(argc, argv, i, arg)));
            options.adaptive_playback_max_frames = static_cast<std::size_t>(parsed);
        } else if (arg == "--adaptive-playback-release-ppm") {
            options.adaptive_playback_release_ppm = std::stoi(std::string(require_value(argc, argv, i, arg)));
            if (options.adaptive_playback_release_ppm < 0 || options.adaptive_playback_release_ppm > 1000000) {
                throw std::runtime_error("--adaptive-playback-release-ppm must be 0..1000000");
            }
        } else if (arg == "--session-id") {
            options.session_id = jam2::parse_hex_u64(require_value(argc, argv, i, arg));
        } else if (arg == "--session-key") {
            const auto key = jam2::hex_decode(require_value(argc, argv, i, arg));
            if (key.size() != 16) {
                throw std::runtime_error("--session-key must be 16 bytes encoded as 32 hex characters");
            }
            std::array<std::uint8_t, 16> parsed{};
            std::copy(key.begin(), key.end(), parsed.begin());
            options.session_key = parsed;
        } else if (arg == "--machine-readable-startup") {
            const std::string value{require_value(argc, argv, i, arg)};
            if (value == "on" || value == "true" || value == "1") {
                options.machine_readable_startup = true;
            } else if (value == "off" || value == "false" || value == "0") {
                options.machine_readable_startup = false;
            } else {
                throw std::runtime_error("--machine-readable-startup must be on or off");
            }
        } else if (arg == "--status-format") {
            const std::string value{require_value(argc, argv, i, arg)};
            if (value == "text") {
                options.status_jsonl = false;
            } else if (value == "jsonl") {
                options.status_jsonl = true;
            } else {
                throw std::runtime_error("--status-format must be text or jsonl");
            }
        } else if (arg == "--audio-device") {
            options.audio_device_id = std::stoi(std::string(require_value(argc, argv, i, arg)));
            if (*options.audio_device_id < 0) {
                throw std::runtime_error("--audio-device must be non-negative");
            }
        } else if (arg == "--headless-audio") {
            const std::string value{require_value(argc, argv, i, arg)};
            if (value == "on" || value == "true" || value == "1") {
                options.headless_audio = true;
            } else if (value == "off" || value == "false" || value == "0") {
                options.headless_audio = false;
            } else {
                throw std::runtime_error("--headless-audio must be on or off");
            }
        } else if (arg == "--audio-buffer-size") {
            options.audio_buffer_size = std::stol(std::string(require_value(argc, argv, i, arg)));
            if (options.audio_buffer_size <= 0) {
                throw std::runtime_error("--audio-buffer-size must be positive");
            }
        } else if (arg == "--input-channels") {
            const std::string value{require_value(argc, argv, i, arg)};
            const auto channels = parse_channel_list(value, 1, 0, arg);
            options.input_channels = jam2::audio::InputChannels::Mono;
            options.channel_selection.input = channels;
        } else if (arg == "--output-channels") {
            const std::string value{require_value(argc, argv, i, arg)};
            const auto channels = parse_channel_list(value, 1, 0, arg);
            options.channel_selection.output = channels;
        } else if (arg == "--capture-ring-frames") {
            const auto parsed = std::stoull(std::string(require_value(argc, argv, i, arg)));
            if (parsed == 0) {
                throw std::runtime_error("--capture-ring-frames must be positive");
            }
            options.capture_ring_frames = static_cast<std::size_t>(parsed);
        } else if (arg == "--playback-ring-frames") {
            const auto parsed = std::stoull(std::string(require_value(argc, argv, i, arg)));
            if (parsed == 0) {
                throw std::runtime_error("--playback-ring-frames must be positive");
            }
            options.playback_ring_frames = static_cast<std::size_t>(parsed);
        } else if (arg == "--playback-prefill-frames") {
            const auto parsed = std::stoull(std::string(require_value(argc, argv, i, arg)));
            options.playback_prefill_frames = static_cast<std::size_t>(parsed);
        } else if (arg == "--playback-max-frames") {
            const auto parsed = std::stoull(std::string(require_value(argc, argv, i, arg)));
            options.playback_max_frames = static_cast<std::size_t>(parsed);
        } else if (arg == "--device") {
            (void)require_value(argc, argv, i, arg);
        } else {
            throw std::runtime_error("unknown option: " + std::string(arg));
        }
    }
    if (!options.stats_enabled && options.stats_interval_ms > 0) {
        throw std::runtime_error("--stats-interval-ms requires --stats enabled");
    }
    if (!options.stats_enabled && options.log_stats_dir) {
        throw std::runtime_error("--log-stats requires --stats enabled");
    }
    if (options.headless_audio && options.audio_device_id) {
        throw std::runtime_error("--headless-audio cannot be used with --audio-device");
    }
    if (options.session_id.has_value() != options.session_key.has_value()) {
        throw std::runtime_error("--session-id and --session-key must be provided together");
    }
    if (options.playout_delay_frames == 0) {
        options.playout_delay_frames = options.playback_prefill_frames;
    }
    if (options.jitter_buffer_frames > 0 && options.jitter_buffer_max_frames == 0) {
        options.jitter_buffer_max_frames = std::max<std::size_t>(
            options.jitter_buffer_frames,
            static_cast<std::size_t>(options.frame_size));
    }
    if (options.adaptive_playback_target_frames == 0) {
        options.adaptive_playback_target_frames = options.playout_delay_frames;
    }
    if (options.adaptive_playback_min_frames == 0) {
        options.adaptive_playback_min_frames = options.playout_delay_frames;
    }
    if (options.adaptive_playback_max_frames == 0) {
        options.adaptive_playback_max_frames =
            options.playback_max_frames > 0 ? options.playback_max_frames : options.playback_ring_frames;
    }
    if (options.adaptive_playback_min_frames > options.adaptive_playback_target_frames ||
        options.adaptive_playback_target_frames > options.adaptive_playback_max_frames) {
        throw std::runtime_error("adaptive playback frames must satisfy min <= target <= max");
    }
    if (options.playout_delay_frames > options.playback_ring_frames) {
        throw std::runtime_error("--playout-delay-frames must fit within playback ring capacity");
    }
    if (options.jitter_buffer_frames == 0 && options.jitter_buffer_max_frames > 0) {
        throw std::runtime_error("--jitter-buffer-max-frames requires --jitter-buffer-frames");
    }
    if (options.jitter_buffer_max_frames > 0 && options.jitter_buffer_max_frames < options.jitter_buffer_frames) {
        throw std::runtime_error("--jitter-buffer-max-frames must be >= --jitter-buffer-frames");
    }
    if (options.adaptive_playback_max_frames > options.playback_ring_frames) {
        throw std::runtime_error("--adaptive-playback-max-frames must fit within playback ring capacity");
    }
    for (const auto& peer : options.mesh_peers) {
        if (peer.host == options.bind.host && peer.port == options.bind.port) {
            throw std::runtime_error("--peers must not include the local --bind endpoint");
        }
    }
    return options;
}

void apply_socket_options(jam2::UdpSocket& socket, const Options& options)
{
    if (options.socket_send_buffer) {
        socket.set_send_buffer_size(*options.socket_send_buffer);
    }
    if (options.socket_recv_buffer) {
        socket.set_recv_buffer_size(*options.socket_recv_buffer);
    }
}

void print_socket_options(const jam2::UdpSocket& socket)
{
    std::cout << "UDP send buffer bytes: " << socket.send_buffer_size() << "\n";
    std::cout << "UDP receive buffer bytes: " << socket.recv_buffer_size() << "\n";
}

struct StreamConfig {
    std::uint32_t sample_rate = 0;
    std::uint32_t frame_size = 0;
};

void put_u32(std::vector<std::uint8_t>& out, std::size_t offset, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i) {
        out[offset + i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xffU);
    }
}

std::uint32_t read_u32(std::span<const std::uint8_t> in, std::size_t offset)
{
    std::uint32_t value = 0;
    for (int i = 3; i >= 0; --i) {
        value = (value << 8) | in[offset + i];
    }
    return value;
}

std::array<std::uint8_t, 8> encode_stream_config(const Options& options)
{
    std::array<std::uint8_t, 8> payload{};
    std::vector<std::uint8_t> out(payload.size());
    put_u32(out, 0, static_cast<std::uint32_t>(options.sample_rate));
    put_u32(out, 4, static_cast<std::uint32_t>(options.frame_size));
    std::copy(out.begin(), out.end(), payload.begin());
    return payload;
}

StreamConfig decode_stream_config(std::span<const std::uint8_t> payload)
{
    if (payload.size() != 8) {
        throw std::runtime_error("handshake stream config payload size mismatch");
    }
    return StreamConfig{
        read_u32(payload, 0),
        read_u32(payload, 4),
    };
}

std::vector<std::uint8_t> make_handshake_packet(
    jam2::protocol::PacketType type,
    const jam2::SessionInfo& session,
    std::uint32_t sequence,
    const Options& options)
{
    const auto payload = encode_stream_config(options);
    const jam2::protocol::Header header{
        type,
        0,
        session.session_id,
        sequence,
        0,
        jam2::monotonic_us(),
        0,
        0,
    };
    return jam2::protocol::encode_packet(header, payload, session.key);
}

StreamConfig decode_handshake_config(
    std::span<const std::uint8_t> bytes,
    const jam2::protocol::Header& header)
{
    return decode_stream_config(std::span<const std::uint8_t>(
        bytes.data() + jam2::protocol::kHeaderSize,
        header.payload_length));
}

void require_matching_stream_config(const StreamConfig& remote, const Options& options)
{
    if (remote.sample_rate != static_cast<std::uint32_t>(options.sample_rate) ||
        remote.frame_size != static_cast<std::uint32_t>(options.frame_size)) {
        std::ostringstream message;
        message << "peer stream config mismatch: local sample_rate=" << options.sample_rate
                << " frame_size=" << options.frame_size
                << ", remote sample_rate=" << remote.sample_rate
                << " frame_size=" << remote.frame_size;
        throw std::runtime_error(message.str());
    }
}

struct UdpParseStats {
    std::uint64_t short_packet = 0;
    std::uint64_t wrong_magic = 0;
    std::uint64_t wrong_version = 0;
    std::uint64_t unknown_type = 0;
    std::uint64_t invalid_flags = 0;
    std::uint64_t invalid_reserved = 0;
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
        case jam2::protocol::ParseError::InvalidFlags: ++invalid_flags; break;
        case jam2::protocol::ParseError::InvalidReserved: ++invalid_reserved; break;
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
        invalid_flags += other.invalid_flags;
        invalid_reserved += other.invalid_reserved;
        wrong_session += other.wrong_session;
        invalid_payload_size += other.invalid_payload_size;
        authentication_failed += other.authentication_failed;
    }

    std::uint64_t total() const
    {
        return short_packet + wrong_magic + wrong_version + unknown_type + invalid_flags +
            invalid_reserved + wrong_session + invalid_payload_size + authentication_failed;
    }
};

void print_udp_parse_stats(const UdpParseStats& stats, std::ostream& out = std::cout)
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

struct OutstandingPing {
    std::uint32_t sequence = 0;
    std::uint64_t send_time_us = 0;
    bool active = false;
};

struct RuntimeState {
    mutable std::mutex recording_mutex;
    mutable std::mutex track_take_mutex;
    mutable std::mutex transport_mutex;
    std::atomic<bool> quit{false};
    std::atomic<bool> stats_enabled{false};
    std::atomic<bool> print_stats{false};
    std::atomic<bool> print_status{false};
    std::atomic<bool> metronome{false};
    std::atomic<int> bpm{120};
    std::atomic<int> metronome_beats_per_bar{4};
    std::atomic<int> metronome_division{1};
    std::atomic<int> metronome_step_count{4};
    std::atomic<std::uint64_t> metronome_play_mask_low{0x0fULL};
    std::atomic<std::uint64_t> metronome_play_mask_high{0};
    std::atomic<std::uint64_t> metronome_accent_mask_low{0x01ULL};
    std::atomic<std::uint64_t> metronome_accent_mask_high{0};
    std::atomic<int> metronome_level_ppm{1000000};
    std::atomic<int> remote_level_ppm{1000000};
    std::atomic<int> send_level_ppm{1000000};
    std::atomic<bool> local_monitor{false};
    std::atomic<int> local_monitor_level_ppm{250000};
    std::atomic<int> metronome_mode{0};
    std::atomic<bool> leader_audio_local_click{false};
    std::atomic<bool> metronome_local_authority{false};
    std::atomic<std::uint64_t> metronome_epoch_sample_time{0};
    std::atomic<bool> metronome_epoch_valid{false};
    std::atomic<std::int64_t> metronome_render_offset_frames{0};
    std::atomic<std::uint64_t> metronome_revision{0};
    std::atomic<std::uint64_t> metronome_epoch_revision{0};
    // Changed only by local controls. Network-applied grid state deliberately
    // does not touch this counter, preventing authority update feedback loops.
    std::atomic<std::uint64_t> grid_request_sequence{0};
    std::atomic<std::uint64_t> transport_revision{0};
    std::atomic<std::uint64_t> transport_network_revision{0};
    std::atomic<std::uint64_t> transport_network_target_raw_frame{0};
    std::atomic<int> transport_network_action{0};
    std::atomic<std::uint64_t> transport_target_raw_frame{0};
    std::atomic<std::uint64_t> transport_target_musical_frame{0};
    std::atomic<std::uint64_t> transport_countdown_start_frame{0};
    std::atomic<int> transport_action{0};
    std::atomic<bool> transport_pending{false};
};

void request_grid_revision(RuntimeState& state) noexcept
{
    state.metronome_revision.fetch_add(1, std::memory_order_relaxed);
    state.grid_request_sequence.fetch_add(1, std::memory_order_release);
}

int ppm_from_gain(double value)
{
    return static_cast<int>(std::clamp(value, 0.0, 4.0) * 1000000.0);
}

int ratio_to_ppm(double ratio)
{
    return static_cast<int>(std::clamp(ratio, 0.5, 2.0) * 1000000.0);
}

double unit_from_ppm(int value)
{
    return static_cast<double>(std::clamp(value, 0, 1000000)) / 1000000.0;
}

double gain_from_ppm(int value)
{
    return static_cast<double>(std::clamp(value, 0, 4000000)) / 1000000.0;
}

int pcm24_peak_ppm(std::span<const std::int32_t> samples)
{
    std::int32_t peak = 0;
    for (std::int32_t sample : samples) {
        peak = std::max<std::int32_t>(peak, std::abs(sample));
    }
    const double normalized = static_cast<double>(peak) / 8388607.0;
    return static_cast<int>(std::clamp(normalized, 0.0, 1.0) * 1000000.0);
}

void update_peak(std::atomic<int>& peak, int candidate)
{
    int current = peak.load(std::memory_order_relaxed);
    while (candidate > current &&
           !peak.compare_exchange_weak(current, candidate, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

void apply_send_level(std::span<std::int32_t> frames, int level_ppm)
{
    if (level_ppm == 1000000) {
        return;
    }
    const double level = gain_from_ppm(level_ppm);
    for (std::int32_t& sample : frames) {
        const double scaled = static_cast<double>(sample) * level;
        sample = static_cast<std::int32_t>(std::clamp(scaled, -8388608.0, 8388607.0));
    }
}

jam2::metronome::PatternSnapshot metronome_pattern_from_runtime(const RuntimeState& runtime)
{
    return jam2::metronome::sanitize({
        runtime.bpm.load(std::memory_order_relaxed),
        runtime.metronome_beats_per_bar.load(std::memory_order_relaxed),
        runtime.metronome_division.load(std::memory_order_relaxed),
        runtime.metronome_step_count.load(std::memory_order_relaxed),
        runtime.metronome_play_mask_low.load(std::memory_order_relaxed),
        runtime.metronome_play_mask_high.load(std::memory_order_relaxed),
        runtime.metronome_accent_mask_low.load(std::memory_order_relaxed),
        runtime.metronome_accent_mask_high.load(std::memory_order_relaxed),
    });
}

void store_metronome_pattern(RuntimeState& runtime, const jam2::metronome::PatternSnapshot& pattern)
{
    const jam2::metronome::PatternSnapshot sanitized = jam2::metronome::sanitize(pattern);
    runtime.bpm.store(sanitized.bpm, std::memory_order_relaxed);
    runtime.metronome_beats_per_bar.store(sanitized.beats_per_bar, std::memory_order_relaxed);
    runtime.metronome_division.store(sanitized.division, std::memory_order_relaxed);
    runtime.metronome_step_count.store(sanitized.step_count, std::memory_order_relaxed);
    runtime.metronome_play_mask_low.store(sanitized.play_mask_low, std::memory_order_relaxed);
    runtime.metronome_play_mask_high.store(sanitized.play_mask_high, std::memory_order_relaxed);
    runtime.metronome_accent_mask_low.store(sanitized.accent_mask_low, std::memory_order_relaxed);
    runtime.metronome_accent_mask_high.store(sanitized.accent_mask_high, std::memory_order_relaxed);
}

bool parse_u64(std::string_view token, std::uint64_t& value)
{
    if (token.empty()) {
        return false;
    }
    std::size_t consumed = 0;
    try {
        value = std::stoull(std::string(token), &consumed, 0);
    } catch (const std::exception&) {
        return false;
    }
    return consumed == token.size();
}

void sync_audio_control(
    const RuntimeState& runtime,
    jam2::audio::StreamControl* control,
    double playback_ratio)
{
    if (control == nullptr) {
        return;
    }
    control->metronome_enabled.store(runtime.metronome.load(std::memory_order_relaxed), std::memory_order_relaxed);
    control->metronome_bpm.store(runtime.bpm.load(std::memory_order_relaxed), std::memory_order_relaxed);
    control->metronome_beats_per_bar.store(runtime.metronome_beats_per_bar.load(std::memory_order_relaxed), std::memory_order_relaxed);
    control->metronome_division.store(runtime.metronome_division.load(std::memory_order_relaxed), std::memory_order_relaxed);
    control->metronome_step_count.store(runtime.metronome_step_count.load(std::memory_order_relaxed), std::memory_order_relaxed);
    control->metronome_play_mask_low.store(runtime.metronome_play_mask_low.load(std::memory_order_relaxed), std::memory_order_relaxed);
    control->metronome_play_mask_high.store(runtime.metronome_play_mask_high.load(std::memory_order_relaxed), std::memory_order_relaxed);
    control->metronome_accent_mask_low.store(runtime.metronome_accent_mask_low.load(std::memory_order_relaxed), std::memory_order_relaxed);
    control->metronome_accent_mask_high.store(runtime.metronome_accent_mask_high.load(std::memory_order_relaxed), std::memory_order_relaxed);
    control->metronome_level_ppm.store(runtime.metronome_level_ppm.load(std::memory_order_relaxed), std::memory_order_relaxed);
    control->remote_level_ppm.store(runtime.remote_level_ppm.load(std::memory_order_relaxed), std::memory_order_relaxed);
    control->send_level_ppm.store(runtime.send_level_ppm.load(std::memory_order_relaxed), std::memory_order_relaxed);
    control->local_monitor_enabled.store(runtime.local_monitor.load(std::memory_order_relaxed), std::memory_order_relaxed);
    control->local_monitor_level_ppm.store(runtime.local_monitor_level_ppm.load(std::memory_order_relaxed), std::memory_order_relaxed);
    control->playback_ratio_ppm.store(ratio_to_ppm(playback_ratio), std::memory_order_relaxed);
    control->metronome_mode.store(runtime.metronome_mode.load(std::memory_order_relaxed), std::memory_order_relaxed);
    control->leader_audio_local_click.store(runtime.leader_audio_local_click.load(std::memory_order_relaxed), std::memory_order_relaxed);
    control->metronome_epoch_sample_time.store(runtime.metronome_epoch_sample_time.load(std::memory_order_relaxed), std::memory_order_relaxed);
    control->metronome_epoch_valid.store(runtime.metronome_epoch_valid.load(std::memory_order_relaxed), std::memory_order_relaxed);
    control->metronome_render_offset_frames.store(runtime.metronome_render_offset_frames.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

void set_recording_latency_adjustment(
    jam2::audio::StreamControl* control,
    std::int64_t adjustment_frames)
{
    if (control == nullptr) {
        return;
    }
    const std::int64_t clamped_adjustment = std::clamp<std::int64_t>(adjustment_frames, -1000000, 1000000);
    const std::int64_t reported =
        static_cast<std::int64_t>(control->input_latency_frames.load(std::memory_order_relaxed)) +
        static_cast<std::int64_t>(control->output_latency_frames.load(std::memory_order_relaxed));
    control->recording_latency_adjustment_frames.store(clamped_adjustment, std::memory_order_relaxed);
    control->recording_latency_compensation_frames.store(
        static_cast<std::uint64_t>(std::max<std::int64_t>(0, reported + clamped_adjustment)),
        std::memory_order_relaxed);
}

void print_interactive_help()
{
    std::cout << "Commands:\n"
              << "  help          show this command list\n";
}

void print_interactive_help(bool stats_enabled)
{
    std::cout << "Commands:\n"
              << "  help          show this command list\n";
    if (stats_enabled) {
        std::cout << "  stats         print current stream stats\n";
    }
    std::cout << "  stats on|off        enable or disable interactive stats output\n"
              << "  status              print compact stream status\n"
              << "  metro on            enable local metronome\n"
              << "  metro off           disable local metronome\n"
              << "  metro mode <mode>   set shared-grid|leader-audio|listener-compensated\n"
              << "  metro leader on|off set whether this side supplies leader-audio click\n"
              << "  metro level <0..4>  set local metronome gain\n"
              << "  metro level +/-n    adjust local metronome level\n"
              << "  metro pattern <bpm> <beats> <division> <play_lo> <play_hi> <accent_lo> <accent_hi>\n"
              << "  metro mute          set local metronome level to 0\n"
              << "  metro unmute        restore local metronome level to 1\n"
              << "  remote level <0..4> set peer playback gain\n"
              << "  remote level +/-n   adjust peer playback level\n"
              << "  remote mute         set peer playback level to 0\n"
              << "  remote unmute       restore peer playback level to 1\n"
              << "  send level <0..4>   set local send level, 1 is unity\n"
              << "  send level +/-n     adjust local send level\n"
              << "  monitor on|off      enable or disable local input monitoring\n"
              << "  monitor level <0..4> set local monitor gain\n"
              << "  monitor level +/-n  adjust local monitor level\n"
              << "  track load <wav>    load mono PCM16 prepared mix at the active sample rate\n"
              << "  track play [frame]  play loaded prepared mix now or at engine frame\n"
              << "  track restart       restart loaded mix at the next shared bar and publish transport\n"
              << "  track stop [frame]  stop loaded prepared mix now or at engine frame\n"
              << "  track level <0..4>  set prepared mix gain\n"
              << "  track loop on|off [start end] loop prepared mix, optionally by source-frame range\n"
              << "  record jam start <folder>  start dry jam stem recording\n"
              << "  record jam stop            stop dry jam stem recording\n"
              << "  record jam status          print dry jam recording status\n"
              << "  bpm <1..400>        set metronome tempo\n"
              << "  quit                stop the stream and exit\n"
              << "  exit                stop the stream and exit\n";
}

void print_prompt()
{
    std::cout << "> ";
    std::cout.flush();
}

bool apply_gain_token(std::string_view token, std::atomic<int>& target_ppm, double max_value)
{
    if (token.empty()) {
        return false;
    }
    std::size_t consumed = 0;
    const std::string text{token};
    double value = 0.0;
    try {
        value = std::stod(text, &consumed);
    } catch (const std::exception&) {
        return false;
    }
    if (consumed != text.size()) {
        return false;
    }
    int ppm = 0;
    if (text[0] == '+' || text[0] == '-') {
        ppm = target_ppm.load(std::memory_order_relaxed) + static_cast<int>(value * 1000000.0);
    } else {
        if (value < 0.0 || value > max_value) {
            return false;
        }
        ppm = static_cast<int>(value * 1000000.0);
    }
    target_ppm.store(std::clamp(ppm, 0, static_cast<int>(max_value * 1000000.0)), std::memory_order_relaxed);
    return true;
}

struct PreparedLoadResult {
    bool ok = false;
    std::uint64_t frames = 0;
    std::string error;
};

PreparedLoadResult load_prepared_pcm16_wav(
    jam2::audio::PreparedTrackSource& source,
    const std::filesystem::path& path,
    int expected_sample_rate)
{
    const jam2::wav::InspectResult inspected = jam2::wav::inspect_pcm16_file(path);
    if (!inspected) {
        return {false, 0, "prepared track load failed: " + inspected.error};
    }
    if (inspected.info.channels != 1U) {
        return {false, 0, "prepared track load failed: WAV must be mono PCM16"};
    }
    if (inspected.info.sample_rate != static_cast<std::uint32_t>(expected_sample_rate)) {
        std::ostringstream message;
        message << "prepared track load failed: WAV sample rate " << inspected.info.sample_rate
                << " does not match active engine rate " << expected_sample_rate;
        return {false, 0, message.str()};
    }
    const std::uint64_t frames = inspected.info.frames;
    const std::uint64_t max_frames = static_cast<std::uint64_t>(std::max(1, expected_sample_rate)) * 60ULL * 5ULL;
    if (frames > max_frames) {
        return {false, 0, "prepared track load failed: WAV exceeds five-minute source limit"};
    }
    const int slot = source.claimLoadingSlot();
    if (slot < 0) {
        return {false, 0, "prepared track load failed: no free source slot"};
    }
    std::int16_t* destination = source.loadingData(slot);
    if (destination == nullptr) {
        return {false, 0, "prepared track load failed: source slot unavailable"};
    }
    std::ifstream file(path, std::ios::binary);
    if (!file || inspected.info.data_offset > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
        return {false, 0, "prepared track load failed: cannot reopen WAV data"};
    }
    file.seekg(static_cast<std::streamoff>(inspected.info.data_offset), std::ios::beg);
    std::array<unsigned char, 8192> input{};
    std::uint64_t frame = 0;
    while (frame < frames) {
        const std::uint64_t frames_to_read = std::min<std::uint64_t>(frames - frame, input.size() / 2U);
        const std::streamsize bytes_to_read = static_cast<std::streamsize>(frames_to_read * 2U);
        file.read(reinterpret_cast<char*>(input.data()), bytes_to_read);
        if (file.gcount() != bytes_to_read) {
            return {false, 0, "prepared track load failed: truncated WAV data"};
        }
        for (std::uint64_t index = 0; index < frames_to_read; ++index) {
            const std::size_t byte = static_cast<std::size_t>(index * 2U);
            const std::uint16_t sample = static_cast<std::uint16_t>(input[byte]) |
                static_cast<std::uint16_t>(static_cast<std::uint16_t>(input[byte + 1]) << 8U);
            destination[frame + index] = static_cast<std::int16_t>(sample);
        }
        frame += frames_to_read;
    }
    if (!source.publishReady(slot, frames, expected_sample_rate)) {
        return {false, 0, "prepared track load failed: source slot publish failed"};
    }
    const std::uint64_t replacement_frame = source.playing()
        ? std::min(source.sourceFrame(), frames)
        : 0ULL;
    if (!source.enqueue({jam2::audio::PreparedTrackSource::CommandType::Swap, static_cast<std::uint32_t>(slot), 0, replacement_frame, 0, 1000000})) {
        return {false, 0, "prepared track load failed: command queue full"};
    }
    return {true, frames, {}};
}

bool enqueue_prepared_command(
    jam2::audio::PreparedTrackSource* source,
    jam2::audio::StreamControl* control,
    const jam2::audio::PreparedTrackSource::Command& command)
{
    if (source == nullptr) {
        return false;
    }
    if (!source->enqueue(command)) {
        if (control != nullptr) {
            control->prepared_source_busy_events.fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    }
    return true;
}

bool enqueue_prepared_restart(
    jam2::audio::PreparedTrackSource* source,
    jam2::audio::StreamControl* control,
    std::uint64_t target_frame)
{
    const bool seek_ok = enqueue_prepared_command(
        source,
        control,
        {jam2::audio::PreparedTrackSource::CommandType::Seek, 0, target_frame, 0, 0, 1000000});
    const bool play_ok = enqueue_prepared_command(
        source,
        control,
        {jam2::audio::PreparedTrackSource::CommandType::Play, 0, target_frame, 0, 0, 1000000});
    return seek_ok && play_ok;
}

void write_recording_sidecar(
    const std::filesystem::path& folder,
    const jam2::audio::OutputRecorderStats& stats,
    const Options& options,
    const RuntimeState& state);
void print_recording_status(const jam2::audio::OutputRecorder* recorder);

std::uint64_t current_engine_frame(const jam2::audio::StreamControl* audio_control)
{
    return audio_control != nullptr ?
        audio_control->engine_frame_counter.load(std::memory_order_relaxed) :
        0ULL;
}

void begin_metronome_epoch(
    RuntimeState& state,
    const jam2::audio::StreamControl* audio_control,
    int sample_rate)
{
    const std::uint64_t lead_frames = static_cast<std::uint64_t>(std::max(1, sample_rate)) / 5ULL;
    state.metronome_render_offset_frames.store(0, std::memory_order_relaxed);
    state.metronome_epoch_sample_time.store(
        current_engine_frame(audio_control) + lead_frames,
        std::memory_order_relaxed);
    state.metronome_epoch_valid.store(true, std::memory_order_relaxed);
}

void hold_shared_grid_at_start(
    RuntimeState& state,
    jam2::audio::StreamControl* audio_control)
{
    if (state.metronome_mode.load(std::memory_order_relaxed) !=
        metronome_mode_id(MetronomeMode::SharedGrid)) {
        return;
    }
    state.metronome_epoch_sample_time.store(0, std::memory_order_relaxed);
    state.metronome_epoch_valid.store(false, std::memory_order_relaxed);
    state.metronome_render_offset_frames.store(0, std::memory_order_relaxed);
    if (audio_control != nullptr) {
        audio_control->metronome_epoch_sample_time.store(0, std::memory_order_relaxed);
        audio_control->metronome_epoch_valid.store(false, std::memory_order_relaxed);
        audio_control->metronome_render_offset_frames.store(0, std::memory_order_relaxed);
    }
}

struct QuantizedSchedule {
    std::uint64_t countdown_start_raw_frame = 0;
    std::uint64_t target_raw_frame = 0;
    std::uint64_t target_musical_frame = 0;
};

std::uint64_t musical_frame_from_raw(std::uint64_t raw_frame, std::int64_t offset)
{
    if (offset < 0) {
        const std::uint64_t magnitude = static_cast<std::uint64_t>(-offset);
        return raw_frame > magnitude ? raw_frame - magnitude : 0ULL;
    }
    return raw_frame + static_cast<std::uint64_t>(offset);
}

std::uint64_t raw_frame_from_musical(std::uint64_t musical_frame, std::int64_t offset)
{
    if (offset >= 0) {
        const std::uint64_t magnitude = static_cast<std::uint64_t>(offset);
        return musical_frame > magnitude ? musical_frame - magnitude : 0ULL;
    }
    return musical_frame + static_cast<std::uint64_t>(-offset);
}

QuantizedSchedule next_bar_schedule(
    const RuntimeState& state,
    const jam2::audio::StreamControl* audio_control,
    int sample_rate,
    int count_in_bars)
{
    const auto pattern = metronome_pattern_from_runtime(state);
    const std::uint64_t step_frames = jam2::metronome::step_interval_samples(
        static_cast<double>(std::max(1, sample_rate)), pattern.bpm, pattern.division);
    const std::uint64_t bar_frames = std::max<std::uint64_t>(
        1,
        step_frames * static_cast<std::uint64_t>(pattern.division) *
            static_cast<std::uint64_t>(pattern.beats_per_bar));
    const std::uint64_t raw_now = current_engine_frame(audio_control);
    const std::int64_t offset = state.metronome_render_offset_frames.load(std::memory_order_relaxed);
    const std::uint64_t musical_now = musical_frame_from_raw(raw_now, offset);
    const std::uint64_t epoch = state.metronome_epoch_valid.load(std::memory_order_relaxed)
        ? state.metronome_epoch_sample_time.load(std::memory_order_relaxed)
        : 0ULL;
    const std::uint64_t elapsed = musical_now >= epoch ? musical_now - epoch : 0ULL;
    std::uint64_t next_bar_musical = epoch + (elapsed / bar_frames + 1ULL) * bar_frames;
    const std::uint64_t minimum_lead_frames = static_cast<std::uint64_t>(std::max(1, sample_rate)) / 5ULL;
    if (raw_frame_from_musical(next_bar_musical, offset) <= raw_now + minimum_lead_frames) {
        next_bar_musical += bar_frames;
    }
    const std::uint64_t target_musical = next_bar_musical +
        static_cast<std::uint64_t>(std::max(0, count_in_bars)) * bar_frames;
    return {
        raw_frame_from_musical(next_bar_musical, offset),
        raw_frame_from_musical(target_musical, offset),
        target_musical,
    };
}

std::uint64_t publish_transport_schedule(
    RuntimeState& state,
    jam2::gui_control::TransportAction action,
    const QuantizedSchedule& schedule,
    bool share_with_peer)
{
    std::lock_guard<std::mutex> lock(state.transport_mutex);
    const std::uint64_t revision = state.transport_revision.load(std::memory_order_relaxed) + 1ULL;
    state.transport_target_raw_frame.store(schedule.target_raw_frame, std::memory_order_relaxed);
    state.transport_target_musical_frame.store(schedule.target_musical_frame, std::memory_order_relaxed);
    state.transport_countdown_start_frame.store(schedule.countdown_start_raw_frame, std::memory_order_relaxed);
    state.transport_action.store(static_cast<int>(action), std::memory_order_relaxed);
    state.transport_pending.store(true, std::memory_order_release);
    if (share_with_peer) {
        state.transport_network_target_raw_frame.store(schedule.target_raw_frame, std::memory_order_relaxed);
        state.transport_network_action.store(static_cast<int>(action), std::memory_order_relaxed);
        state.transport_network_revision.store(revision, std::memory_order_release);
    }
    state.transport_revision.store(revision, std::memory_order_release);
    return revision;
}

void commit_due_transport(RuntimeState& state, const jam2::audio::StreamControl* audio_control)
{
    std::lock_guard<std::mutex> lock(state.transport_mutex);
    if (!state.transport_pending.load(std::memory_order_acquire) ||
        current_engine_frame(audio_control) < state.transport_target_raw_frame.load(std::memory_order_relaxed)) {
        return;
    }
    state.transport_pending.store(false, std::memory_order_release);
    const int action = state.transport_action.load(std::memory_order_relaxed);
    if (action == static_cast<int>(jam2::gui_control::TransportAction::TrackRestart) ||
        action == static_cast<int>(jam2::gui_control::TransportAction::RecordStart)) {
        state.metronome_epoch_sample_time.store(
            state.transport_target_musical_frame.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        state.metronome_epoch_valid.store(true, std::memory_order_relaxed);
        state.metronome_revision.fetch_add(1, std::memory_order_relaxed);
    }
}

bool parse_frame_or_now(std::string_view text, const jam2::audio::StreamControl* audio_control, std::uint64_t& out)
{
    if (text == "now" || text.empty()) {
        out = current_engine_frame(audio_control);
        return true;
    }
    return parse_u64(text, out);
}

void apply_gui_control_line(
    RuntimeState& state,
    jam2::audio::OutputRecorder* recorder,
    jam2::audio::PreparedTrackSource* prepared_source,
    jam2::audio::TrackTakeRecorder* track_take_recorder,
    jam2::audio::StreamControl* audio_control,
    int recording_sample_rate,
    const Options& options,
    std::string_view line)
{
    std::istringstream in{std::string(line)};
    std::string command;
    in >> command;
    if (command == "quit" || command == "exit") {
        state.quit.store(true, std::memory_order_relaxed);
        return;
    }
    if (command == "status") {
        state.print_status.store(true, std::memory_order_relaxed);
        return;
    }
    if (command == "metro") {
        std::string value;
        in >> value;
        if (value == "on") {
            if (!state.metronome.exchange(true, std::memory_order_relaxed)) {
                begin_metronome_epoch(state, audio_control, recording_sample_rate);
            }
            request_grid_revision(state);
        } else if (value == "off") {
            state.metronome.store(false, std::memory_order_relaxed);
            request_grid_revision(state);
        } else if (value == "mode") {
            std::string mode;
            in >> mode;
            const int next_mode = metronome_mode_id(parse_metronome_mode(mode));
            const int previous_mode = state.metronome_mode.exchange(next_mode, std::memory_order_relaxed);
            if (previous_mode != next_mode) {
                state.metronome_render_offset_frames.store(0, std::memory_order_relaxed);
                request_grid_revision(state);
                state.metronome_epoch_revision.fetch_add(1, std::memory_order_relaxed);
            }
        } else if (value == "level") {
            std::string level;
            in >> level;
            (void)apply_gain_token(level, state.metronome_level_ppm, 4.0);
        } else if (value == "leader") {
            std::string leader;
            in >> leader;
            if (leader == "on") {
                state.leader_audio_local_click.store(true, std::memory_order_relaxed);
                state.metronome_local_authority.store(true, std::memory_order_relaxed);
            } else if (leader == "off") {
                state.leader_audio_local_click.store(false, std::memory_order_relaxed);
                state.metronome_local_authority.store(false, std::memory_order_relaxed);
            }
        } else if (value == "pattern") {
            int bpm = 0;
            int beats = 0;
            int division = 0;
            std::string play_low_text;
            std::string play_high_text;
            std::string accent_low_text;
            std::string accent_high_text;
            in >> bpm >> beats >> division >> play_low_text >> play_high_text >> accent_low_text >> accent_high_text;
            std::uint64_t play_low = 0;
            std::uint64_t play_high = 0;
            std::uint64_t accent_low = 0;
            std::uint64_t accent_high = 0;
            if (bpm > 0 &&
                bpm <= 400 &&
                beats > 0 &&
                division > 0 &&
                parse_u64(play_low_text, play_low) &&
                parse_u64(play_high_text, play_high) &&
                parse_u64(accent_low_text, accent_low) &&
                parse_u64(accent_high_text, accent_high)) {
                const auto previous_pattern = metronome_pattern_from_runtime(state);
                auto pattern = metronome_pattern_from_runtime(state);
                pattern.bpm = bpm;
                pattern.beats_per_bar = beats;
                pattern.division = division;
                pattern.step_count = jam2::metronome::pattern_step_count(beats, division);
                pattern.play_mask_low = play_low;
                pattern.play_mask_high = play_high;
                pattern.accent_mask_low = accent_low;
                pattern.accent_mask_high = accent_high;
                store_metronome_pattern(state, pattern);
                request_grid_revision(state);
                if (previous_pattern.bpm != pattern.bpm ||
                    previous_pattern.beats_per_bar != pattern.beats_per_bar ||
                    previous_pattern.division != pattern.division) {
                    state.metronome_epoch_revision.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        return;
    }
    if (command == "remote") {
        std::string value;
        in >> value;
        if (value == "level") {
            std::string level;
            in >> level;
            (void)apply_gain_token(level, state.remote_level_ppm, 4.0);
        } else if (value == "mute") {
            state.remote_level_ppm.store(0, std::memory_order_relaxed);
        } else if (value == "unmute") {
            state.remote_level_ppm.store(1000000, std::memory_order_relaxed);
        }
        return;
    }
    if (command == "send") {
        std::string value;
        in >> value;
        if (value == "level") {
            std::string level;
            in >> level;
            (void)apply_gain_token(level, state.send_level_ppm, 4.0);
        } else if (value == "unity") {
            state.send_level_ppm.store(1000000, std::memory_order_relaxed);
        }
        return;
    }
    if (command == "monitor") {
        std::string value;
        in >> value;
        if (value == "on") {
            state.local_monitor.store(true, std::memory_order_relaxed);
        } else if (value == "off") {
            state.local_monitor.store(false, std::memory_order_relaxed);
        } else if (value == "level") {
            std::string level;
            in >> level;
            (void)apply_gain_token(level, state.local_monitor_level_ppm, 4.0);
        }
        return;
    }
    if (command == "bpm") {
        int value = 0;
        in >> value;
        if (value > 0 && value <= 400) {
            state.bpm.store(value, std::memory_order_relaxed);
            request_grid_revision(state);
            state.metronome_epoch_revision.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    if (command == "record") {
        std::string target;
        std::string action;
        in >> target >> action;
        if (target != "jam" || recorder == nullptr) {
            return;
        }
        if (action == "start") {
            std::string folder;
            std::getline(in >> std::ws, folder);
            if (!folder.empty()) {
                std::lock_guard<std::mutex> lock(state.recording_mutex);
                std::string error;
                (void)recorder->start(std::filesystem::path(folder), recording_sample_rate, error);
            }
        } else if (action == "stop") {
            std::lock_guard<std::mutex> lock(state.recording_mutex);
            const auto before = recorder->stats();
            std::string error;
            (void)recorder->stop(error);
            const auto after = recorder->stats();
            if (!before.folder.empty()) {
                write_recording_sidecar(std::filesystem::path(before.folder), after, options, state);
            }
        } else if (action == "status") {
            state.print_status.store(true, std::memory_order_relaxed);
        }
        return;
    }
    if (command == "track") {
        std::string action;
        in >> action;
        if (action == "load" && prepared_source != nullptr) {
            std::string path;
            std::getline(in >> std::ws, path);
            if (!path.empty()) {
                const PreparedLoadResult result = load_prepared_pcm16_wav(*prepared_source, std::filesystem::path(path), recording_sample_rate);
                if (result.ok) {
                    enqueue_prepared_command(
                        prepared_source,
                        audio_control,
                        {jam2::audio::PreparedTrackSource::CommandType::SetLoop, 0, 0, 0, result.frames, 1000000});
                } else {
                    std::cerr << result.error << "\n";
                }
            }
        } else if (action == "play") {
            std::string target_text;
            in >> target_text;
            std::uint64_t target_frame = 0;
            if (parse_frame_or_now(target_text, audio_control, target_frame)) {
                enqueue_prepared_command(
                    prepared_source,
                    audio_control,
                    {jam2::audio::PreparedTrackSource::CommandType::Play, 0, target_frame, 0, 0, 1000000});
            }
        } else if (action == "restart" && prepared_source != nullptr && audio_control != nullptr) {
            const QuantizedSchedule schedule = next_bar_schedule(
                state,
                audio_control,
                recording_sample_rate,
                0);
            if (enqueue_prepared_restart(prepared_source, audio_control, schedule.target_raw_frame)) {
                publish_transport_schedule(
                    state,
                    jam2::gui_control::TransportAction::TrackRestart,
                    schedule,
                    true);
            }
        } else if (action == "stop") {
            std::string target_text;
            in >> target_text;
            std::uint64_t target_frame = 0;
            if (parse_frame_or_now(target_text, audio_control, target_frame)) {
                enqueue_prepared_command(
                    prepared_source,
                    audio_control,
                    {jam2::audio::PreparedTrackSource::CommandType::Stop, 0, target_frame, 0, 0, 1000000});
            }
        } else if (action == "seek") {
            std::string frame_text;
            in >> frame_text;
            std::uint64_t frame = 0;
            if (parse_frame_or_now(frame_text, audio_control, frame)) {
                std::string target_text;
                in >> target_text;
                std::uint64_t target_frame = 0;
                if (!parse_frame_or_now(target_text, audio_control, target_frame)) {
                    return;
                }
                enqueue_prepared_command(
                    prepared_source,
                    audio_control,
                    {jam2::audio::PreparedTrackSource::CommandType::Seek, 0, target_frame, frame, 0, 1000000});
            }
        } else if (action == "level") {
            std::string level;
            in >> level;
            std::atomic<int> level_ppm{1000000};
            if (apply_gain_token(level, level_ppm, 4.0)) {
                enqueue_prepared_command(
                    prepared_source,
                    audio_control,
                    {jam2::audio::PreparedTrackSource::CommandType::SetLevel, 0, 0, 0, 0, level_ppm.load(std::memory_order_relaxed)});
            }
        } else if (action == "loop") {
            std::string enabled;
            in >> enabled;
            std::uint64_t start = 0;
            std::uint64_t end = 0;
            if (enabled == "on") {
                std::string start_text;
                std::string end_text;
                in >> start_text >> end_text;
                if (!start_text.empty() && !parse_u64(start_text, start)) {
                    return;
                }
                if (!end_text.empty() && !parse_u64(end_text, end)) {
                    return;
                }
                if (end <= start) {
                    end = std::numeric_limits<std::uint64_t>::max();
                }
            }
            enqueue_prepared_command(
                prepared_source,
                audio_control,
                {jam2::audio::PreparedTrackSource::CommandType::SetLoop, 0, 0, start, end, 1000000});
        }
        return;
    }
    if (command == "track_take") {
        std::string action;
        in >> action;
        if (track_take_recorder == nullptr) {
            return;
        }
        if (action == "arm") {
            std::string mode;
            std::string take_id;
            in >> mode >> take_id;
            std::string output;
            std::getline(in >> std::ws, output);
            if (mode == "input" && !take_id.empty() && !output.empty()) {
                std::lock_guard<std::mutex> lock(state.track_take_mutex);
                std::string error;
                (void)track_take_recorder->arm(
                    take_id,
                    std::filesystem::path(output),
                    recording_sample_rate,
                    error);
            }
        } else if (action == "start") {
            std::string take_id;
            std::string frame_text;
            in >> take_id >> frame_text;
            std::uint64_t frame = 0;
            if (parse_frame_or_now(frame_text, audio_control, frame)) {
                std::lock_guard<std::mutex> lock(state.track_take_mutex);
                std::string error;
                (void)track_take_recorder->start_at(frame, error);
            }
        } else if (action == "stop") {
            std::string take_id;
            std::string frame_text;
            in >> take_id >> frame_text;
            std::uint64_t frame = 0;
            if (parse_frame_or_now(frame_text, audio_control, frame)) {
                std::lock_guard<std::mutex> lock(state.track_take_mutex);
                std::string error;
                (void)track_take_recorder->stop_at(frame, error);
            }
        } else if (action == "cancel") {
            std::lock_guard<std::mutex> lock(state.track_take_mutex);
            track_take_recorder->cancel();
        }
        return;
    }
}

void stdin_command_loop(
    RuntimeState& state,
    jam2::audio::OutputRecorder* recorder,
    jam2::audio::PreparedTrackSource* prepared_source,
    jam2::audio::StreamControl* audio_control,
    int recording_sample_rate,
    const Options& options)
{
    std::string line;
    print_prompt();
    while (!state.quit.load(std::memory_order_relaxed) && std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string command;
        in >> command;
        if (command == "quit" || command == "exit") {
            state.quit.store(true, std::memory_order_relaxed);
            break;
        }
        if (command == "help") {
            print_interactive_help(state.stats_enabled.load(std::memory_order_relaxed));
            print_prompt();
            continue;
        }
        if (command == "stats") {
            std::string value;
            in >> value;
            if (value == "on") {
                state.stats_enabled.store(true, std::memory_order_relaxed);
            } else if (value == "off") {
                state.stats_enabled.store(false, std::memory_order_relaxed);
            } else if (value.empty() && state.stats_enabled.load(std::memory_order_relaxed)) {
                state.print_stats.store(true, std::memory_order_relaxed);
            } else {
                std::cout << "Stats are disabled. Use `stats on` to enable interactive stats output.\n";
            }
            print_prompt();
            continue;
        }
        if (command == "status") {
            state.print_status.store(true, std::memory_order_relaxed);
            print_prompt();
            continue;
        }
        if (command == "metro") {
            std::string value;
            in >> value;
            if (value == "on") {
                if (!state.metronome.exchange(true, std::memory_order_relaxed)) {
                    begin_metronome_epoch(state, audio_control, recording_sample_rate);
                }
                request_grid_revision(state);
            } else if (value == "off") {
                state.metronome.store(false, std::memory_order_relaxed);
                request_grid_revision(state);
            } else if (value == "mode") {
                std::string mode;
                in >> mode;
                try {
                    const int next_mode = metronome_mode_id(parse_metronome_mode(mode));
                    const int previous_mode = state.metronome_mode.exchange(next_mode, std::memory_order_relaxed);
                    if (previous_mode != next_mode) {
                        state.metronome_render_offset_frames.store(0, std::memory_order_relaxed);
                        request_grid_revision(state);
                        state.metronome_epoch_revision.fetch_add(1, std::memory_order_relaxed);
                    }
                } catch (const std::exception& error) {
                    std::cerr << error.what() << "\n";
                }
            } else if (value == "level") {
                std::string level;
                in >> level;
                if (!apply_gain_token(level, state.metronome_level_ppm, 4.0)) {
                    std::cerr << "metro level must be 0..4 or a relative +/- adjustment\n";
                }
            } else if (value == "leader") {
                std::string leader;
                in >> leader;
                if (leader == "on") {
                    state.leader_audio_local_click.store(true, std::memory_order_relaxed);
                    state.metronome_local_authority.store(true, std::memory_order_relaxed);
                } else if (leader == "off") {
                    state.leader_audio_local_click.store(false, std::memory_order_relaxed);
                    state.metronome_local_authority.store(false, std::memory_order_relaxed);
                } else {
                    std::cerr << "metro leader must be on or off\n";
                }
            } else if (value == "pattern") {
                int bpm = 0;
                int beats = 0;
                int division = 0;
                std::string play_low_text;
                std::string play_high_text;
                std::string accent_low_text;
                std::string accent_high_text;
                in >> bpm >> beats >> division >> play_low_text >> play_high_text >> accent_low_text >> accent_high_text;
                std::uint64_t play_low = 0;
                std::uint64_t play_high = 0;
                std::uint64_t accent_low = 0;
                std::uint64_t accent_high = 0;
                if (bpm <= 0 ||
                    bpm > 400 ||
                    beats <= 0 ||
                    division <= 0 ||
                    !parse_u64(play_low_text, play_low) ||
                    !parse_u64(play_high_text, play_high) ||
                    !parse_u64(accent_low_text, accent_low) ||
                    !parse_u64(accent_high_text, accent_high)) {
                    std::cerr << "metro pattern must be: bpm beats division play_low play_high accent_low accent_high\n";
                } else {
                    const auto previous_pattern = metronome_pattern_from_runtime(state);
                    auto pattern = metronome_pattern_from_runtime(state);
                    pattern.bpm = bpm;
                    pattern.beats_per_bar = beats;
                    pattern.division = division;
                    pattern.step_count = jam2::metronome::pattern_step_count(beats, division);
                    pattern.play_mask_low = play_low;
                    pattern.play_mask_high = play_high;
                    pattern.accent_mask_low = accent_low;
                    pattern.accent_mask_high = accent_high;
                    store_metronome_pattern(state, pattern);
                    request_grid_revision(state);
                    if (previous_pattern.bpm != pattern.bpm ||
                        previous_pattern.beats_per_bar != pattern.beats_per_bar ||
                        previous_pattern.division != pattern.division) {
                        state.metronome_epoch_revision.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            } else if (value == "mute") {
                state.metronome_level_ppm.store(0, std::memory_order_relaxed);
            } else if (value == "unmute") {
                state.metronome_level_ppm.store(1000000, std::memory_order_relaxed);
            } else {
                std::cerr << "unknown metro command; use: metro on|off|mode|level|pattern|mute|unmute\n";
            }
            print_prompt();
            continue;
        }
        if (command == "remote") {
            std::string value;
            in >> value;
            if (value == "level") {
                std::string level;
                in >> level;
                if (!apply_gain_token(level, state.remote_level_ppm, 4.0)) {
                    std::cerr << "remote level must be 0..4 or a relative +/- adjustment\n";
                }
            } else if (value == "mute") {
                state.remote_level_ppm.store(0, std::memory_order_relaxed);
            } else if (value == "unmute") {
                state.remote_level_ppm.store(1000000, std::memory_order_relaxed);
            } else {
                std::cerr << "unknown remote command; use: remote level|mute|unmute\n";
            }
            print_prompt();
            continue;
        }
        if (command == "send") {
            std::string value;
            in >> value;
            if (value == "level") {
                std::string level;
                in >> level;
                if (!apply_gain_token(level, state.send_level_ppm, 4.0)) {
                    std::cerr << "send level must be 0..4 or a relative +/- adjustment\n";
                }
            } else if (value == "unity") {
                state.send_level_ppm.store(1000000, std::memory_order_relaxed);
            } else {
                std::cerr << "unknown send command; use: send level|unity\n";
            }
            print_prompt();
            continue;
        }
        if (command == "monitor") {
            std::string value;
            in >> value;
            if (value == "on") {
                state.local_monitor.store(true, std::memory_order_relaxed);
            } else if (value == "off") {
                state.local_monitor.store(false, std::memory_order_relaxed);
            } else if (value == "level") {
                std::string level;
                in >> level;
                if (!apply_gain_token(level, state.local_monitor_level_ppm, 4.0)) {
                    std::cerr << "monitor level must be 0..4 or a relative +/- adjustment\n";
                }
            } else {
                std::cerr << "unknown monitor command; use: monitor on|off|level\n";
            }
            print_prompt();
            continue;
        }
        if (command == "record") {
            std::string target;
            std::string action;
            in >> target >> action;
            if (target != "jam" || (action != "start" && action != "stop" && action != "status")) {
                std::cerr << "unknown record command; use: record jam start|stop|status\n";
                print_prompt();
                continue;
            }
            if (recorder == nullptr) {
                std::cerr << "record jam requires an active audio device\n";
                print_prompt();
                continue;
            }
            if (action == "start") {
                std::string folder;
                std::getline(in >> std::ws, folder);
                if (folder.empty()) {
                    std::cerr << "record jam start requires a folder\n";
                } else {
                    std::lock_guard<std::mutex> lock(state.recording_mutex);
                    std::string error;
                    if (recorder->start(std::filesystem::path(folder), recording_sample_rate, error)) {
                        std::cout << "Recording jam: " << folder << "\n";
                    } else {
                        std::cerr << "record jam start failed: " << error << "\n";
                    }
                }
            } else if (action == "stop") {
                std::lock_guard<std::mutex> lock(state.recording_mutex);
                const auto before = recorder->stats();
                std::string error;
                const bool ok = recorder->stop(error);
                const auto after = recorder->stats();
                if (!before.folder.empty()) {
                    write_recording_sidecar(std::filesystem::path(before.folder), after, options, state);
                }
                if (ok) {
                    std::cout << "Recording jam stopped: " << after.folder << "\n";
                } else {
                    std::cerr << "record jam stop failed: " << error << "\n";
                }
            } else {
                print_recording_status(recorder);
            }
            print_prompt();
            continue;
        }
        if (command == "track") {
            apply_gui_control_line(state, recorder, prepared_source, nullptr, audio_control, recording_sample_rate, options, line);
            print_prompt();
            continue;
        }
        if (command == "bpm") {
            int value = 0;
            in >> value;
            if (value > 0 && value <= 400) {
                state.bpm.store(value, std::memory_order_relaxed);
                request_grid_revision(state);
                state.metronome_epoch_revision.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::cerr << "bpm must be 1..400\n";
            }
            print_prompt();
            continue;
        }
        if (!command.empty()) {
            std::cerr << "unknown command: " << command << " (use: help)\n";
        }
        print_prompt();
    }
}

void write_u64_le(std::span<std::uint8_t> payload, std::size_t offset, std::uint64_t value)
{
    for (int i = 0; i < 8; ++i) {
        payload[offset + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xffU);
    }
}

std::uint64_t read_u64_le(std::span<const std::uint8_t> payload, std::size_t offset)
{
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | payload[offset + static_cast<std::size_t>(i)];
    }
    return value;
}

enum class GridMessageKind : std::uint8_t {
    LegacyState = 0,
    Proposal = 1,
    Assignment = 2,
    AuthorityState = 3,
};

constexpr std::uint8_t kGridMessageMarker = 0x80;

std::array<std::uint8_t, 56> encode_metronome_payload(
    int bpm,
    std::uint64_t revision_or_request,
    std::uint64_t epoch_sample_time,
    jam2::metronome::PatternSnapshot pattern,
    GridMessageKind kind,
    std::uint8_t mode,
    jam2::GridRunState run_state)
{
    pattern = jam2::metronome::sanitize(pattern);
    std::array<std::uint8_t, 56> payload{};
    payload[0] = static_cast<std::uint8_t>(bpm & 0xff);
    payload[1] = static_cast<std::uint8_t>((bpm >> 8) & 0xff);
    payload[2] = static_cast<std::uint8_t>((bpm >> 16) & 0xff);
    payload[3] = static_cast<std::uint8_t>((bpm >> 24) & 0xff);
    write_u64_le(std::span<std::uint8_t>(payload), 4, revision_or_request);
    write_u64_le(std::span<std::uint8_t>(payload), 12, epoch_sample_time);
    payload[20] = static_cast<std::uint8_t>(
        kGridMessageMarker |
        (static_cast<std::uint8_t>(kind) & 0x03U) |
        ((mode & 0x03U) << 2U) |
        ((static_cast<std::uint8_t>(run_state) & 0x03U) << 4U));
    payload[21] = static_cast<std::uint8_t>(pattern.beats_per_bar);
    payload[22] = static_cast<std::uint8_t>(pattern.division);
    payload[23] = static_cast<std::uint8_t>(pattern.step_count);
    write_u64_le(std::span<std::uint8_t>(payload), 24, pattern.play_mask_low);
    write_u64_le(std::span<std::uint8_t>(payload), 32, pattern.play_mask_high);
    write_u64_le(std::span<std::uint8_t>(payload), 40, pattern.accent_mask_low);
    write_u64_le(std::span<std::uint8_t>(payload), 48, pattern.accent_mask_high);
    return payload;
}

struct MetronomePayload {
    int bpm = 120;
    std::uint64_t revision_or_request = 0;
    std::uint64_t epoch_sample_time = 0;
    jam2::metronome::PatternSnapshot pattern;
    bool has_pattern = false;
    GridMessageKind kind = GridMessageKind::LegacyState;
    std::uint8_t mode = 0;
    jam2::GridRunState run_state = jam2::GridRunState::Stopped;
};

MetronomePayload decode_metronome_payload(std::span<const std::uint8_t> payload)
{
    if (payload.size() != 12 && payload.size() != 20 && payload.size() != 56) {
        throw std::runtime_error("metronome payload size mismatch");
    }
    const int bpm = static_cast<int>(payload[0]) |
        (static_cast<int>(payload[1]) << 8) |
        (static_cast<int>(payload[2]) << 16) |
        (static_cast<int>(payload[3]) << 24);
    const std::uint64_t revision_or_request = read_u64_le(payload, 4);
    std::uint64_t epoch = 0;
    if (payload.size() == 20) {
        epoch = read_u64_le(payload, 12);
    }
    MetronomePayload decoded;
    decoded.bpm = bpm;
    decoded.revision_or_request = revision_or_request;
    decoded.epoch_sample_time = epoch;
    if (payload.size() == 56) {
        decoded.epoch_sample_time = read_u64_le(payload, 12);
        decoded.pattern = jam2::metronome::sanitize({
            std::abs(bpm),
            static_cast<int>(payload[21]),
            static_cast<int>(payload[22]),
            static_cast<int>(payload[23]),
            read_u64_le(payload, 24),
            read_u64_le(payload, 32),
            read_u64_le(payload, 40),
            read_u64_le(payload, 48),
        });
        const std::uint8_t control = payload[20];
        decoded.has_pattern = control == 1 || (control & kGridMessageMarker) != 0;
        if ((control & kGridMessageMarker) != 0) {
            decoded.kind = static_cast<GridMessageKind>(control & 0x03U);
            decoded.mode = static_cast<std::uint8_t>((control >> 2U) & 0x03U);
            decoded.run_state = static_cast<jam2::GridRunState>((control >> 4U) & 0x03U);
            if (decoded.kind == GridMessageKind::LegacyState || decoded.mode > 2 ||
                static_cast<std::uint8_t>(decoded.run_state) >
                    static_cast<std::uint8_t>(jam2::GridRunState::AuthorityMissing)) {
                throw std::runtime_error("invalid grid authority message");
            }
        } else {
            decoded.run_state = bpm > 0
                ? jam2::GridRunState::Running
                : jam2::GridRunState::Stopped;
        }
    }
    return decoded;
}

struct TransportPayload {
    jam2::gui_control::TransportAction action = jam2::gui_control::TransportAction::None;
    std::uint32_t event_counter = 0;
    std::uint32_t grid_revision = 0;
    std::uint64_t target_sender_frame = 0;
};

std::array<std::uint8_t, 20> encode_transport_payload(const TransportPayload& value)
{
    std::array<std::uint8_t, 20> payload{};
    payload[0] = 1;
    payload[1] = static_cast<std::uint8_t>(value.action);
    const std::uint64_t identity =
        (static_cast<std::uint64_t>(value.grid_revision) << 32U) |
        static_cast<std::uint64_t>(value.event_counter);
    write_u64_le(std::span<std::uint8_t>(payload), 4, identity);
    write_u64_le(std::span<std::uint8_t>(payload), 12, value.target_sender_frame);
    return payload;
}

TransportPayload decode_transport_payload(std::span<const std::uint8_t> payload)
{
    if (payload.size() != 20 || payload[0] != 1) {
        throw std::runtime_error("transport payload size or version mismatch");
    }
    const std::uint64_t identity = read_u64_le(payload, 4);
    return {
        static_cast<jam2::gui_control::TransportAction>(payload[1]),
        static_cast<std::uint32_t>(identity & 0xffffffffULL),
        static_cast<std::uint32_t>(identity >> 32U),
        read_u64_le(payload, 12),
    };
}

void observe_timing(
    std::uint64_t value,
    std::uint64_t& min_value,
    std::uint64_t& sum_value,
    std::uint64_t& max_value)
{
    if (min_value == 0 || value < min_value) {
        min_value = value;
    }
    if (value > max_value) {
        max_value = value;
    }
    sum_value += value;
}

double frames_to_ms(std::size_t frames, double sample_rate)
{
    return sample_rate > 0.0 ? (static_cast<double>(frames) * 1000.0 / sample_rate) : 0.0;
}

std::int64_t ms_to_signed_frames(double ms, double sample_rate)
{
    return sample_rate > 0.0 ? static_cast<std::int64_t>(std::llround(ms * sample_rate / 1000.0)) : 0;
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

std::string os_error_text(unsigned long code)
{
    if (code == 0) {
        return {};
    }
    return "error " + std::to_string(code);
}

#if defined(_WIN32)
std::string win_priority_class_text(DWORD value)
{
    switch (value) {
    case IDLE_PRIORITY_CLASS: return "idle";
    case BELOW_NORMAL_PRIORITY_CLASS: return "below-normal";
    case NORMAL_PRIORITY_CLASS: return "normal";
    case ABOVE_NORMAL_PRIORITY_CLASS: return "above-normal";
    case HIGH_PRIORITY_CLASS: return "high";
    case REALTIME_PRIORITY_CLASS: return "realtime";
    default: return "unknown-" + std::to_string(value);
    }
}

std::string win_thread_priority_text(int value)
{
    switch (value) {
    case THREAD_PRIORITY_IDLE: return "idle";
    case THREAD_PRIORITY_LOWEST: return "lowest";
    case THREAD_PRIORITY_BELOW_NORMAL: return "below-normal";
    case THREAD_PRIORITY_NORMAL: return "normal";
    case THREAD_PRIORITY_ABOVE_NORMAL: return "above-normal";
    case THREAD_PRIORITY_HIGHEST: return "highest";
    case THREAD_PRIORITY_TIME_CRITICAL: return "time-critical";
    case THREAD_PRIORITY_ERROR_RETURN: return "error";
    default: return std::to_string(value);
    }
}
#endif

#if defined(__APPLE__)
std::string mac_qos_text(qos_class_t value)
{
    switch (value) {
    case QOS_CLASS_USER_INTERACTIVE: return "user-interactive";
    case QOS_CLASS_USER_INITIATED: return "user-initiated";
    case QOS_CLASS_DEFAULT: return "default";
    case QOS_CLASS_UTILITY: return "utility";
    case QOS_CLASS_BACKGROUND: return "background";
    case QOS_CLASS_UNSPECIFIED: return "unspecified";
    default: return std::to_string(static_cast<int>(value));
    }
}

std::uint64_t ns_to_mach_absolute(std::uint64_t ns)
{
    mach_timebase_info_data_t info{};
    if (mach_timebase_info(&info) != KERN_SUCCESS || info.numer == 0) {
        return ns;
    }
    return (ns * static_cast<std::uint64_t>(info.denom)) / static_cast<std::uint64_t>(info.numer);
}
#endif

class OsPriorityScope {
public:
    explicit OsPriorityScope(const Options& options)
    {
        status_.requested = options.os_priority;
        status_.platform = platform_name();
        status_.cpu_count = std::thread::hardware_concurrency();
        apply(options);
    }

    ~OsPriorityScope()
    {
#if defined(_WIN32)
        if (mmcss_handle_ != nullptr) {
            (void)AvRevertMmThreadCharacteristics(mmcss_handle_);
        }
        if (timer_resolution_active_) {
            (void)timeEndPeriod(1);
        }
#endif
    }

    OsPriorityScope(const OsPriorityScope&) = delete;
    OsPriorityScope& operator=(const OsPriorityScope&) = delete;

    const OsSchedulingStatus& status() const { return status_; }

private:
    void apply(const Options& options)
    {
#if defined(_WIN32)
        apply_windows(options);
#elif defined(__APPLE__)
        apply_macos(options);
#else
        (void)options;
        status_.process_priority = "unsupported";
        status_.thread_priority = "unsupported";
#endif
    }

#if defined(_WIN32)
    void apply_windows(const Options& options)
    {
        status_.process_priority = win_priority_class_text(GetPriorityClass(GetCurrentProcess()));
        status_.thread_priority = win_thread_priority_text(GetThreadPriority(GetCurrentThread()));
        status_.mmcss_requested = "off";
        status_.mmcss_active = "off";
        status_.timer_resolution_requested = "off";
        status_.timer_resolution_active = "off";
        if (options.os_priority == OsPriorityMode::Off) {
            return;
        }

        const DWORD priority_class = options.os_priority == OsPriorityMode::Realtime ?
            REALTIME_PRIORITY_CLASS :
            HIGH_PRIORITY_CLASS;
        if (!SetPriorityClass(GetCurrentProcess(), priority_class)) {
            status_.process_priority = "request-failed:" + os_error_text(GetLastError());
        } else {
            status_.process_priority = win_priority_class_text(GetPriorityClass(GetCurrentProcess()));
        }

        const int thread_priority = options.os_priority == OsPriorityMode::Realtime ?
            THREAD_PRIORITY_TIME_CRITICAL :
            THREAD_PRIORITY_HIGHEST;
        if (!SetThreadPriority(GetCurrentThread(), thread_priority)) {
            status_.thread_priority = "request-failed:" + os_error_text(GetLastError());
        } else {
            status_.thread_priority = win_thread_priority_text(GetThreadPriority(GetCurrentThread()));
        }

        status_.timer_resolution_requested = "1ms";
        const MMRESULT timer_result = timeBeginPeriod(1);
        if (timer_result == TIMERR_NOERROR) {
            timer_resolution_active_ = true;
            status_.timer_resolution_active = "1ms";
        } else {
            status_.timer_resolution_active = "off";
            status_.timer_resolution_error = os_error_text(timer_result);
        }

        status_.mmcss_requested = "Pro Audio";
        DWORD task_index = 0;
        mmcss_handle_ = AvSetMmThreadCharacteristicsA("Pro Audio", &task_index);
        if (mmcss_handle_ == nullptr) {
            const DWORD pro_audio_error = GetLastError();
            task_index = 0;
            mmcss_handle_ = AvSetMmThreadCharacteristicsA("Audio", &task_index);
            if (mmcss_handle_ != nullptr) {
                status_.mmcss_active = "on";
                status_.mmcss_profile = "Audio";
                status_.mmcss_error = "Pro Audio failed: " + os_error_text(pro_audio_error);
            } else {
                status_.mmcss_active = "off";
                status_.mmcss_error = "Pro Audio failed: " + os_error_text(pro_audio_error) +
                    "; Audio failed: " + os_error_text(GetLastError());
            }
        } else {
            status_.mmcss_active = "on";
            status_.mmcss_profile = "Pro Audio";
        }
    }
#endif

#if defined(__APPLE__)
    void apply_macos(const Options& options)
    {
        status_.process_priority = "unchanged";
        status_.qos_requested = "off";
        status_.qos_active = "off";
        status_.realtime_requested = "off";
        status_.realtime_active = "off";
        qos_class_t active_qos = QOS_CLASS_UNSPECIFIED;
        int relative_priority = 0;
        if (pthread_get_qos_class_np(pthread_self(), &active_qos, &relative_priority) != 0) {
            active_qos = QOS_CLASS_UNSPECIFIED;
        }
        status_.thread_priority = mac_qos_text(active_qos);
        if (options.os_priority == OsPriorityMode::Off) {
            return;
        }
        if (options.os_priority == OsPriorityMode::High) {
            status_.qos_requested = "user-interactive";
            const int result = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
            if (result != 0) {
                status_.qos_error = "error " + std::to_string(result);
            }
            if (pthread_get_qos_class_np(pthread_self(), &active_qos, &relative_priority) != 0) {
                active_qos = QOS_CLASS_UNSPECIFIED;
            }
            status_.qos_active = mac_qos_text(active_qos);
            status_.thread_priority = status_.qos_active;
            return;
        }

        status_.realtime_requested = "thread-time-constraint";
        const std::uint64_t packet_period_ns =
            options.sample_rate > 0 && options.frame_size > 0 ?
                (static_cast<std::uint64_t>(options.frame_size) * 1000000000ULL) /
                    static_cast<std::uint64_t>(options.sample_rate) :
                1000000ULL;
        const std::uint64_t period = ns_to_mach_absolute(packet_period_ns);
        thread_time_constraint_policy_data_t policy{};
        policy.period = static_cast<std::uint32_t>(std::max<std::uint64_t>(period, 1));
        policy.computation = static_cast<std::uint32_t>(std::max<std::uint64_t>(period / 4, 1));
        policy.constraint = static_cast<std::uint32_t>(std::max<std::uint64_t>(period / 2, policy.computation + 1));
        policy.preemptible = TRUE;
        const kern_return_t result = thread_policy_set(
            pthread_mach_thread_np(pthread_self()),
            THREAD_TIME_CONSTRAINT_POLICY,
            reinterpret_cast<thread_policy_t>(&policy),
            THREAD_TIME_CONSTRAINT_POLICY_COUNT);
        if (result == KERN_SUCCESS) {
            status_.realtime_active = "on";
            status_.thread_priority = "thread-time-constraint";
        } else {
            status_.realtime_active = "off";
            status_.realtime_error = mach_error_string(result);
        }
    }
#endif

    OsSchedulingStatus status_;
#if defined(_WIN32)
    HANDLE mmcss_handle_ = nullptr;
    bool timer_resolution_active_ = false;
#endif
};

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

std::string shell_quote(std::string_view value)
{
    std::string out = "\"";
    for (const char c : value) {
        if (c == '"') {
            out += "\\\"";
        } else {
            out.push_back(c);
        }
    }
    out += "\"";
    return out;
}

std::string bool_on_off(bool value)
{
    return value ? "on" : "off";
}

std::string channel_option_text(const std::vector<int>& channels)
{
    std::ostringstream out;
    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << (channels[i] + 1);
    }
    return out.str();
}

std::string input_channels_option_text(const Options& options)
{
    return channel_option_text(options.channel_selection.input);
}

std::string output_channels_option_text(const Options& options)
{
    return channel_option_text(options.channel_selection.output);
}

std::string make_headless_client_command(std::string_view executable, const std::string& connection_url, const Options& options)
{
    std::ostringstream out;
    out << executable << " connect " << shell_quote(connection_url)
        << " --audio-device <replace-client-device-id>"
        << " --profile " << options.profile_name
        << " --sample-rate " << options.sample_rate
        << " --audio-buffer-size " << options.audio_buffer_size
        << " --frame-size " << options.frame_size
        << " --playback-prefill-frames " << options.playback_prefill_frames
        << " --playback-ring-frames " << options.playback_ring_frames
        << " --playback-max-frames " << options.playback_max_frames
        << " --capture-ring-frames " << options.capture_ring_frames
        << " --input-channels " << input_channels_option_text(options)
        << " --output-channels " << output_channels_option_text(options)
        << " --stats " << (options.stats_enabled ? "enabled" : "disabled")
        << " --stats-warmup-ms " << options.stats_warmup_ms
        << " --stats-interval-ms " << options.stats_interval_ms
        << " --os-priority " << os_priority_text(options.os_priority)
        << " --metronome " << bool_on_off(options.metronome)
        << " --bpm " << options.bpm
        << " --metronome-level " << options.metronome_level
        << " --remote-level " << options.remote_level
        << " --send-level " << options.send_level
        << " --local-monitor " << bool_on_off(options.local_monitor)
        << " --local-monitor-level " << options.local_monitor_level
        << " --metronome-mode " << metronome_mode_text(options.metronome_mode)
        << " --metronome-compensation-max-ms " << options.metronome_compensation_max_ms
        << " --metronome-compensation-smoothing-ms " << options.metronome_compensation_smoothing_ms
        << " --metronome-compensation-deadband-ms " << options.metronome_compensation_deadband_ms
        << " --metronome-compensation-slew-ms-per-sec " << options.metronome_compensation_slew_ms_per_sec
        << " --sample-time-playout " << bool_on_off(options.sample_time_playout)
        << " --playout-delay-frames " << options.playout_delay_frames
        << " --jitter-buffer-frames " << options.jitter_buffer_frames
        << " --jitter-buffer-max-frames " << options.jitter_buffer_max_frames
        << " --adaptive-playback-cushion " << bool_on_off(options.adaptive_playback_cushion)
        << " --adaptive-playback-target-frames " << options.adaptive_playback_target_frames
        << " --adaptive-playback-min-frames " << options.adaptive_playback_min_frames
        << " --adaptive-playback-max-frames " << options.adaptive_playback_max_frames
        << " --adaptive-playback-release-ppm " << options.adaptive_playback_release_ppm
        << " --drift-correction " << bool_on_off(options.drift_correction)
        << " --drift-smoothing " << options.drift_smoothing
        << " --drift-deadband-ppm " << options.drift_deadband_ppm
        << " --drift-max-correction-ppm " << options.drift_max_correction_ppm
        << " --stream-linger-ms " << options.stream_linger_ms;
    if (options.wait_ms > 0) {
        out << " --wait-ms " << options.wait_ms;
    }
    if (options.stream_ms > 0) {
        out << " --stream-ms " << options.stream_ms;
    }
    if (options.log_stats_dir) {
        out << " --log-stats " << shell_quote(options.log_stats_dir->string());
    }
    if (options.socket_send_buffer) {
        out << " --socket-send-buffer " << *options.socket_send_buffer;
    }
    if (options.socket_recv_buffer) {
        out << " --socket-recv-buffer " << *options.socket_recv_buffer;
    }
    if (options.status_jsonl) {
        out << " --status-format jsonl";
    }
    if (options.machine_readable_startup) {
        out << " --machine-readable-startup on";
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

std::string json_escape(std::string_view value)
{
    std::string out;
    out.reserve(value.size() + 2);
    for (const char c : value) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    return out;
}

void write_recording_sidecar(
    const std::filesystem::path& folder,
    const jam2::audio::OutputRecorderStats& stats,
    const Options& options,
    const RuntimeState& state)
{
    if (folder.empty()) {
        return;
    }
    std::filesystem::create_directories(folder);
    std::ofstream out(folder / "recording.json", std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "record jam sidecar failed: " << (folder / "recording.json").string() << "\n";
        return;
    }
    const auto pattern = metronome_pattern_from_runtime(state);
    out << "{\n"
        << "  \"format\": \"pcm16_mono_wav\",\n"
        << "  \"sample_rate\": " << stats.sample_rate << ",\n"
        << "  \"stems\": [\"mix.wav\", \"my-input.wav\", \"their-input.wav\", \"inputs-mix.wav\", \"metronome.wav\"],\n"
        << "  \"recording_folder\": \"" << json_escape(folder.string()) << "\",\n"
        << "  \"start_audio_frame\": " << stats.start_audio_frame << ",\n"
        << "  \"stop_audio_frame\": " << stats.stop_audio_frame << ",\n"
        << "  \"frames_queued\": " << stats.frames_queued << ",\n"
        << "  \"frames_written\": " << stats.frames_written << ",\n"
        << "  \"dropped_frames\": " << stats.dropped_frames << ",\n"
        << "  \"drop_events\": " << stats.drop_events << ",\n"
        << "  \"writer_errors\": " << stats.writer_errors << ",\n"
        << "  \"queue_capacity_frames\": " << stats.queue_capacity_frames << ",\n"
        << "  \"metronome\": \"" << (state.metronome.load(std::memory_order_relaxed) ? "on" : "off") << "\",\n"
        << "  \"bpm\": " << state.bpm.load(std::memory_order_relaxed) << ",\n"
        << "  \"metronome_level\": " << gain_from_ppm(state.metronome_level_ppm.load(std::memory_order_relaxed)) << ",\n"
        << "  \"remote_level\": " << gain_from_ppm(state.remote_level_ppm.load(std::memory_order_relaxed)) << ",\n"
        << "  \"send_level\": " << gain_from_ppm(state.send_level_ppm.load(std::memory_order_relaxed)) << ",\n"
        << "  \"local_monitor\": \"" << (state.local_monitor.load(std::memory_order_relaxed) ? "on" : "off") << "\",\n"
        << "  \"local_monitor_level\": " << gain_from_ppm(state.local_monitor_level_ppm.load(std::memory_order_relaxed)) << ",\n"
        << "  \"metronome_mode\": \"" << metronome_mode_text(state.metronome_mode.load(std::memory_order_relaxed)) << "\",\n"
        << "  \"test_input\": \"" << test_input_mode_text(options.test_input) << "\",\n"
        << "  \"metronome_epoch_sample_time\": "
        << state.metronome_epoch_sample_time.load(std::memory_order_relaxed) << ",\n"
        << "  \"metronome_epoch_valid\": "
        << (state.metronome_epoch_valid.load(std::memory_order_relaxed) ? "true" : "false") << ",\n"
        << "  \"metronome_beats_per_bar\": " << pattern.beats_per_bar << ",\n"
        << "  \"metronome_division\": " << pattern.division << ",\n"
        << "  \"metronome_step_count\": " << pattern.step_count << ",\n"
        << "  \"metronome_play_mask_low\": " << pattern.play_mask_low << ",\n"
        << "  \"metronome_play_mask_high\": " << pattern.play_mask_high << ",\n"
        << "  \"metronome_accent_mask_low\": " << pattern.accent_mask_low << ",\n"
        << "  \"metronome_accent_mask_high\": " << pattern.accent_mask_high << ",\n"
        << "  \"sample_time_playout\": " << (options.sample_time_playout ? "true" : "false") << ",\n"
        << "  \"playout_delay_frames\": " << options.playout_delay_frames << "\n"
        << "}\n";
}

void print_recording_status(const jam2::audio::OutputRecorder* recorder)
{
    if (recorder == nullptr) {
        std::cout << "recording_active=0 recording_available=0\n";
        return;
    }
    const auto stats = recorder->stats();
    std::cout << "recording_active=" << (stats.active ? 1 : 0)
              << " recording_folder=" << stats.folder
              << " recording_sample_rate=" << stats.sample_rate
              << " recording_frames_written=" << stats.frames_written
              << " recording_dropped_frames=" << stats.dropped_frames
              << " recording_drop_events=" << stats.drop_events
              << " recording_queue_depth_frames=" << stats.queue_depth_frames
              << " recording_queue_capacity_frames=" << stats.queue_capacity_frames
              << " recording_writer_errors=" << stats.writer_errors
              << "\n";
}

void print_startup_json(
    std::string_view mode,
    std::string_view stage,
    const Options& options,
    const jam2::Endpoint& local,
    const std::optional<jam2::Endpoint>& peer,
    std::string_view endpoint_mode,
    std::string_view connection_url,
    std::string_view error = {})
{
    if (!options.machine_readable_startup) {
        return;
    }
    std::cout << "{\"event\":\"startup\""
              << ",\"mode\":\"" << json_escape(mode) << "\""
              << ",\"stage\":\"" << json_escape(stage) << "\""
              << ",\"profile\":\"" << json_escape(options.profile_name) << "\""
              << ",\"local_endpoint\":\"" << json_escape(jam2::endpoint_to_string(local)) << "\"";
    if (peer) {
        std::cout << ",\"peer_endpoint\":\"" << json_escape(jam2::endpoint_to_string(*peer)) << "\"";
    }
    if (!endpoint_mode.empty()) {
        std::cout << ",\"endpoint_mode\":\"" << json_escape(endpoint_mode) << "\"";
    }
    if (!connection_url.empty()) {
        std::cout << ",\"connection_url\":\"" << json_escape(connection_url) << "\"";
    }
    std::cout << ",\"sample_rate\":" << options.sample_rate
              << ",\"frame_size\":" << options.frame_size
              << ",\"audio_device_id\":" << options.audio_device_id.value_or(-1)
              << ",\"audio_buffer_size\":" << options.audio_buffer_size
              << ",\"os_priority\":\"" << os_priority_text(options.os_priority) << "\""
              << ",\"capture_ring_frames\":" << options.capture_ring_frames
              << ",\"playback_ring_frames\":" << options.playback_ring_frames
              << ",\"playback_prefill_frames\":" << options.playback_prefill_frames
              << ",\"playback_max_frames\":" << options.playback_max_frames
              << ",\"input_channels\":\"" << mono_mix_mode_text(options.channel_selection.input.size()) << "\""
              << ",\"channel_selection\":\"" << json_escape(channel_selection_text(options.channel_selection)) << "\""
              << ",\"metronome\":\"" << (options.metronome ? "on" : "off") << "\""
              << ",\"bpm\":" << options.bpm
              << ",\"metronome_level\":" << options.metronome_level
              << ",\"metronome_mode\":\"" << metronome_mode_text(options.metronome_mode) << "\""
              << ",\"remote_level\":" << options.remote_level
              << ",\"send_level\":" << options.send_level
              << ",\"local_monitor\":" << (options.local_monitor ? "true" : "false")
              << ",\"local_monitor_level\":" << options.local_monitor_level
              << ",\"drift_correction\":" << (options.drift_correction ? "true" : "false")
              << ",\"drift_smoothing\":" << options.drift_smoothing
              << ",\"drift_deadband_ppm\":" << options.drift_deadband_ppm
              << ",\"drift_max_correction_ppm\":" << options.drift_max_correction_ppm
              << ",\"sample_time_playout\":" << (options.sample_time_playout ? "true" : "false")
              << ",\"playout_delay_frames\":" << options.playout_delay_frames
              << ",\"jitter_buffer_frames\":" << options.jitter_buffer_frames
              << ",\"jitter_buffer_max_frames\":" << options.jitter_buffer_max_frames
              << ",\"adaptive_playback_cushion\":" << (options.adaptive_playback_cushion ? "true" : "false")
              << ",\"adaptive_playback_target_frames\":" << options.adaptive_playback_target_frames
              << ",\"adaptive_playback_min_frames\":" << options.adaptive_playback_min_frames
              << ",\"adaptive_playback_max_frames\":" << options.adaptive_playback_max_frames
              << ",\"adaptive_playback_release_ppm\":" << options.adaptive_playback_release_ppm;
    if (!error.empty()) {
        std::cout << ",\"error\":\"" << json_escape(error) << "\"";
    }
    std::cout << "}\n";
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
                "udp_short_packets,udp_wrong_magic,udp_wrong_version,udp_unknown_type,udp_invalid_flags,"
                "udp_invalid_reserved,udp_wrong_session,udp_invalid_payload_size,udp_authentication_failed,"
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
                "transport_applied_target_frame\n";
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
             << stats.udp_parse.invalid_flags << ','
             << stats.udp_parse.invalid_reserved << ','
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
             << stats.transport_applied_target_frame;
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
        std::vector<std::string> fields(345);
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
        set(263, stats.udp_parse.invalid_flags);
        set(264, stats.udp_parse.invalid_reserved);
        set(265, stats.udp_parse.wrong_session);
        set(266, stats.udp_parse.invalid_payload_size);
        set(267, stats.udp_parse.authentication_failed);
        set(268, stats.udp_replay_rejects);
        set(269, stats.udp_forward_gap_rejects);
        set(270, stats.udp_forward_gap_resyncs);
        set(271, stats.udp_sequence_ambiguous_rejects);
        set(272, stats.udp_sample_time_stale_rejects);
        set(273, stats.udp_sample_time_future_rejects);
        set(274, stats.udp_unmatched_pongs);
        set(275, stats.udp_ping_slot_overwrites);
        set(276, stats.udp_work_budget_yields);
        set(277, stats.reorder_pending_high_water);
        set(278, stats.reorder_capacity_drops);
        set(279, stats.jitter_pending_high_water);
        set(280, stats.jitter_capacity_drops);
        set(281, stats.jitter_buffer_forced_releases);
        fields[282] = audio.network_capture_enabled ? "yes" : "no";
        fields[283] = audio.network_capture_ready ? "yes" : "no";
        set(284, audio.network_capture_generation);
        set(285, audio.network_capture_epoch_frame);
        set(286, audio.network_capture_stale_frames_discarded);
        fields[287] = audio.network_playback_enabled ? "yes" : "no";
        set(288, stats.local_peer_id);
        set(289, stats.remote_peer_id);
        fields[290] = stats.bootstrap_role;
        set(291, stats.session_protocol_version);
        fields[292] = stats.session_audio_format;
        set(293, stats.session_sample_rate);
        set(294, stats.session_frames_per_packet);
        set(295, stats.network_peer_count);
        set(296, stats.network_active_peer_count);
        set(297, stats.mix_contributing_peers);
        set(298, stats.mix_active_slots);
        set(299, stats.mix_max_slots);
        set(300, stats.mix_active_slots_high_water);
        set(301, stats.mix_released_slots);
        set(302, stats.mix_complete_slots);
        set(303, stats.mix_deadline_slots);
        set(304, stats.mix_missing_peer_contributions);
        set(305, stats.mix_missing_peer_frames);
        set(306, stats.mix_late_after_release_frames);
        set(307, stats.mix_capacity_drops);
        set(308, stats.mix_capacity_dropped_frames);
        set(309, stats.mix_clipped_samples);
        set(310, stats.mix_output_frames);
        set(311, stats.mix_output_drop_requested_frames);
        set(312, stats.mix_output_drop_request_events);
        set(313, stats.mix_output_dropped_frames);
        set(314, stats.mix_work_budget_yields);
        set(315, stats.bootstrap_coordinator_peer_id);
        set(316, stats.arrangement_authority_peer_id);
        set(317, stats.grid_authority_peer_id);
        set(318, stats.grid_revision);
        set(319, stats.grid_run_state);
        set(320, stats.grid_mode);
        set(321, stats.grid_authority_epoch_frame);
        set(322, stats.grid_mapped_epoch_frame);
        set(323, stats.grid_authority_packet_frame);
        set(324, stats.grid_mapping_error_frames);
        set(325, stats.grid_proposals_sent);
        set(326, stats.grid_proposals_accepted);
        set(327, stats.grid_proposals_rejected);
        set(328, stats.grid_assignments_sent);
        set(329, stats.grid_assignments_accepted);
        set(330, stats.grid_assignments_rejected);
        set(331, stats.grid_authority_states_sent);
        set(332, stats.grid_authority_states_accepted);
        set(333, stats.grid_authority_states_rejected);
        set(334, stats.grid_authority_missing_events);
        set(335, stats.transport_source_peer_id);
        set(336, stats.transport_event_counter);
        set(337, stats.transport_grid_revision);
        set(338, stats.transport_events_accepted);
        set(339, stats.transport_events_rejected);
        set(340, stats.leader_audio_source_peer_id);
        set(341, stats.leader_audio_injected_packets);
        set(342, stats.transport_source_frame);
        set(343, stats.transport_requested_target_frame);
        set(344, stats.transport_applied_target_frame);

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

CsvStatsLog::AudioSnapshot make_audio_snapshot(
    const jam2::audio::DeviceStream* stream,
    const jam2::audio::MonoRingBuffer* capture_ring,
    const jam2::audio::MonoRingBuffer* playback_ring,
    jam2::audio::StreamControl* control = nullptr)
{
    CsvStatsLog::AudioSnapshot snapshot;
    if (stream == nullptr) {
        return snapshot;
    }
    snapshot.has_audio = true;
    snapshot.stream = stream->info();
    snapshot.callbacks = stream->callbacks();
    snapshot.callback_timing = stream->callback_timing_stats();
    snapshot.playback_prefilled = stream->playback_prefilled();
    if (capture_ring != nullptr) {
        const auto stats = capture_ring->stats();
        snapshot.capture_ring_overruns = stats.overruns;
        snapshot.capture_ring_overrun_events = stats.overrun_events;
        snapshot.capture_ring_overrun_event_max_frames = stats.overrun_event_max_frames;
        snapshot.capture_ring_underruns = stats.underruns;
        snapshot.capture_ring_underrun_events = stats.underrun_events;
        snapshot.capture_ring_readable = capture_ring->available_read();
    }
    if (playback_ring != nullptr) {
        const auto stats = playback_ring->stats();
        snapshot.playback_ring_overruns = stats.overruns;
        snapshot.playback_ring_underruns = stats.underruns;
        snapshot.playback_ring_underrun_events = stats.underrun_events;
        snapshot.playback_ring_underrun_event_max_frames = stats.underrun_event_max_frames;
        snapshot.playback_ring_underrun_burst_events = stats.underrun_burst_events;
        snapshot.playback_ring_underrun_burst_max_frames = stats.underrun_burst_max_frames;
        snapshot.playback_ring_underrun_burst_current_frames = stats.underrun_burst_current_frames;
        snapshot.playback_depth_under_2ms_frames = stats.depth_under_2ms_frames;
        snapshot.playback_depth_under_5ms_frames = stats.depth_under_5ms_frames;
        snapshot.playback_depth_under_10ms_frames = stats.depth_under_10ms_frames;
        snapshot.playback_depth_10ms_plus_frames = stats.depth_10ms_plus_frames;
        snapshot.playback_depth_observed_frames = stats.depth_observed_frames;
        snapshot.playback_drop_requested_frames = stats.drop_requested_frames;
        snapshot.playback_drop_applied_frames = stats.drop_applied_frames;
        snapshot.playback_drop_coalesced_requests = stats.drop_coalesced_requests;
        snapshot.playback_drop_pending_frames = stats.drop_pending_frames;
        snapshot.playback_drop_max_batch_frames = stats.drop_max_batch_frames;
        snapshot.playback_ring_readable = playback_ring->available_read();
    }
    if (control != nullptr) {
        snapshot.input_peak = unit_from_ppm(control->input_peak_ppm.exchange(0, std::memory_order_relaxed));
        snapshot.send_peak = unit_from_ppm(control->send_peak_ppm.exchange(0, std::memory_order_relaxed));
        snapshot.monitor_peak = unit_from_ppm(control->monitor_peak_ppm.exchange(0, std::memory_order_relaxed));
        snapshot.remote_peak = unit_from_ppm(control->remote_peak_ppm.exchange(0, std::memory_order_relaxed));
        snapshot.metronome_peak = unit_from_ppm(control->metronome_peak_ppm.exchange(0, std::memory_order_relaxed));
        snapshot.output_peak = unit_from_ppm(control->output_peak_ppm.exchange(0, std::memory_order_relaxed));
        snapshot.output_clipped_samples = control->output_clipped_samples.load(std::memory_order_relaxed);
        snapshot.prepared_source_frame = control->prepared_source_frame.load(std::memory_order_relaxed);
        snapshot.prepared_source_scheduled_start_frame =
            control->prepared_source_scheduled_start_frame.load(std::memory_order_relaxed);
        snapshot.prepared_source_actual_start_frame =
            control->prepared_source_actual_start_frame.load(std::memory_order_relaxed);
        snapshot.prepared_source_underruns = control->prepared_source_underruns.load(std::memory_order_relaxed);
        snapshot.prepared_source_busy_events = control->prepared_source_busy_events.load(std::memory_order_relaxed);
        snapshot.network_capture_enabled = control->network_capture_enabled.load(std::memory_order_relaxed);
        snapshot.network_capture_generation =
            control->network_capture_generation_requested.load(std::memory_order_relaxed);
        snapshot.network_capture_ready = snapshot.network_capture_enabled &&
            snapshot.network_capture_generation ==
                control->network_capture_generation_applied.load(std::memory_order_relaxed);
        snapshot.network_capture_epoch_frame =
            control->network_capture_epoch_frame.load(std::memory_order_relaxed);
        snapshot.network_capture_stale_frames_discarded =
            control->network_capture_stale_frames_discarded.load(std::memory_order_relaxed);
        snapshot.network_playback_enabled =
            control->network_playback_enabled.load(std::memory_order_relaxed);
    }
    return snapshot;
}

CsvStatsLog::Context make_csv_context(
    int argc,
    char** argv,
    std::string_view mode,
    const Options& options,
    const jam2::UdpSocket& socket,
    const jam2::Endpoint& local,
    const jam2::Endpoint& peer,
    std::string_view endpoint_mode)
{
    CsvStatsLog::Context context;
    context.command_line = command_line_text(argc, argv);
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

void print_compact_status(
    const AudioPacketStats& stats,
    const Options& options,
    const RuntimeState& runtime,
    const jam2::audio::DeviceStream* audio_stream,
    const jam2::audio::MonoRingBuffer* playback_ring,
    jam2::audio::StreamControl* audio_control,
    const jam2::audio::OutputRecorder* recorder,
    std::uint64_t elapsed_ms)
{
    const std::size_t playback_depth = playback_ring != nullptr ? playback_ring->available_read() : 0;
    const double playback_depth_ms = frames_to_ms(playback_depth, options.sample_rate);
    const double metro_level = gain_from_ppm(runtime.metronome_level_ppm.load(std::memory_order_relaxed));
    const double remote_level = gain_from_ppm(runtime.remote_level_ppm.load(std::memory_order_relaxed));
    const double send_level = gain_from_ppm(runtime.send_level_ppm.load(std::memory_order_relaxed));
    const bool local_monitor = runtime.local_monitor.load(std::memory_order_relaxed);
    const double local_monitor_level = gain_from_ppm(runtime.local_monitor_level_ppm.load(std::memory_order_relaxed));
    const int live_metronome_mode = runtime.metronome_mode.load(std::memory_order_relaxed);
    const bool playback_prefilled = audio_stream != nullptr && audio_stream->playback_prefilled();
    const auto recording_stats = recorder != nullptr ? recorder->stats() : jam2::audio::OutputRecorderStats{};
    const auto audio_snapshot = make_audio_snapshot(audio_stream, nullptr, playback_ring, audio_control);
    const double playback_underrun_time_ms =
        frames_to_ms(static_cast<std::size_t>(audio_snapshot.playback_ring_underruns), options.sample_rate);
    const double playback_underrun_event_max_ms =
        frames_to_ms(static_cast<std::size_t>(audio_snapshot.playback_ring_underrun_event_max_frames), options.sample_rate);
    const double playback_underrun_burst_max_ms =
        frames_to_ms(static_cast<std::size_t>(audio_snapshot.playback_ring_underrun_burst_max_frames), options.sample_rate);
    if (options.status_jsonl) {
        std::cout << "{\"event\":\"status\""
                  << ",\"elapsed_ms\":" << elapsed_ms
                  << ",\"recv_packets\":" << stats.recv_packets
                  << ",\"frame_size\":" << options.frame_size
                  << ",\"metronome\":\"" << (runtime.metronome.load(std::memory_order_relaxed) ? "on" : "off") << "\""
                  << ",\"bpm\":" << runtime.bpm.load(std::memory_order_relaxed)
                  << ",\"metronome_beats_per_bar\":" << runtime.metronome_beats_per_bar.load(std::memory_order_relaxed)
                  << ",\"metronome_division\":" << runtime.metronome_division.load(std::memory_order_relaxed)
                  << ",\"metronome_epoch_sample_frame\":" << runtime.metronome_epoch_sample_time.load(std::memory_order_relaxed)
                  << ",\"bootstrap_coordinator_peer_id\":" << stats.bootstrap_coordinator_peer_id
                  << ",\"arrangement_authority_peer_id\":" << stats.arrangement_authority_peer_id
                  << ",\"grid_authority_peer_id\":" << stats.grid_authority_peer_id
                  << ",\"grid_revision\":" << stats.grid_revision
                  << ",\"grid_run_state\":" << stats.grid_run_state
                  << ",\"grid_mode\":" << stats.grid_mode
                  << ",\"grid_authority_epoch_frame\":" << stats.grid_authority_epoch_frame
                  << ",\"grid_mapped_epoch_frame\":" << stats.grid_mapped_epoch_frame
                  << ",\"grid_mapping_error_frames\":" << stats.grid_mapping_error_frames
                  << ",\"leader_audio_source_peer_id\":" << stats.leader_audio_source_peer_id
                  << ",\"leader_audio_injected_packets\":" << stats.leader_audio_injected_packets
                   << ",\"engine_frame\":"
                   << (audio_control != nullptr ? audio_control->engine_frame_counter.load(std::memory_order_relaxed) : 0ULL)
                   << ",\"network_capture_enabled\":" << (audio_control && audio_control->network_capture_enabled.load(std::memory_order_relaxed) ? "true" : "false")
                   << ",\"network_capture_ready\":"
                   << (audio_control &&
                       audio_control->network_capture_enabled.load(std::memory_order_relaxed) &&
                       audio_control->network_capture_generation_requested.load(std::memory_order_relaxed) ==
                           audio_control->network_capture_generation_applied.load(std::memory_order_relaxed)
                           ? "true" : "false")
                   << ",\"network_capture_generation\":" << (audio_control ? audio_control->network_capture_generation_requested.load(std::memory_order_relaxed) : 0ULL)
                   << ",\"network_capture_epoch_frame\":" << (audio_control ? audio_control->network_capture_epoch_frame.load(std::memory_order_relaxed) : 0ULL)
                   << ",\"network_capture_stale_frames_discarded\":" << (audio_control ? audio_control->network_capture_stale_frames_discarded.load(std::memory_order_relaxed) : 0ULL)
                   << ",\"network_playback_enabled\":" << (audio_control && audio_control->network_playback_enabled.load(std::memory_order_relaxed) ? "true" : "false")
                  << ",\"recording_input_latency_frames\":" << (audio_control ? audio_control->input_latency_frames.load(std::memory_order_relaxed) : 0U)
                  << ",\"recording_output_latency_frames\":" << (audio_control ? audio_control->output_latency_frames.load(std::memory_order_relaxed) : 0U)
                  << ",\"recording_latency_adjustment_frames\":" << (audio_control ? audio_control->recording_latency_adjustment_frames.load(std::memory_order_relaxed) : 0)
                  << ",\"recording_latency_compensation_frames\":" << (audio_control ? audio_control->recording_latency_compensation_frames.load(std::memory_order_relaxed) : 0ULL)
                  << ",\"metronome_render_offset_frames\":" << runtime.metronome_render_offset_frames.load(std::memory_order_relaxed)
                  << ",\"transport_revision\":" << runtime.transport_revision.load(std::memory_order_relaxed)
                  << ",\"transport_action\":" << runtime.transport_action.load(std::memory_order_relaxed)
                  << ",\"transport_target_frame\":" << runtime.transport_target_raw_frame.load(std::memory_order_relaxed)
                  << ",\"transport_countdown_start_frame\":" << runtime.transport_countdown_start_frame.load(std::memory_order_relaxed)
                  << ",\"transport_pending\":" << (runtime.transport_pending.load(std::memory_order_relaxed) ? "true" : "false")
                  << ",\"audio_sample_rate\":" << options.sample_rate
                  << ",\"prepared_source_frame\":" << (audio_control ? audio_control->prepared_source_frame.load(std::memory_order_relaxed) : 0ULL)
                  << ",\"prepared_source_scheduled_start_frame\":" << (audio_control ? audio_control->prepared_source_scheduled_start_frame.load(std::memory_order_relaxed) : 0ULL)
                  << ",\"prepared_source_actual_start_frame\":" << (audio_control ? audio_control->prepared_source_actual_start_frame.load(std::memory_order_relaxed) : 0ULL)
                  << ",\"prepared_source_underruns\":" << (audio_control ? audio_control->prepared_source_underruns.load(std::memory_order_relaxed) : 0ULL)
                  << ",\"prepared_source_busy_events\":" << (audio_control ? audio_control->prepared_source_busy_events.load(std::memory_order_relaxed) : 0ULL)
                  << ",\"metronome_level\":" << metro_level
                  << ",\"remote_level\":" << remote_level
                  << ",\"send_level\":" << send_level
                  << ",\"local_monitor\":" << (local_monitor ? "true" : "false")
                  << ",\"local_monitor_level\":" << local_monitor_level
                  << ",\"metronome_mode\":\"" << metronome_mode_text(live_metronome_mode) << "\""
                  << ",\"sample_time_playout\":" << (options.sample_time_playout ? "true" : "false")
                  << ",\"playout_delay_frames\":" << options.playout_delay_frames
                  << ",\"jitter_buffer\":" << (stats.jitter_buffer_enabled ? "true" : "false")
                  << ",\"jitter_buffer_target_frames\":" << stats.jitter_buffer_target_frames
                  << ",\"jitter_buffer_max_frames\":" << stats.jitter_buffer_max_frames
                  << ",\"jitter_buffer_depth_frames\":" << stats.jitter_buffer_depth_frames
                  << ",\"jitter_buffer_released_packets\":" << stats.jitter_buffer_released_packets
                  << ",\"jitter_buffer_late_packets\":" << stats.jitter_buffer_late_packets
                  << ",\"jitter_buffer_dropped_packets\":" << stats.jitter_buffer_dropped_packets
                  << ",\"metronome_compensation_active\":" << (stats.metronome_compensation_active ? "true" : "false")
                  << ",\"metronome_compensation_offset_frames\":" << stats.metronome_compensation_offset_frames
                  << ",\"metronome_compensation_offset_ms\":"
                  << signed_frames_to_ms(stats.metronome_compensation_offset_frames, options.sample_rate)
                  << ",\"metronome_compensation_target_frames\":" << stats.metronome_compensation_target_frames
                  << ",\"metronome_compensation_target_ms\":"
                  << signed_frames_to_ms(stats.metronome_compensation_target_frames, options.sample_rate)
                  << ",\"recording_active\":" << (recording_stats.active ? "true" : "false")
                  << ",\"recording_folder\":\"" << json_escape(recording_stats.folder) << "\""
                  << ",\"recording_frames_written\":" << recording_stats.frames_written
                  << ",\"recording_dropped_frames\":" << recording_stats.dropped_frames
                  << ",\"recording_queue_depth_frames\":" << recording_stats.queue_depth_frames
                  << ",\"recording_queue_capacity_frames\":" << recording_stats.queue_capacity_frames
                  << ",\"recording_writer_errors\":" << recording_stats.writer_errors
                  << ",\"playback_prefilled\":" << (playback_prefilled ? "true" : "false")
                  << ",\"playback_depth_frames\":" << playback_depth
                  << ",\"playback_depth_ms\":" << playback_depth_ms
                  << ",\"expected_remote_sample_time\":" << stats.expected_remote_sample_time
                  << ",\"last_received_sample_time\":" << stats.last_received_sample_time
                  << ",\"last_played_remote_sample_time\":" << stats.last_played_remote_sample_time
                  << ",\"missing_audio_frames_inserted\":" << stats.missing_audio_frames_inserted
                  << ",\"late_audio_frames_dropped\":" << stats.late_audio_frames_dropped
                  << ",\"jitter_buffer_forced_releases\":" << stats.jitter_buffer_forced_releases
                  << ",\"send_interval_avg_ms\":" << avg_us_to_ms(stats.send_interval_sum_us, stats.send_interval_samples)
                  << ",\"send_schedule_error_max_ms\":"
                  << (stats.send_schedule_error_samples > 0 ? static_cast<double>(stats.send_schedule_error_max_us) / 1000.0 : 0.0)
                  << ",\"receive_loop_gap_max_ms\":"
                  << (stats.receive_loop_gap_samples > 0 ? static_cast<double>(stats.receive_loop_gap_max_us) / 1000.0 : 0.0)
                  << ",\"send_catchup_events\":" << stats.send_catchup_events
                  << ",\"adaptive_playback_cushion\":" << (stats.adaptive_playback_cushion_enabled ? "true" : "false")
                  << ",\"adaptive_playback_target_frames\":" << stats.adaptive_playback_target_frames
                  << ",\"adaptive_playback_raise_events\":" << stats.adaptive_playback_raise_events
                  << ",\"adaptive_playback_release_events\":" << stats.adaptive_playback_release_events
                  << ",\"rtt_avg_ms\":" << rtt_avg_ms(stats)
                  << ",\"jitter_avg_ms\":" << avg_us_to_ms(stats.jitter_sum_us, stats.jitter_samples)
                  << ",\"jitter_max_ms\":"
                  << (stats.jitter_samples > 0 ? static_cast<double>(stats.jitter_max_us) / 1000.0 : 0.0)
                  << ",\"sequence_lost\":" << stats.sequence.lost
                  << ",\"sequence_loss_events\":" << stats.sequence.loss_events
                  << ",\"sequence_loss_max_gap\":" << stats.sequence.loss_max_gap
                  << ",\"sequence_loss_percent\":" << sequence_loss_percent(stats)
                  << ",\"sequence_duplicate\":" << stats.sequence.duplicate
                  << ",\"sequence_out_of_order\":" << stats.sequence.out_of_order
                  << ",\"sequence_late\":" << stats.sequence.late
                  << ",\"reordered_recovered\":" << stats.reordered_recovered
                  << ",\"reordered_lost\":" << stats.reordered_lost
                  << ",\"reordered_lost_events\":" << stats.reordered_lost_events
                  << ",\"reordered_max_distance_packets\":" << stats.reordered_max_distance_packets
                  << ",\"audio_packet_gap_max_ms\":"
                  << (stats.audio_packet_gap_samples > 0 ? static_cast<double>(stats.audio_packet_gap_max_us) / 1000.0 : 0.0)
                  << ",\"audio_packet_gap_over_2x_count\":" << stats.audio_packet_gap_over_2x_count
                  << ",\"audio_packet_gap_over_4x_count\":" << stats.audio_packet_gap_over_4x_count
                  << ",\"audio_packet_gap_samples\":" << stats.audio_packet_gap_samples
                  << ",\"receive_burst_packets_max\":" << stats.receive_burst_packets_max
                  << ",\"playback_ring_readable_ms\":"
                  << frames_to_ms(audio_snapshot.playback_ring_readable, options.sample_rate)
                  << ",\"playback_ring_underruns\":" << audio_snapshot.playback_ring_underruns
                  << ",\"playback_ring_underrun_events\":" << audio_snapshot.playback_ring_underrun_events
                  << ",\"playback_ring_underrun_time_ms\":" << playback_underrun_time_ms
                  << ",\"playback_ring_underrun_event_max_ms\":" << playback_underrun_event_max_ms
                  << ",\"playback_ring_underrun_burst_events\":" << audio_snapshot.playback_ring_underrun_burst_events
                  << ",\"playback_ring_underrun_burst_max_ms\":" << playback_underrun_burst_max_ms
                  << ",\"input_peak\":" << audio_snapshot.input_peak
                  << ",\"send_peak\":" << audio_snapshot.send_peak
                  << ",\"monitor_peak\":" << audio_snapshot.monitor_peak
                  << ",\"remote_peak\":" << audio_snapshot.remote_peak
                  << ",\"metronome_peak\":" << audio_snapshot.metronome_peak
                  << ",\"output_peak\":" << audio_snapshot.output_peak
                  << ",\"output_clipped_samples\":" << audio_snapshot.output_clipped_samples
                  << ",\"drift_ppm\":" << stats.drift_ppm
                  << ",\"resampler_ratio\":" << stats.resampler_ratio
                  << ",\"os_priority_requested\":\"" << os_priority_text(stats.os_scheduling.requested) << "\""
                  << ",\"os_cpu_count\":" << stats.os_scheduling.cpu_count
                  << ",\"os_process_priority_active\":\"" << json_escape(stats.os_scheduling.process_priority) << "\""
                  << ",\"os_thread_priority_active\":\"" << json_escape(stats.os_scheduling.thread_priority) << "\""
                  << ",\"os_mmcss_active\":\"" << json_escape(stats.os_scheduling.mmcss_active) << "\""
                  << ",\"os_mmcss_profile\":\"" << json_escape(stats.os_scheduling.mmcss_profile) << "\""
                  << ",\"os_timer_resolution_active\":\"" << json_escape(stats.os_scheduling.timer_resolution_active) << "\""
                  << ",\"os_qos_active\":\"" << json_escape(stats.os_scheduling.qos_active) << "\""
                  << ",\"os_realtime_active\":\"" << json_escape(stats.os_scheduling.realtime_active) << "\""
                  << "}\n";
        return;
    }
    std::cout << "status elapsed_ms=" << elapsed_ms
              << " recv_packets=" << stats.recv_packets
              << " frame_size=" << options.frame_size
              << " metro=" << (runtime.metronome.load(std::memory_order_relaxed) ? "on" : "off")
              << " bpm=" << runtime.bpm.load(std::memory_order_relaxed)
              << " metro_level=" << metro_level
              << " remote_level=" << remote_level
              << " send_level=" << send_level
              << " local_monitor=" << (local_monitor ? "on" : "off")
              << " local_monitor_level=" << local_monitor_level
              << " metronome_mode=" << metronome_mode_text(live_metronome_mode)
              << " sample_time_playout=" << (options.sample_time_playout ? "on" : "off")
              << " playout_delay_frames=" << options.playout_delay_frames
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
              << " recording_active=" << (recording_stats.active ? "yes" : "no")
              << " recording_folder=" << recording_stats.folder
              << " recording_frames_written=" << recording_stats.frames_written
              << " recording_dropped_frames=" << recording_stats.dropped_frames
              << " recording_queue_depth_frames=" << recording_stats.queue_depth_frames
              << " recording_writer_errors=" << recording_stats.writer_errors
              << " playback_prefilled=" << (playback_prefilled ? "yes" : "no")
              << " playback_depth_ms=" << playback_depth_ms
              << " expected_remote_sample_time=" << stats.expected_remote_sample_time
              << " last_received_sample_time=" << stats.last_received_sample_time
              << " last_played_remote_sample_time=" << stats.last_played_remote_sample_time
              << " missing_audio_frames_inserted=" << stats.missing_audio_frames_inserted
              << " late_audio_frames_dropped=" << stats.late_audio_frames_dropped
              << " send_interval_avg_ms=" << avg_us_to_ms(stats.send_interval_sum_us, stats.send_interval_samples)
              << " send_schedule_error_max_ms="
              << (stats.send_schedule_error_samples > 0 ? static_cast<double>(stats.send_schedule_error_max_us) / 1000.0 : 0.0)
              << " receive_loop_gap_max_ms="
              << (stats.receive_loop_gap_samples > 0 ? static_cast<double>(stats.receive_loop_gap_max_us) / 1000.0 : 0.0)
              << " send_catchup_events=" << stats.send_catchup_events
              << " adaptive_playback_cushion=" << (stats.adaptive_playback_cushion_enabled ? "on" : "off")
              << " adaptive_playback_target_frames=" << stats.adaptive_playback_target_frames
              << " adaptive_playback_raise_events=" << stats.adaptive_playback_raise_events
              << " adaptive_playback_release_events=" << stats.adaptive_playback_release_events
              << " rtt_avg_ms=" << rtt_avg_ms(stats)
              << " jitter_avg_ms=" << avg_us_to_ms(stats.jitter_sum_us, stats.jitter_samples)
              << " jitter_max_ms="
              << (stats.jitter_samples > 0 ? static_cast<double>(stats.jitter_max_us) / 1000.0 : 0.0)
              << " sequence_lost=" << stats.sequence.lost
              << " sequence_loss_events=" << stats.sequence.loss_events
              << " sequence_loss_max_gap=" << stats.sequence.loss_max_gap
              << " sequence_loss_percent=" << sequence_loss_percent(stats)
              << " sequence_duplicate=" << stats.sequence.duplicate
              << " sequence_out_of_order=" << stats.sequence.out_of_order
              << " sequence_late=" << stats.sequence.late
              << " reordered_recovered=" << stats.reordered_recovered
              << " reordered_lost=" << stats.reordered_lost
              << " reordered_lost_events=" << stats.reordered_lost_events
              << " reordered_max_distance_packets=" << stats.reordered_max_distance_packets
              << " audio_packet_gap_max_ms="
              << (stats.audio_packet_gap_samples > 0 ? static_cast<double>(stats.audio_packet_gap_max_us) / 1000.0 : 0.0)
              << " audio_packet_gap_over_2x_count=" << stats.audio_packet_gap_over_2x_count
              << " audio_packet_gap_over_4x_count=" << stats.audio_packet_gap_over_4x_count
              << " audio_packet_gap_samples=" << stats.audio_packet_gap_samples
              << " receive_burst_packets_max=" << stats.receive_burst_packets_max
              << " playback_ring_readable_ms=" << frames_to_ms(audio_snapshot.playback_ring_readable, options.sample_rate)
              << " playback_ring_underruns=" << audio_snapshot.playback_ring_underruns
              << " playback_ring_underrun_events=" << audio_snapshot.playback_ring_underrun_events
              << " playback_ring_underrun_time_ms=" << playback_underrun_time_ms
              << " playback_ring_underrun_event_max_ms=" << playback_underrun_event_max_ms
              << " playback_ring_underrun_burst_events=" << audio_snapshot.playback_ring_underrun_burst_events
              << " playback_ring_underrun_burst_max_ms=" << playback_underrun_burst_max_ms
              << " input_peak=" << audio_snapshot.input_peak
              << " send_peak=" << audio_snapshot.send_peak
              << " monitor_peak=" << audio_snapshot.monitor_peak
              << " remote_peak=" << audio_snapshot.remote_peak
              << " metronome_peak=" << audio_snapshot.metronome_peak
              << " output_peak=" << audio_snapshot.output_peak
              << " output_clipped_samples=" << audio_snapshot.output_clipped_samples
              << " drift_ppm=" << stats.drift_ppm
              << " resampler_ratio=" << stats.resampler_ratio
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

void mix_leader_click_into_packet(
    std::span<std::int32_t> samples,
    std::uint64_t packet_sample_time,
    int sample_rate,
    double level,
    std::uint64_t epoch_sample_time,
    jam2::metronome::PatternSnapshot pattern)
{
    if (sample_rate <= 0 || samples.empty()) {
        return;
    }
    pattern = jam2::metronome::sanitize(pattern);
    const std::uint64_t step_interval =
        jam2::metronome::step_interval_samples(static_cast<double>(sample_rate), pattern.bpm, pattern.division);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const std::uint64_t absolute_sample = packet_sample_time + static_cast<std::uint64_t>(i);
        if (absolute_sample < epoch_sample_time) {
            continue;
        }
        const std::uint64_t grid_sample = absolute_sample - epoch_sample_time;
        samples[i] = jam2::metronome::mix_pcm24(
            samples[i],
            jam2::metronome::render_sample(pattern, grid_sample, step_interval, static_cast<double>(sample_rate), level));
    }
}

class CliPeerStreamPlayback final : public jam2::PeerStreamPlayback {
public:
    explicit CliPeerStreamPlayback(jam2::Engine* engine) noexcept
        : engine_(engine)
    {
    }

    std::size_t depthFrames() const noexcept override
    {
        return engine_ != nullptr ? engine_->networkPlaybackDepth() : 0;
    }

    std::size_t pushFrames(std::span<const std::int32_t> frames) noexcept override
    {
        return engine_ != nullptr ? engine_->pushNetworkPlayback(frames) : 0;
    }

    void requestDropFrames(std::size_t frames) noexcept override
    {
        if (engine_ != nullptr) {
            engine_->requestNetworkPlaybackDrop(frames);
        }
    }

    void setResamplerRatio(double ratio) noexcept override
    {
        if (engine_ != nullptr) {
            engine_->setNetworkPlaybackRatio(ratio);
        }
    }

private:
    jam2::Engine* engine_ = nullptr;
};

jam2::PeerStreamConfig make_peer_stream_config(const Options& options, bool collect_diagnostics)
{
    jam2::PeerStreamConfig config;
    config.sample_rate = options.sample_rate;
    config.frames_per_packet = options.frame_size;
    config.sample_time_playout = options.sample_time_playout;
    config.playout_delay_frames = options.playout_delay_frames;
    config.playback_max_frames = options.playback_max_frames;
    config.playback_queue_capacity_frames = options.playback_ring_frames;
    config.jitter_buffer_frames = options.jitter_buffer_frames;
    config.jitter_buffer_max_frames = options.jitter_buffer_max_frames;
    config.adaptive_playback_cushion = options.adaptive_playback_cushion;
    config.adaptive_playback_target_frames = options.adaptive_playback_target_frames;
    config.adaptive_playback_min_frames = options.adaptive_playback_min_frames;
    config.adaptive_playback_max_frames = options.adaptive_playback_max_frames;
    config.adaptive_playback_release_ppm = options.adaptive_playback_release_ppm;
    config.drift_correction = options.drift_correction;
    config.drift_smoothing = options.drift_smoothing;
    config.drift_deadband_ppm = options.drift_deadband_ppm;
    config.drift_max_correction_ppm = options.drift_max_correction_ppm;
    config.stats_warmup_us = static_cast<std::uint64_t>(options.stats_warmup_ms) * 1000ULL;
    config.collect_diagnostics = collect_diagnostics;
    return config;
}

jam2::NetworkSessionContract make_network_session_contract(const Options& options)
{
    return {1, jam2::NetworkAudioFormat::Pcm24Mono, options.sample_rate, options.frame_size};
}

jam2::PeerId compatibility_peer_id(const jam2::Endpoint& endpoint) noexcept
{
    constexpr std::uint64_t offset_basis = 14695981039346656037ULL;
    constexpr std::uint64_t prime = 1099511628211ULL;
    std::uint64_t value = offset_basis;
    for (const unsigned char byte : endpoint.host) {
        value = (value ^ static_cast<std::uint64_t>(byte)) * prime;
    }
    value = (value ^ 0xffULL) * prime;
    value = (value ^ static_cast<std::uint64_t>(endpoint.port & 0xffU)) * prime;
    value = (value ^ static_cast<std::uint64_t>((endpoint.port >> 8U) & 0xffU)) * prime;
    return jam2::PeerId{value == 0 ? 1ULL : value};
}

AudioPacketStats run_audio_packet_exchange(
    jam2::NetworkSession& network_session,
    const Options& options,
    RuntimeState& runtime,
    jam2::Engine* engine,
    jam2::NetworkCaptureAttachment network_capture,
    jam2::audio::PreparedTrackSource* prepared_source,
    jam2::audio::StreamControl* audio_control,
    jam2::audio::MonoRingBuffer* capture_ring,
    jam2::audio::MonoRingBuffer* playback_ring,
    jam2::audio::DeviceStream* audio_stream,
    jam2::audio::OutputRecorder* recorder,
    std::uint64_t startup_drained_packets,
    CsvStatsLog* csv_log)
{
    AudioPacketStats stats;
    stats.local_peer_id = network_session.localPeerId().value;
    stats.remote_peer_id = network_session.remotePeer().peer_id.value;
    switch (network_session.bootstrapRole()) {
    case jam2::SessionBootstrapRole::Creator: stats.bootstrap_role = "creator"; break;
    case jam2::SessionBootstrapRole::Joiner: stats.bootstrap_role = "joiner"; break;
    case jam2::SessionBootstrapRole::Static: stats.bootstrap_role = "static"; break;
    }
    stats.session_protocol_version = network_session.contract().protocol_version;
    stats.session_audio_format = "pcm24-mono";
    stats.session_sample_rate = network_session.contract().sample_rate;
    stats.session_frames_per_packet = network_session.contract().frames_per_packet;
    const std::uint64_t bootstrap_coordinator_peer_id =
        network_session.bootstrapRole() == jam2::SessionBootstrapRole::Creator
        ? stats.local_peer_id
        : (network_session.bootstrapRole() == jam2::SessionBootstrapRole::Joiner
            ? stats.remote_peer_id
            : std::min(stats.local_peer_id, stats.remote_peer_id));
    jam2::SessionAuthority authority(
        stats.local_peer_id,
        bootstrap_coordinator_peer_id,
        bootstrap_coordinator_peer_id);
    OsPriorityScope os_priority_scope(options);
    stats.os_scheduling = os_priority_scope.status();
    stats.startup_drained_packets = startup_drained_packets;
    stats.sample_time_playout_enabled = options.sample_time_playout;
    stats.playout_delay_frames = static_cast<std::uint64_t>(options.playout_delay_frames);
    stats.jitter_buffer_enabled = options.jitter_buffer_frames > 0;
    stats.jitter_buffer_target_frames = static_cast<std::uint64_t>(options.jitter_buffer_frames);
    stats.jitter_buffer_max_frames = static_cast<std::uint64_t>(options.jitter_buffer_max_frames);
    stats.adaptive_playback_cushion_enabled = options.adaptive_playback_cushion;
    stats.adaptive_playback_target_frames = static_cast<std::uint64_t>(options.adaptive_playback_target_frames);
    stats.adaptive_playback_min_frames = static_cast<std::uint64_t>(options.adaptive_playback_min_frames);
    stats.adaptive_playback_max_frames = static_cast<std::uint64_t>(options.adaptive_playback_max_frames);
    const bool bounded_stream = options.stream_ms > 0;
    const bool collect_stats = true;
    const bool collect_diagnostics = collect_stats && (csv_log != nullptr || options.stats_interval_ms > 0);

    auto& packet_schedule = network_session.schedule();
    auto& peer_stream = network_session.peerStream();
    std::uint64_t session_work_budget_yields = 0;
    auto sync_peer_stats = [&] {
        copy_peer_stream_stats(stats, peer_stream.stats());
        stats.network_peer_count = network_session.peerCount();
        copy_peer_mixer_stats(stats, network_session.mixStats());
        stats.udp_work_budget_yields =
            session_work_budget_yields + peer_stream.stats().reorder_work_budget_yields;
        const auto& grid = authority.grid();
        const auto& authority_stats = authority.stats();
        stats.bootstrap_coordinator_peer_id = authority.bootstrapCoordinatorPeerId();
        stats.arrangement_authority_peer_id = authority.arrangementAuthorityPeerId();
        stats.grid_authority_peer_id = grid.authority_peer_id;
        stats.grid_revision = grid.revision;
        stats.grid_run_state = static_cast<std::uint64_t>(grid.run_state);
        stats.grid_mode = grid.mode;
        stats.grid_authority_epoch_frame = grid.authority_epoch_frame;
        stats.grid_mapped_epoch_frame =
            runtime.metronome_epoch_sample_time.load(std::memory_order_relaxed);
        stats.grid_authority_packet_frame = grid.authority_packet_frame;
        stats.grid_proposals_accepted = authority_stats.grid_proposals_accepted;
        stats.grid_proposals_rejected = authority_stats.grid_proposals_rejected;
        stats.grid_assignments_accepted = authority_stats.grid_assignments_accepted;
        stats.grid_assignments_rejected = authority_stats.grid_assignments_rejected;
        stats.grid_authority_states_accepted = authority_stats.grid_authority_states_accepted;
        stats.grid_authority_states_rejected = authority_stats.grid_authority_states_rejected;
        stats.grid_authority_missing_events = authority_stats.grid_authority_missing_events;
        stats.transport_events_accepted = authority_stats.transport_events_accepted;
        stats.transport_events_rejected = authority_stats.transport_events_rejected;
    };
    sync_peer_stats();

    std::vector<std::int32_t> asio_frames(static_cast<std::size_t>(options.frame_size), 0);
    std::vector<std::int32_t> network_frames(static_cast<std::size_t>(options.frame_size), 0);
    const auto silence_payload = jam2::protocol::pack_pcm24(network_frames);
    const std::uint16_t audio_payload_size = static_cast<std::uint16_t>(silence_payload.size());
    std::vector<std::uint8_t> packed_audio_payload(audio_payload_size);
    auto send_packet = [&](const jam2::protocol::Header& header, std::span<const std::uint8_t> payload) {
        return network_session.send(
            header.type,
            header.sequence,
            header.sample_time,
            header.send_time_us,
            payload);
    };
    constexpr std::size_t kMaxDatagramsPerWake = 64;
    constexpr std::size_t kOutstandingPingSlots = 8;
    std::array<OutstandingPing, kOutstandingPingSlots> outstanding_pings{};
    constexpr std::uint64_t kGridStateIntervalUs = 20000ULL;
    std::uint64_t next_local_grid_request_id = 1;
    std::uint64_t last_local_grid_request_sequence =
        runtime.grid_request_sequence.load(std::memory_order_acquire);
    std::optional<jam2::GridProposal> pending_local_grid_proposal;
    std::uint64_t next_grid_proposal_send_us = 0;
    std::uint64_t next_grid_assignment_send_us = 0;
    std::uint64_t sending_transport_revision = 0;
    std::uint64_t next_transport_send = 0;
    bool remote_metronome_epoch_accepted = false;
    bool remote_metronome_epoch_valid = false;
    std::uint64_t remote_metronome_epoch_sample_time = 0;
    std::uint64_t last_authority_state_received_us = 0;
    double metronome_compensation_offset_frames = 0.0;
    std::uint64_t metronome_compensation_last_update_us = 0;
    bool metronome_compensation_was_stale = false;
    std::int64_t shared_grid_target_offset_frames = 0;
    std::uint64_t shared_grid_last_update_us = 0;
    bool shared_grid_target_valid = false;
    std::uint64_t last_send_time_us = 0;
    std::uint64_t last_stream_loop_us = 0;
    const std::uint64_t start_time = packet_schedule.startTimeUs();
    std::uint64_t next_stats = collect_stats && options.stats_interval_ms > 0 ?
        start_time + static_cast<std::uint64_t>(options.stats_interval_ms) * 1000ULL :
        0;
    const std::uint64_t send_deadline = bounded_stream ?
        packet_schedule.nextAudioSendUs() + static_cast<std::uint64_t>(options.stream_ms) * 1000ULL :
        UINT64_MAX;
    const std::uint64_t receive_deadline = bounded_stream ?
        send_deadline + static_cast<std::uint64_t>(options.stream_linger_ms) * 1000ULL :
        UINT64_MAX;
    auto grid_run_state_from_runtime = [&]() {
        return runtime.metronome.load(std::memory_order_relaxed)
            ? jam2::GridRunState::Running
            : jam2::GridRunState::Stopped;
    };

    auto choose_safe_local_epoch = [&]() {
        const auto pattern = metronome_pattern_from_runtime(runtime);
        const std::uint64_t step_frames = jam2::metronome::step_interval_samples(
            static_cast<double>(options.sample_rate), pattern.bpm, pattern.division);
        const std::uint64_t bar_frames =
            step_frames * static_cast<std::uint64_t>(pattern.step_count);
        const std::uint64_t rtt_frames = stats.rtt_min_us *
            static_cast<std::uint64_t>(options.sample_rate) / 1000000ULL;
        const std::uint64_t lead_frames = std::max(
            static_cast<std::uint64_t>(options.sample_rate) / 2ULL,
            rtt_frames + static_cast<std::uint64_t>(options.sample_rate) / 5ULL);
        const std::uint64_t minimum = current_engine_frame(audio_control) + lead_frames;
        const std::uint64_t current_epoch =
            runtime.metronome_epoch_sample_time.load(std::memory_order_relaxed);
        if (!runtime.metronome_epoch_valid.load(std::memory_order_relaxed) ||
            current_epoch == 0 || bar_frames == 0 || minimum <= current_epoch) {
            return minimum;
        }
        return current_epoch +
            ((minimum - current_epoch + bar_frames - 1ULL) / bar_frames) * bar_frames;
    };

    auto apply_authority_role = [&]() {
        const bool local_authority = authority.localIsGridAuthority();
        const auto& grid = authority.grid();
        runtime.metronome_local_authority.store(local_authority, std::memory_order_relaxed);
        runtime.leader_audio_local_click.store(
            local_authority && grid.run_state == jam2::GridRunState::Running &&
                grid.mode == metronome_mode_id(MetronomeMode::LeaderAudio),
            std::memory_order_relaxed);
    };

    auto activate_local_grid = [&]() {
        const std::uint64_t packet_frame = current_engine_frame(audio_control);
        const std::uint64_t epoch = choose_safe_local_epoch();
        if (!authority.activateLocalGrid(epoch, packet_frame)) {
            return false;
        }
        const auto& grid = authority.grid();
        runtime.metronome.store(
            grid.run_state == jam2::GridRunState::Running,
            std::memory_order_relaxed);
        runtime.metronome_mode.store(grid.mode, std::memory_order_relaxed);
        runtime.metronome_epoch_sample_time.store(epoch, std::memory_order_relaxed);
        runtime.metronome_epoch_valid.store(true, std::memory_order_relaxed);
        runtime.metronome_render_offset_frames.store(0, std::memory_order_relaxed);
        runtime.metronome_revision.store(grid.revision, std::memory_order_relaxed);
        apply_authority_role();
        return true;
    };

    auto align_shared_grid_to_remote_bar = [&](
        const MetronomePayload& payload,
        std::uint64_t remote_packet_frame) -> bool {
        if (audio_control == nullptr || stats.recv_pongs == 0 || stats.rtt_min_us == 0) {
            return false;
        }
        const jam2::metronome::PatternSnapshot pattern = payload.has_pattern
            ? jam2::metronome::sanitize(payload.pattern)
            : metronome_pattern_from_runtime(runtime);
        const std::uint64_t step_frames = jam2::metronome::step_interval_samples(
            static_cast<double>(options.sample_rate),
            pattern.bpm,
            pattern.division);
        const std::uint64_t bar_frames = step_frames * static_cast<std::uint64_t>(pattern.step_count);
        if (bar_frames == 0) {
            return false;
        }

        const std::uint64_t one_way_frames =
            stats.rtt_min_us * static_cast<std::uint64_t>(options.sample_rate) / 2000000ULL;
        const std::uint64_t projected_remote_frame = remote_packet_frame + one_way_frames;
        std::uint64_t frames_until_bar = 0;
        if (projected_remote_frame < payload.epoch_sample_time) {
            frames_until_bar = payload.epoch_sample_time - projected_remote_frame;
        } else {
            const std::uint64_t remote_grid_frame = projected_remote_frame - payload.epoch_sample_time;
            const std::uint64_t phase = remote_grid_frame % bar_frames;
            frames_until_bar = phase == 0 ? bar_frames : bar_frames - phase;
        }

        const std::uint64_t local_engine_frame =
            audio_control->engine_frame_counter.load(std::memory_order_relaxed);
        runtime.metronome_epoch_sample_time.store(
            local_engine_frame + frames_until_bar,
            std::memory_order_relaxed);
        runtime.metronome_epoch_valid.store(true, std::memory_order_relaxed);
        runtime.metronome_render_offset_frames.store(0, std::memory_order_relaxed);
        shared_grid_target_offset_frames = 0;
        shared_grid_target_valid = true;
        return true;
    };

    auto observe_shared_grid_phase = [&] (
        const MetronomePayload& payload,
        std::uint64_t remote_packet_frame) {
        if (authority.localIsGridAuthority() || audio_control == nullptr ||
            stats.rtt_min_us == 0 || !shared_grid_target_valid) {
            return;
        }
        const auto pattern = payload.has_pattern
            ? jam2::metronome::sanitize(payload.pattern)
            : metronome_pattern_from_runtime(runtime);
        const std::uint64_t step_frames = jam2::metronome::step_interval_samples(
            static_cast<double>(options.sample_rate), pattern.bpm, pattern.division);
        const std::uint64_t bar_frames = step_frames * static_cast<std::uint64_t>(pattern.step_count);
        if (bar_frames == 0 || bar_frames > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
            return;
        }
        const std::uint64_t one_way_frames =
            stats.rtt_min_us * static_cast<std::uint64_t>(options.sample_rate) / 2000000ULL;
        const std::uint64_t projected_remote_frame = remote_packet_frame + one_way_frames;
        const std::uint64_t local_frame = current_engine_frame(audio_control);
        const std::uint64_t local_epoch = runtime.metronome_epoch_sample_time.load(std::memory_order_relaxed);
        const std::int64_t current_offset = runtime.metronome_render_offset_frames.load(std::memory_order_relaxed);
        auto signed_delta = [](std::uint64_t value, std::uint64_t origin) {
            return value >= origin
                ? static_cast<std::int64_t>(value - origin)
                : -static_cast<std::int64_t>(origin - value);
        };
        auto phase = [](std::int64_t position, std::int64_t interval) {
            std::int64_t value = position % interval;
            return value < 0 ? value + interval : value;
        };
        const std::int64_t interval = static_cast<std::int64_t>(bar_frames);
        const std::int64_t remote_phase = phase(
            signed_delta(projected_remote_frame, payload.epoch_sample_time), interval);
        const std::uint64_t adjusted_local_frame = current_offset >= 0
            ? local_frame + static_cast<std::uint64_t>(current_offset)
            : (local_frame > static_cast<std::uint64_t>(-current_offset)
                ? local_frame - static_cast<std::uint64_t>(-current_offset)
                : 0ULL);
        const std::int64_t local_phase = phase(signed_delta(adjusted_local_frame, local_epoch), interval);
        std::int64_t phase_error = remote_phase - local_phase;
        const std::int64_t half_interval = interval / 2;
        if (phase_error > half_interval) {
            phase_error -= interval;
        } else if (phase_error < -half_interval) {
            phase_error += interval;
        }
        shared_grid_target_offset_frames = current_offset + phase_error;
    };

    runtime.metronome_epoch_sample_time.store(0, std::memory_order_relaxed);
    runtime.metronome_epoch_valid.store(false, std::memory_order_relaxed);
    runtime.metronome_render_offset_frames.store(0, std::memory_order_relaxed);
    runtime.metronome_local_authority.store(false, std::memory_order_relaxed);
    runtime.leader_audio_local_click.store(false, std::memory_order_relaxed);
    if (authority.localIsBootstrapCoordinator()) {
        const auto initial = authority.orderGridProposal({
            stats.local_peer_id,
            next_local_grid_request_id++,
            grid_run_state_from_runtime(),
            static_cast<std::uint8_t>(runtime.metronome_mode.load(std::memory_order_relaxed)),
            0,
        });
        if (initial) {
            (void)activate_local_grid();
        }
    }

    auto update_metronome_compensation = [&](std::uint64_t now_us) {
        const bool listener_compensated =
            runtime.metronome_mode.load(std::memory_order_relaxed) ==
            metronome_mode_id(MetronomeMode::ListenerCompensated);
        const bool shared_grid = runtime.metronome_mode.load(std::memory_order_relaxed) ==
            metronome_mode_id(MetronomeMode::SharedGrid);
        const bool can_compensate =
            listener_compensated &&
            authority.grid().authority_peer_id == stats.remote_peer_id &&
            authority.grid().run_state != jam2::GridRunState::AuthorityMissing &&
            last_authority_state_received_us != 0 &&
            now_us - last_authority_state_received_us <= 500000ULL &&
            options.sample_time_playout &&
            playback_ring != nullptr &&
            audio_control != nullptr &&
            peer_stream.playoutSampleTimeInitialized() &&
            remote_metronome_epoch_valid &&
            runtime.metronome_epoch_valid.load(std::memory_order_relaxed);
        if (!can_compensate) {
            if (!listener_compensated && !shared_grid) {
                metronome_compensation_offset_frames = 0.0;
                runtime.metronome_render_offset_frames.store(0, std::memory_order_relaxed);
            }
            stats.metronome_compensation_active = false;
            stats.metronome_compensation_offset_frames =
                runtime.metronome_render_offset_frames.load(std::memory_order_relaxed);
            stats.metronome_compensation_target_frames = 0;
            metronome_compensation_last_update_us = now_us;
            if (listener_compensated && !metronome_compensation_was_stale) {
                ++stats.metronome_compensation_stale_events;
            }
            metronome_compensation_was_stale = listener_compensated;
            return;
        }

        metronome_compensation_was_stale = false;
        const std::uint64_t playback_depth = playback_ring->available_read();
        const std::uint64_t next_playout_remote_sample_time =
            peer_stream.nextPlayoutRemoteSampleTime();
        const std::uint64_t remote_playback_head =
            next_playout_remote_sample_time > playback_depth ?
                next_playout_remote_sample_time - playback_depth :
                0ULL;
        const std::int64_t remote_position =
            remote_playback_head >= remote_metronome_epoch_sample_time ?
                static_cast<std::int64_t>(remote_playback_head - remote_metronome_epoch_sample_time) :
                -static_cast<std::int64_t>(remote_metronome_epoch_sample_time - remote_playback_head);
        const std::uint64_t local_epoch =
            runtime.metronome_epoch_sample_time.load(std::memory_order_relaxed);
        const std::uint64_t local_counter =
            audio_control->engine_frame_counter.load(std::memory_order_relaxed);
        const std::int64_t local_position =
            local_counter >= local_epoch ?
                static_cast<std::int64_t>(local_counter - local_epoch) :
                -static_cast<std::int64_t>(local_epoch - local_counter);
        const auto pattern = metronome_pattern_from_runtime(runtime);
        const std::uint64_t step_interval =
            jam2::metronome::step_interval_samples(
                static_cast<double>(options.sample_rate),
                pattern.bpm,
                pattern.division);
        auto phase_frames = [](std::int64_t position, std::int64_t interval) -> std::int64_t {
            if (interval <= 0) {
                return 0;
            }
            std::int64_t phase = position % interval;
            if (phase < 0) {
                phase += interval;
            }
            return phase;
        };
        auto nearest_phase_error = [](std::int64_t target_phase, std::int64_t local_phase, std::int64_t interval) -> std::int64_t {
            if (interval <= 0) {
                return 0;
            }
            std::int64_t error = target_phase - local_phase;
            const std::int64_t half_interval = interval / 2;
            if (error > half_interval) {
                error -= interval;
            } else if (error < -half_interval) {
                error += interval;
            }
            return error;
        };
        std::int64_t target_frames = 0;
        if (step_interval > 0) {
            const std::int64_t interval = static_cast<std::int64_t>(step_interval);
            target_frames = nearest_phase_error(
                phase_frames(remote_position, interval),
                phase_frames(local_position, interval),
                interval);
        }
        const std::int64_t max_frames = ms_to_signed_frames(options.metronome_compensation_max_ms, options.sample_rate);
        if (max_frames >= 0 && target_frames > max_frames) {
            target_frames = max_frames;
            ++stats.metronome_compensation_clamp_events;
        } else if (max_frames >= 0 && target_frames < -max_frames) {
            target_frames = -max_frames;
            ++stats.metronome_compensation_clamp_events;
        }

        const double elapsed_ms =
            metronome_compensation_last_update_us != 0 && now_us > metronome_compensation_last_update_us ?
                static_cast<double>(now_us - metronome_compensation_last_update_us) / 1000.0 :
                0.0;
        metronome_compensation_last_update_us = now_us;
        const double deadband_frames =
            std::abs(static_cast<double>(ms_to_signed_frames(options.metronome_compensation_deadband_ms, options.sample_rate)));
        double next_offset = metronome_compensation_offset_frames;
        const double diff = static_cast<double>(target_frames) - next_offset;
        if (std::abs(diff) >= deadband_frames) {
            const double smoothing_alpha =
                options.metronome_compensation_smoothing_ms > 0.0 ?
                    std::clamp(elapsed_ms / options.metronome_compensation_smoothing_ms, 0.0, 1.0) :
                    1.0;
            double step = diff * smoothing_alpha;
            const double max_step =
                options.metronome_compensation_slew_ms_per_sec > 0.0 ?
                    static_cast<double>(ms_to_signed_frames(
                        options.metronome_compensation_slew_ms_per_sec * elapsed_ms / 1000.0,
                        options.sample_rate)) :
                    std::abs(diff);
            if (max_step > 0.0 && std::abs(step) > max_step) {
                step = step > 0.0 ? max_step : -max_step;
            }
            next_offset += step;
        }
        metronome_compensation_offset_frames = next_offset;
        const std::int64_t applied_frames = static_cast<std::int64_t>(std::llround(next_offset));
        runtime.metronome_render_offset_frames.store(applied_frames, std::memory_order_relaxed);
        stats.metronome_compensation_active = true;
        stats.metronome_compensation_offset_frames = applied_frames;
        stats.metronome_compensation_target_frames = target_frames;
    };

    auto update_shared_grid_compensation = [&](std::uint64_t now_us) {
        const bool shared_grid = runtime.metronome_mode.load(std::memory_order_relaxed) ==
            metronome_mode_id(MetronomeMode::SharedGrid);
        if (!shared_grid || authority.localIsGridAuthority() || !shared_grid_target_valid) {
            shared_grid_last_update_us = now_us;
            return;
        }
        if (shared_grid_last_update_us != 0 && now_us - shared_grid_last_update_us < 10000ULL) {
            return;
        }
        const std::int64_t current = runtime.metronome_render_offset_frames.load(std::memory_order_relaxed);
        std::int64_t target = shared_grid_target_offset_frames;
        const std::int64_t max_frames = std::abs(ms_to_signed_frames(
            options.metronome_compensation_max_ms, options.sample_rate));
        target = std::clamp(target, -max_frames, max_frames);
        const double elapsed_ms = shared_grid_last_update_us != 0 && now_us > shared_grid_last_update_us
            ? static_cast<double>(now_us - shared_grid_last_update_us) / 1000.0
            : 0.0;
        shared_grid_last_update_us = now_us;
        const double deadband_frames = std::abs(static_cast<double>(ms_to_signed_frames(
            options.metronome_compensation_deadband_ms, options.sample_rate)));
        const double difference = static_cast<double>(target - current);
        std::int64_t applied = current;
        if (std::abs(difference) >= deadband_frames) {
            const double alpha = options.metronome_compensation_smoothing_ms > 0.0
                ? std::clamp(elapsed_ms / options.metronome_compensation_smoothing_ms, 0.0, 1.0)
                : 1.0;
            double step = difference * alpha;
            const double max_step = options.metronome_compensation_slew_ms_per_sec > 0.0
                ? std::abs(static_cast<double>(ms_to_signed_frames(
                    options.metronome_compensation_slew_ms_per_sec * elapsed_ms / 1000.0,
                    options.sample_rate)))
                : std::abs(difference);
            if (max_step > 0.0 && std::abs(step) > max_step) {
                step = std::copysign(max_step, step);
            }
            applied = current + static_cast<std::int64_t>(std::llround(step));
            const std::int64_t applied_delta = applied - current;
            if (applied_delta != 0 && prepared_source != nullptr && prepared_source->playing() &&
                !enqueue_prepared_command(
                    prepared_source,
                    audio_control,
                    {jam2::audio::PreparedTrackSource::CommandType::NudgeSource,
                        0,
                        current_engine_frame(audio_control),
                        0,
                        0,
                        static_cast<std::int32_t>(applied_delta)})) {
                return;
            }
            runtime.metronome_render_offset_frames.store(applied, std::memory_order_relaxed);
        }
        stats.metronome_compensation_active = true;
        stats.metronome_compensation_offset_frames = applied;
        stats.metronome_compensation_target_frames = target;
    };

    while (jam2::monotonic_us() < receive_deadline && !stats.received_bye && !runtime.quit.load(std::memory_order_relaxed)) {
        const std::uint64_t now = jam2::monotonic_us();
        network_session.advance(now);
        sync_peer_stats();
        if (collect_diagnostics && last_stream_loop_us != 0 && now >= last_stream_loop_us) {
            observe_timing(now - last_stream_loop_us, stats.receive_loop_gap_min_us, stats.receive_loop_gap_sum_us, stats.receive_loop_gap_max_us);
            ++stats.receive_loop_gap_samples;
        }
        last_stream_loop_us = now;
        commit_due_transport(runtime, audio_control);
        const std::uint64_t local_grid_request_sequence =
            runtime.grid_request_sequence.load(std::memory_order_acquire);
        if (local_grid_request_sequence != last_local_grid_request_sequence) {
            last_local_grid_request_sequence = local_grid_request_sequence;
            jam2::GridProposal proposal{
                stats.local_peer_id,
                next_local_grid_request_id++,
                grid_run_state_from_runtime(),
                static_cast<std::uint8_t>(runtime.metronome_mode.load(std::memory_order_relaxed)),
                runtime.metronome_epoch_sample_time.load(std::memory_order_relaxed),
            };
            if (authority.localIsBootstrapCoordinator()) {
                if (authority.orderGridProposal(proposal)) {
                    pending_local_grid_proposal.reset();
                    remote_metronome_epoch_accepted = false;
                    shared_grid_target_valid = false;
                    (void)activate_local_grid();
                    next_grid_assignment_send_us = 0;
                }
            } else {
                pending_local_grid_proposal = proposal;
                next_grid_proposal_send_us = 0;
                runtime.metronome_epoch_valid.store(false, std::memory_order_relaxed);
                runtime.metronome_local_authority.store(false, std::memory_order_relaxed);
                runtime.leader_audio_local_click.store(false, std::memory_order_relaxed);
            }
        }
        const bool metronome_enabled =
            runtime.metronome.load(std::memory_order_relaxed);
        update_metronome_compensation(now);
        update_shared_grid_compensation(now);
        stats.grid_mapping_error_frames = shared_grid_target_valid
            ? shared_grid_target_offset_frames -
                runtime.metronome_render_offset_frames.load(std::memory_order_relaxed)
            : 0;
        sync_audio_control(runtime, audio_control, stats.resampler_ratio);
        int sends_this_loop = 0;
        while (now >= packet_schedule.nextAudioSendUs() &&
               packet_schedule.nextAudioSendUs() < send_deadline &&
               sends_this_loop < 8) {
            std::span<const std::uint8_t> payload = silence_payload;
            if (capture_ring != nullptr) {
                if (engine != nullptr && network_capture.generation != 0) {
                    (void)engine->popNetworkCapture(network_capture, asio_frames);
                } else {
                    (void)capture_ring->pop(asio_frames);
                }
                for (std::size_t i = 0; i < asio_frames.size(); ++i) {
                    network_frames[i] = asio_frames[i] / 256;
                }
                const int live_send_level_ppm = runtime.send_level_ppm.load(std::memory_order_relaxed);
                apply_send_level(network_frames, live_send_level_ppm);
                if (audio_control != nullptr) {
                    const int send_peak_ppm = pcm24_peak_ppm(network_frames);
                    update_peak(audio_control->send_peak_ppm, send_peak_ppm);
                    update_peak(audio_control->gui_send_peak_ppm, send_peak_ppm);
                }
                const int live_metronome_mode = runtime.metronome_mode.load(std::memory_order_relaxed);
                const bool leader_audio_local_click = runtime.leader_audio_local_click.load(std::memory_order_relaxed);
                if (live_metronome_mode == metronome_mode_id(MetronomeMode::LeaderAudio) && leader_audio_local_click && metronome_enabled) {
                    mix_leader_click_into_packet(
                        network_frames,
                        packet_schedule.sampleTime(),
                        options.sample_rate,
                        gain_from_ppm(runtime.metronome_level_ppm.load(std::memory_order_relaxed)),
                        runtime.metronome_epoch_sample_time.load(std::memory_order_relaxed),
                        metronome_pattern_from_runtime(runtime));
                    stats.leader_audio_source_peer_id = stats.local_peer_id;
                    ++stats.leader_audio_injected_packets;
                }
                (void)jam2::protocol::pack_pcm24_into(network_frames, packed_audio_payload);
                payload = packed_audio_payload;
            } else {
                if (audio_control != nullptr) {
                    audio_control->send_peak_ppm.store(0, std::memory_order_relaxed);
                    audio_control->gui_send_peak_ppm.store(0, std::memory_order_relaxed);
                }
                const int live_metronome_mode = runtime.metronome_mode.load(std::memory_order_relaxed);
                const bool leader_audio_local_click = runtime.leader_audio_local_click.load(std::memory_order_relaxed);
                if (live_metronome_mode == metronome_mode_id(MetronomeMode::LeaderAudio) && leader_audio_local_click && metronome_enabled) {
                    std::fill(network_frames.begin(), network_frames.end(), 0);
                    mix_leader_click_into_packet(
                        network_frames,
                        packet_schedule.sampleTime(),
                        options.sample_rate,
                        gain_from_ppm(runtime.metronome_level_ppm.load(std::memory_order_relaxed)),
                        runtime.metronome_epoch_sample_time.load(std::memory_order_relaxed),
                        metronome_pattern_from_runtime(runtime));
                    stats.leader_audio_source_peer_id = stats.local_peer_id;
                    ++stats.leader_audio_injected_packets;
                    (void)jam2::protocol::pack_pcm24_into(network_frames, packed_audio_payload);
                    payload = packed_audio_payload;
                }
            }
            const jam2::protocol::Header header{
                jam2::protocol::PacketType::Audio,
                0,
                network_session.sessionId(),
                packet_schedule.audioSequence(),
                packet_schedule.sampleTime(),
                now,
                0,
                0,
            };
            const std::size_t packet_size = send_packet(header, payload);
            const std::uint64_t actual_send_time = jam2::monotonic_us();
            if (collect_stats) {
                ++stats.sent_packets;
                stats.sent_bytes += packet_size;
            }
            if (collect_diagnostics) {
                if (last_send_time_us != 0 && actual_send_time >= last_send_time_us) {
                    observe_timing(
                        actual_send_time - last_send_time_us,
                        stats.send_interval_min_us,
                        stats.send_interval_sum_us,
                        stats.send_interval_max_us);
                    ++stats.send_interval_samples;
                }
                const std::uint64_t schedule_error =
                    actual_send_time >= packet_schedule.nextAudioSendUs()
                    ? actual_send_time - packet_schedule.nextAudioSendUs()
                    : packet_schedule.nextAudioSendUs() - actual_send_time;
                observe_timing(
                    schedule_error,
                    stats.send_schedule_error_min_us,
                    stats.send_schedule_error_sum_us,
                    stats.send_schedule_error_max_us);
                ++stats.send_schedule_error_samples;
                last_send_time_us = actual_send_time;
            }
            packet_schedule.commitAudioPacket();
            ++sends_this_loop;
        }
        if (collect_diagnostics && sends_this_loop > 1) {
            ++stats.send_catchup_events;
            if (static_cast<std::uint64_t>(sends_this_loop) > stats.send_catchup_max_packets) {
                stats.send_catchup_max_packets = static_cast<std::uint64_t>(sends_this_loop);
            }
        }
        if (now >= packet_schedule.nextPingUs() && now < send_deadline) {
            const std::uint32_t ping_sequence = packet_schedule.takeControlSequence();
            OutstandingPing& outstanding = outstanding_pings[ping_sequence % outstanding_pings.size()];
            if (outstanding.active) {
                ++stats.udp_ping_slot_overwrites;
            }
            outstanding = OutstandingPing{ping_sequence, now, true};
            const jam2::protocol::Header ping{
                jam2::protocol::PacketType::Ping,
                0,
                network_session.sessionId(),
                ping_sequence,
                0,
                now,
                0,
                0,
            };
            (void)send_packet(ping, {});
            if (collect_stats) {
                ++stats.sent_pings;
            }
            packet_schedule.commitPing();
        }
        if (now >= packet_schedule.nextGridStateUs() && now < send_deadline) {
            const int current_bpm = runtime.bpm.load(std::memory_order_relaxed);
            const std::uint64_t epoch = runtime.metronome_epoch_sample_time.load(std::memory_order_relaxed);
            const auto pattern = metronome_pattern_from_runtime(runtime);
            bool sent_grid_message = false;
            auto send_grid_message = [&](GridMessageKind kind,
                                         std::uint64_t revision_or_request,
                                         std::uint64_t payload_epoch,
                                         std::uint64_t header_frame,
                                         std::uint8_t mode,
                                         jam2::GridRunState run_state) {
                const auto metro_payload = encode_metronome_payload(
                    run_state == jam2::GridRunState::Running ? current_bpm : -current_bpm,
                    revision_or_request,
                    payload_epoch,
                    pattern,
                    kind,
                    mode,
                    run_state);
                const jam2::protocol::Header metro{
                    jam2::protocol::PacketType::MetronomeState,
                    0,
                    network_session.sessionId(),
                    packet_schedule.takeControlSequence(),
                    header_frame,
                    now,
                    0,
                    0,
                };
                (void)send_packet(metro, metro_payload);
                ++stats.metronome_sent;
                sent_grid_message = true;
            };
            if (pending_local_grid_proposal && now >= next_grid_proposal_send_us) {
                const auto& proposal = *pending_local_grid_proposal;
                send_grid_message(
                    GridMessageKind::Proposal,
                    proposal.request_id,
                    proposal.proposed_epoch_frame,
                    current_engine_frame(audio_control),
                    proposal.mode,
                    proposal.run_state);
                ++stats.grid_proposals_sent;
                next_grid_proposal_send_us = now + kGridStateIntervalUs;
            }
            if (authority.localIsBootstrapCoordinator() && authority.grid().revision != 0 &&
                now >= next_grid_assignment_send_us) {
                const auto& grid = authority.grid();
                send_grid_message(
                    GridMessageKind::Assignment,
                    grid.revision,
                    grid.authority_epoch_frame,
                    grid.authority_peer_id,
                    grid.mode,
                    grid.run_state);
                ++stats.grid_assignments_sent;
                next_grid_assignment_send_us = now + 100000ULL;
            }
            if (authority.localIsGridAuthority() &&
                runtime.metronome_epoch_valid.load(std::memory_order_relaxed)) {
                const auto& grid = authority.grid();
                send_grid_message(
                    GridMessageKind::AuthorityState,
                    grid.revision,
                    epoch,
                    current_engine_frame(audio_control),
                    grid.mode,
                    grid.run_state);
                ++stats.grid_authority_states_sent;
            }
            stats.local_metronome_beat = authority.grid().revision;
            stats.metronome_epoch_sample_time = epoch;
            stats.metronome_alignment_valid = runtime.metronome_epoch_valid.load(std::memory_order_relaxed);
            packet_schedule.scheduleNextGridState(
                now,
                sent_grid_message ? kGridStateIntervalUs : 1000ULL);
        }
        std::uint64_t network_transport_revision = 0;
        std::uint64_t transport_target = 0;
        int transport_action = 0;
        {
            std::lock_guard<std::mutex> lock(runtime.transport_mutex);
            network_transport_revision = runtime.transport_network_revision.load(std::memory_order_relaxed);
            transport_target = runtime.transport_network_target_raw_frame.load(std::memory_order_relaxed);
            transport_action = runtime.transport_network_action.load(std::memory_order_relaxed);
        }
        if (network_transport_revision != sending_transport_revision) {
            sending_transport_revision = network_transport_revision;
            next_transport_send = 0;
        }
        if (authority.localIsArrangementAuthority() &&
            authority.grid().revision <= (std::numeric_limits<std::uint32_t>::max)() &&
            sending_transport_revision != 0 &&
            sending_transport_revision <= (std::numeric_limits<std::uint32_t>::max)() &&
            now >= next_transport_send &&
            current_engine_frame(audio_control) <= transport_target &&
            now < send_deadline) {
            const std::uint64_t engine_now = current_engine_frame(audio_control);
            const auto transport_payload = encode_transport_payload({
                static_cast<jam2::gui_control::TransportAction>(
                    transport_action),
                static_cast<std::uint32_t>(sending_transport_revision),
                static_cast<std::uint32_t>(authority.grid().revision),
                transport_target,
            });
            const jam2::protocol::Header transport_header{
                jam2::protocol::PacketType::TransportState,
                0,
                network_session.sessionId(),
                packet_schedule.takeControlSequence(),
                engine_now,
                now,
                0,
                0,
            };
            (void)send_packet(transport_header, transport_payload);
            stats.transport_source_peer_id = stats.local_peer_id;
            stats.transport_event_counter = sending_transport_revision;
            stats.transport_grid_revision = authority.grid().revision;
            stats.transport_source_frame = engine_now;
            stats.transport_requested_target_frame = transport_target;
            stats.transport_applied_target_frame = transport_target;
            next_transport_send = now + 20000ULL;
        }
        if (collect_stats && runtime.print_stats.exchange(false, std::memory_order_relaxed)) {
            const std::uint64_t elapsed_ms = (now - start_time) / 1000ULL;
            print_periodic_stream_stats(
                stats,
                options,
                make_audio_snapshot(audio_stream, capture_ring, playback_ring, audio_control),
                elapsed_ms);
        }
        if (runtime.print_status.exchange(false, std::memory_order_relaxed)) {
            const std::uint64_t elapsed_ms = (now - start_time) / 1000ULL;
            print_compact_status(stats, options, runtime, audio_stream, playback_ring, audio_control, recorder, elapsed_ms);
        }
        if (!stats.sent_bye && now >= send_deadline) {
            const jam2::protocol::Header bye{
                jam2::protocol::PacketType::Bye,
                0,
                network_session.sessionId(),
                packet_schedule.takeControlSequence(),
                0,
                now,
                0,
                0,
            };
            (void)send_packet(bye, {});
            stats.sent_bye = true;
        }
        if (next_stats != 0 && now >= next_stats) {
            const std::uint64_t elapsed_ms = (now - start_time) / 1000ULL;
            if (runtime.stats_enabled.load(std::memory_order_relaxed)) {
                print_periodic_stream_stats(
                    stats,
                    options,
                    make_audio_snapshot(audio_stream, capture_ring, playback_ring, audio_control),
                    elapsed_ms);
            }
            if (csv_log != nullptr) {
                csv_log->write_periodic(
                    elapsed_ms,
                    stats,
                    options,
                    make_audio_snapshot(audio_stream, capture_ring, playback_ring, audio_control));
            }
            next_stats += static_cast<std::uint64_t>(options.stats_interval_ms) * 1000ULL;
        }

        bool received_any = false;
        std::uint64_t received_this_loop = 0;
        std::size_t datagrams_this_wake = 0;
        const std::uint64_t wait_now = jam2::monotonic_us();
        std::uint64_t next_network_deadline = std::min(receive_deadline, wait_now + 1000ULL);
        if (packet_schedule.nextAudioSendUs() < send_deadline) {
            next_network_deadline = std::min(next_network_deadline, packet_schedule.nextAudioSendUs());
        }
        if (packet_schedule.nextPingUs() < send_deadline) {
            next_network_deadline = std::min(next_network_deadline, packet_schedule.nextPingUs());
        }
        if (packet_schedule.nextGridStateUs() < send_deadline) {
            next_network_deadline = std::min(next_network_deadline, packet_schedule.nextGridStateUs());
        }
        if (next_stats != 0) {
            next_network_deadline = std::min(next_network_deadline, next_stats);
        }
        const std::uint64_t first_wait_us = next_network_deadline > wait_now
            ? next_network_deadline - wait_now
            : 0ULL;
        while (datagrams_this_wake < kMaxDatagramsPerWake) {
            if (received_any && packet_schedule.nextAudioSendUs() < send_deadline &&
                jam2::monotonic_us() >= packet_schedule.nextAudioSendUs()) {
                ++session_work_budget_yields;
                sync_peer_stats();
                break;
            }
            const auto received = network_session.receiveFor(received_any ? 0ULL : first_wait_us);
            if (!received) {
                break;
            }
            ++datagrams_this_wake;
            received_any = true;
            if (collect_diagnostics) {
                ++received_this_loop;
            }
            const auto& from = received->endpoint;
            const std::span<const std::uint8_t> bytes = received->bytes;
            if (!network_session.acceptsEndpoint(from)) {
                if (collect_stats) {
                    ++stats.ignored_packets;
                }
                continue;
            }
            const auto parsed = network_session.parse(bytes);
            if (!parsed) {
                if (collect_stats) {
                    stats.udp_parse.observe(parsed.error);
                    ++stats.ignored_packets;
                }
                continue;
            }
            try {
                const auto& header = parsed.header;
                if (header.type == jam2::protocol::PacketType::Audio) {
                    if (header.payload_length != audio_payload_size) {
                        ++stats.ignored_packets;
                        continue;
                    }
                    ++stats.recv_packets;
                    stats.recv_bytes += bytes.size();
                    const auto payload = std::span<const std::uint8_t>(
                        bytes.data() + jam2::protocol::kHeaderSize,
                        header.payload_length);
                    const auto result = peer_stream.receiveAudio(
                        header,
                        payload,
                        jam2::monotonic_us());
                    sync_peer_stats();
                    if (result != jam2::PeerAudioResult::Accepted) {
                        ++stats.ignored_packets;
                    }
                } else if (header.type == jam2::protocol::PacketType::Ping) {
                    if (!peer_stream.acceptReplay(jam2::PeerReplayChannel::Ping, header.sequence)) {
                        sync_peer_stats();
                        ++stats.ignored_packets;
                        continue;
                    }
                    const jam2::protocol::Header pong{
                        jam2::protocol::PacketType::Pong,
                        0,
                        network_session.sessionId(),
                        header.sequence,
                        0,
                        header.send_time_us,
                        0,
                        0,
                    };
                    (void)send_packet(pong, {});
                    if (collect_stats) {
                        ++stats.sent_pongs;
                    }
                } else if (header.type == jam2::protocol::PacketType::Pong) {
                    OutstandingPing& outstanding = outstanding_pings[header.sequence % outstanding_pings.size()];
                    if (!outstanding.active ||
                        outstanding.sequence != header.sequence ||
                        outstanding.send_time_us != header.send_time_us) {
                        ++stats.udp_unmatched_pongs;
                        ++stats.ignored_packets;
                        continue;
                    }
                    outstanding.active = false;
                    const std::uint64_t receive_time = jam2::monotonic_us();
                    if (receive_time >= outstanding.send_time_us) {
                        peer_stream.observeRtt(receive_time - outstanding.send_time_us);
                        sync_peer_stats();
                    }
                    ++stats.recv_pongs;
                } else if (header.type == jam2::protocol::PacketType::Bye) {
                    if (!peer_stream.acceptReplay(jam2::PeerReplayChannel::Bye, header.sequence)) {
                        sync_peer_stats();
                        ++stats.ignored_packets;
                        continue;
                    }
                    if (authority.markPeerInactive(stats.remote_peer_id)) {
                        runtime.leader_audio_local_click.store(false, std::memory_order_relaxed);
                        runtime.metronome_local_authority.store(false, std::memory_order_relaxed);
                        remote_metronome_epoch_valid = false;
                        shared_grid_target_valid = false;
                    }
                    stats.received_bye = true;
                    break;
                } else if (header.type == jam2::protocol::PacketType::MetronomeState) {
                    if (!peer_stream.acceptReplay(jam2::PeerReplayChannel::Metronome, header.sequence)) {
                        sync_peer_stats();
                        ++stats.ignored_packets;
                        continue;
                    }
                    const auto payload = std::span<const std::uint8_t>(
                        bytes.data() + jam2::protocol::kHeaderSize,
                        header.payload_length);
                    const auto metronome_payload = decode_metronome_payload(payload);
                    const int remote_bpm = metronome_payload.bpm;
                    stats.last_remote_beat = metronome_payload.revision_or_request;
                    stats.remote_metronome_beat = metronome_payload.revision_or_request;
                    if (collect_stats) {
                        ++stats.metronome_received;
                    }
                    const int remote_abs_bpm = std::abs(remote_bpm);
                    if (remote_abs_bpm <= 0 || remote_abs_bpm > 400 ||
                        metronome_payload.kind == GridMessageKind::LegacyState) {
                        ++stats.ignored_packets;
                        continue;
                    }
                    auto store_grid_settings = [&] {
                        if (runtime.grid_request_sequence.load(std::memory_order_acquire) !=
                            last_local_grid_request_sequence) {
                            return;
                        }
                        if (metronome_payload.has_pattern) {
                            store_metronome_pattern(runtime, metronome_payload.pattern);
                        }
                        runtime.bpm.store(remote_abs_bpm, std::memory_order_relaxed);
                        runtime.metronome.store(
                            metronome_payload.run_state == jam2::GridRunState::Running,
                            std::memory_order_relaxed);
                        runtime.metronome_mode.store(metronome_payload.mode, std::memory_order_relaxed);
                    };
                    if (metronome_payload.kind == GridMessageKind::Proposal) {
                        const auto ordered = authority.orderGridProposal({
                            stats.remote_peer_id,
                            metronome_payload.revision_or_request,
                            metronome_payload.run_state,
                            metronome_payload.mode,
                            metronome_payload.epoch_sample_time,
                        });
                        if (ordered) {
                            store_grid_settings();
                            runtime.metronome_revision.store(ordered->revision, std::memory_order_relaxed);
                            runtime.metronome_epoch_valid.store(false, std::memory_order_relaxed);
                            remote_metronome_epoch_accepted = false;
                            remote_metronome_epoch_valid = false;
                            shared_grid_target_valid = false;
                            apply_authority_role();
                            next_grid_assignment_send_us = 0;
                        }
                    } else if (metronome_payload.kind == GridMessageKind::Assignment) {
                        const jam2::GridAuthorityState assignment{
                            metronome_payload.revision_or_request,
                            header.sample_time,
                            metronome_payload.run_state,
                            metronome_payload.mode,
                            metronome_payload.epoch_sample_time,
                            0,
                        };
                        const auto result = authority.acceptGridAssignment(
                            stats.remote_peer_id,
                            assignment);
                        if (result == jam2::AuthorityUpdateResult::Accepted) {
                            store_grid_settings();
                            runtime.metronome_revision.store(assignment.revision, std::memory_order_relaxed);
                            pending_local_grid_proposal.reset();
                            remote_metronome_epoch_accepted = false;
                            remote_metronome_epoch_valid = false;
                            shared_grid_target_valid = false;
                            if (authority.localIsGridAuthority()) {
                                (void)activate_local_grid();
                            } else {
                                runtime.metronome_epoch_valid.store(false, std::memory_order_relaxed);
                                apply_authority_role();
                            }
                        }
                    } else if (metronome_payload.kind == GridMessageKind::AuthorityState) {
                        const jam2::GridAuthorityState remote_state{
                            metronome_payload.revision_or_request,
                            stats.remote_peer_id,
                            metronome_payload.run_state,
                            metronome_payload.mode,
                            metronome_payload.epoch_sample_time,
                            header.sample_time,
                        };
                        const auto result = authority.acceptGridAuthorityState(
                            stats.remote_peer_id,
                            remote_state);
                        if (result == jam2::AuthorityUpdateResult::Accepted) {
                            store_grid_settings();
                            runtime.metronome_revision.store(remote_state.revision, std::memory_order_relaxed);
                            remote_metronome_epoch_sample_time = metronome_payload.epoch_sample_time;
                            remote_metronome_epoch_valid = true;
                            last_authority_state_received_us = jam2::monotonic_us();
                            if (!remote_metronome_epoch_accepted) {
                                remote_metronome_epoch_accepted = align_shared_grid_to_remote_bar(
                                    metronome_payload,
                                    header.sample_time);
                            }
                            if (metronome_payload.mode ==
                                metronome_mode_id(MetronomeMode::SharedGrid)) {
                                observe_shared_grid_phase(metronome_payload, header.sample_time);
                            }
                            apply_authority_role();
                        }
                    }
                    stats.metronome_epoch_sample_time = runtime.metronome_epoch_sample_time.load(std::memory_order_relaxed);
                    stats.metronome_alignment_valid = runtime.metronome_epoch_valid.load(std::memory_order_relaxed);
                } else if (header.type == jam2::protocol::PacketType::TransportState) {
                    if (!peer_stream.acceptReplay(jam2::PeerReplayChannel::Transport, header.sequence)) {
                        sync_peer_stats();
                        ++stats.ignored_packets;
                        continue;
                    }
                    const auto payload = std::span<const std::uint8_t>(
                        bytes.data() + jam2::protocol::kHeaderSize,
                        header.payload_length);
                    const TransportPayload transport = decode_transport_payload(payload);
                    const bool accepted_transport = authority.acceptTransportEvent(
                        stats.remote_peer_id,
                        transport.event_counter,
                        transport.grid_revision);
                    if (accepted_transport) {
                        stats.transport_source_peer_id = stats.remote_peer_id;
                        stats.transport_event_counter = transport.event_counter;
                        stats.transport_grid_revision = transport.grid_revision;
                        stats.transport_source_frame = header.sample_time;
                        stats.transport_requested_target_frame = transport.target_sender_frame;
                    }
                    if ((transport.action == jam2::gui_control::TransportAction::TrackRestart ||
                         transport.action == jam2::gui_control::TransportAction::RecordStart) &&
                        accepted_transport &&
                        stats.recv_pongs > 0 &&
                        stats.rtt_min_us > 0 &&
                        prepared_source != nullptr) {
                        const std::uint64_t sender_lead_frames =
                            transport.target_sender_frame > header.sample_time
                            ? transport.target_sender_frame - header.sample_time
                            : 0ULL;
                        const std::uint64_t one_way_frames = stats.rtt_min_us *
                            static_cast<std::uint64_t>(options.sample_rate) / 2000000ULL;
                        const std::uint64_t frames_until_target = sender_lead_frames > one_way_frames
                            ? sender_lead_frames - one_way_frames
                            : 0ULL;
                        const std::uint64_t target_raw_frame =
                            current_engine_frame(audio_control) + frames_until_target;
                        stats.transport_applied_target_frame = target_raw_frame;
                        const std::int64_t offset =
                            runtime.metronome_render_offset_frames.load(std::memory_order_relaxed);
                        const QuantizedSchedule schedule{
                            target_raw_frame,
                            target_raw_frame,
                            musical_frame_from_raw(target_raw_frame, offset),
                        };
                        const bool seek_ok = enqueue_prepared_command(
                            prepared_source,
                            audio_control,
                            {jam2::audio::PreparedTrackSource::CommandType::Seek, 0, schedule.target_raw_frame, 0, 0, 1000000});
                        const bool play_ok = enqueue_prepared_command(
                            prepared_source,
                            audio_control,
                            {jam2::audio::PreparedTrackSource::CommandType::Play, 0, schedule.target_raw_frame, 0, 0, 1000000});
                        if (seek_ok && play_ok) {
                            publish_transport_schedule(
                                runtime,
                                jam2::gui_control::TransportAction::TrackRestart,
                                schedule,
                                false);
                        }
                    }
                } else {
                    if (collect_stats) {
                        ++stats.ignored_packets;
                    }
                }
            } catch (const std::exception&) {
                if (collect_stats) {
                    ++stats.ignored_packets;
                }
            }
        }
        if (datagrams_this_wake == kMaxDatagramsPerWake) {
            ++session_work_budget_yields;
            sync_peer_stats();
        }
        if (collect_diagnostics) {
            ++stats.recv_loop_iterations;
            stats.recv_loop_batch_sum += received_this_loop;
            if (received_this_loop > stats.recv_loop_batch_max) {
                stats.recv_loop_batch_max = received_this_loop;
            }
            if (received_this_loop > stats.receive_packets_per_loop_max) {
                stats.receive_packets_per_loop_max = received_this_loop;
            }
            if (received_this_loop > 1 && received_this_loop > stats.receive_burst_packets_max) {
                stats.receive_burst_packets_max = received_this_loop;
            }
            if (received_this_loop == 0) {
                ++stats.recv_loop_idle_count;
            }
        }
        if (!received_any) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    if (!stats.sent_bye) {
        const std::uint64_t now = jam2::monotonic_us();
        const jam2::protocol::Header bye{
            jam2::protocol::PacketType::Bye,
            0,
            network_session.sessionId(),
            packet_schedule.takeControlSequence(),
            0,
            now,
            0,
            0,
        };
        (void)send_packet(bye, {});
        stats.sent_bye = true;
    }

    const std::uint64_t finish_time = jam2::monotonic_us();
    network_session.finish(finish_time);
    sync_peer_stats();

    stats.elapsed_ms = (finish_time - start_time) / 1000ULL;
    stats.final_metronome_enabled = runtime.metronome.load(std::memory_order_relaxed);
    stats.final_bpm = runtime.bpm.load(std::memory_order_relaxed);
    stats.final_metronome_level = gain_from_ppm(runtime.metronome_level_ppm.load(std::memory_order_relaxed));
    stats.final_remote_level = gain_from_ppm(runtime.remote_level_ppm.load(std::memory_order_relaxed));
    stats.final_send_level = gain_from_ppm(runtime.send_level_ppm.load(std::memory_order_relaxed));
    stats.final_local_monitor_enabled = runtime.local_monitor.load(std::memory_order_relaxed);
    stats.final_local_monitor_level = gain_from_ppm(runtime.local_monitor_level_ppm.load(std::memory_order_relaxed));
    stats.metronome_epoch_sample_time = runtime.metronome_epoch_sample_time.load(std::memory_order_relaxed);
    stats.metronome_alignment_valid = runtime.metronome_epoch_valid.load(std::memory_order_relaxed);
    stats.metronome_compensation_offset_frames = runtime.metronome_render_offset_frames.load(std::memory_order_relaxed);
    if (csv_log != nullptr) {
        csv_log->write(
            "final",
            stats.elapsed_ms,
            stats,
            options,
            make_audio_snapshot(audio_stream, capture_ring, playback_ring, audio_control));
    }
    network_session.close();
    return stats;
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

template <typename T>
struct EngineObserver {
    T* value = nullptr;

    T* get() const noexcept { return value; }
    T* operator->() const noexcept { return value; }
    explicit operator bool() const noexcept { return value != nullptr; }
    bool operator==(std::nullptr_t) const noexcept { return value == nullptr; }
    bool operator!=(std::nullptr_t) const noexcept { return value != nullptr; }
};

struct OptionalAudioStream {
    std::unique_ptr<jam2::Engine> engine;
    EngineObserver<jam2::audio::StreamControl> control;
    EngineObserver<jam2::audio::OutputRecorder> recorder;
    EngineObserver<jam2::audio::TrackTakeRecorder> track_take_recorder;
    EngineObserver<jam2::audio::PreparedTrackSource> prepared_source;
    EngineObserver<jam2::audio::MonoRingBuffer> capture_ring;
    EngineObserver<jam2::audio::MonoRingBuffer> playback_ring;
    EngineObserver<jam2::audio::DeviceStream> stream;
    jam2::NetworkCaptureAttachment network_capture;
};

OptionalAudioStream start_optional_audio(const Options& options, bool leader_audio_local_click)
{
    OptionalAudioStream audio;
    if (!options.audio_device_id && !options.headless_audio) {
        return audio;
    }
    const bool diagnostics_enabled =
        options.stats_enabled && (options.log_stats_dir.has_value() || options.stats_interval_ms > 0);
    jam2::EngineConfig config;
    config.backend = options.headless_audio
        ? jam2::EngineAudioBackend::Headless
        : jam2::EngineAudioBackend::Device;
    config.audio_device_id = options.audio_device_id.value_or(-1);
    config.sample_rate = options.sample_rate;
    config.audio_buffer_frames = options.headless_audio && options.audio_buffer_size <= 0
        ? static_cast<long>(options.frame_size)
        : options.audio_buffer_size;
    config.input_channels = options.input_channels;
    config.channels = options.channel_selection;
    config.capture_ring_frames = options.capture_ring_frames;
    config.playback_ring_frames = options.playback_ring_frames;
    config.playback_prefill_frames = options.playback_prefill_frames;
    config.diagnostics_enabled = diagnostics_enabled;
    config.metronome_enabled = options.metronome;
    config.metronome_pattern = jam2::metronome::sanitize({options.bpm, 4, 1, 4, 0x0fULL, 0, 0x01ULL, 0});
    config.metronome_level_ppm = ppm_from_gain(options.metronome_level);
    config.remote_level_ppm = ppm_from_gain(options.remote_level);
    config.send_level_ppm = ppm_from_gain(options.send_level);
    config.local_monitor_enabled = options.local_monitor;
    config.local_monitor_level_ppm = ppm_from_gain(options.local_monitor_level);
    config.metronome_mode = static_cast<jam2::EngineMetronomeMode>(metronome_mode_id(options.metronome_mode));
    config.leader_audio_local_click = leader_audio_local_click;
    config.test_input = static_cast<jam2::EngineTestInput>(test_input_mode_id(options.test_input));
    config.test_input_level_ppm = 125000;
    config.prepared_track_max_frames =
        static_cast<std::size_t>(std::max(1, options.sample_rate)) * 60U * 5U;

    audio.engine = std::make_unique<jam2::Engine>();
    audio.engine->start(config);
    const auto view = audio.engine->compatibilityView();
    audio.control.value = view.control;
    audio.recorder.value = view.recorder;
    audio.track_take_recorder.value = view.track_take_recorder;
    audio.prepared_source.value = view.prepared_source;
    audio.capture_ring.value = view.capture_ring;
    audio.playback_ring.value = view.playback_ring;
    audio.stream.value = view.stream;
    return audio;
}

void attach_network_capture(OptionalAudioStream& audio)
{
    if (!audio.engine || !audio.stream || !audio.capture_ring) {
        return;
    }
    audio.network_capture = audio.engine->attachNetworkCapture();
    if (audio.network_capture.generation == 0) {
        throw std::runtime_error("failed to attach the local engine capture tap");
    }
    const std::uint64_t deadline = jam2::monotonic_us() + 5000000ULL;
    while (!audio.engine->networkCaptureReady(audio.network_capture)) {
        if (jam2::monotonic_us() >= deadline) {
            audio.engine->detachNetworkCapture(audio.network_capture);
            audio.network_capture = {};
            throw std::runtime_error("audio callback did not acknowledge the network capture epoch within 5 seconds");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void detach_network_capture(OptionalAudioStream& audio) noexcept
{
    if (audio.engine && audio.network_capture.generation != 0) {
        audio.engine->detachNetworkCapture(audio.network_capture);
        audio.network_capture = {};
        if (audio.control) {
            const std::uint64_t deadline = jam2::monotonic_us() + 5000000ULL;
            while (audio.control->network_capture_generation_applied.load(std::memory_order_acquire) !=
                   audio.control->network_capture_generation_requested.load(std::memory_order_acquire)) {
                if (jam2::monotonic_us() >= deadline) {
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
}

int drain_pending_udp(jam2::UdpSocket& socket)
{
    int drained = 0;
    for (;;) {
        const auto received = socket.recv_from(0);
        if (!received) {
            break;
        }
        ++drained;
    }
    return drained;
}

struct CommandThread {
    RuntimeState state;
    std::thread thread;

    CommandThread(
        const Options& options,
        jam2::audio::OutputRecorder* recorder,
        jam2::audio::PreparedTrackSource* prepared_source,
        jam2::audio::StreamControl* audio_control,
        int recording_sample_rate,
        bool leader_audio_local_click)
    {
        state.metronome.store(options.metronome, std::memory_order_relaxed);
        state.bpm.store(options.bpm, std::memory_order_relaxed);
        state.metronome_beats_per_bar.store(4, std::memory_order_relaxed);
        state.metronome_division.store(1, std::memory_order_relaxed);
        state.metronome_step_count.store(4, std::memory_order_relaxed);
        state.metronome_play_mask_low.store(0x0fULL, std::memory_order_relaxed);
        state.metronome_play_mask_high.store(0, std::memory_order_relaxed);
        state.metronome_accent_mask_low.store(0x01ULL, std::memory_order_relaxed);
        state.metronome_accent_mask_high.store(0, std::memory_order_relaxed);
        state.metronome_level_ppm.store(ppm_from_gain(options.metronome_level), std::memory_order_relaxed);
        state.remote_level_ppm.store(ppm_from_gain(options.remote_level), std::memory_order_relaxed);
        state.send_level_ppm.store(ppm_from_gain(options.send_level), std::memory_order_relaxed);
        state.local_monitor.store(options.local_monitor, std::memory_order_relaxed);
        state.local_monitor_level_ppm.store(ppm_from_gain(options.local_monitor_level), std::memory_order_relaxed);
        state.metronome_mode.store(metronome_mode_id(options.metronome_mode), std::memory_order_relaxed);
        state.leader_audio_local_click.store(leader_audio_local_click, std::memory_order_relaxed);
        state.metronome_local_authority.store(leader_audio_local_click, std::memory_order_relaxed);
        state.metronome_epoch_sample_time.store(0, std::memory_order_relaxed);
        state.metronome_epoch_valid.store(true, std::memory_order_relaxed);
        state.metronome_render_offset_frames.store(0, std::memory_order_relaxed);
        state.stats_enabled.store(options.stats_enabled, std::memory_order_relaxed);
        thread = std::thread([this, recorder, prepared_source, audio_control, recording_sample_rate, options] {
            stdin_command_loop(state, recorder, prepared_source, audio_control, recording_sample_rate, options);
        });
    }

    ~CommandThread()
    {
        state.quit.store(true, std::memory_order_relaxed);
        if (thread.joinable()) {
            thread.detach();
        }
    }
};

#if defined(_WIN32)
using GuiSocketHandle = SOCKET;
constexpr GuiSocketHandle kInvalidGuiSocket = INVALID_SOCKET;
#else
using GuiSocketHandle = int;
constexpr GuiSocketHandle kInvalidGuiSocket = -1;
#endif

void close_gui_socket(GuiSocketHandle socket)
{
    if (socket == kInvalidGuiSocket) {
        return;
    }
#if defined(_WIN32)
    closesocket(socket);
#else
    close(socket);
#endif
}

GuiSocketHandle connect_gui_socket(const jam2::Endpoint& endpoint)
{
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* results = nullptr;
    const std::string port = std::to_string(endpoint.port);
    if (getaddrinfo(endpoint.host.c_str(), port.c_str(), &hints, &results) != 0 || results == nullptr) {
        return kInvalidGuiSocket;
    }

    GuiSocketHandle connected = kInvalidGuiSocket;
    for (addrinfo* address = results; address != nullptr; address = address->ai_next) {
        GuiSocketHandle socket = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (socket == kInvalidGuiSocket) {
            continue;
        }
        if (::connect(socket, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) {
            connected = socket;
            break;
        }
        close_gui_socket(socket);
    }
    freeaddrinfo(results);
    return connected;
}

bool gui_send_all(GuiSocketHandle socket, std::string_view text)
{
    const char* cursor = text.data();
    std::size_t remaining = text.size();
    while (remaining > 0) {
        const int chunk = static_cast<int>(std::min<std::size_t>(remaining, 16 * 1024));
        const int sent = send(socket, cursor, chunk, 0);
        if (sent <= 0) {
            return false;
        }
        cursor += sent;
        remaining -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool gui_send_bytes(GuiSocketHandle socket, const std::vector<std::uint8_t>& bytes)
{
    const char* cursor = reinterpret_cast<const char*>(bytes.data());
    std::size_t remaining = bytes.size();
    while (remaining > 0) {
        const int chunk = static_cast<int>(std::min<std::size_t>(remaining, 16 * 1024));
        const int sent = send(socket, cursor, chunk, 0);
        if (sent <= 0) {
            return false;
        }
        cursor += sent;
        remaining -= static_cast<std::size_t>(sent);
    }
    return true;
}

void append_u16(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xffU));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffU));
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_u64(std::vector<std::uint8_t>& out, std::uint64_t value)
{
    for (int shift = 0; shift < 64; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
}

void append_i64(std::vector<std::uint8_t>& out, std::int64_t value)
{
    append_u64(out, static_cast<std::uint64_t>(value));
}

void append_f64(std::vector<std::uint8_t>& out, double value)
{
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    append_u64(out, bits);
}

std::uint16_t payload_u16(const std::vector<std::uint8_t>& in, std::size_t offset)
{
    return static_cast<std::uint16_t>(in[offset]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[offset + 1]) << 8);
}

std::uint32_t payload_u32(const std::vector<std::uint8_t>& in, std::size_t offset)
{
    std::uint32_t value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        value |= static_cast<std::uint32_t>(in[offset + static_cast<std::size_t>(shift / 8)]) << shift;
    }
    return value;
}

std::uint64_t payload_u64(const std::vector<std::uint8_t>& in, std::size_t offset)
{
    std::uint64_t value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
        value |= static_cast<std::uint64_t>(in[offset + static_cast<std::size_t>(shift / 8)]) << shift;
    }
    return value;
}

std::int64_t payload_i64(const std::vector<std::uint8_t>& in, std::size_t offset)
{
    return static_cast<std::int64_t>(payload_u64(in, offset));
}

double payload_f64(const std::vector<std::uint8_t>& in, std::size_t offset)
{
    const std::uint64_t bits = payload_u64(in, offset);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::vector<std::uint8_t> gui_frame(jam2::gui_control::MessageType type, std::uint32_t sequence, const std::vector<std::uint8_t>& payload)
{
    std::vector<std::uint8_t> out;
    out.reserve(sizeof(jam2::gui_control::Header) + payload.size());
    append_u32(out, jam2::gui_control::kMagic);
    append_u16(out, jam2::gui_control::kVersion);
    append_u16(out, static_cast<std::uint16_t>(type));
    append_u32(out, static_cast<std::uint32_t>(payload.size()));
    append_u32(out, sequence);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

bool gui_send_frame(GuiSocketHandle socket, jam2::gui_control::MessageType type, std::uint32_t sequence, const std::vector<std::uint8_t>& payload)
{
    return gui_send_bytes(socket, gui_frame(type, sequence, payload));
}

struct GuiCommandPayload {
    jam2::gui_control::CommandOpcode opcode = jam2::gui_control::CommandOpcode::None;
    std::uint16_t flags = 0;
    std::array<std::int64_t, 8> i{};
    std::array<double, 4> d{};
    std::string text;
};

bool decode_command_payload(const std::vector<std::uint8_t>& payload, GuiCommandPayload& command)
{
    constexpr std::size_t fixed_size = 2 + 2 + 4 + 8 * 8 + 4 * 8;
    if (payload.size() < fixed_size) {
        return false;
    }
    command.opcode = static_cast<jam2::gui_control::CommandOpcode>(payload_u16(payload, 0));
    command.flags = payload_u16(payload, 2);
    const std::uint32_t text_size = payload_u32(payload, 4);
    if (payload.size() != fixed_size + text_size) {
        return false;
    }
    std::size_t offset = 8;
    for (std::int64_t& value : command.i) {
        value = payload_i64(payload, offset);
        offset += 8;
    }
    for (double& value : command.d) {
        value = payload_f64(payload, offset);
        offset += 8;
    }
    command.text.assign(
        reinterpret_cast<const char*>(payload.data() + fixed_size),
        static_cast<std::size_t>(text_size));
    return true;
}

int gui_select_width(GuiSocketHandle socket)
{
#if defined(_WIN32)
    (void)socket;
    return 0;
#else
    return socket + 1;
#endif
}

struct GuiControlThread {
    const Options& options;
    RuntimeState& state;
    jam2::audio::OutputRecorder* recorder = nullptr;
    jam2::audio::PreparedTrackSource* prepared_source = nullptr;
    jam2::audio::TrackTakeRecorder* track_take_recorder = nullptr;
    int recording_sample_rate = 0;
    const jam2::audio::DeviceStream* audio_stream = nullptr;
    const jam2::audio::MonoRingBuffer* capture_ring = nullptr;
    const jam2::audio::MonoRingBuffer* playback_ring = nullptr;
    jam2::audio::StreamControl* audio_control = nullptr;
    std::atomic<bool> stop{false};
    std::thread thread;
    std::mutex socket_mutex;
    GuiSocketHandle socket = kInvalidGuiSocket;

    GuiControlThread(
        const Options& options_in,
        RuntimeState& state_in,
        jam2::audio::OutputRecorder* recorder_in,
        jam2::audio::PreparedTrackSource* prepared_source_in,
        jam2::audio::TrackTakeRecorder* track_take_recorder_in,
        int recording_sample_rate_in,
        const jam2::audio::DeviceStream* audio_stream_in,
        const jam2::audio::MonoRingBuffer* capture_ring_in,
        const jam2::audio::MonoRingBuffer* playback_ring_in,
        jam2::audio::StreamControl* audio_control_in)
        : options(options_in)
        , state(state_in)
        , recorder(recorder_in)
        , prepared_source(prepared_source_in)
        , track_take_recorder(track_take_recorder_in)
        , recording_sample_rate(recording_sample_rate_in)
        , audio_stream(audio_stream_in)
        , capture_ring(capture_ring_in)
        , playback_ring(playback_ring_in)
        , audio_control(audio_control_in)
    {
        if (!options.gui_control) {
            return;
        }
        thread = std::thread([this] { run(); });
    }

    ~GuiControlThread()
    {
        stop.store(true, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(socket_mutex);
            close_gui_socket(socket);
            socket = kInvalidGuiSocket;
        }
        if (thread.joinable()) {
            thread.join();
        }
    }

    bool apply_binary_command(const GuiCommandPayload& command)
    {
        switch (command.opcode) {
        case jam2::gui_control::CommandOpcode::MetronomeEnabled: {
            const bool enabled = command.flags != 0;
            const bool was_enabled = state.metronome.exchange(enabled, std::memory_order_relaxed);
            if (enabled && !was_enabled) {
                begin_metronome_epoch(state, audio_control, recording_sample_rate);
            }
            request_grid_revision(state);
            return true;
        }
        case jam2::gui_control::CommandOpcode::MetronomeLeader: {
            const bool local_authority = command.flags != 0;
            state.leader_audio_local_click.store(local_authority, std::memory_order_relaxed);
            state.metronome_local_authority.store(local_authority, std::memory_order_relaxed);
            return true;
        }
        case jam2::gui_control::CommandOpcode::MetronomeMode: {
            const int previous_mode = state.metronome_mode.exchange(
                metronome_mode_id(parse_metronome_mode(command.text)),
                std::memory_order_relaxed);
            if (previous_mode != state.metronome_mode.load(std::memory_order_relaxed)) {
                state.metronome_render_offset_frames.store(0, std::memory_order_relaxed);
                request_grid_revision(state);
                state.metronome_epoch_revision.fetch_add(1, std::memory_order_relaxed);
            }
            return true;
        }
        case jam2::gui_control::CommandOpcode::MetronomeLevel:
            state.metronome_level_ppm.store(ppm_from_gain(command.d[0]), std::memory_order_relaxed);
            return true;
        case jam2::gui_control::CommandOpcode::MetronomePattern: {
            jam2::metronome::PatternSnapshot pattern;
            pattern.bpm = static_cast<int>(command.i[0]);
            pattern.beats_per_bar = static_cast<int>(command.i[1]);
            pattern.division = static_cast<int>(command.i[2]);
            pattern.step_count = jam2::metronome::pattern_step_count(pattern.beats_per_bar, pattern.division);
            pattern.play_mask_low = static_cast<std::uint64_t>(command.i[3]);
            pattern.play_mask_high = static_cast<std::uint64_t>(command.i[4]);
            pattern.accent_mask_low = static_cast<std::uint64_t>(command.i[5]);
            pattern.accent_mask_high = static_cast<std::uint64_t>(command.i[6]);
            const auto previous_pattern = metronome_pattern_from_runtime(state);
            pattern = jam2::metronome::sanitize(pattern);
            store_metronome_pattern(state, pattern);
            request_grid_revision(state);
            if (previous_pattern.bpm != pattern.bpm ||
                previous_pattern.beats_per_bar != pattern.beats_per_bar ||
                previous_pattern.division != pattern.division) {
                state.metronome_epoch_revision.fetch_add(1, std::memory_order_relaxed);
            }
            return true;
        }
        case jam2::gui_control::CommandOpcode::RemoteLevel:
            state.remote_level_ppm.store(ppm_from_gain(command.d[0]), std::memory_order_relaxed);
            return true;
        case jam2::gui_control::CommandOpcode::SendLevel:
            state.send_level_ppm.store(ppm_from_gain(command.d[0]), std::memory_order_relaxed);
            return true;
        case jam2::gui_control::CommandOpcode::MonitorEnabled:
            state.local_monitor.store(command.flags != 0, std::memory_order_relaxed);
            return true;
        case jam2::gui_control::CommandOpcode::MonitorLevel:
            state.local_monitor_level_ppm.store(ppm_from_gain(command.d[0]), std::memory_order_relaxed);
            return true;
        case jam2::gui_control::CommandOpcode::Bpm:
            if (command.i[0] > 0 && command.i[0] <= 400) {
                state.bpm.store(static_cast<int>(command.i[0]), std::memory_order_relaxed);
                request_grid_revision(state);
                state.metronome_epoch_revision.fetch_add(1, std::memory_order_relaxed);
            }
            return true;
        case jam2::gui_control::CommandOpcode::RecordJamStart:
            if (recorder != nullptr && !command.text.empty()) {
                std::lock_guard<std::mutex> lock(state.recording_mutex);
                std::string error;
                (void)recorder->start(std::filesystem::path(command.text), recording_sample_rate, error);
            }
            return true;
        case jam2::gui_control::CommandOpcode::RecordJamStop:
            if (recorder != nullptr) {
                std::lock_guard<std::mutex> lock(state.recording_mutex);
                const auto before = recorder->stats();
                std::string error;
                (void)recorder->stop(error);
                const auto after = recorder->stats();
                if (!before.folder.empty()) {
                    write_recording_sidecar(std::filesystem::path(before.folder), after, options, state);
                }
            }
            return true;
        case jam2::gui_control::CommandOpcode::TrackLoad:
            if (prepared_source != nullptr && !command.text.empty()) {
                const PreparedLoadResult result = load_prepared_pcm16_wav(
                    *prepared_source,
                    std::filesystem::path(command.text),
                    recording_sample_rate);
                if (result.ok) {
                    enqueue_prepared_command(
                        prepared_source,
                        audio_control,
                        {jam2::audio::PreparedTrackSource::CommandType::SetLoop, 0, 0, 0, result.frames, 1000000});
                }
            }
            return true;
        case jam2::gui_control::CommandOpcode::TrackPlay:
            enqueue_prepared_command(prepared_source, audio_control, {jam2::audio::PreparedTrackSource::CommandType::Play, 0, static_cast<std::uint64_t>(command.i[0]), 0, 0, 1000000});
            return true;
        case jam2::gui_control::CommandOpcode::TrackRestartQuantized: {
            if (prepared_source == nullptr || audio_control == nullptr) {
                return false;
            }
            const QuantizedSchedule schedule = next_bar_schedule(state, audio_control, recording_sample_rate, 0);
            if (!enqueue_prepared_restart(prepared_source, audio_control, schedule.target_raw_frame)) {
                return false;
            }
            publish_transport_schedule(
                state,
                jam2::gui_control::TransportAction::TrackRestart,
                schedule,
                true);
            return true;
        }
        case jam2::gui_control::CommandOpcode::TrackStop:
            enqueue_prepared_command(prepared_source, audio_control, {jam2::audio::PreparedTrackSource::CommandType::Stop, 0, static_cast<std::uint64_t>(command.i[0]), 0, 0, 1000000});
            return true;
        case jam2::gui_control::CommandOpcode::TrackSeek:
            enqueue_prepared_command(prepared_source, audio_control, {jam2::audio::PreparedTrackSource::CommandType::Seek, 0, static_cast<std::uint64_t>(command.i[1]), static_cast<std::uint64_t>(command.i[0]), 0, 1000000});
            return true;
        case jam2::gui_control::CommandOpcode::TrackLevel: {
            const int level_ppm = ppm_from_gain(command.d[0]);
            enqueue_prepared_command(prepared_source, audio_control, {jam2::audio::PreparedTrackSource::CommandType::SetLevel, 0, 0, 0, 0, level_ppm});
            return true;
        }
        case jam2::gui_control::CommandOpcode::TrackLoop: {
            std::uint64_t start = 0;
            std::uint64_t end = 0;
            if (command.flags != 0) {
                start = command.i[0] > 0 ? static_cast<std::uint64_t>(command.i[0]) : 0ULL;
                end = command.i[1] > command.i[0] ? static_cast<std::uint64_t>(command.i[1]) : std::numeric_limits<std::uint64_t>::max();
            }
            enqueue_prepared_command(prepared_source, audio_control, {jam2::audio::PreparedTrackSource::CommandType::SetLoop, 0, 0, start, end, 1000000});
            return true;
        }
        case jam2::gui_control::CommandOpcode::TrackTakeArmInput:
            if (track_take_recorder != nullptr) {
                const std::size_t split = command.text.find('\n');
                if (split != std::string::npos) {
                    std::lock_guard<std::mutex> lock(state.track_take_mutex);
                    std::string error;
                    (void)track_take_recorder->arm(
                        command.text.substr(0, split),
                        std::filesystem::path(command.text.substr(split + 1)),
                        recording_sample_rate,
                        error);
                }
            }
            return true;
        case jam2::gui_control::CommandOpcode::TrackTakeStart:
            if (track_take_recorder != nullptr) {
                std::lock_guard<std::mutex> lock(state.track_take_mutex);
                std::string error;
                (void)track_take_recorder->start_at(static_cast<std::uint64_t>(command.i[0]), error);
            }
            return true;
        case jam2::gui_control::CommandOpcode::TrackTakeStartQuantized:
            if (track_take_recorder != nullptr && prepared_source != nullptr && audio_control != nullptr) {
                const QuantizedSchedule schedule = next_bar_schedule(
                    state,
                    audio_control,
                    recording_sample_rate,
                    static_cast<int>(std::max<std::int64_t>(0, command.i[0])));
                std::lock_guard<std::mutex> lock(state.track_take_mutex);
                std::string error;
                if (!track_take_recorder->start_at(schedule.target_raw_frame, error)) {
                    return false;
                }
                if (!enqueue_prepared_restart(prepared_source, audio_control, schedule.target_raw_frame)) {
                    track_take_recorder->cancel();
                    return false;
                }
                publish_transport_schedule(
                    state,
                    jam2::gui_control::TransportAction::RecordStart,
                    schedule,
                    true);
            }
            return true;
        case jam2::gui_control::CommandOpcode::TrackTakeStop:
            if (track_take_recorder != nullptr) {
                std::lock_guard<std::mutex> lock(state.track_take_mutex);
                std::string error;
                (void)track_take_recorder->stop_at(static_cast<std::uint64_t>(command.i[0]), error);
            }
            return true;
        case jam2::gui_control::CommandOpcode::TrackTakeCancel:
            if (track_take_recorder != nullptr) {
                std::lock_guard<std::mutex> lock(state.track_take_mutex);
                track_take_recorder->cancel();
            }
            return true;
        case jam2::gui_control::CommandOpcode::RecordingLatencyAdjustment:
            set_recording_latency_adjustment(audio_control, command.i[0]);
            return audio_control != nullptr;
        case jam2::gui_control::CommandOpcode::RequestSnapshot:
            return true;
        case jam2::gui_control::CommandOpcode::None:
        default:
            return false;
        }
    }

    void send_ack(GuiSocketHandle connected, std::uint32_t sequence, bool ok)
    {
        std::vector<std::uint8_t> payload;
        append_u16(payload, ok ? 1U : 0U);
        append_u16(payload, 0);
        (void)gui_send_frame(connected, jam2::gui_control::MessageType::CommandAck, sequence, payload);
    }

    void run() noexcept
    {
        try {
            const GuiSocketHandle connected = connect_gui_socket(*options.gui_control);
            if (connected == kInvalidGuiSocket) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(socket_mutex);
                socket = connected;
            }
            (void)gui_send_frame(connected, jam2::gui_control::MessageType::Hello, 0, {});
            std::vector<std::uint8_t> input_buffer;
            std::uint64_t next_meter_us = 0;
            while (!stop.load(std::memory_order_relaxed) && !state.quit.load(std::memory_order_relaxed)) {
                fd_set read_set;
                FD_ZERO(&read_set);
                FD_SET(connected, &read_set);
                timeval timeout{};
                timeout.tv_sec = 0;
                timeout.tv_usec = 20000;
                const int ready = select(gui_select_width(connected), &read_set, nullptr, nullptr, &timeout);
                if (ready > 0 && FD_ISSET(connected, &read_set)) {
                    char buffer[1024];
                    const int received = recv(connected, buffer, static_cast<int>(sizeof(buffer)), 0);
                    if (received <= 0) {
                        break;
                    }
                    input_buffer.insert(
                        input_buffer.end(),
                        reinterpret_cast<const std::uint8_t*>(buffer),
                        reinterpret_cast<const std::uint8_t*>(buffer) + received);
                    for (;;) {
                        constexpr std::size_t header_size = 16;
                        if (input_buffer.size() < header_size) {
                            break;
                        }
                        const std::uint32_t magic = payload_u32(input_buffer, 0);
                        const std::uint16_t version = payload_u16(input_buffer, 4);
                        const auto type = static_cast<jam2::gui_control::MessageType>(payload_u16(input_buffer, 6));
                        const std::uint32_t payload_size = payload_u32(input_buffer, 8);
                        const std::uint32_t sequence = payload_u32(input_buffer, 12);
                        if (magic != jam2::gui_control::kMagic ||
                            version != jam2::gui_control::kVersion ||
                            payload_size > jam2::gui_control::kMaxPayloadBytes) {
                            return;
                        }
                        if (input_buffer.size() < header_size + payload_size) {
                            break;
                        }
                        std::vector<std::uint8_t> payload(
                            input_buffer.begin() + static_cast<std::ptrdiff_t>(header_size),
                            input_buffer.begin() + static_cast<std::ptrdiff_t>(header_size + payload_size));
                        input_buffer.erase(
                            input_buffer.begin(),
                            input_buffer.begin() + static_cast<std::ptrdiff_t>(header_size + payload_size));
                        if (type == jam2::gui_control::MessageType::Command) {
                            GuiCommandPayload command;
                            const bool ok = decode_command_payload(payload, command) && apply_binary_command(command);
                            send_ack(connected, sequence, ok);
                        }
                    }
                } else if (ready < 0) {
                    break;
                }

                const std::uint64_t now_us = jam2::monotonic_us();
                if (now_us >= next_meter_us) {
                    if (!send_clock_state(connected) ||
                        !send_transport_state(connected) ||
                        !send_track_events(connected) ||
                        !send_meters(connected)) {
                        break;
                    }
                    next_meter_us = now_us + 16667;
                }
            }
        } catch (const std::exception&) {
        }

        std::lock_guard<std::mutex> lock(socket_mutex);
        close_gui_socket(socket);
        socket = kInvalidGuiSocket;
    }

    bool send_meters(GuiSocketHandle connected)
    {
        (void)audio_stream;
        (void)capture_ring;
        (void)playback_ring;
        double input_peak = 0.0;
        double send_peak = 0.0;
        double monitor_peak = 0.0;
        double remote_peak = 0.0;
        double metronome_peak = 0.0;
        double output_peak = 0.0;
        std::uint64_t output_clipped_samples = 0;
        if (audio_control != nullptr) {
            input_peak = unit_from_ppm(audio_control->gui_input_peak_ppm.exchange(0, std::memory_order_relaxed));
            send_peak = unit_from_ppm(audio_control->gui_send_peak_ppm.exchange(0, std::memory_order_relaxed));
            monitor_peak = unit_from_ppm(audio_control->gui_monitor_peak_ppm.exchange(0, std::memory_order_relaxed));
            remote_peak = unit_from_ppm(audio_control->gui_remote_peak_ppm.exchange(0, std::memory_order_relaxed));
            metronome_peak = unit_from_ppm(audio_control->gui_metronome_peak_ppm.exchange(0, std::memory_order_relaxed));
            output_peak = unit_from_ppm(audio_control->gui_output_peak_ppm.exchange(0, std::memory_order_relaxed));
            output_clipped_samples = audio_control->output_clipped_samples.load(std::memory_order_relaxed);
        }
        std::vector<std::uint8_t> payload;
        append_f64(payload, input_peak);
        append_f64(payload, send_peak);
        append_f64(payload, monitor_peak);
        append_f64(payload, remote_peak);
        append_f64(payload, metronome_peak);
        append_f64(payload, output_peak);
        append_u64(payload, output_clipped_samples);
        return gui_send_frame(connected, jam2::gui_control::MessageType::Meters, 0, payload);
    }

    bool send_clock_state(GuiSocketHandle connected)
    {
        commit_due_transport(state, audio_control);
        const std::uint64_t raw_frame = current_engine_frame(audio_control);
        const std::int64_t offset = state.metronome_render_offset_frames.load(std::memory_order_relaxed);
        std::uint64_t musical_frame = raw_frame;
        if (offset < 0) {
            const std::uint64_t magnitude = static_cast<std::uint64_t>(-offset);
            musical_frame = musical_frame > magnitude ? musical_frame - magnitude : 0ULL;
        } else {
            musical_frame += static_cast<std::uint64_t>(offset);
        }
        const auto pattern = metronome_pattern_from_runtime(state);
        std::vector<std::uint8_t> payload;
        append_u64(payload, raw_frame);
        append_u64(payload, musical_frame);
        append_u64(payload, state.metronome_epoch_sample_time.load(std::memory_order_relaxed));
        append_i64(payload, offset);
        append_u32(payload, static_cast<std::uint32_t>(recording_sample_rate));
        append_u32(payload, static_cast<std::uint32_t>(pattern.bpm));
        append_u32(payload, static_cast<std::uint32_t>(pattern.beats_per_bar));
        append_u32(payload, static_cast<std::uint32_t>(pattern.division));
        append_u32(payload, static_cast<std::uint32_t>(state.metronome_revision.load(std::memory_order_relaxed)));
        append_u32(payload, static_cast<std::uint32_t>(state.metronome_mode.load(std::memory_order_relaxed)));
        append_u16(payload, audio_control != nullptr &&
                state.metronome_epoch_valid.load(std::memory_order_relaxed)
            ? 1U
            : 0U);
        append_u16(payload, 0);
        append_u32(payload, audio_control != nullptr
            ? audio_control->input_latency_frames.load(std::memory_order_relaxed)
            : 0U);
        append_u32(payload, audio_control != nullptr
            ? audio_control->output_latency_frames.load(std::memory_order_relaxed)
            : 0U);
        append_i64(payload, audio_control != nullptr
            ? audio_control->recording_latency_adjustment_frames.load(std::memory_order_relaxed)
            : 0);
        append_u64(payload, audio_control != nullptr
            ? audio_control->recording_latency_compensation_frames.load(std::memory_order_relaxed)
            : 0ULL);
        return gui_send_frame(connected, jam2::gui_control::MessageType::ClockState, 0, payload);
    }

    bool send_transport_state(GuiSocketHandle connected)
    {
        std::vector<std::uint8_t> payload;
        {
            std::lock_guard<std::mutex> lock(state.transport_mutex);
            append_u64(payload, state.transport_revision.load(std::memory_order_relaxed));
            append_u64(payload, state.transport_target_raw_frame.load(std::memory_order_relaxed));
            append_u64(payload, state.transport_target_musical_frame.load(std::memory_order_relaxed));
            append_u64(payload, state.transport_countdown_start_frame.load(std::memory_order_relaxed));
            append_u16(payload, static_cast<std::uint16_t>(state.transport_action.load(std::memory_order_relaxed)));
            append_u16(payload, state.transport_pending.load(std::memory_order_relaxed) ? 1U : 0U);
        }
        append_u32(payload, 0);
        return gui_send_frame(connected, jam2::gui_control::MessageType::TransportState, 0, payload);
    }

    bool send_track_events(GuiSocketHandle connected)
    {
        if (track_take_recorder != nullptr) {
            const auto completion = track_take_recorder->consume_completion();
            if (completion.available) {
                std::vector<std::uint8_t> payload;
                append_u16(payload, static_cast<std::uint16_t>(
                    completion.ok ? jam2::gui_control::TrackTakeEventType::Stopped : jam2::gui_control::TrackTakeEventType::Error));
                append_u16(payload, 0);
                append_u32(payload, static_cast<std::uint32_t>(completion.take_id.size()));
                append_u32(payload, static_cast<std::uint32_t>(completion.output_path.size()));
                append_u32(payload, static_cast<std::uint32_t>(completion.error.size()));
                append_u32(payload, static_cast<std::uint32_t>(completion.sample_rate));
                append_u64(payload, completion.start_frame);
                append_u64(payload, completion.stop_frame);
                append_u64(payload, completion.frames_written);
                append_u64(payload, completion.dropped_frames);
                append_u64(payload, completion.writer_errors);
                payload.insert(payload.end(), completion.take_id.begin(), completion.take_id.end());
                payload.insert(payload.end(), completion.output_path.begin(), completion.output_path.end());
                payload.insert(payload.end(), completion.error.begin(), completion.error.end());
                if (!gui_send_frame(connected, jam2::gui_control::MessageType::TrackTakeEvent, 0, payload)) {
                    return false;
                }
            }
        }

        const std::uint64_t engine_frame = current_engine_frame(audio_control);
        const std::uint64_t source_frame = audio_control != nullptr ?
            audio_control->prepared_source_frame.load(std::memory_order_relaxed) :
            0ULL;
        const bool playing = prepared_source != nullptr && prepared_source->playing();
        std::vector<std::uint8_t> payload;
        append_u64(payload, engine_frame);
        append_u64(payload, source_frame);
        append_u64(payload, audio_control != nullptr ? audio_control->prepared_source_scheduled_start_frame.load(std::memory_order_relaxed) : 0ULL);
        append_u64(payload, audio_control != nullptr ? audio_control->prepared_source_actual_start_frame.load(std::memory_order_relaxed) : 0ULL);
        append_u64(payload, audio_control != nullptr ? audio_control->prepared_source_underruns.load(std::memory_order_relaxed) : 0ULL);
        append_u64(payload, audio_control != nullptr ? audio_control->prepared_source_busy_events.load(std::memory_order_relaxed) : 0ULL);
        append_u32(payload, static_cast<std::uint32_t>(recording_sample_rate));
        append_u16(payload, playing ? 1U : 0U);
        append_u16(payload, 0);
        return gui_send_frame(connected, jam2::gui_control::MessageType::TrackState, 0, payload);
    }
};

void finalize_active_recording(OptionalAudioStream& audio, const Options& options, RuntimeState& state)
{
    if (!audio.recorder) {
        return;
    }
    std::lock_guard<std::mutex> lock(state.recording_mutex);
    const auto before = audio.recorder->stats();
    if (!before.active) {
        return;
    }
    std::string error;
    const bool ok = audio.recorder->stop(error);
    const auto after = audio.recorder->stats();
    if (!before.folder.empty()) {
        write_recording_sidecar(std::filesystem::path(before.folder), after, options, state);
    }
    if (!ok) {
        std::cerr << "record jam finalization failed: " << error << "\n";
    }
}

void start_startup_recording(
    OptionalAudioStream& audio,
    const Options& options,
    RuntimeState& state,
    int recording_sample_rate)
{
    if (!options.record_jam_folder || !audio.recorder) {
        return;
    }
    std::lock_guard<std::mutex> lock(state.recording_mutex);
    std::string error;
    if (!audio.recorder->start(*options.record_jam_folder, recording_sample_rate, error)) {
        std::cerr << "record jam start failed: " << error << "\n";
    }
}

void print_optional_audio_stats(const OptionalAudioStream& audio, const Options& options)
{
    if (!audio.stream || !audio.capture_ring || !audio.playback_ring) {
        return;
    }
    const auto capture_stats = audio.capture_ring->stats();
    const auto playback_stats = audio.playback_ring->stats();
    const auto callback_stats = audio.stream->callback_timing_stats();
    const std::size_t capture_readable = audio.capture_ring->available_read();
    const std::size_t playback_readable = audio.playback_ring->available_read();
    const auto stream_info = audio.stream->info();
    const auto engine_snapshot = audio.engine ? audio.engine->snapshot() : jam2::EngineSnapshot{};
    std::cout << "Audio callbacks: " << audio.stream->callbacks() << "\n";
    if (callback_stats.interval_samples > 0) {
        std::cout << "Audio callback interval ms min: "
                  << static_cast<double>(callback_stats.interval_min_us) / 1000.0 << "\n";
        std::cout << "Audio callback interval ms avg: "
                  << avg_us_to_ms(callback_stats.interval_sum_us, callback_stats.interval_samples) << "\n";
        std::cout << "Audio callback interval ms max: "
                  << static_cast<double>(callback_stats.interval_max_us) / 1000.0 << "\n";
    }
    std::cout << "Audio callback gaps over 1.1x: " << callback_stats.gap_over_1_1x_count << "\n";
    std::cout << "Audio callback gaps over 1.5x: " << callback_stats.gap_over_1_5x_count << "\n";
    std::cout << "Audio callback gaps over 2x: " << callback_stats.gap_over_2x_count << "\n";
    if (options.audio_device_id) {
        std::cout << "Audio device id: " << *options.audio_device_id << "\n";
    }
    std::cout << "Requested sample rate: " << options.sample_rate << "\n";
    std::cout << "Active sample rate: " << stream_info.sample_rate << "\n";
    std::cout << "Requested audio buffer size frames: " << options.audio_buffer_size << "\n";
    std::cout << "Requested audio buffer size ms: "
              << frames_to_ms(static_cast<std::size_t>(options.audio_buffer_size > 0 ? options.audio_buffer_size : 0), options.sample_rate)
              << "\n";
    std::cout << "Active audio buffer size frames: " << stream_info.buffer_size << "\n";
    std::cout << "Active audio buffer size ms: "
              << frames_to_ms(static_cast<std::size_t>(stream_info.buffer_size > 0 ? stream_info.buffer_size : 0), stream_info.sample_rate)
              << "\n";
    std::cout << "Requested input mix: " << mono_mix_mode_text(options.channel_selection.input.size()) << "\n";
    std::cout << "Active input mix: " << mono_mix_mode_text(stream_info.channels.input.size()) << "\n";
    std::cout << "Requested audio channels: " << channel_selection_text(options.channel_selection) << "\n";
    std::cout << "Active audio channels: " << channel_selection_text(stream_info.channels) << "\n";
    std::cout << "Backend sample format: " << stream_info.sample_format << "\n";
    std::cout << "Output channels: duplicated mono to selected output channels\n";
    std::cout << "Capture ring capacity frames: " << audio.capture_ring->capacity() << "\n";
    std::cout << "Playback ring capacity frames: " << audio.playback_ring->capacity() << "\n";
    if (audio.recorder) {
        const auto recording = audio.recorder->stats();
        std::cout << "Recording active: " << (recording.active ? "yes" : "no") << "\n";
        std::cout << "Recording folder: " << recording.folder << "\n";
        std::cout << "Recording sample rate: " << recording.sample_rate << "\n";
        std::cout << "Recording frames written: " << recording.frames_written << "\n";
        std::cout << "Recording dropped frames: " << recording.dropped_frames << "\n";
        std::cout << "Recording drop events: " << recording.drop_events << "\n";
        std::cout << "Recording queue depth frames: " << recording.queue_depth_frames << "\n";
        std::cout << "Recording queue capacity frames: " << recording.queue_capacity_frames << "\n";
        std::cout << "Recording writer errors: " << recording.writer_errors << "\n";
    }
    std::cout << "Playback prefilled: " << (audio.stream->playback_prefilled() ? "yes" : "no") << "\n";
    std::cout << "Playback prefill frames: " << options.playback_prefill_frames << "\n";
    std::cout << "Playback prefill ms: " << frames_to_ms(options.playback_prefill_frames, options.sample_rate) << "\n";
    std::cout << "Capture ring overruns frames: " << capture_stats.overruns << "\n";
    std::cout << "Capture ring underruns frames: " << capture_stats.underruns << "\n";
    std::cout << "Capture ring underrun events: " << capture_stats.underrun_events << "\n";
    std::cout << "Capture ring readable frames: " << capture_readable << "\n";
    std::cout << "Capture ring readable ms: " << frames_to_ms(capture_readable, options.sample_rate) << "\n";
    std::cout << "Network capture enabled: " << (engine_snapshot.network_capture_enabled ? "yes" : "no") << "\n";
    std::cout << "Network capture ready: " << (engine_snapshot.network_capture_ready ? "yes" : "no") << "\n";
    std::cout << "Network capture generation: " << engine_snapshot.network_capture_generation << "\n";
    std::cout << "Network capture epoch frame: " << engine_snapshot.network_capture_epoch_frame << "\n";
    std::cout << "Network capture stale frames discarded: "
              << engine_snapshot.network_capture_stale_frames_discarded << "\n";
    std::cout << "Network capture attach count: " << engine_snapshot.network_capture_attach_count << "\n";
    std::cout << "Network capture detach count: " << engine_snapshot.network_capture_detach_count << "\n";
    std::cout << "Network playback enabled: " << (engine_snapshot.network_playback_enabled ? "yes" : "no") << "\n";
    std::cout << "Engine command queue depth/capacity/high-water/rejections: "
              << engine_snapshot.command_queue_depth << "/"
              << engine_snapshot.command_queue_capacity << "/"
              << engine_snapshot.command_queue_high_water << "/"
              << engine_snapshot.command_queue_rejections << "\n";
    std::cout << "Engine scheduled command depth/capacity/high-water/rejections: "
              << engine_snapshot.scheduled_command_depth << "/"
              << engine_snapshot.scheduled_command_capacity << "/"
              << engine_snapshot.scheduled_command_high_water << "/"
              << engine_snapshot.scheduled_command_rejections << "\n";
    std::cout << "Engine event queue depth/capacity/high-water/drops: "
              << engine_snapshot.event_queue_depth << "/"
              << engine_snapshot.event_queue_capacity << "/"
              << engine_snapshot.event_queue_high_water << "/"
              << engine_snapshot.event_queue_drops << "\n";
    std::cout << "Playback ring overruns frames: " << playback_stats.overruns << "\n";
    std::cout << "Playback ring underruns frames: " << playback_stats.underruns << "\n";
    std::cout << "Playback ring underrun events: " << playback_stats.underrun_events << "\n";
    std::cout << "Playback ring underrun event max frames: " << playback_stats.underrun_event_max_frames << "\n";
    std::cout << "Playback ring underrun event max ms: "
              << frames_to_ms(static_cast<std::size_t>(playback_stats.underrun_event_max_frames), stream_info.sample_rate) << "\n";
    std::cout << "Playback ring underrun burst events: " << playback_stats.underrun_burst_events << "\n";
    std::cout << "Playback ring underrun burst max frames: " << playback_stats.underrun_burst_max_frames << "\n";
    std::cout << "Playback ring underrun burst max ms: "
              << frames_to_ms(static_cast<std::size_t>(playback_stats.underrun_burst_max_frames), stream_info.sample_rate) << "\n";
    std::cout << "Playback ring underrun time ms: "
              << frames_to_ms(static_cast<std::size_t>(playback_stats.underruns), stream_info.sample_rate) << "\n";
    std::cout << "Playback depth observed frames: " << playback_stats.depth_observed_frames << "\n";
    std::cout << "Playback depth under 2ms frames: " << playback_stats.depth_under_2ms_frames << "\n";
    std::cout << "Playback depth under 2ms percent: "
              << frames_percent(playback_stats.depth_under_2ms_frames, playback_stats.depth_observed_frames) << "\n";
    std::cout << "Playback depth under 5ms frames: " << playback_stats.depth_under_5ms_frames << "\n";
    std::cout << "Playback depth under 5ms percent: "
              << frames_percent(playback_stats.depth_under_5ms_frames, playback_stats.depth_observed_frames) << "\n";
    std::cout << "Playback depth under 10ms frames: " << playback_stats.depth_under_10ms_frames << "\n";
    std::cout << "Playback depth under 10ms percent: "
              << frames_percent(playback_stats.depth_under_10ms_frames, playback_stats.depth_observed_frames) << "\n";
    std::cout << "Playback ring readable frames: " << playback_readable << "\n";
    std::cout << "Playback ring readable ms: " << frames_to_ms(playback_readable, options.sample_rate) << "\n";
    if (audio.control) {
        std::cout << "Audio control metronome: "
                  << (audio.control->metronome_enabled.load(std::memory_order_relaxed) ? "on" : "off") << "\n";
        std::cout << "Audio control BPM: " << audio.control->metronome_bpm.load(std::memory_order_relaxed) << "\n";
        std::cout << "Audio control metronome level: "
                  << gain_from_ppm(audio.control->metronome_level_ppm.load(std::memory_order_relaxed)) << "\n";
        std::cout << "Audio control remote playback level: "
                  << gain_from_ppm(audio.control->remote_level_ppm.load(std::memory_order_relaxed)) << "\n";
        std::cout << "Audio control metronome mode: "
                  << metronome_mode_text(audio.control->metronome_mode.load(std::memory_order_relaxed)) << "\n";
        std::cout << "Audio control metronome epoch sample time: "
                  << audio.control->metronome_epoch_sample_time.load(std::memory_order_relaxed) << "\n";
        std::cout << "Audio control metronome epoch valid: "
                  << (audio.control->metronome_epoch_valid.load(std::memory_order_relaxed) ? "yes" : "no") << "\n";
        std::cout << "Audio control resampler ratio: "
                  << (static_cast<double>(audio.control->playback_ratio_ppm.load(std::memory_order_relaxed)) / 1000000.0) << "\n";
    }
}

int run_test_device(int argc, char** argv)
{
    if (argc < 3) {
        throw std::runtime_error("test-device requires a device id");
    }
    const int id = std::stoi(argv[2]);
    double sample_rate = 48000.0;
    for (int i = 3; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--sample-rate") {
            sample_rate = std::stod(std::string(require_value(argc, argv, i, arg)));
            if (sample_rate <= 0.0) {
                throw std::runtime_error("--sample-rate must be positive");
            }
        } else {
            throw std::runtime_error("unknown option: " + std::string(arg));
        }
    }

    const auto probe = jam2::audio::probe_device(id, sample_rate);
    std::cout << "Device: [" << probe.device.id << "] " << probe.device.backend << " " << probe.device.name << "\n";
    if (!probe.driver_name.empty()) {
        std::cout << "Driver name: " << probe.driver_name << "\n";
    }
    std::cout << "Driver version: " << probe.driver_version << "\n";
    std::cout << "Channels: input=" << probe.input_channels << " output=" << probe.output_channels << "\n";
    std::cout << "Latencies samples: input=" << probe.input_latency_samples
              << " output=" << probe.output_latency_samples << "\n";
    std::cout << "Buffer sizes samples: min=" << probe.min_buffer_size
              << " max=" << probe.max_buffer_size
              << " preferred=" << probe.preferred_buffer_size
              << " granularity=" << probe.buffer_granularity << "\n";
    std::cout << "Current sample rate: " << probe.current_sample_rate << "\n";
    std::cout << "Requested sample rate " << sample_rate << ": "
              << (probe.requested_sample_rate_supported ? "supported" : "not supported") << "\n";
    std::cout << "Input channel ids: ";
    if (probe.input_channels <= 0) {
        std::cout << "none";
    } else {
        for (long channel = 1; channel <= probe.input_channels; ++channel) {
            std::cout << (channel == 1 ? "" : ",") << channel;
        }
    }
    std::cout << "\n";
    std::cout << "Output channel ids: ";
    if (probe.output_channels <= 0) {
        std::cout << "none";
    } else {
        for (long channel = 1; channel <= probe.output_channels; ++channel) {
            std::cout << (channel == 1 ? "" : ",") << channel;
        }
    }
    std::cout << "\n";
    if (probe.input_channels > 0) {
        std::cout << "Example one-channel input mixed to mono stream: --input-channels 1\n";
    }
    if (probe.input_channels > 1) {
        std::cout << "Example two-channel input mixed to mono stream: --input-channels 1,2\n";
    }
    if (probe.input_channels > 2) {
        std::cout << "Example multi-input mix to mono stream: --input-channels 1,2,3,4\n";
    }
    if (probe.output_channels > 0) {
        std::cout << "Example one-channel output: --output-channels 1\n";
    }
    if (probe.output_channels > 1) {
        std::cout << "Example duplicated two-output mono: --output-channels 1,2\n";
    }
    if (probe.output_channels > 2) {
        std::cout << "Example duplicated multi-output: --output-channels 1,2,3,4\n";
    }
    return 0;
}

int run_meter_device(int argc, char** argv)
{
    if (argc < 3) {
        throw std::runtime_error("meter-device requires a device id");
    }
    const int id = std::stoi(argv[2]);
    double sample_rate = 48000.0;
    long buffer_size = 0;
    int duration_ms = 3000;
    for (int i = 3; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--sample-rate") {
            sample_rate = std::stod(std::string(require_value(argc, argv, i, arg)));
            if (sample_rate <= 0.0) {
                throw std::runtime_error("--sample-rate must be positive");
            }
        } else if (arg == "--buffer-size") {
            buffer_size = std::stol(std::string(require_value(argc, argv, i, arg)));
            if (buffer_size <= 0) {
                throw std::runtime_error("--buffer-size must be positive");
            }
        } else if (arg == "--duration-ms") {
            duration_ms = std::stoi(std::string(require_value(argc, argv, i, arg)));
            if (duration_ms <= 0) {
                throw std::runtime_error("--duration-ms must be positive");
            }
        } else {
            throw std::runtime_error("unknown option: " + std::string(arg));
        }
    }

    const auto result = jam2::audio::meter_device(id, sample_rate, buffer_size, duration_ms);
    std::cout << "Device: [" << result.device.id << "] " << result.device.backend << " " << result.device.name << "\n";
    std::cout << "Sample rate: " << result.sample_rate << "\n";
    std::cout << "Buffer size samples: " << result.buffer_size << "\n";
    std::cout << "Callbacks: " << result.callbacks << "\n";
    std::cout << "Input sample type: " << result.input_sample_type << "\n";
    std::cout << "Output sample type: " << result.output_sample_type << "\n";
    std::cout << "Input peak: " << result.input_peak << "\n";
    return 0;
}

int run_ring_device(int argc, char** argv)
{
    if (argc < 3) {
        throw std::runtime_error("ring-device requires a device id");
    }
    const int id = std::stoi(argv[2]);
    double sample_rate = 48000.0;
    long buffer_size = 0;
    int duration_ms = 3000;
    std::size_t ring_frames = 4096;
    for (int i = 3; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--sample-rate") {
            sample_rate = std::stod(std::string(require_value(argc, argv, i, arg)));
            if (sample_rate <= 0.0) {
                throw std::runtime_error("--sample-rate must be positive");
            }
        } else if (arg == "--buffer-size") {
            buffer_size = std::stol(std::string(require_value(argc, argv, i, arg)));
            if (buffer_size <= 0) {
                throw std::runtime_error("--buffer-size must be positive");
            }
        } else if (arg == "--duration-ms") {
            duration_ms = std::stoi(std::string(require_value(argc, argv, i, arg)));
            if (duration_ms <= 0) {
                throw std::runtime_error("--duration-ms must be positive");
            }
        } else if (arg == "--ring-frames") {
            const auto parsed = std::stoull(std::string(require_value(argc, argv, i, arg)));
            if (parsed == 0) {
                throw std::runtime_error("--ring-frames must be positive");
            }
            ring_frames = static_cast<std::size_t>(parsed);
        } else {
            throw std::runtime_error("unknown option: " + std::string(arg));
        }
    }

    const auto result = jam2::audio::ring_device(id, sample_rate, buffer_size, duration_ms, ring_frames);
    std::cout << "Device: [" << result.device.id << "] " << result.device.backend << " " << result.device.name << "\n";
    std::cout << "Sample rate: " << result.sample_rate << "\n";
    std::cout << "Buffer size samples: " << result.buffer_size << "\n";
    std::cout << "Callbacks: " << result.callbacks << "\n";
    std::cout << "Ring overruns frames: " << result.ring_overruns << "\n";
    std::cout << "Ring underruns frames: " << result.ring_underruns << "\n";
    std::cout << "Ring underrun events: " << result.ring_underrun_events << "\n";
    std::cout << "Ring readable frames: " << result.ring_readable << "\n";
    return 0;
}

int run_local(int argc, char** argv)
{
    const Options options = parse_options(argc, argv, 2);
    if (!options.audio_device_id && !options.headless_audio) {
        throw std::runtime_error("local requires --audio-device; use list-devices to inspect available low-latency devices");
    }

    // Deliberately do not create UdpSocket or STUN here.
    // This command is the local engine audio path, with optional GUI control
    // for mixer and Track take commands. On Windows the
    // local GUI-control TCP socket still needs NetworkRuntime for WSAStartup.
    std::optional<jam2::NetworkRuntime> gui_network;
    if (options.gui_control) {
        gui_network.emplace();
    }
    auto audio = start_optional_audio(options, true);
    if (!audio.stream || !audio.control) {
        throw std::runtime_error("local failed to start an audio stream");
    }

    const int recording_sample_rate = static_cast<int>(std::lround(audio.stream->info().sample_rate));
    CommandThread commands(options, audio.recorder.get(), audio.prepared_source.get(), audio.control.get(), recording_sample_rate, true);
    GuiControlThread gui_control(
        options,
        commands.state,
        audio.recorder.get(),
        audio.prepared_source.get(),
        audio.track_take_recorder.get(),
        recording_sample_rate,
        audio.stream.get(),
        audio.capture_ring.get(),
        audio.playback_ring.get(),
        audio.control.get());
    start_startup_recording(audio, options, commands.state, recording_sample_rate);

    std::cout << "{\"event\":\"startup\",\"mode\":\"local\",\"stage\":\"running\"}\n";
    std::cout.flush();
    AudioPacketStats stats;
    const std::uint64_t started_us = jam2::monotonic_us();
    std::uint64_t next_status_us = started_us;
    std::uint64_t local_epoch_revision =
        commands.state.metronome_epoch_revision.load(std::memory_order_relaxed);
    auto local_epoch_pattern = metronome_pattern_from_runtime(commands.state);
    const std::uint64_t status_interval_us = static_cast<std::uint64_t>(
        std::max(100, options.stats_interval_ms)) * 1000ULL;
    while (!commands.state.quit.load(std::memory_order_relaxed)) {
        const std::uint64_t epoch_revision =
            commands.state.metronome_epoch_revision.load(std::memory_order_relaxed);
        if (epoch_revision != local_epoch_revision) {
            const auto next_pattern = metronome_pattern_from_runtime(commands.state);
            const bool timing_changed =
                local_epoch_pattern.bpm != next_pattern.bpm ||
                local_epoch_pattern.beats_per_bar != next_pattern.beats_per_bar ||
                local_epoch_pattern.division != next_pattern.division;
            const bool restart_playing_track =
                timing_changed && audio.prepared_source != nullptr && audio.prepared_source->playing();
            begin_metronome_epoch(commands.state, audio.control.get(), recording_sample_rate);
            const std::uint64_t epoch_frame =
                commands.state.metronome_epoch_sample_time.load(std::memory_order_relaxed);
            if (restart_playing_track &&
                enqueue_prepared_restart(audio.prepared_source.get(), audio.control.get(), epoch_frame)) {
                publish_transport_schedule(
                    commands.state,
                    jam2::gui_control::TransportAction::TrackRestart,
                    {epoch_frame, epoch_frame, epoch_frame},
                    false);
            }
            local_epoch_pattern = next_pattern;
            local_epoch_revision = epoch_revision;
        }
        commit_due_transport(commands.state, audio.control.get());
        sync_audio_control(commands.state, audio.control.get(), 1.0);
        const std::uint64_t now_us = jam2::monotonic_us();
        if (options.status_jsonl && now_us >= next_status_us) {
            print_compact_status(
                stats,
                options,
                commands.state,
                audio.stream.get(),
                audio.playback_ring.get(),
                audio.control.get(),
                audio.recorder.get(),
                (now_us - started_us) / 1000ULL);
            next_status_us = now_us + status_interval_us;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    finalize_active_recording(audio, options, commands.state);
    return 0;
}

int run_listen(int argc, char** argv)
{
    const Options options = parse_options(argc, argv, 2);
    jam2::NetworkRuntime network;
    jam2::UdpSocket socket;
    apply_socket_options(socket, options);
    socket.bind(options.bind);
    const jam2::Endpoint local = socket.local_endpoint();

    jam2::Endpoint advertised;
    std::string endpoint_mode;
    if (options.public_endpoint) {
        advertised = *options.public_endpoint;
        endpoint_mode = "manual public endpoint";
    } else if (options.no_stun) {
        advertised = local;
        advertised.host = "127.0.0.1";
        endpoint_mode = "manual/no-stun local fallback";
    } else {
        std::cout << "Mode: listen\n";
        std::cout << "Local UDP bind: " << jam2::endpoint_to_string(local) << "\n";
        std::cout << "STUN: " << jam2::endpoint_to_string(options.stun_server) << "\n";
        print_socket_options(socket);
        advertised = jam2::stun::discover_public_endpoint(
            socket,
            options.stun_server,
            options.stun_timeout_ms,
            options.stun_retries);
        endpoint_mode = "STUN discovered";
    }

    jam2::SessionInfo session{
        advertised,
        options.session_id.value_or(jam2::random_u64()),
        options.session_key.value_or(jam2::random_key())};
    const std::string connection_url = jam2::make_jam_url(session);
    if (options.public_endpoint || options.no_stun) {
        std::cout << "Mode: listen\n";
        std::cout << "Local UDP bind: " << jam2::endpoint_to_string(local) << "\n";
    }
    if (options.public_endpoint || options.no_stun) {
        print_socket_options(socket);
    }
    if (!options.no_stun && !options.public_endpoint) {
        std::cout << "Public endpoint: " << jam2::endpoint_to_string(advertised) << "\n";
    }
    std::cout << "Endpoint mode: " << endpoint_mode << "\n";
    std::cout << "Connection string:\n" << connection_url << "\n\n";
    if (!options.session_id && !options.session_key) {
        std::cout << "Client command:\n"
                  << make_headless_client_command(argv[0], connection_url, options) << "\n\n";
    }
    print_startup_json("listen", "waiting", options, local, std::nullopt, endpoint_mode, connection_url);
    std::cout << "Waiting for peer...\n";
    std::cout.flush();

    std::optional<jam2::Endpoint> locked_peer;
    int ignored_malformed = 0;
    int ignored_wrong_endpoint = 0;
    UdpParseStats handshake_parse;
    std::array<std::uint8_t, jam2::protocol::kMaxDatagramSize> handshake_packet{};
    const std::uint64_t deadline = options.wait_ms > 0 ?
        jam2::monotonic_us() + static_cast<std::uint64_t>(options.wait_ms) * 1000ULL :
        UINT64_MAX;
    while (jam2::monotonic_us() < deadline) {
        const auto received = socket.recv_from(handshake_packet, 250);
        if (!received) {
            continue;
        }
        const auto& from = received->endpoint;
        const std::span<const std::uint8_t> bytes(handshake_packet.data(), received->size);
        if (locked_peer && (from.host != locked_peer->host || from.port != locked_peer->port)) {
            ++ignored_wrong_endpoint;
            continue;
        }
        const auto parsed = jam2::protocol::parse_packet(bytes, session.key, session.session_id);
        if (!parsed) {
            handshake_parse.observe(parsed.error);
            ++ignored_malformed;
            continue;
        }
        const auto& header = parsed.header;
        if (header.type != jam2::protocol::PacketType::Hello) {
            ++ignored_malformed;
            continue;
        }
        try {
            const auto remote_config = decode_handshake_config(bytes, header);
            require_matching_stream_config(remote_config, options);
        } catch (const std::exception& error) {
            const std::string message = error.what();
            socket.send_to(from, make_handshake_packet(jam2::protocol::PacketType::HelloAck, session, header.sequence, options));
            std::cerr << message << "\n";
            print_startup_json("listen", "error", options, local, from, endpoint_mode, connection_url, message);
            return 4;
        }
        locked_peer = from;
        socket.send_to(from, make_handshake_packet(jam2::protocol::PacketType::HelloAck, session, header.sequence, options));
        std::cout << "Peer locked: " << jam2::endpoint_to_string(from) << "\n";
        std::cout << "Handshake complete\n";
        std::cout << "Ignored malformed/auth/session packets: " << ignored_malformed << "\n";
        std::cout << "Ignored wrong-endpoint packets: " << ignored_wrong_endpoint << "\n";
        print_udp_parse_stats(handshake_parse);
        print_startup_json("listen", "connected", options, local, from, endpoint_mode, connection_url);
        auto audio = start_optional_audio(options, false);
        attach_network_capture(audio);
        const int drained_startup_packets = drain_pending_udp(socket);
        if (drained_startup_packets > 0) {
            std::cout << "Drained startup UDP packets: " << drained_startup_packets << "\n";
        }
        std::optional<CsvStatsLog> csv_log;
        if (options.log_stats_dir) {
            csv_log.emplace(
                *options.log_stats_dir,
                make_csv_context(argc, argv, "listen", options, socket, local, from, endpoint_mode));
            std::cout << "Stats CSV: " << csv_log->path().string() << "\n";
        }
        const int recording_sample_rate = audio.stream
            ? static_cast<int>(std::lround(audio.stream->info().sample_rate))
            : options.sample_rate;
        CommandThread commands(options, audio.recorder.get(), audio.prepared_source.get(), audio.control.get(), recording_sample_rate, false);
        hold_shared_grid_at_start(commands.state, audio.control.get());
        GuiControlThread gui_control(
            options,
            commands.state,
            audio.recorder.get(),
            audio.prepared_source.get(),
            audio.track_take_recorder.get(),
            recording_sample_rate,
            audio.stream.get(),
            audio.capture_ring.get(),
            audio.playback_ring.get(),
            audio.control.get());
        start_startup_recording(audio, options, commands.state, recording_sample_rate);
        CliPeerStreamPlayback peer_playback(audio.engine.get());
        jam2::NetworkSession network_session(
            std::move(socket),
            session,
            make_network_session_contract(options),
            jam2::SessionBootstrapRole::Creator,
            jam2::PeerId{1},
            {jam2::PeerId{2}, from, jam2::PeerEndpointState::Active},
            make_peer_stream_config(
                options,
                csv_log.has_value() || options.stats_interval_ms > 0),
            audio.playback_ring != nullptr ? &peer_playback : nullptr);
        auto audio_stats = run_audio_packet_exchange(
            network_session,
            options,
            commands.state,
            audio.engine.get(),
            audio.network_capture,
            audio.prepared_source.get(),
            audio.control.get(),
            audio.capture_ring.get(),
            audio.playback_ring.get(),
            audio.stream.get(),
            audio.recorder.get(),
            static_cast<std::uint64_t>(drained_startup_packets),
            csv_log ? &*csv_log : nullptr);
        detach_network_capture(audio);
        audio_stats.startup_drained_packets = static_cast<std::uint64_t>(drained_startup_packets);
        if (commands.state.stats_enabled.load(std::memory_order_relaxed)) {
            print_audio_packet_stats(audio_stats, options);
            print_optional_audio_stats(audio, options);
        }
        finalize_active_recording(audio, options, commands.state);
        std::cout.flush();
        return 0;
    }
    std::cerr << "Timed out waiting for authenticated peer\n";
    std::cerr << "Ignored malformed/auth/session packets: " << ignored_malformed << "\n";
    std::cerr << "Ignored wrong-endpoint packets: " << ignored_wrong_endpoint << "\n";
    print_udp_parse_stats(handshake_parse, std::cerr);
    print_startup_json("listen", "error", options, local, std::nullopt, endpoint_mode, connection_url, "timed out waiting for authenticated peer");
    return 3;
}

int run_connect(int argc, char** argv)
{
    if (argc < 3) {
        throw std::runtime_error("connect requires a jam2 URL");
    }
    auto session = jam2::parse_jam_url(argv[2]);
    session.endpoint = jam2::resolve_udp_endpoint(session.endpoint);
    const Options options = parse_options(argc, argv, 3);
    jam2::NetworkRuntime network;
    jam2::UdpSocket socket;
    apply_socket_options(socket, options);
    socket.bind({"0.0.0.0", 0});

    std::cout << "Mode: connect\n";
    std::cout << "Local UDP bind: " << jam2::endpoint_to_string(socket.local_endpoint()) << "\n";
    std::cout << "Peer endpoint: " << jam2::endpoint_to_string(session.endpoint) << "\n";
    print_socket_options(socket);
    print_startup_json("connect", "connecting", options, socket.local_endpoint(), session.endpoint, "jam2-url", "");

    const auto hello = make_handshake_packet(jam2::protocol::PacketType::Hello, session, 1, options);
    const std::uint64_t deadline = options.wait_ms > 0 ?
        jam2::monotonic_us() + static_cast<std::uint64_t>(options.wait_ms) * 1000ULL :
        UINT64_MAX;
    int attempts = 0;
    int ignored = 0;
    UdpParseStats handshake_parse;
    std::array<std::uint8_t, jam2::protocol::kMaxDatagramSize> handshake_packet{};
    while (jam2::monotonic_us() < deadline) {
        ++attempts;
        socket.send_to(session.endpoint, hello);
        const auto received = socket.recv_from(handshake_packet, 500);
        if (!received) {
            continue;
        }
        const auto& from = received->endpoint;
        const std::span<const std::uint8_t> bytes(handshake_packet.data(), received->size);
        if (from.host != session.endpoint.host || from.port != session.endpoint.port) {
            ++ignored;
            continue;
        }
        const auto parsed = jam2::protocol::parse_packet(bytes, session.key, session.session_id);
        if (!parsed) {
            handshake_parse.observe(parsed.error);
            ++ignored;
            continue;
        }
        const auto& header = parsed.header;
        if (header.type == jam2::protocol::PacketType::HelloAck) {
                try {
                    const auto remote_config = decode_handshake_config(bytes, header);
                    require_matching_stream_config(remote_config, options);
                } catch (const std::exception& error) {
                    const std::string message = error.what();
                    std::cerr << message << "\n";
                    print_startup_json("connect", "error", options, socket.local_endpoint(), session.endpoint, "jam2-url", "", message);
                    return 4;
                }
                std::cout << "Handshake complete\n";
                std::cout << "Attempts: " << attempts << "\n";
                std::cout << "Ignored packets: " << ignored << "\n";
                print_udp_parse_stats(handshake_parse);
                print_startup_json("connect", "connected", options, socket.local_endpoint(), session.endpoint, "jam2-url", "");
                auto audio = start_optional_audio(options, false);
                attach_network_capture(audio);
                const int drained_startup_packets = drain_pending_udp(socket);
                if (drained_startup_packets > 0) {
                    std::cout << "Drained startup UDP packets: " << drained_startup_packets << "\n";
                }
                std::optional<CsvStatsLog> csv_log;
                if (options.log_stats_dir) {
                    csv_log.emplace(
                        *options.log_stats_dir,
                        make_csv_context(
                            argc,
                            argv,
                            "connect",
                            options,
                            socket,
                            socket.local_endpoint(),
                            session.endpoint,
                            "jam2-url"));
                    std::cout << "Stats CSV: " << csv_log->path().string() << "\n";
                }
                const int recording_sample_rate = audio.stream
                    ? static_cast<int>(std::lround(audio.stream->info().sample_rate))
                    : options.sample_rate;
                CommandThread commands(options, audio.recorder.get(), audio.prepared_source.get(), audio.control.get(), recording_sample_rate, false);
                hold_shared_grid_at_start(commands.state, audio.control.get());
                GuiControlThread gui_control(
                    options,
                    commands.state,
                    audio.recorder.get(),
                    audio.prepared_source.get(),
                    audio.track_take_recorder.get(),
                    recording_sample_rate,
                    audio.stream.get(),
                    audio.capture_ring.get(),
                    audio.playback_ring.get(),
                    audio.control.get());
                start_startup_recording(audio, options, commands.state, recording_sample_rate);
                CliPeerStreamPlayback peer_playback(audio.engine.get());
                jam2::NetworkSession network_session(
                    std::move(socket),
                    session,
                    make_network_session_contract(options),
                    jam2::SessionBootstrapRole::Joiner,
                    jam2::PeerId{2},
                    {jam2::PeerId{1}, session.endpoint, jam2::PeerEndpointState::Active},
                    make_peer_stream_config(
                        options,
                        csv_log.has_value() || options.stats_interval_ms > 0),
                    audio.playback_ring != nullptr ? &peer_playback : nullptr);
                auto audio_stats = run_audio_packet_exchange(
                    network_session,
                    options,
                    commands.state,
                    audio.engine.get(),
                    audio.network_capture,
                    audio.prepared_source.get(),
                    audio.control.get(),
                    audio.capture_ring.get(),
                    audio.playback_ring.get(),
                    audio.stream.get(),
                    audio.recorder.get(),
                    static_cast<std::uint64_t>(drained_startup_packets),
                    csv_log ? &*csv_log : nullptr);
                detach_network_capture(audio);
                audio_stats.startup_drained_packets = static_cast<std::uint64_t>(drained_startup_packets);
                if (commands.state.stats_enabled.load(std::memory_order_relaxed)) {
                    print_audio_packet_stats(audio_stats, options);
                    print_optional_audio_stats(audio, options);
                }
                finalize_active_recording(audio, options, commands.state);
                std::cout.flush();
                return 0;
        } else {
            ++ignored;
        }
    }
    std::cerr << "Timed out waiting for HELLO_ACK\n";
    std::cerr << "Attempts: " << attempts << "\n";
    std::cerr << "Ignored packets: " << ignored << "\n";
    print_udp_parse_stats(handshake_parse, std::cerr);
    print_startup_json("connect", "error", options, socket.local_endpoint(), session.endpoint, "jam2-url", "", "timed out waiting for HELLO_ACK");
    return 3;
}

int run_mesh(int argc, char** argv)
{
    Options options = parse_options(argc, argv, 2);
    if (!options.session_id || !options.session_key) {
        throw std::runtime_error("mesh requires --session-id and --session-key");
    }
    if (!options.mesh_peers_configured) {
        throw std::runtime_error("mesh requires --peers, use --peers \"\" for an empty initial peer list");
    }
    for (auto& peer : options.mesh_peers) {
        peer = jam2::resolve_udp_endpoint(peer);
    }

    jam2::NetworkRuntime network;
    jam2::UdpSocket socket;
    apply_socket_options(socket, options);
    socket.bind(options.bind);
    const jam2::Endpoint local = socket.local_endpoint();
    const jam2::SessionInfo session{local, *options.session_id, *options.session_key};

    std::cout << "Mode: mesh\n";
    std::cout << "Local UDP bind: " << jam2::endpoint_to_string(local) << "\n";
    std::cout << "Mesh peers: " << options.mesh_peers.size() << "\n";
    for (std::size_t i = 0; i < options.mesh_peers.size(); ++i) {
        std::cout << "  peer" << (i + 1) << ": " << jam2::endpoint_to_string(options.mesh_peers[i]) << "\n";
    }
    print_socket_options(socket);
    print_startup_json("mesh", "running", options, local, std::nullopt, "gui-peer-list", "");

    auto audio = start_optional_audio(options, false);
    attach_network_capture(audio);
    const int drained_startup_packets = drain_pending_udp(socket);
    if (drained_startup_packets > 0) {
        std::cout << "Drained startup UDP packets: " << drained_startup_packets << "\n";
    }
    std::optional<CsvStatsLog> csv_log;
    if (options.log_stats_dir) {
        const jam2::Endpoint peer_context{
            options.mesh_peers.empty() ? std::string("mesh:none") : std::string("mesh:") + std::to_string(options.mesh_peers.size()),
            0};
        csv_log.emplace(
            *options.log_stats_dir,
            make_csv_context(argc, argv, "mesh", options, socket, local, peer_context, "gui-peer-list"));
        std::cout << "Stats CSV: " << csv_log->path().string() << "\n";
    }
    const int recording_sample_rate = audio.stream
        ? static_cast<int>(std::lround(audio.stream->info().sample_rate))
        : options.sample_rate;
    CommandThread commands(options, audio.recorder.get(), audio.prepared_source.get(), audio.control.get(), recording_sample_rate, false);
    hold_shared_grid_at_start(commands.state, audio.control.get());
    GuiControlThread gui_control(
        options,
        commands.state,
        audio.recorder.get(),
        audio.prepared_source.get(),
        audio.track_take_recorder.get(),
        recording_sample_rate,
        audio.stream.get(),
        audio.capture_ring.get(),
        audio.playback_ring.get(),
        audio.control.get());
    start_startup_recording(audio, options, commands.state, recording_sample_rate);

    enum class EndpointProofState {
        Candidate,
        Probing,
        Active,
        Failed,
    };

    struct ProbeChallenge {
        std::uint32_t sequence = 0;
        std::uint64_t send_time_us = 0;
        bool used = false;
    };

    struct MeshPeerState {
        jam2::Endpoint endpoint;
        jam2::PeerId peer_id;
        EndpointProofState endpoint_proof = EndpointProofState::Candidate;
        std::array<ProbeChallenge, 8> probe_challenges{};
        std::uint32_t proof_attempts = 0;
        std::uint64_t proof_deadline_us = 0;
        std::uint64_t next_probe_us = 0;
        std::uint64_t proof_successes = 0;
        std::uint64_t proof_failures = 0;
        std::uint64_t proof_unverified_drops = 0;
        std::uint64_t proof_unmatched_pongs = 0;
        std::uint64_t proof_challenge_overwrites = 0;
        std::uint64_t sent_packets = 0;
        std::uint64_t sent_bytes = 0;
        std::uint64_t recv_packets = 0;
        std::uint64_t recv_bytes = 0;
        std::uint64_t ignored_packets = 0;
        UdpParseStats udp_parse;
        std::uint64_t sent_pings = 0;
        std::uint64_t sent_pongs = 0;
        std::uint64_t recv_pongs = 0;
        std::uint64_t last_transport_revision = 0;
    };

    auto endpoint_key = [](const jam2::Endpoint& endpoint) {
        return endpoint.host + ":" + std::to_string(endpoint.port);
    };

    auto endpoint_proof_name = [](EndpointProofState state) -> const char* {
        switch (state) {
        case EndpointProofState::Candidate:
            return "candidate";
        case EndpointProofState::Probing:
            return "probing";
        case EndpointProofState::Active:
            return "active";
        case EndpointProofState::Failed:
            return "failed";
        }
        return "unknown";
    };

    std::map<std::string, MeshPeerState> peers;
    for (const auto& peer : options.mesh_peers) {
        const auto inserted = peers.emplace(
            endpoint_key(peer),
            MeshPeerState{peer, compatibility_peer_id(peer)});
        if (!inserted.second) {
            throw std::runtime_error("mesh peer list contains a duplicate endpoint");
        }
    }

    const jam2::PeerId local_peer_id = compatibility_peer_id(local);
    std::vector<jam2::NetworkPeerDescriptor> peer_descriptors;
    peer_descriptors.reserve(peers.size());
    for (const auto& entry : peers) {
        const auto& peer = entry.second;
        if (peer.peer_id == local_peer_id) {
            throw std::runtime_error("mesh peer identity collides with the local endpoint");
        }
        const bool duplicate_id = std::any_of(
            peer_descriptors.begin(),
            peer_descriptors.end(),
            [&](const auto& descriptor) { return descriptor.peer_id == peer.peer_id; });
        if (duplicate_id) {
            throw std::runtime_error("mesh endpoint hash collision; use a different local port");
        }
        peer_descriptors.push_back({
            peer.peer_id,
            peer.endpoint,
            jam2::PeerEndpointState::Candidate,
        });
    }

    CliPeerStreamPlayback mesh_playback(audio.engine.get());
    jam2::NetworkSession network_session(
        std::move(socket),
        session,
        make_network_session_contract(options),
        jam2::SessionBootstrapRole::Static,
        local_peer_id,
        peer_descriptors,
        make_peer_stream_config(
            options,
            options.stats_enabled && (csv_log.has_value() || options.stats_interval_ms > 0)),
        audio.engine ? &mesh_playback : nullptr);
    auto& packet_schedule = network_session.schedule();
    std::uint64_t bootstrap_coordinator_peer_id = local_peer_id.value;
    for (const auto& descriptor : peer_descriptors) {
        bootstrap_coordinator_peer_id = std::min(
            bootstrap_coordinator_peer_id,
            descriptor.peer_id.value);
    }
    jam2::SessionAuthority authority(
        local_peer_id.value,
        bootstrap_coordinator_peer_id,
        bootstrap_coordinator_peer_id);

    std::vector<std::int32_t> asio_frames(static_cast<std::size_t>(options.frame_size), 0);
    std::vector<std::int32_t> network_frames(static_cast<std::size_t>(options.frame_size), 0);
    const auto silence_payload = jam2::protocol::pack_pcm24(network_frames);
    const std::uint16_t audio_payload_size = static_cast<std::uint16_t>(silence_payload.size());
    std::vector<std::uint8_t> packed_audio_payload(audio_payload_size);
    std::uint64_t mesh_work_budget_yields = 0;
    std::uint64_t next_local_grid_request_id = 1;
    std::uint64_t last_local_grid_request_sequence =
        commands.state.grid_request_sequence.load(std::memory_order_acquire);
    std::optional<jam2::GridProposal> pending_local_grid_proposal;
    std::uint64_t next_grid_proposal_send_us = 0;
    std::uint64_t next_grid_assignment_send_us = 0;
    std::uint64_t sending_transport_revision = 0;
    std::uint64_t next_transport_send = 0;
    const std::uint64_t start_time = packet_schedule.startTimeUs();
    std::uint64_t next_stats = options.stats_enabled && options.stats_interval_ms > 0
        ? start_time + static_cast<std::uint64_t>(options.stats_interval_ms) * 1000ULL
        : 0;
    const std::uint64_t send_deadline = options.stream_ms > 0
        ? start_time + static_cast<std::uint64_t>(options.stream_ms) * 1000ULL
        : UINT64_MAX;
    const std::uint64_t receive_deadline = options.stream_ms > 0
        ? send_deadline + static_cast<std::uint64_t>(options.stream_linger_ms) * 1000ULL
        : UINT64_MAX;
    const std::uint64_t playout_delay_frames = options.jitter_buffer_frames > 0
        ? static_cast<std::uint64_t>(options.jitter_buffer_frames)
        : static_cast<std::uint64_t>(options.playout_delay_frames);

    std::uint64_t mesh_grid_proposals_sent = 0;
    std::uint64_t mesh_grid_assignments_sent = 0;
    std::uint64_t mesh_grid_authority_states_sent = 0;
    std::uint64_t mesh_transport_source_peer_id = 0;
    std::uint64_t mesh_transport_event_counter = 0;
    std::uint64_t mesh_transport_grid_revision = 0;
    std::uint64_t mesh_leader_audio_source_peer_id = 0;
    std::uint64_t mesh_leader_audio_injected_packets = 0;
    std::uint64_t mesh_transport_source_frame = 0;
    std::uint64_t mesh_transport_requested_target_frame = 0;
    std::uint64_t mesh_transport_applied_target_frame = 0;
    std::uint64_t mesh_compensation_stale_events = 0;
    bool mesh_compensation_was_stale = false;
    std::uint64_t last_authority_state_received_us = 0;
    std::uint64_t remote_authority_epoch_frame = 0;
    std::int64_t mesh_grid_target_offset_frames = 0;
    bool mesh_grid_target_valid = false;
    std::uint64_t mesh_grid_last_update_us = 0;

    auto grid_run_state_from_runtime = [&]() {
        return commands.state.metronome.load(std::memory_order_relaxed)
            ? jam2::GridRunState::Running
            : jam2::GridRunState::Stopped;
    };
    auto choose_safe_local_epoch = [&]() {
        const auto pattern = metronome_pattern_from_runtime(commands.state);
        const std::uint64_t step_frames = jam2::metronome::step_interval_samples(
            static_cast<double>(options.sample_rate), pattern.bpm, pattern.division);
        const std::uint64_t bar_frames =
            step_frames * static_cast<std::uint64_t>(pattern.step_count);
        std::uint64_t max_rtt_us = 0;
        for (const auto& entry : peers) {
            max_rtt_us = std::max(
                max_rtt_us,
                network_session.peerStream(entry.second.peer_id).stats().rtt_min_us);
        }
        const std::uint64_t rtt_frames = max_rtt_us *
            static_cast<std::uint64_t>(options.sample_rate) / 1000000ULL;
        const std::uint64_t lead_frames = std::max(
            static_cast<std::uint64_t>(options.sample_rate) / 2ULL,
            rtt_frames + static_cast<std::uint64_t>(options.sample_rate) / 5ULL);
        const std::uint64_t minimum = current_engine_frame(audio.control.get()) + lead_frames;
        const std::uint64_t current_epoch =
            commands.state.metronome_epoch_sample_time.load(std::memory_order_relaxed);
        if (!commands.state.metronome_epoch_valid.load(std::memory_order_relaxed) ||
            current_epoch == 0 || bar_frames == 0 || minimum <= current_epoch) {
            return minimum;
        }
        return current_epoch +
            ((minimum - current_epoch + bar_frames - 1ULL) / bar_frames) * bar_frames;
    };
    auto apply_authority_role = [&]() {
        const auto& grid = authority.grid();
        const bool local_authority = authority.localIsGridAuthority();
        commands.state.metronome_local_authority.store(local_authority, std::memory_order_relaxed);
        commands.state.leader_audio_local_click.store(
            local_authority && grid.run_state == jam2::GridRunState::Running &&
                grid.mode == metronome_mode_id(MetronomeMode::LeaderAudio),
            std::memory_order_relaxed);
    };
    auto activate_local_grid = [&]() {
        const std::uint64_t packet_frame = current_engine_frame(audio.control.get());
        const std::uint64_t epoch = choose_safe_local_epoch();
        if (!authority.activateLocalGrid(epoch, packet_frame)) {
            return false;
        }
        const auto& grid = authority.grid();
        commands.state.metronome.store(
            grid.run_state == jam2::GridRunState::Running,
            std::memory_order_relaxed);
        commands.state.metronome_mode.store(grid.mode, std::memory_order_relaxed);
        commands.state.metronome_epoch_sample_time.store(epoch, std::memory_order_relaxed);
        commands.state.metronome_epoch_valid.store(true, std::memory_order_relaxed);
        commands.state.metronome_render_offset_frames.store(0, std::memory_order_relaxed);
        commands.state.metronome_revision.store(grid.revision, std::memory_order_relaxed);
        mesh_grid_target_valid = false;
        apply_authority_role();
        return true;
    };
    auto align_to_authority_bar = [&](const MetronomePayload& metronome,
                                      std::uint64_t authority_packet_frame,
                                      const jam2::PeerStream& stream) {
        if (stream.stats().rtt_min_us == 0 || audio.control == nullptr) {
            return false;
        }
        const auto pattern = metronome.has_pattern
            ? jam2::metronome::sanitize(metronome.pattern)
            : metronome_pattern_from_runtime(commands.state);
        const std::uint64_t step_frames = jam2::metronome::step_interval_samples(
            static_cast<double>(options.sample_rate), pattern.bpm, pattern.division);
        const std::uint64_t bar_frames =
            step_frames * static_cast<std::uint64_t>(pattern.step_count);
        if (bar_frames == 0) {
            return false;
        }
        const std::uint64_t one_way_frames = stream.stats().rtt_min_us *
            static_cast<std::uint64_t>(options.sample_rate) / 2000000ULL;
        const std::uint64_t projected = authority_packet_frame + one_way_frames;
        std::uint64_t frames_until_bar = 0;
        if (projected < metronome.epoch_sample_time) {
            frames_until_bar = metronome.epoch_sample_time - projected;
        } else {
            const std::uint64_t phase =
                (projected - metronome.epoch_sample_time) % bar_frames;
            frames_until_bar = phase == 0 ? bar_frames : bar_frames - phase;
        }
        commands.state.metronome_epoch_sample_time.store(
            current_engine_frame(audio.control.get()) + frames_until_bar,
            std::memory_order_relaxed);
        commands.state.metronome_epoch_valid.store(true, std::memory_order_relaxed);
        commands.state.metronome_render_offset_frames.store(0, std::memory_order_relaxed);
        mesh_grid_target_offset_frames = 0;
        mesh_grid_target_valid = true;
        return true;
    };

    commands.state.metronome_epoch_sample_time.store(0, std::memory_order_relaxed);
    commands.state.metronome_epoch_valid.store(false, std::memory_order_relaxed);
    commands.state.metronome_local_authority.store(false, std::memory_order_relaxed);
    commands.state.leader_audio_local_click.store(false, std::memory_order_relaxed);
    if (authority.localIsBootstrapCoordinator()) {
        if (authority.orderGridProposal({
                local_peer_id.value,
                next_local_grid_request_id++,
                grid_run_state_from_runtime(),
                static_cast<std::uint8_t>(
                    commands.state.metronome_mode.load(std::memory_order_relaxed)),
                0,
            })) {
            (void)activate_local_grid();
        }
    }

    auto aggregate_stats = [&]() {
        AudioPacketStats stats;
        stats.local_peer_id = network_session.localPeerId().value;
        stats.bootstrap_role = "static";
        stats.session_protocol_version = network_session.contract().protocol_version;
        stats.session_audio_format = "pcm24-mono";
        stats.session_sample_rate = network_session.contract().sample_rate;
        stats.session_frames_per_packet = network_session.contract().frames_per_packet;
        stats.network_peer_count = network_session.peerCount();
        const auto& grid = authority.grid();
        const auto& authority_stats = authority.stats();
        stats.bootstrap_coordinator_peer_id = authority.bootstrapCoordinatorPeerId();
        stats.arrangement_authority_peer_id = authority.arrangementAuthorityPeerId();
        stats.grid_authority_peer_id = grid.authority_peer_id;
        stats.grid_revision = grid.revision;
        stats.grid_run_state = static_cast<std::uint64_t>(grid.run_state);
        stats.grid_mode = grid.mode;
        stats.grid_authority_epoch_frame = grid.authority_epoch_frame;
        stats.grid_mapped_epoch_frame =
            commands.state.metronome_epoch_sample_time.load(std::memory_order_relaxed);
        stats.grid_authority_packet_frame = grid.authority_packet_frame;
        stats.grid_proposals_accepted = authority_stats.grid_proposals_accepted;
        stats.grid_proposals_rejected = authority_stats.grid_proposals_rejected;
        stats.grid_assignments_accepted = authority_stats.grid_assignments_accepted;
        stats.grid_assignments_rejected = authority_stats.grid_assignments_rejected;
        stats.grid_authority_states_accepted = authority_stats.grid_authority_states_accepted;
        stats.grid_authority_states_rejected = authority_stats.grid_authority_states_rejected;
        stats.grid_authority_missing_events = authority_stats.grid_authority_missing_events;
        stats.transport_events_accepted = authority_stats.transport_events_accepted;
        stats.transport_events_rejected = authority_stats.transport_events_rejected;
        stats.grid_proposals_sent = mesh_grid_proposals_sent;
        stats.grid_assignments_sent = mesh_grid_assignments_sent;
        stats.grid_authority_states_sent = mesh_grid_authority_states_sent;
        stats.grid_mapping_error_frames = mesh_grid_target_valid
            ? mesh_grid_target_offset_frames -
                commands.state.metronome_render_offset_frames.load(std::memory_order_relaxed)
            : 0;
        stats.transport_source_peer_id = mesh_transport_source_peer_id;
        stats.transport_event_counter = mesh_transport_event_counter;
        stats.transport_grid_revision = mesh_transport_grid_revision;
        stats.leader_audio_source_peer_id = mesh_leader_audio_source_peer_id;
        stats.leader_audio_injected_packets = mesh_leader_audio_injected_packets;
        stats.transport_source_frame = mesh_transport_source_frame;
        stats.transport_requested_target_frame = mesh_transport_requested_target_frame;
        stats.transport_applied_target_frame = mesh_transport_applied_target_frame;
        stats.metronome_compensation_stale_events = mesh_compensation_stale_events;
        stats.startup_drained_packets = static_cast<std::uint64_t>(drained_startup_packets);
        stats.sample_time_playout_enabled = true;
        stats.playout_delay_frames = playout_delay_frames;
        stats.jitter_buffer_enabled = playout_delay_frames > 0;
        stats.jitter_buffer_target_frames = playout_delay_frames;
        stats.jitter_buffer_max_frames = static_cast<std::uint64_t>(options.jitter_buffer_max_frames);
        stats.udp_work_budget_yields = mesh_work_budget_yields;
        for (const auto& entry : peers) {
            const auto& peer = entry.second;
            stats.sent_packets += peer.sent_packets;
            stats.sent_bytes += peer.sent_bytes;
            stats.recv_packets += peer.recv_packets;
            stats.recv_bytes += peer.recv_bytes;
            stats.ignored_packets += peer.ignored_packets;
            stats.udp_parse.add(peer.udp_parse);
            stats.sent_pings += peer.sent_pings;
            stats.sent_pongs += peer.sent_pongs;
            stats.recv_pongs += peer.recv_pongs;
            add_peer_stream_stats(stats, network_session.peerStream(peer.peer_id).stats());
        }
        copy_peer_mixer_stats(stats, network_session.mixStats());
        stats.udp_work_budget_yields += network_session.mixStats().work_budget_yields;
        return stats;
    };

    auto print_mesh_stats = [&](std::uint64_t now_us, const CsvStatsLog::AudioSnapshot* provided_audio_snapshot = nullptr) {
        const std::uint64_t elapsed_ms = (now_us - start_time) / 1000ULL;
        const AudioPacketStats stats = aggregate_stats();
        CsvStatsLog::AudioSnapshot current_audio_snapshot;
        if (provided_audio_snapshot == nullptr) {
            current_audio_snapshot = make_audio_snapshot(
                audio.stream.get(),
                audio.capture_ring.get(),
                audio.playback_ring.get(),
                audio.control.get());
            provided_audio_snapshot = &current_audio_snapshot;
        }
        const auto& audio_snapshot = *provided_audio_snapshot;
        if (options.status_jsonl) {
            std::cout << "{\"event\":\"mesh_stats\",\"elapsed_ms\":" << elapsed_ms
                      << ",\"peer_count\":" << peers.size()
                      << ",\"bootstrap_coordinator_peer_id\":" << stats.bootstrap_coordinator_peer_id
                      << ",\"arrangement_authority_peer_id\":" << stats.arrangement_authority_peer_id
                      << ",\"grid_authority_peer_id\":" << stats.grid_authority_peer_id
                      << ",\"grid_revision\":" << stats.grid_revision
                      << ",\"grid_run_state\":" << stats.grid_run_state
                      << ",\"grid_mode\":" << stats.grid_mode
                      << ",\"grid_authority_epoch_frame\":" << stats.grid_authority_epoch_frame
                      << ",\"grid_mapped_epoch_frame\":" << stats.grid_mapped_epoch_frame
                      << ",\"grid_mapping_error_frames\":" << stats.grid_mapping_error_frames
                      << ",\"leader_audio_source_peer_id\":" << stats.leader_audio_source_peer_id
                      << ",\"leader_audio_injected_packets\":" << stats.leader_audio_injected_packets
                      << ",\"metronome\":\"" << (commands.state.metronome.load(std::memory_order_relaxed) ? "on" : "off") << "\""
                      << ",\"bpm\":" << commands.state.bpm.load(std::memory_order_relaxed)
                      << ",\"metronome_beats_per_bar\":" << commands.state.metronome_beats_per_bar.load(std::memory_order_relaxed)
                      << ",\"metronome_division\":" << commands.state.metronome_division.load(std::memory_order_relaxed)
                      << ",\"metronome_epoch_sample_frame\":" << commands.state.metronome_epoch_sample_time.load(std::memory_order_relaxed)
                      << ",\"engine_frame\":"
                      << (audio.control ? audio.control->engine_frame_counter.load(std::memory_order_relaxed) : 0ULL)
                      << ",\"recording_input_latency_frames\":" << (audio.control ? audio.control->input_latency_frames.load(std::memory_order_relaxed) : 0U)
                      << ",\"recording_output_latency_frames\":" << (audio.control ? audio.control->output_latency_frames.load(std::memory_order_relaxed) : 0U)
                      << ",\"recording_latency_adjustment_frames\":" << (audio.control ? audio.control->recording_latency_adjustment_frames.load(std::memory_order_relaxed) : 0)
                      << ",\"recording_latency_compensation_frames\":" << (audio.control ? audio.control->recording_latency_compensation_frames.load(std::memory_order_relaxed) : 0ULL)
                      << ",\"metronome_render_offset_frames\":" << commands.state.metronome_render_offset_frames.load(std::memory_order_relaxed)
                      << ",\"transport_revision\":" << commands.state.transport_revision.load(std::memory_order_relaxed)
                      << ",\"transport_action\":" << commands.state.transport_action.load(std::memory_order_relaxed)
                      << ",\"transport_target_frame\":" << commands.state.transport_target_raw_frame.load(std::memory_order_relaxed)
                      << ",\"transport_pending\":" << (commands.state.transport_pending.load(std::memory_order_relaxed) ? "true" : "false")
                      << ",\"audio_sample_rate\":" << options.sample_rate
                      << ",\"sent_packets\":" << stats.sent_packets
                      << ",\"recv_packets\":" << stats.recv_packets
                      << ",\"sent_bytes\":" << stats.sent_bytes
                      << ",\"recv_bytes\":" << stats.recv_bytes
                      << ",\"ignored_packets\":" << stats.ignored_packets
                      << ",\"sent_pings\":" << stats.sent_pings
                      << ",\"sent_pongs\":" << stats.sent_pongs
                      << ",\"recv_pongs\":" << stats.recv_pongs
                      << ",\"rtt_avg_ms\":" << rtt_avg_ms(stats)
                      << ",\"jitter_avg_ms\":" << avg_us_to_ms(stats.jitter_sum_us, stats.jitter_samples)
                      << ",\"jitter_max_ms\":"
                      << (stats.jitter_samples > 0 ? static_cast<double>(stats.jitter_max_us) / 1000.0 : 0.0)
                      << ",\"sequence_lost\":" << stats.sequence.lost
                      << ",\"sequence_loss_events\":" << stats.sequence.loss_events
                      << ",\"sequence_loss_max_gap\":" << stats.sequence.loss_max_gap
                      << ",\"sequence_loss_percent\":" << sequence_loss_percent(stats)
                      << ",\"sequence_duplicate\":" << stats.sequence.duplicate
                      << ",\"sequence_out_of_order\":" << stats.sequence.out_of_order
                      << ",\"sequence_late\":" << stats.sequence.late
                      << ",\"playout_delay_frames\":" << playout_delay_frames
                      << ",\"jitter_buffer\":\"" << (stats.jitter_buffer_enabled ? "on" : "off") << "\""
                      << ",\"jitter_buffer_target_frames\":" << stats.jitter_buffer_target_frames
                      << ",\"jitter_buffer_target_ms\":"
                      << frames_to_ms(static_cast<std::size_t>(stats.jitter_buffer_target_frames), options.sample_rate)
                      << ",\"network_active_peer_count\":" << stats.network_active_peer_count
                      << ",\"mix_contributing_peers\":" << stats.mix_contributing_peers
                      << ",\"mix_released_slots\":" << stats.mix_released_slots
                      << ",\"mix_complete_slots\":" << stats.mix_complete_slots
                      << ",\"mix_deadline_slots\":" << stats.mix_deadline_slots
                      << ",\"mix_missing_peer_frames\":" << stats.mix_missing_peer_frames
                      << ",\"mix_late_after_release_frames\":" << stats.mix_late_after_release_frames
                      << ",\"mix_capacity_drops\":" << stats.mix_capacity_drops
                      << ",\"mix_clipped_samples\":" << stats.mix_clipped_samples
                      << ",\"capture_ring_readable_ms\":"
                      << frames_to_ms(audio_snapshot.capture_ring_readable, options.sample_rate)
                      << ",\"playback_ring_readable_ms\":" << frames_to_ms(audio_snapshot.playback_ring_readable, options.sample_rate)
                      << ",\"playback_ring_underruns\":" << audio_snapshot.playback_ring_underruns
                      << ",\"playback_ring_underrun_events\":" << audio_snapshot.playback_ring_underrun_events
                      << ",\"input_peak\":" << audio_snapshot.input_peak
                      << ",\"send_peak\":" << audio_snapshot.send_peak
                      << ",\"remote_peak\":" << audio_snapshot.remote_peak
                      << ",\"output_peak\":" << audio_snapshot.output_peak
                      << ",\"output_clipped_samples\":" << audio_snapshot.output_clipped_samples
                      << ",\"peers\":[";
            bool first = true;
            for (const auto& entry : peers) {
                const auto& peer = entry.second;
                const auto& peer_stats = network_session.peerStream(peer.peer_id).stats();
                const auto* mix_stats = network_session.peerMixStats(peer.peer_id);
                if (!first) {
                    std::cout << ",";
                }
                first = false;
                std::cout << "{\"endpoint\":\"" << json_escape(jam2::endpoint_to_string(peer.endpoint)) << "\""
                          << ",\"peer_id\":" << peer.peer_id.value
                          << ",\"endpoint_proof_state\":\"" << endpoint_proof_name(peer.endpoint_proof) << "\""
                          << ",\"endpoint_proof_attempts\":" << peer.proof_attempts
                          << ",\"endpoint_proof_successes\":" << peer.proof_successes
                          << ",\"endpoint_proof_failures\":" << peer.proof_failures
                          << ",\"endpoint_unverified_drops\":" << peer.proof_unverified_drops
                          << ",\"endpoint_unmatched_pongs\":" << peer.proof_unmatched_pongs
                          << ",\"endpoint_challenge_overwrites\":" << peer.proof_challenge_overwrites
                          << ",\"sent_packets\":" << peer.sent_packets
                          << ",\"recv_packets\":" << peer.recv_packets
                          << ",\"ignored_packets\":" << peer.ignored_packets
                          << ",\"sequence_lost\":" << peer_stats.sequence.lost
                          << ",\"sequence_duplicate\":" << peer_stats.sequence.duplicate
                          << ",\"sequence_out_of_order\":" << peer_stats.sequence.out_of_order
                          << ",\"sequence_late\":" << peer_stats.sequence.late
                          << ",\"rtt_avg_ms\":"
                          << (peer_stats.rtt_samples > 0 ? static_cast<double>(peer_stats.rtt_sum_us) / static_cast<double>(peer_stats.rtt_samples) / 1000.0 : 0.0)
                          << ",\"jitter_avg_ms\":"
                          << (peer_stats.jitter_samples > 0 ? static_cast<double>(peer_stats.jitter_sum_us) / static_cast<double>(peer_stats.jitter_samples) / 1000.0 : 0.0)
                          << ",\"drift_ppm\":" << peer_stats.drift_ppm
                          << ",\"resampler_ratio\":" << peer_stats.resampler_ratio
                          << ",\"mix_queue_depth_frames\":" << (mix_stats != nullptr ? mix_stats->queue_depth_frames : 0ULL)
                          << ",\"mix_queue_high_water_frames\":" << (mix_stats != nullptr ? mix_stats->queue_high_water_frames : 0ULL)
                          << ",\"mix_late_after_release_frames\":" << (mix_stats != nullptr ? mix_stats->late_after_release_frames : 0ULL)
                          << "}";
            }
            std::cout << "]}\n";
        } else {
            std::cout << "mesh_stats elapsed_ms=" << elapsed_ms
                      << " peer_count=" << peers.size()
                      << " metronome=" << (commands.state.metronome.load(std::memory_order_relaxed) ? "on" : "off")
                      << " bpm=" << commands.state.bpm.load(std::memory_order_relaxed)
                      << " metronome_beats_per_bar=" << commands.state.metronome_beats_per_bar.load(std::memory_order_relaxed)
                      << " metronome_division=" << commands.state.metronome_division.load(std::memory_order_relaxed)
                      << " metronome_epoch_sample_frame=" << commands.state.metronome_epoch_sample_time.load(std::memory_order_relaxed)
                      << " engine_frame="
                      << (audio.control ? audio.control->engine_frame_counter.load(std::memory_order_relaxed) : 0ULL)
                      << " metronome_render_offset_frames=" << commands.state.metronome_render_offset_frames.load(std::memory_order_relaxed)
                      << " audio_sample_rate=" << options.sample_rate
                      << " sent_packets=" << stats.sent_packets
                      << " recv_packets=" << stats.recv_packets
                      << " sequence_lost=" << stats.sequence.lost
                      << " sequence_loss_percent=" << sequence_loss_percent(stats)
                      << " jitter_avg_ms=" << avg_us_to_ms(stats.jitter_sum_us, stats.jitter_samples)
                      << " rtt_avg_ms=" << rtt_avg_ms(stats)
                      << " active_peers=" << stats.network_active_peer_count
                      << " mix_released_slots=" << stats.mix_released_slots
                      << " mix_deadline_slots=" << stats.mix_deadline_slots
                      << " mix_missing_peer_frames=" << stats.mix_missing_peer_frames
                      << " mix_capacity_drops=" << stats.mix_capacity_drops
                      << " playback_ring_readable_ms=" << frames_to_ms(audio_snapshot.playback_ring_readable, options.sample_rate)
                      << " remote_peak=" << audio_snapshot.remote_peak
                      << " output_peak=" << audio_snapshot.output_peak << "\n";
            for (const auto& entry : peers) {
                const auto& peer = entry.second;
                const auto& peer_stats = network_session.peerStream(peer.peer_id).stats();
                const auto* mix_stats = network_session.peerMixStats(peer.peer_id);
                std::cout << "mesh_peer endpoint=" << jam2::endpoint_to_string(peer.endpoint)
                          << " peer_id=" << peer.peer_id.value
                          << " endpoint_proof_state=" << endpoint_proof_name(peer.endpoint_proof)
                          << " endpoint_proof_attempts=" << peer.proof_attempts
                          << " endpoint_proof_successes=" << peer.proof_successes
                          << " endpoint_proof_failures=" << peer.proof_failures
                          << " endpoint_unverified_drops=" << peer.proof_unverified_drops
                          << " endpoint_unmatched_pongs=" << peer.proof_unmatched_pongs
                          << " endpoint_challenge_overwrites=" << peer.proof_challenge_overwrites
                          << " sent_packets=" << peer.sent_packets
                          << " recv_packets=" << peer.recv_packets
                          << " ignored_packets=" << peer.ignored_packets
                          << " sequence_lost=" << peer_stats.sequence.lost
                          << " sequence_duplicate=" << peer_stats.sequence.duplicate
                          << " sequence_out_of_order=" << peer_stats.sequence.out_of_order
                          << " sequence_late=" << peer_stats.sequence.late
                          << " drift_ppm=" << peer_stats.drift_ppm
                          << " resampler_ratio=" << peer_stats.resampler_ratio
                          << " mix_queue_depth_frames=" << (mix_stats != nullptr ? mix_stats->queue_depth_frames : 0ULL)
                          << " mix_queue_high_water_frames=" << (mix_stats != nullptr ? mix_stats->queue_high_water_frames : 0ULL)
                          << " mix_late_after_release_frames=" << (mix_stats != nullptr ? mix_stats->late_after_release_frames : 0ULL)
                          << "\n";
            }
        }
    };

    while (jam2::monotonic_us() < receive_deadline && !commands.state.quit.load(std::memory_order_relaxed)) {
        const std::uint64_t now = jam2::monotonic_us();
        const std::uint64_t local_grid_request_sequence =
            commands.state.grid_request_sequence.load(std::memory_order_acquire);
        if (local_grid_request_sequence != last_local_grid_request_sequence) {
            last_local_grid_request_sequence = local_grid_request_sequence;
            jam2::GridProposal proposal{
                local_peer_id.value,
                next_local_grid_request_id++,
                grid_run_state_from_runtime(),
                static_cast<std::uint8_t>(
                    commands.state.metronome_mode.load(std::memory_order_relaxed)),
                commands.state.metronome_epoch_sample_time.load(std::memory_order_relaxed),
            };
            if (authority.localIsBootstrapCoordinator()) {
                if (authority.orderGridProposal(proposal)) {
                    pending_local_grid_proposal.reset();
                    (void)activate_local_grid();
                    next_grid_assignment_send_us = 0;
                }
            } else {
                pending_local_grid_proposal = proposal;
                next_grid_proposal_send_us = 0;
                commands.state.metronome_epoch_valid.store(false, std::memory_order_relaxed);
                commands.state.metronome_local_authority.store(false, std::memory_order_relaxed);
                commands.state.leader_audio_local_click.store(false, std::memory_order_relaxed);
            }
        }
        const int grid_mode = commands.state.metronome_mode.load(std::memory_order_relaxed);
        const bool listener_compensated =
            grid_mode == metronome_mode_id(MetronomeMode::ListenerCompensated);
        if (listener_compensated && !authority.localIsGridAuthority()) {
            const jam2::PeerId authority_peer{authority.grid().authority_peer_id};
            const jam2::PeerStream* authority_stream = nullptr;
            for (const auto& entry : peers) {
                if (entry.second.peer_id == authority_peer) {
                    authority_stream = &network_session.peerStream(authority_peer);
                    break;
                }
            }
            const auto* authority_mix = network_session.peerMixStats(authority_peer);
            const bool fresh = authority_stream != nullptr && authority_mix != nullptr &&
                authority.grid().run_state != jam2::GridRunState::AuthorityMissing &&
                last_authority_state_received_us != 0 &&
                now - last_authority_state_received_us <= 500000ULL &&
                authority_stream->playoutSampleTimeInitialized() &&
                commands.state.metronome_epoch_valid.load(std::memory_order_relaxed);
            if (fresh) {
                const std::uint64_t remote_head =
                    authority_stream->nextPlayoutRemoteSampleTime() > authority_mix->queue_depth_frames
                    ? authority_stream->nextPlayoutRemoteSampleTime() - authority_mix->queue_depth_frames
                    : 0ULL;
                const std::uint64_t local_frame = current_engine_frame(audio.control.get());
                const std::uint64_t local_epoch =
                    commands.state.metronome_epoch_sample_time.load(std::memory_order_relaxed);
                const auto pattern = metronome_pattern_from_runtime(commands.state);
                const std::int64_t interval = static_cast<std::int64_t>(
                    jam2::metronome::step_interval_samples(
                        static_cast<double>(options.sample_rate), pattern.bpm, pattern.division));
                if (interval > 0) {
                    auto phase = [&](std::uint64_t frame, std::uint64_t epoch) {
                        const std::int64_t position = frame >= epoch
                            ? static_cast<std::int64_t>(frame - epoch)
                            : -static_cast<std::int64_t>(epoch - frame);
                        const std::int64_t value = position % interval;
                        return value < 0 ? value + interval : value;
                    };
                    std::int64_t error = phase(remote_head, remote_authority_epoch_frame) -
                        phase(local_frame, local_epoch);
                    if (error > interval / 2) error -= interval;
                    if (error < -interval / 2) error += interval;
                    mesh_grid_target_offset_frames =
                        commands.state.metronome_render_offset_frames.load(std::memory_order_relaxed) + error;
                    mesh_grid_target_valid = true;
                }
                mesh_compensation_was_stale = false;
            } else if (!mesh_compensation_was_stale) {
                ++mesh_compensation_stale_events;
                mesh_compensation_was_stale = true;
            }
        }
        const bool correction_enabled = !authority.localIsGridAuthority() &&
            (grid_mode == metronome_mode_id(MetronomeMode::SharedGrid) || listener_compensated) &&
            mesh_grid_target_valid;
        if (correction_enabled &&
            (mesh_grid_last_update_us == 0 || now - mesh_grid_last_update_us >= 10000ULL)) {
            const std::int64_t current =
                commands.state.metronome_render_offset_frames.load(std::memory_order_relaxed);
            const std::int64_t max_frames = std::abs(ms_to_signed_frames(
                options.metronome_compensation_max_ms, options.sample_rate));
            const std::int64_t target = std::clamp(
                mesh_grid_target_offset_frames, -max_frames, max_frames);
            const double elapsed_ms = mesh_grid_last_update_us == 0
                ? 10.0
                : static_cast<double>(now - mesh_grid_last_update_us) / 1000.0;
            const double alpha = options.metronome_compensation_smoothing_ms > 0.0
                ? std::clamp(elapsed_ms / options.metronome_compensation_smoothing_ms, 0.0, 1.0)
                : 1.0;
            std::int64_t step = static_cast<std::int64_t>(
                std::llround(static_cast<double>(target - current) * alpha));
            const std::int64_t max_step = std::abs(ms_to_signed_frames(
                options.metronome_compensation_slew_ms_per_sec * elapsed_ms / 1000.0,
                options.sample_rate));
            if (max_step > 0) {
                step = std::clamp(step, -max_step, max_step);
            }
            commands.state.metronome_render_offset_frames.store(
                current + step,
                std::memory_order_relaxed);
            mesh_grid_last_update_us = now;
        }
        commit_due_transport(commands.state, audio.control.get());
        sync_audio_control(commands.state, audio.control.get(), 1.0);
        network_session.advance(now);

        int sends_this_loop = 0;
        while (now >= packet_schedule.nextAudioSendUs() &&
               packet_schedule.nextAudioSendUs() < send_deadline &&
               sends_this_loop < 8) {
            std::span<const std::uint8_t> payload = silence_payload;
            if (audio.capture_ring != nullptr) {
                if (audio.capture_ring->available_read() < static_cast<std::size_t>(options.frame_size)) {
                    break;
                }
                (void)audio.capture_ring->pop(asio_frames);
                for (std::size_t i = 0; i < asio_frames.size(); ++i) {
                    network_frames[i] = asio_frames[i] / 256;
                }
                apply_send_level(network_frames, commands.state.send_level_ppm.load(std::memory_order_relaxed));
                if (audio.control != nullptr) {
                    const int send_peak_ppm = pcm24_peak_ppm(network_frames);
                    update_peak(audio.control->send_peak_ppm, send_peak_ppm);
                    update_peak(audio.control->gui_send_peak_ppm, send_peak_ppm);
                }
                if (commands.state.metronome_mode.load(std::memory_order_relaxed) ==
                        metronome_mode_id(MetronomeMode::LeaderAudio) &&
                    commands.state.leader_audio_local_click.load(std::memory_order_relaxed) &&
                    commands.state.metronome.load(std::memory_order_relaxed)) {
                    mix_leader_click_into_packet(
                        network_frames,
                        packet_schedule.sampleTime(),
                        options.sample_rate,
                        gain_from_ppm(commands.state.metronome_level_ppm.load(std::memory_order_relaxed)),
                        commands.state.metronome_epoch_sample_time.load(std::memory_order_relaxed),
                        metronome_pattern_from_runtime(commands.state));
                    mesh_leader_audio_source_peer_id = local_peer_id.value;
                    ++mesh_leader_audio_injected_packets;
                }
                (void)jam2::protocol::pack_pcm24_into(network_frames, packed_audio_payload);
                payload = packed_audio_payload;
            } else if (commands.state.metronome_mode.load(std::memory_order_relaxed) ==
                           metronome_mode_id(MetronomeMode::LeaderAudio) &&
                       commands.state.leader_audio_local_click.load(std::memory_order_relaxed) &&
                       commands.state.metronome.load(std::memory_order_relaxed)) {
                std::fill(network_frames.begin(), network_frames.end(), 0);
                mix_leader_click_into_packet(
                    network_frames,
                    packet_schedule.sampleTime(),
                    options.sample_rate,
                    gain_from_ppm(commands.state.metronome_level_ppm.load(std::memory_order_relaxed)),
                    commands.state.metronome_epoch_sample_time.load(std::memory_order_relaxed),
                    metronome_pattern_from_runtime(commands.state));
                mesh_leader_audio_source_peer_id = local_peer_id.value;
                ++mesh_leader_audio_injected_packets;
                (void)jam2::protocol::pack_pcm24_into(network_frames, packed_audio_payload);
                payload = packed_audio_payload;
            }
            const auto send_result = network_session.sendToActive(
                jam2::protocol::PacketType::Audio,
                packet_schedule.audioSequence(),
                packet_schedule.sampleTime(),
                now,
                payload);
            for (auto& entry : peers) {
                auto& peer = entry.second;
                if (peer.endpoint_proof != EndpointProofState::Active) {
                    continue;
                }
                ++peer.sent_packets;
                peer.sent_bytes += send_result.packet_size;
            }
            packet_schedule.commitAudioPacket();
            ++sends_this_loop;
        }

        if (now >= packet_schedule.nextPingUs() && now < send_deadline) {
            for (auto& entry : peers) {
                auto& peer = entry.second;
                if (peer.endpoint_proof == EndpointProofState::Probing &&
                    peer.proof_attempts >= 8 &&
                    now >= peer.proof_deadline_us) {
                    peer.endpoint_proof = EndpointProofState::Failed;
                    network_session.setPeerEndpointState(peer.peer_id, jam2::PeerEndpointState::Failed);
                    ++peer.proof_failures;
                    peer.next_probe_us = now + 5000000ULL;
                }
                if (peer.endpoint_proof == EndpointProofState::Failed) {
                    if (now < peer.next_probe_us) {
                        continue;
                    }
                    peer.endpoint_proof = EndpointProofState::Candidate;
                    network_session.setPeerEndpointState(peer.peer_id, jam2::PeerEndpointState::Candidate);
                    peer.proof_attempts = 0;
                    peer.proof_deadline_us = 0;
                    for (ProbeChallenge& challenge : peer.probe_challenges) {
                        challenge.used = false;
                    }
                }
                if (peer.endpoint_proof != EndpointProofState::Active) {
                    if (peer.proof_attempts >= 8 || now < peer.next_probe_us) {
                        continue;
                    }
                    peer.endpoint_proof = EndpointProofState::Probing;
                    network_session.setPeerEndpointState(peer.peer_id, jam2::PeerEndpointState::Probing);
                    peer.next_probe_us = now + 250000ULL;
                }
                const std::uint32_t ping_sequence = packet_schedule.takeControlSequence();
                ProbeChallenge& challenge =
                    peer.probe_challenges[ping_sequence % peer.probe_challenges.size()];
                if (challenge.used) {
                    ++peer.proof_challenge_overwrites;
                }
                challenge = ProbeChallenge{ping_sequence, now, true};
                if (peer.endpoint_proof == EndpointProofState::Probing) {
                    ++peer.proof_attempts;
                    if (peer.proof_attempts == 8) {
                        peer.proof_deadline_us = now + 1000000ULL;
                    }
                }
                network_session.sendToPeer(
                    peer.peer_id,
                    jam2::protocol::PacketType::Ping,
                    ping_sequence,
                    0,
                    now,
                    {},
                    true);
                ++peer.sent_pings;
            }
            packet_schedule.commitPing();
        }

        if (now >= packet_schedule.nextGridStateUs() && now < send_deadline) {
            const int bpm = commands.state.bpm.load(std::memory_order_relaxed);
            const auto pattern = metronome_pattern_from_runtime(commands.state);
            auto make_grid_payload = [&](GridMessageKind kind,
                                         std::uint64_t revision_or_request,
                                         std::uint64_t epoch,
                                         std::uint8_t mode,
                                         jam2::GridRunState run_state) {
                return encode_metronome_payload(
                    run_state == jam2::GridRunState::Running ? bpm : -bpm,
                    revision_or_request,
                    epoch,
                    pattern,
                    kind,
                    mode,
                    run_state);
            };
            if (pending_local_grid_proposal && now >= next_grid_proposal_send_us) {
                const auto& proposal = *pending_local_grid_proposal;
                const auto payload = make_grid_payload(
                    GridMessageKind::Proposal,
                    proposal.request_id,
                    proposal.proposed_epoch_frame,
                    proposal.mode,
                    proposal.run_state);
                network_session.sendToPeer(
                    jam2::PeerId{authority.bootstrapCoordinatorPeerId()},
                    jam2::protocol::PacketType::MetronomeState,
                    packet_schedule.takeControlSequence(),
                    current_engine_frame(audio.control.get()),
                    now,
                    payload);
                ++mesh_grid_proposals_sent;
                next_grid_proposal_send_us = now + 20000ULL;
            }
            if (authority.localIsBootstrapCoordinator() && authority.grid().revision != 0 &&
                now >= next_grid_assignment_send_us) {
                const auto& grid = authority.grid();
                const auto payload = make_grid_payload(
                    GridMessageKind::Assignment,
                    grid.revision,
                    grid.authority_epoch_frame,
                    grid.mode,
                    grid.run_state);
                network_session.sendToActive(
                    jam2::protocol::PacketType::MetronomeState,
                    packet_schedule.takeControlSequence(),
                    grid.authority_peer_id,
                    now,
                    payload);
                ++mesh_grid_assignments_sent;
                next_grid_assignment_send_us = now + 100000ULL;
            }
            if (authority.localIsGridAuthority() &&
                commands.state.metronome_epoch_valid.load(std::memory_order_relaxed)) {
                const auto& grid = authority.grid();
                const auto payload = make_grid_payload(
                    GridMessageKind::AuthorityState,
                    grid.revision,
                    commands.state.metronome_epoch_sample_time.load(std::memory_order_relaxed),
                    grid.mode,
                    grid.run_state);
                network_session.sendToActive(
                    jam2::protocol::PacketType::MetronomeState,
                    packet_schedule.takeControlSequence(),
                    current_engine_frame(audio.control.get()),
                    now,
                    payload);
                ++mesh_grid_authority_states_sent;
            }
            packet_schedule.scheduleNextGridState(now, 20000ULL);
        }

        std::uint64_t network_transport_revision = 0;
        std::uint64_t transport_target = 0;
        int transport_action = 0;
        {
            std::lock_guard<std::mutex> lock(commands.state.transport_mutex);
            network_transport_revision = commands.state.transport_network_revision.load(std::memory_order_relaxed);
            transport_target = commands.state.transport_network_target_raw_frame.load(std::memory_order_relaxed);
            transport_action = commands.state.transport_network_action.load(std::memory_order_relaxed);
        }
        if (network_transport_revision != sending_transport_revision) {
            sending_transport_revision = network_transport_revision;
            next_transport_send = 0;
        }
        if (authority.localIsArrangementAuthority() &&
            authority.grid().revision <= (std::numeric_limits<std::uint32_t>::max)() &&
            sending_transport_revision != 0 &&
            sending_transport_revision <= (std::numeric_limits<std::uint32_t>::max)() &&
            now >= next_transport_send &&
            current_engine_frame(audio.control.get()) <= transport_target &&
            now < send_deadline) {
            const std::uint64_t engine_now = current_engine_frame(audio.control.get());
            const auto payload = encode_transport_payload({
                static_cast<jam2::gui_control::TransportAction>(
                    transport_action),
                static_cast<std::uint32_t>(sending_transport_revision),
                static_cast<std::uint32_t>(authority.grid().revision),
                transport_target,
            });
            network_session.sendToActive(
                jam2::protocol::PacketType::TransportState,
                packet_schedule.takeControlSequence(),
                engine_now,
                now,
                payload);
            mesh_transport_source_peer_id = local_peer_id.value;
            mesh_transport_event_counter = sending_transport_revision;
            mesh_transport_grid_revision = authority.grid().revision;
            mesh_transport_source_frame = engine_now;
            mesh_transport_requested_target_frame = transport_target;
            mesh_transport_applied_target_frame = transport_target;
            next_transport_send = now + 20000ULL;
        }

        if (commands.state.print_stats.exchange(false, std::memory_order_relaxed) ||
            commands.state.print_status.exchange(false, std::memory_order_relaxed)) {
            print_mesh_stats(now);
        }
        if (next_stats != 0 && now >= next_stats) {
            if (commands.state.stats_enabled.load(std::memory_order_relaxed)) {
                print_mesh_stats(now);
            }
            if (csv_log) {
                csv_log->write_periodic(
                    (now - start_time) / 1000ULL,
                    aggregate_stats(),
                    options,
                    make_audio_snapshot(audio.stream.get(), audio.capture_ring.get(), audio.playback_ring.get(), audio.control.get()));
            }
            next_stats += static_cast<std::uint64_t>(options.stats_interval_ms) * 1000ULL;
        }

        bool received_any = false;
        std::size_t mesh_datagrams_this_wake = 0;
        while (mesh_datagrams_this_wake < 64) {
            const auto received = network_session.receiveFor(received_any ? 0ULL : 1000ULL);
            if (!received) {
                break;
            }
            ++mesh_datagrams_this_wake;
            received_any = true;
            const auto& from = received->endpoint;
            const std::span<const std::uint8_t> bytes = received->bytes;
            auto peer_it = peers.find(endpoint_key(from));
            if (peer_it == peers.end()) {
                continue;
            }
            auto& peer = peer_it->second;
            const auto parsed = network_session.parse(bytes);
            if (!parsed) {
                peer.udp_parse.observe(parsed.error);
                ++peer.ignored_packets;
                continue;
            }
            try {
                const auto& header = parsed.header;
                if (header.type != jam2::protocol::PacketType::Ping &&
                    header.type != jam2::protocol::PacketType::Pong &&
                    (peer.endpoint_proof != EndpointProofState::Active ||
                     !network_session.acceptsEndpoint(from))) {
                    ++peer.proof_unverified_drops;
                    ++peer.ignored_packets;
                    continue;
                }
                if (header.type == jam2::protocol::PacketType::Audio) {
                    if (header.payload_length != audio_payload_size) {
                        ++peer.ignored_packets;
                        continue;
                    }
                    const std::uint64_t receive_time = jam2::monotonic_us();
                    const auto received_payload = std::span<const std::uint8_t>(
                        bytes.data() + jam2::protocol::kHeaderSize,
                        header.payload_length);
                    const auto receive_result = network_session.peerStream(peer.peer_id).receiveAudio(
                        header,
                        received_payload,
                        receive_time);
                    if (receive_result != jam2::PeerAudioResult::Accepted) {
                        ++peer.ignored_packets;
                        continue;
                    }
                    ++peer.recv_packets;
                    peer.recv_bytes += bytes.size();
                } else if (header.type == jam2::protocol::PacketType::Ping) {
                    auto& peer_stream = network_session.peerStream(peer.peer_id);
                    if (!peer_stream.acceptReplay(jam2::PeerReplayChannel::Ping, header.sequence)) {
                        ++peer.ignored_packets;
                        continue;
                    }
                    network_session.sendToPeer(
                        peer.peer_id,
                        jam2::protocol::PacketType::Pong,
                        header.sequence,
                        0,
                        header.send_time_us,
                        {},
                        true);
                    ++peer.sent_pongs;
                } else if (header.type == jam2::protocol::PacketType::Pong) {
                    ProbeChallenge& challenge =
                        peer.probe_challenges[header.sequence % peer.probe_challenges.size()];
                    if (!challenge.used ||
                        challenge.sequence != header.sequence ||
                        challenge.send_time_us != header.send_time_us) {
                        ++peer.proof_unmatched_pongs;
                        ++peer.ignored_packets;
                        continue;
                    }
                    challenge.used = false;
                    const std::uint64_t receive_time = jam2::monotonic_us();
                    if (receive_time >= challenge.send_time_us) {
                        network_session.peerStream(peer.peer_id).observeRtt(
                            receive_time - challenge.send_time_us);
                    }
                    if (peer.endpoint_proof != EndpointProofState::Active) {
                        peer.endpoint_proof = EndpointProofState::Active;
                        network_session.setPeerEndpointState(
                            peer.peer_id,
                            jam2::PeerEndpointState::Active);
                        ++peer.proof_successes;
                    }
                    ++peer.recv_pongs;
                } else if (header.type == jam2::protocol::PacketType::MetronomeState) {
                    auto& peer_stream = network_session.peerStream(peer.peer_id);
                    if (!peer_stream.acceptReplay(
                            jam2::PeerReplayChannel::Metronome,
                            header.sequence)) {
                        ++peer.ignored_packets;
                        continue;
                    }
                    const auto payload = std::span<const std::uint8_t>(
                        bytes.data() + jam2::protocol::kHeaderSize,
                        header.payload_length);
                    const MetronomePayload metronome = decode_metronome_payload(payload);
                    const int remote_abs_bpm = std::abs(metronome.bpm);
                    if (remote_abs_bpm <= 0 || remote_abs_bpm > 400 ||
                        metronome.kind == GridMessageKind::LegacyState) {
                        ++peer.ignored_packets;
                        continue;
                    }
                    auto store_grid_settings = [&] {
                        if (commands.state.grid_request_sequence.load(std::memory_order_acquire) !=
                            last_local_grid_request_sequence) {
                            return;
                        }
                        if (metronome.has_pattern) {
                            store_metronome_pattern(commands.state, metronome.pattern);
                        }
                        commands.state.bpm.store(remote_abs_bpm, std::memory_order_relaxed);
                        commands.state.metronome.store(
                            metronome.run_state == jam2::GridRunState::Running,
                            std::memory_order_relaxed);
                        commands.state.metronome_mode.store(metronome.mode, std::memory_order_relaxed);
                    };
                    if (metronome.kind == GridMessageKind::Proposal) {
                        const auto ordered = authority.orderGridProposal({
                            peer.peer_id.value,
                            metronome.revision_or_request,
                            metronome.run_state,
                            metronome.mode,
                            metronome.epoch_sample_time,
                        });
                        if (ordered) {
                            store_grid_settings();
                            commands.state.metronome_revision.store(
                                ordered->revision,
                                std::memory_order_relaxed);
                            commands.state.metronome_epoch_valid.store(false, std::memory_order_relaxed);
                            mesh_grid_target_valid = false;
                            apply_authority_role();
                            next_grid_assignment_send_us = 0;
                        }
                    } else if (metronome.kind == GridMessageKind::Assignment) {
                        const jam2::GridAuthorityState assignment{
                            metronome.revision_or_request,
                            header.sample_time,
                            metronome.run_state,
                            metronome.mode,
                            metronome.epoch_sample_time,
                            0,
                        };
                        const auto result = authority.acceptGridAssignment(
                            peer.peer_id.value,
                            assignment);
                        if (result == jam2::AuthorityUpdateResult::Accepted) {
                            store_grid_settings();
                            commands.state.metronome_revision.store(
                                assignment.revision,
                                std::memory_order_relaxed);
                            pending_local_grid_proposal.reset();
                            mesh_grid_target_valid = false;
                            if (authority.localIsGridAuthority()) {
                                (void)activate_local_grid();
                            } else {
                                commands.state.metronome_epoch_valid.store(false, std::memory_order_relaxed);
                                apply_authority_role();
                            }
                        }
                    } else if (metronome.kind == GridMessageKind::AuthorityState) {
                        const jam2::GridAuthorityState remote_state{
                            metronome.revision_or_request,
                            peer.peer_id.value,
                            metronome.run_state,
                            metronome.mode,
                            metronome.epoch_sample_time,
                            header.sample_time,
                        };
                        const auto result = authority.acceptGridAuthorityState(
                            peer.peer_id.value,
                            remote_state);
                        if (result == jam2::AuthorityUpdateResult::Accepted) {
                            store_grid_settings();
                            commands.state.metronome_revision.store(
                                remote_state.revision,
                                std::memory_order_relaxed);
                            remote_authority_epoch_frame = metronome.epoch_sample_time;
                            last_authority_state_received_us = jam2::monotonic_us();
                            if (!commands.state.metronome_epoch_valid.load(std::memory_order_relaxed)) {
                                (void)align_to_authority_bar(
                                    metronome,
                                    header.sample_time,
                                    peer_stream);
                            }
                            if (metronome.mode == metronome_mode_id(MetronomeMode::SharedGrid) &&
                                commands.state.metronome_epoch_valid.load(std::memory_order_relaxed)) {
                                const auto pattern = metronome_pattern_from_runtime(commands.state);
                                const std::uint64_t step_frames = jam2::metronome::step_interval_samples(
                                    static_cast<double>(options.sample_rate), pattern.bpm, pattern.division);
                                const std::uint64_t bar_frames = step_frames *
                                    static_cast<std::uint64_t>(pattern.step_count);
                                if (bar_frames > 0) {
                                    const std::uint64_t projected = header.sample_time +
                                        peer_stream.stats().rtt_min_us *
                                            static_cast<std::uint64_t>(options.sample_rate) / 2000000ULL;
                                    const std::uint64_t local_frame = current_engine_frame(audio.control.get());
                                    const std::uint64_t local_epoch =
                                        commands.state.metronome_epoch_sample_time.load(std::memory_order_relaxed);
                                    auto phase = [&](std::uint64_t frame, std::uint64_t epoch) {
                                        return frame >= epoch
                                            ? static_cast<std::int64_t>((frame - epoch) % bar_frames)
                                            : -static_cast<std::int64_t>((epoch - frame) % bar_frames);
                                    };
                                    std::int64_t error = phase(projected, metronome.epoch_sample_time) -
                                        phase(local_frame, local_epoch);
                                    const std::int64_t interval = static_cast<std::int64_t>(bar_frames);
                                    if (error > interval / 2) error -= interval;
                                    if (error < -interval / 2) error += interval;
                                    mesh_grid_target_offset_frames =
                                        commands.state.metronome_render_offset_frames.load(
                                            std::memory_order_relaxed) + error;
                                    mesh_grid_target_valid = true;
                                }
                            }
                            apply_authority_role();
                        }
                    }
                } else if (header.type == jam2::protocol::PacketType::TransportState) {
                    auto& peer_stream = network_session.peerStream(peer.peer_id);
                    if (!peer_stream.acceptReplay(
                            jam2::PeerReplayChannel::Transport,
                            header.sequence)) {
                        ++peer.ignored_packets;
                        continue;
                    }
                    const auto payload = std::span<const std::uint8_t>(
                        bytes.data() + jam2::protocol::kHeaderSize,
                        header.payload_length);
                    const TransportPayload transport = decode_transport_payload(payload);
                    const bool accepted_transport = authority.acceptTransportEvent(
                        peer.peer_id.value,
                        transport.event_counter,
                        transport.grid_revision);
                    if (accepted_transport) {
                        mesh_transport_source_peer_id = peer.peer_id.value;
                        mesh_transport_event_counter = transport.event_counter;
                        mesh_transport_grid_revision = transport.grid_revision;
                        mesh_transport_source_frame = header.sample_time;
                        mesh_transport_requested_target_frame = transport.target_sender_frame;
                    }
                    if ((transport.action == jam2::gui_control::TransportAction::TrackRestart ||
                         transport.action == jam2::gui_control::TransportAction::RecordStart) &&
                        accepted_transport &&
                        peer.recv_pongs > 0 &&
                        peer_stream.stats().rtt_min_us > 0 &&
                        audio.prepared_source != nullptr) {
                        const std::uint64_t sender_lead_frames =
                            transport.target_sender_frame > header.sample_time
                            ? transport.target_sender_frame - header.sample_time
                            : 0ULL;
                        const std::uint64_t one_way_frames = peer_stream.stats().rtt_min_us *
                            static_cast<std::uint64_t>(options.sample_rate) / 2000000ULL;
                        const std::uint64_t frames_until_target = sender_lead_frames > one_way_frames
                            ? sender_lead_frames - one_way_frames
                            : 0ULL;
                        const std::uint64_t target_raw_frame =
                            current_engine_frame(audio.control.get()) + frames_until_target;
                        mesh_transport_applied_target_frame = target_raw_frame;
                        const std::int64_t offset =
                            commands.state.metronome_render_offset_frames.load(std::memory_order_relaxed);
                        const QuantizedSchedule schedule{
                            target_raw_frame,
                            target_raw_frame,
                            musical_frame_from_raw(target_raw_frame, offset),
                        };
                        const bool seek_ok = enqueue_prepared_command(
                            audio.prepared_source.get(),
                            audio.control.get(),
                            {jam2::audio::PreparedTrackSource::CommandType::Seek, 0, schedule.target_raw_frame, 0, 0, 1000000});
                        const bool play_ok = enqueue_prepared_command(
                            audio.prepared_source.get(),
                            audio.control.get(),
                            {jam2::audio::PreparedTrackSource::CommandType::Play, 0, schedule.target_raw_frame, 0, 0, 1000000});
                        if (seek_ok && play_ok) {
                            publish_transport_schedule(
                                commands.state,
                                jam2::gui_control::TransportAction::TrackRestart,
                                schedule,
                                false);
                        }
                    }
                } else if (header.type == jam2::protocol::PacketType::Bye) {
                    (void)network_session.peerStream(peer.peer_id).acceptReplay(
                        jam2::PeerReplayChannel::Bye,
                        header.sequence);
                    if (authority.markPeerInactive(peer.peer_id.value)) {
                        commands.state.leader_audio_local_click.store(false, std::memory_order_relaxed);
                        commands.state.metronome_local_authority.store(false, std::memory_order_relaxed);
                        mesh_grid_target_valid = false;
                    }
                    ++peer.ignored_packets;
                } else {
                    ++peer.ignored_packets;
                }
            } catch (const std::exception&) {
                ++peer.ignored_packets;
            }
        }
        if (mesh_datagrams_this_wake == 64) {
            ++mesh_work_budget_yields;
        }
    }

    const std::uint64_t now = jam2::monotonic_us();
    network_session.finish(now);
    network_session.sendToActive(
        jam2::protocol::PacketType::Bye,
        packet_schedule.takeControlSequence(),
        0,
        now,
        {});
    detach_network_capture(audio);
    const auto final_audio_snapshot = make_audio_snapshot(
        audio.stream.get(),
        audio.capture_ring.get(),
        audio.playback_ring.get(),
        audio.control.get());
    print_mesh_stats(now, &final_audio_snapshot);
    auto final_stats = aggregate_stats();
    final_stats.elapsed_ms = (now - start_time) / 1000ULL;
    if (csv_log) {
        csv_log->write(
            "final",
            final_stats.elapsed_ms,
            final_stats,
            options,
            final_audio_snapshot);
    }
    if (commands.state.stats_enabled.load(std::memory_order_relaxed)) {
        print_audio_packet_stats(final_stats, options);
        print_optional_audio_stats(audio, options);
    }
    finalize_active_recording(audio, options, commands.state);
    std::cout.flush();
    return 0;
}

int run(int argc, char** argv)
{
    if (argc <= 1) {
        std::cout << kUsage;
        return 0;
    }

    const std::string_view command{argv[1]};
    if (command == "--help" || command == "-h" || command == "help") {
        std::cout << kUsage;
        return 0;
    }

    if (command == "list-devices") {
        const auto devices = jam2::audio::list_devices();
        if (devices.empty()) {
            std::cout << "No audio devices found for this MVP backend.\n";
            return 0;
        }
        for (const auto& device : devices) {
            std::cout << "[" << device.id << "] " << device.backend << " " << device.name << "\n";
        }
        return 0;
    }

    if (command == "test-device") {
        return run_test_device(argc, argv);
    }

    if (command == "meter-device") {
        return run_meter_device(argc, argv);
    }

    if (command == "ring-device") {
        return run_ring_device(argc, argv);
    }

    if (command == "local") {
        return run_local(argc, argv);
    }

    if (command == "listen") {
        return run_listen(argc, argv);
    }

    if (command == "connect") {
        return run_connect(argc, argv);
    }

    if (command == "mesh") {
        return run_mesh(argc, argv);
    }

    std::cerr << "Unknown command: " << command << "\n\n" << kUsage;
    return 2;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "fatal: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "fatal: unknown error\n";
        return 1;
    }
}

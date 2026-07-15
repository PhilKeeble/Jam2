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
#include "metronome.hpp"
#include "network_session.hpp"
#include "pcm16_wav.hpp"
#include "peer_stream.hpp"
#include "protocol.hpp"
#include "session_authority.hpp"
#include "stun.hpp"
#include "tuning_profile.hpp"
#include "udp_socket.hpp"
#include "cli_entry.hpp"
#include "runtime_entry.hpp"

namespace {

constexpr std::string_view kUsage = R"(Jam2 - direct low-latency music streaming

Usage:
  jam2                              Launch the GUI
  jam2 <command> [options]

Commands:
  list-devices                      List available low-latency audio devices
  test-device <id>                  Show device capabilities
  meter-device <id>                 Run an input meter
  ring-device <id>                  Exercise device/ring handoff
  local                             Run the local audio engine without networking
  network create                    Create a coordinated direct Jam2 session
  network join <jam2-url>           Join a direct Jam2 session
  debug describe                    Describe the automation interface
  debug run <scenario.json>         Run a bounded automation scenario

Help:
  jam2 -h | --help | help
  jam2 <command> -h
  jam2 network -h
  jam2 network <create|join> -h
  jam2 debug <describe|run> -h
)";

constexpr std::string_view kNetworkUsage = R"(Usage:
  jam2 network create [options]
  jam2 network join <jam2-url> [options]

Subcommands:
  create    Create the TCP coordinator and direct UDP session
  join      Join using a jam2:// invitation URL
Run `jam2 network <subcommand> -h` for its options.
)";

bool is_help_argument(std::string_view argument) noexcept
{
    return argument == "-h" || argument == "--help" || argument == "help";
}

bool has_help_argument(int argc, char** argv, int start) noexcept
{
    for (int index = start; index < argc; ++index) {
        if (is_help_argument(argv[index])) {
            return true;
        }
    }
    return false;
}

void print_device_help(std::string_view command)
{
    if (command == "list-devices") {
        std::cout << R"(Usage:
  jam2 list-devices

Lists the numeric device id, backend, and name for every available low-latency
audio device. Use an id with --audio-device or the device diagnostic commands.
)";
    } else if (command == "test-device") {
        std::cout << R"(Usage:
  jam2 test-device <id> [--sample-rate <hz>]

Options:
  --sample-rate <hz>    Rate to check for device support (default: 48000)
)";
    } else if (command == "meter-device") {
        std::cout << R"(Usage:
  jam2 meter-device <id> [options]

Options:
  --sample-rate <hz>       Device sample rate (default: 48000)
  --buffer-size <frames>   Requested callback buffer size
  --duration-ms <ms>       Meter duration (default: 3000)
)";
    } else {
        std::cout << R"(Usage:
  jam2 ring-device <id> [options]

Options:
  --sample-rate <hz>       Device sample rate (default: 48000)
  --buffer-size <frames>   Requested callback buffer size
  --duration-ms <ms>       Test duration (default: 3000)
  --ring-frames <frames>   Ring capacity (default: 4096)
)";
    }
}

void print_audio_options_help()
{
    std::cout << R"(
Audio and engine options:
  --profile <fast|moderate|safe>       Apply a tuning profile; later flags override it
  --audio-device <id>                  Use a real audio device
  --headless-audio <on|off>            Use the synthetic test device
  --headless-clock-drift-ppm <-5000..5000>
                                       Test-only synthetic clock offset
  --sample-rate <hz>                   Device/session sample rate
  --audio-buffer-size <frames>         Requested audio callback size
  --frame-size <32|64|128|256>         Audio frames per UDP packet
  --input-channels <1,2,...>           One-based input channels mixed to mono
  --output-channels <1,2,...>          One-based outputs receiving mono playback
  --capture-ring-frames <frames>       Capture ring capacity
  --playback-ring-frames <frames>      Playback ring capacity
  --playback-prefill-frames <frames>   Initial playback fill target
  --playback-max-frames <frames>       Maximum retained playback depth
  --test-input <off|silence|tone-440|pulse-1s|metro-pulse>
                                       Deterministic native input source

Playout and drift options:
  --sample-time-playout <on|off>       Use packet sample times for playout
  --playout-delay-frames <frames>      Target sample-time playout delay
  --jitter-buffer-frames <frames>      Jitter-buffer target
  --jitter-buffer-max-frames <frames>  Jitter-buffer capacity/span limit
  --adaptive-playback-cushion <on|off> Enable explicit adaptive cushion changes
  --adaptive-playback-target-frames <frames>
  --adaptive-playback-min-frames <frames>
  --adaptive-playback-max-frames <frames>
  --adaptive-playback-release-ppm <0..1000000>
  --drift-correction <on|off>          Enable receive-side resampling
  --drift-smoothing <0..1>             Drift estimate smoothing alpha
  --drift-deadband-ppm <0..50000>      Correction deadband
  --drift-max-correction-ppm <0..50000>
                                       Maximum resampler correction

Metronome and mix options:
  --metronome <on|off>
  --bpm <1..400>
  --metronome-level <0..4>
  --metronome-mode <shared-grid|leader-audio|listener-compensated>
  --metronome-compensation-max-ms <0..1000>
  --metronome-compensation-smoothing-ms <0..10000>
  --metronome-compensation-deadband-ms <0..1000>
  --metronome-compensation-slew-ms-per-sec <0..10000>
  --remote-level <0..4>
  --send-level <0..4>
  --local-monitor <on|off>
  --local-monitor-level <0..4>

Runtime, diagnostics, and artifacts:
  --stream-ms <ms>                    Stop after this duration; 0 means no limit
  --stream-linger-ms <ms>             Receive linger after the send deadline
  --stats <enabled|disabled>
  --stats-interval-ms <ms>            Periodic stats interval; 0 disables it
  --stats-warmup-ms <ms>              Exclude startup packets from measurements
  --log-stats <folder>                Write the exact emitted CSV path there
  --record-jam-folder <folder>        Record local jam stems
  --os-priority <off|high|realtime>   Process/UDP worker scheduling request
)";
}

void print_local_help()
{
    std::cout << R"(Usage:
  jam2 local (--audio-device <id> | --headless-audio on) [options]

Runs the persistent local audio engine without opening UDP or using STUN.
)";
    print_audio_options_help();
}

void print_network_options_help()
{
    std::cout << R"(
Network options:
  --bind <host:port>                  Local network bind endpoint
  --socket-send-buffer <bytes>        Request the OS UDP send buffer size
  --socket-recv-buffer <bytes>        Request the OS UDP receive buffer size
  --grid-coordinator <on|off>         Start as the initial grid coordinator
)";
}

void print_network_create_help()
{
    std::cout << R"(Usage:
  jam2 network create [options]

Creates the TCP coordinator and direct UDP session, using the same local port
number for both protocols, then prints a jam2:// invitation. STUN discovers
only the public UDP candidate; audio remains direct UDP.

Create/bootstrap options:
  --stun <host:port>                  STUN server endpoint
  --public-endpoint <host:port>       Explicit advertised UDP candidate
  --no-stun                           Skip STUN discovery
  --stun-timeout-ms <ms>              Timeout for each STUN attempt
  --stun-retries <count>              Positive STUN attempt count
  --wait-ms <ms>                      Bootstrap timeout; 0 uses the default
  --session-id <hex>                  Explicit test/session id (requires key)
  --session-key <32-hex-chars>        Explicit 16-byte session key (requires id)
  --max-peers <count>                 Optional remote-peer limit; 0 is unlimited
)";
    print_network_options_help();
    print_audio_options_help();
}

void print_network_join_help()
{
    std::cout << R"(Usage:
  jam2 network join <jam2-url> [options]

Joins the endpoint/session carried by a jam2:// invitation URL.

Join/bootstrap options:
  --wait-ms <ms>                      Coordinator bootstrap timeout; 0 uses the default
)";
    print_network_options_help();
    print_audio_options_help();
}

using MetronomeMode = Jam2MetronomeMode;
using TestInputMode = Jam2TestInputMode;
using OsPriorityMode = Jam2OsPriorityMode;
using Options = Jam2RuntimeOptions;

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

struct ParsedPeerList {
    std::vector<jam2::Endpoint> endpoints;
    std::vector<std::uint64_t> peer_ids;
};

std::uint64_t parse_peer_id(std::string_view value, std::string_view option)
{
    std::size_t consumed = 0;
    std::uint64_t peer_id = 0;
    try {
        peer_id = std::stoull(std::string(value), &consumed, 10);
    } catch (const std::exception&) {
        throw std::runtime_error(std::string(option) + " must be a non-zero unsigned integer");
    }
    if (peer_id == 0 || consumed != value.size()) {
        throw std::runtime_error(std::string(option) + " must be a non-zero unsigned integer");
    }
    return peer_id;
}

ParsedPeerList parse_peer_list(std::string_view value, const jam2::Endpoint& self)
{
    ParsedPeerList peers;
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
        const std::size_t identity_separator = part.find('@');
        const std::uint64_t peer_id = identity_separator == std::string_view::npos
            ? 0ULL
            : parse_peer_id(part.substr(0, identity_separator), "--peers peer id");
        const std::string_view endpoint_text = identity_separator == std::string_view::npos
            ? part
            : part.substr(identity_separator + 1);
        if (endpoint_text.empty()) {
            throw std::runtime_error("--peers contains an empty endpoint");
        }
        const jam2::Endpoint endpoint = jam2::parse_endpoint(endpoint_text);
        if (endpoint.host == self.host && endpoint.port == self.port) {
            throw std::runtime_error("--peers must not include the local --bind endpoint");
        }
        const auto duplicate = std::find_if(peers.endpoints.begin(), peers.endpoints.end(), [&](const jam2::Endpoint& existing) {
            return existing.host == endpoint.host && existing.port == endpoint.port;
        });
        if (duplicate != peers.endpoints.end()) {
            throw std::runtime_error("--peers contains a duplicate endpoint: " + jam2::endpoint_to_string(endpoint));
        }
        if (peer_id != 0 && std::find(peers.peer_ids.begin(), peers.peer_ids.end(), peer_id) != peers.peer_ids.end()) {
            throw std::runtime_error("--peers contains a duplicate peer id");
        }
        peers.endpoints.push_back(endpoint);
        peers.peer_ids.push_back(peer_id);
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
            ParsedPeerList peers = parse_peer_list(require_value(argc, argv, i, arg), options.bind);
            options.mesh_peers = std::move(peers.endpoints);
            options.mesh_peer_ids = std::move(peers.peer_ids);
        } else if (arg == "--local-peer-id") {
            options.local_peer_id = parse_peer_id(require_value(argc, argv, i, arg), arg);
        } else if (arg == "--bootstrap-coordinator-peer-id") {
            options.bootstrap_coordinator_peer_id = parse_peer_id(require_value(argc, argv, i, arg), arg);
        } else if (arg == "--bootstrap-role") {
            const std::string value{require_value(argc, argv, i, arg)};
            if (value == "creator") {
                options.bootstrap_role = jam2::SessionBootstrapRole::Creator;
            } else if (value == "joiner") {
                options.bootstrap_role = jam2::SessionBootstrapRole::Joiner;
            } else {
                throw std::runtime_error("--bootstrap-role must be creator or joiner");
            }
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
        } else if (arg == "--arm-stream-on-first-peer") {
            options.arm_stream_on_first_peer = true;
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
        } else if (arg == "--headless-clock-drift-ppm") {
            options.headless_clock_drift_ppm = std::stoi(std::string(require_value(argc, argv, i, arg)));
            if (options.headless_clock_drift_ppm < -5000 || options.headless_clock_drift_ppm > 5000) {
                throw std::runtime_error("--headless-clock-drift-ppm must be -5000..5000");
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
    if (!options.headless_audio && options.headless_clock_drift_ppm != 0) {
        throw std::runtime_error("--headless-clock-drift-ppm requires --headless-audio on");
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

struct EngineControlMirror {
    bool initialized = false;
    jam2::EngineSnapshot snapshot;
};

void sync_engine_control(
    const RuntimeState& runtime,
    jam2::Engine* engine,
    double playback_ratio,
    EngineControlMirror& mirror)
{
    if (engine == nullptr) {
        return;
    }
    if (!mirror.initialized) {
        mirror.snapshot = engine->snapshot();
        mirror.initialized = true;
    }
    auto submit = [&](jam2::EngineCommand command) {
        return engine->submit(command);
    };
    const bool enabled = runtime.metronome.load(std::memory_order_relaxed);
    if (mirror.snapshot.metronome_enabled != enabled) {
        jam2::EngineCommand command; command.type = jam2::EngineCommandType::SetMetronomeEnabled; command.enabled = enabled;
        if (submit(command)) mirror.snapshot.metronome_enabled = enabled;
    }
    const auto pattern = metronome_pattern_from_runtime(runtime);
    const auto& previous = mirror.snapshot.metronome_pattern;
    if (pattern.bpm != previous.bpm || pattern.beats_per_bar != previous.beats_per_bar ||
        pattern.division != previous.division || pattern.play_mask_low != previous.play_mask_low ||
        pattern.play_mask_high != previous.play_mask_high || pattern.accent_mask_low != previous.accent_mask_low ||
        pattern.accent_mask_high != previous.accent_mask_high) {
        jam2::EngineCommand command; command.type = jam2::EngineCommandType::SetMetronomePattern; command.pattern = pattern;
        if (submit(command)) mirror.snapshot.metronome_pattern = pattern;
    }
    const auto sync_value = [&](jam2::EngineCommandType type, int value, int& cached) {
        if (value == cached) return;
        jam2::EngineCommand command; command.type = type; command.value = value;
        if (submit(command)) cached = value;
    };
    sync_value(jam2::EngineCommandType::SetMetronomeLevel, runtime.metronome_level_ppm.load(std::memory_order_relaxed), mirror.snapshot.metronome_level_ppm);
    sync_value(jam2::EngineCommandType::SetRemoteLevel, runtime.remote_level_ppm.load(std::memory_order_relaxed), mirror.snapshot.remote_level_ppm);
    sync_value(jam2::EngineCommandType::SetSendLevel, runtime.send_level_ppm.load(std::memory_order_relaxed), mirror.snapshot.send_level_ppm);
    sync_value(jam2::EngineCommandType::SetLocalMonitorLevel, runtime.local_monitor_level_ppm.load(std::memory_order_relaxed), mirror.snapshot.local_monitor_level_ppm);
    const bool monitor = runtime.local_monitor.load(std::memory_order_relaxed);
    if (mirror.snapshot.local_monitor_enabled != monitor) {
        jam2::EngineCommand command; command.type = jam2::EngineCommandType::SetLocalMonitorEnabled; command.enabled = monitor;
        if (submit(command)) mirror.snapshot.local_monitor_enabled = monitor;
    }
    const int mode = runtime.metronome_mode.load(std::memory_order_relaxed);
    if (static_cast<int>(mirror.snapshot.metronome_mode) != mode) {
        jam2::EngineCommand command; command.type = jam2::EngineCommandType::SetMetronomeMode; command.value = mode;
        if (submit(command)) mirror.snapshot.metronome_mode = static_cast<jam2::EngineMetronomeMode>(mode);
    }
    const std::uint64_t epoch = runtime.metronome_epoch_sample_time.load(std::memory_order_relaxed);
    const bool epoch_valid = runtime.metronome_epoch_valid.load(std::memory_order_relaxed);
    if (mirror.snapshot.metronome_epoch_frame != epoch || mirror.snapshot.metronome_epoch_valid != epoch_valid) {
        jam2::EngineCommand command; command.type = jam2::EngineCommandType::SetMetronomeEpoch; command.frame = epoch; command.enabled = epoch_valid;
        if (submit(command)) { mirror.snapshot.metronome_epoch_frame = epoch; mirror.snapshot.metronome_epoch_valid = epoch_valid; }
    }
    const std::int64_t offset = runtime.metronome_render_offset_frames.load(std::memory_order_relaxed);
    if (mirror.snapshot.metronome_render_offset_frames != offset) {
        jam2::EngineCommand command; command.type = jam2::EngineCommandType::SetMetronomeRenderOffset; command.signed_value = offset;
        if (submit(command)) mirror.snapshot.metronome_render_offset_frames = offset;
    }
    engine->setNetworkPlaybackRatio(playback_ratio);
}

bool enqueue_prepared_restart(
    jam2::Engine* engine,
    std::uint64_t target_frame)
{
    if (engine == nullptr) return false;
    jam2::EngineCommand seek; seek.type = jam2::EngineCommandType::PreparedSeek; seek.frame = target_frame;
    jam2::EngineCommand play; play.type = jam2::EngineCommandType::PreparedPlay; play.frame = target_frame;
    return engine->submit(seek) && engine->submit(play);
}

void write_recording_sidecar(
    const std::filesystem::path& folder,
    const jam2::EngineRecordingSnapshot& stats,
    int sample_rate,
    const Options& options,
    const RuntimeState& state);
std::uint64_t current_engine_frame(const jam2::Engine* engine)
{
    return engine != nullptr ? engine->snapshot().engine_frame : 0ULL;
}

void begin_metronome_epoch(
    RuntimeState& state,
    const jam2::Engine* engine,
    int sample_rate)
{
    const std::uint64_t lead_frames = static_cast<std::uint64_t>(std::max(1, sample_rate)) / 5ULL;
    state.metronome_render_offset_frames.store(0, std::memory_order_relaxed);
    state.metronome_epoch_sample_time.store(
        current_engine_frame(engine) + lead_frames,
        std::memory_order_relaxed);
    state.metronome_epoch_valid.store(true, std::memory_order_relaxed);
}

void hold_shared_grid_at_start(
    RuntimeState& state,
    jam2::Engine* engine)
{
    if (state.metronome_mode.load(std::memory_order_relaxed) !=
        metronome_mode_id(MetronomeMode::SharedGrid)) {
        return;
    }
    state.metronome_epoch_sample_time.store(0, std::memory_order_relaxed);
    state.metronome_epoch_valid.store(false, std::memory_order_relaxed);
    state.metronome_render_offset_frames.store(0, std::memory_order_relaxed);
    if (engine != nullptr) {
        jam2::EngineCommand epoch; epoch.type = jam2::EngineCommandType::SetMetronomeEpoch; epoch.frame = 0; epoch.enabled = false;
        jam2::EngineCommand offset; offset.type = jam2::EngineCommandType::SetMetronomeRenderOffset; offset.signed_value = 0;
        (void)engine->submit(epoch);
        (void)engine->submit(offset);
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
    const jam2::Engine* engine,
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
    const std::uint64_t raw_now = current_engine_frame(engine);
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
    jam2::EngineTransportAction action,
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

void commit_due_transport(RuntimeState& state, const jam2::Engine* engine)
{
    std::lock_guard<std::mutex> lock(state.transport_mutex);
    if (!state.transport_pending.load(std::memory_order_acquire) ||
        current_engine_frame(engine) < state.transport_target_raw_frame.load(std::memory_order_relaxed)) {
        return;
    }
    state.transport_pending.store(false, std::memory_order_release);
    const int action = state.transport_action.load(std::memory_order_relaxed);
    if (action == static_cast<int>(jam2::EngineTransportAction::TrackRestart) ||
        action == static_cast<int>(jam2::EngineTransportAction::RecordStart)) {
        state.metronome_epoch_sample_time.store(
            state.transport_target_musical_frame.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        state.metronome_epoch_valid.store(true, std::memory_order_relaxed);
        state.metronome_revision.fetch_add(1, std::memory_order_relaxed);
    }
}

bool parse_frame_or_now(std::string_view text, const jam2::Engine* engine, std::uint64_t& out)
{
    if (text == "now" || text.empty()) {
        out = current_engine_frame(engine);
        return true;
    }
    return parse_u64(text, out);
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
    jam2::EngineTransportAction action = jam2::EngineTransportAction::None;
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
    const auto action = static_cast<jam2::EngineTransportAction>(payload[1]);
    if (action != jam2::EngineTransportAction::TrackRestart &&
        action != jam2::EngineTransportAction::TrackStop &&
        action != jam2::EngineTransportAction::TrackPlay &&
        action != jam2::EngineTransportAction::RecordStart &&
        action != jam2::EngineTransportAction::RecordStop) {
        throw std::runtime_error("transport action is invalid");
    }
    return {
        action,
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
        if (original_thread_priority_ != THREAD_PRIORITY_ERROR_RETURN) {
            (void)SetThreadPriority(GetCurrentThread(), original_thread_priority_);
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
        original_thread_priority_ = GetThreadPriority(GetCurrentThread());
        status_.thread_priority = win_thread_priority_text(original_thread_priority_);
        status_.mmcss_requested = "off";
        status_.mmcss_active = "off";
        status_.timer_resolution_requested = "off";
        status_.timer_resolution_active = "off";
        if (options.os_priority == OsPriorityMode::Off) {
            return;
        }

        // Keep the whole application at high priority. Realtime is restricted
        // to this packet worker (plus MMCSS) so Qt/file/control workers cannot
        // starve the OS if a GUI task misbehaves.
        const DWORD priority_class = HIGH_PRIORITY_CLASS;
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
    int original_thread_priority_ = THREAD_PRIORITY_ERROR_RETURN;
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
    const jam2::EngineRecordingSnapshot& stats,
    int sample_rate,
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
        << "  \"sample_rate\": " << sample_rate << ",\n"
        << "  \"stems\": [\"mix.wav\", \"my-input.wav\", \"their-input.wav\", \"inputs-mix.wav\", \"metronome.wav\"],\n"
        << "  \"recording_folder\": \"" << json_escape(folder.string()) << "\",\n"
        << "  \"start_audio_frame\": " << stats.start_frame << ",\n"
        << "  \"stop_audio_frame\": " << stats.stop_frame << ",\n"
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
                "transport_applied_target_frame,transport_action\n";
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
             << stats.transport_applied_target_frame << ','
             << stats.transport_action;
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
        std::vector<std::string> fields(346);
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
        set(345, stats.transport_action);

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

void mix_leader_click_into_packet(
    std::span<std::int32_t> samples,
    std::uint64_t packet_sample_time,
    int sample_rate,
    double level,
    std::uint64_t epoch_sample_time,
    jam2::metronome::PatternSnapshot pattern)
{
    if (sample_rate <= 0 || samples.empty()) return;
    pattern = jam2::metronome::sanitize(pattern);
    const std::uint64_t step_interval = jam2::metronome::step_interval_samples(
        static_cast<double>(sample_rate), pattern.bpm, pattern.division);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const std::uint64_t absolute_sample = packet_sample_time + static_cast<std::uint64_t>(i);
        if (absolute_sample < epoch_sample_time) continue;
        const std::uint64_t grid_sample = absolute_sample - epoch_sample_time;
        samples[i] = jam2::metronome::mix_pcm24(
            samples[i],
            jam2::metronome::render_sample(
                pattern, grid_sample, step_interval, static_cast<double>(sample_rate), level));
    }
}

class CliPeerStreamPlayback final : public jam2::PeerStreamPlayback {
public:
    explicit CliPeerStreamPlayback(jam2::Engine* engine) noexcept : engine_(engine) {}
    bool acceptsFrames() const noexcept override { return engine_ != nullptr; }
    std::size_t depthFrames() const noexcept override
    {
        return engine_ != nullptr
            ? engine_->networkPlaybackDepth()
            : (std::numeric_limits<std::size_t>::max)() / 2U;
    }
    std::size_t pushFrames(std::span<const std::int32_t> frames) noexcept override
    {
        return engine_ != nullptr ? engine_->pushNetworkPlayback(frames) : 0;
    }
    void requestDropFrames(std::size_t frames) noexcept override
    {
        if (engine_ != nullptr) engine_->requestNetworkPlaybackDrop(frames);
    }
    void setResamplerRatio(double ratio) noexcept override
    {
        if (engine_ != nullptr) engine_->setNetworkPlaybackRatio(ratio);
    }
    void detach() noexcept { engine_ = nullptr; }

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
    std::unique_ptr<jam2::Engine> owned_engine;
    EngineObserver<jam2::Engine> engine;
    jam2::NetworkCaptureAttachment network_capture;
    bool persistent_engine = false;
};

bool engine_restart_required(
    const jam2::EngineConfig& active,
    const jam2::EngineConfig& requested) noexcept
{
    return active.backend != requested.backend ||
        active.audio_device_id != requested.audio_device_id ||
        active.sample_rate != requested.sample_rate ||
        active.audio_buffer_frames != requested.audio_buffer_frames ||
        active.headless_clock_drift_ppm != requested.headless_clock_drift_ppm ||
        active.input_channels != requested.input_channels ||
        active.channels.input != requested.channels.input ||
        active.channels.output != requested.channels.output ||
        active.capture_ring_frames != requested.capture_ring_frames ||
        active.playback_ring_frames != requested.playback_ring_frames ||
        active.playback_prefill_frames != requested.playback_prefill_frames ||
        active.diagnostics_enabled != requested.diagnostics_enabled ||
        active.test_input != requested.test_input ||
        active.prepared_track_max_frames != requested.prepared_track_max_frames;
}

jam2::EngineConfig make_engine_config_impl(const Options& options, bool leader_audio_local_click)
{
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
    config.headless_clock_drift_ppm = options.headless_clock_drift_ppm;
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
    return config;
}

OptionalAudioStream start_optional_audio(
    const Options& options,
    bool leader_audio_local_click,
    Jam2RuntimeHost* runtime_host = nullptr)
{
    OptionalAudioStream audio;
    if (!options.audio_device_id && !options.headless_audio) {
        return audio;
    }
    const jam2::EngineConfig config = make_engine_config_impl(options, leader_audio_local_click);

    if (runtime_host != nullptr && runtime_host->engine != nullptr) {
        const jam2::EngineConfig* active = runtime_host->engine->config();
        if (active == nullptr || engine_restart_required(*active, config)) {
            throw std::runtime_error("native runtime engine configuration does not match the active engine");
        }
        audio.engine.value = runtime_host->engine;
        audio.persistent_engine = true;
    } else {
        audio.owned_engine = std::make_unique<jam2::Engine>();
        audio.owned_engine->start(config);
        audio.engine.value = audio.owned_engine.get();
    }
    return audio;
}

void attach_network_capture(OptionalAudioStream& audio)
{
    if (!audio.engine) {
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
        const std::uint64_t deadline = jam2::monotonic_us() + 5000000ULL;
        while (audio.engine->snapshot().network_capture_enabled) {
            if (jam2::monotonic_us() >= deadline) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        audio.engine->requestNetworkPlaybackDrop(audio.engine->networkPlaybackDepth());
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

    CommandThread(
        const Options& options,
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
    }
};

void finalize_active_recording(OptionalAudioStream& audio, const Options& options, RuntimeState& state)
{
    if (!audio.engine) {
        return;
    }
    std::lock_guard<std::mutex> lock(state.recording_mutex);
    const auto before = audio.engine->snapshot().jam_recording;
    const auto cold_before = audio.engine->coldSnapshot();
    if (!before.active) {
        return;
    }
    jam2::EngineCommand command;
    command.type = jam2::EngineCommandType::StopJamRecording;
    const bool ok = audio.engine->submit(command);
    const std::uint64_t deadline = jam2::monotonic_us() + 5000000ULL;
    auto after = before;
    while (ok && jam2::monotonic_us() < deadline) {
        after = audio.engine->snapshot().jam_recording;
        if (!after.active) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!cold_before.recording_folder.empty()) {
        write_recording_sidecar(
            std::filesystem::path(cold_before.recording_folder),
            after,
            cold_before.recording_sample_rate,
            options,
            state);
    }
    if (!ok) {
        std::cerr << "record jam finalization command was rejected\n";
    }
}

void start_startup_recording(
    OptionalAudioStream& audio,
    const Options& options,
    RuntimeState& state)
{
    if (!options.record_jam_folder || !audio.engine) {
        return;
    }
    std::lock_guard<std::mutex> lock(state.recording_mutex);
    jam2::EngineCommand command;
    command.type = jam2::EngineCommandType::StartJamRecording;
    if (!jam2::engine_command_set_text(command, options.record_jam_folder->string()) ||
        !audio.engine->submit(command)) {
        std::cerr << "record jam start command was rejected\n";
    }
}

void print_optional_audio_stats(const OptionalAudioStream& audio, const Options& options)
{
    if (!audio.engine) {
        return;
    }
    const auto engine_snapshot = audio.engine->snapshot();
    const auto cold_snapshot = audio.engine->coldSnapshot();
    const auto capture_stats = engine_snapshot.capture_ring;
    const auto playback_stats = engine_snapshot.playback_ring;
    const auto callback_stats = engine_snapshot.callback_timing;
    const std::size_t capture_readable = engine_snapshot.capture_ring_depth_frames;
    const std::size_t playback_readable = engine_snapshot.playback_ring_depth_frames;
    const auto& stream_info = cold_snapshot.stream;
    std::cout << "Audio callbacks: " << engine_snapshot.callbacks << "\n";
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
    std::cout << "Capture ring capacity frames: " << engine_snapshot.capture_ring_capacity_frames << "\n";
    std::cout << "Playback ring capacity frames: " << engine_snapshot.playback_ring_capacity_frames << "\n";
    const auto& recording = engine_snapshot.jam_recording;
    std::cout << "Recording active: " << (recording.active ? "yes" : "no") << "\n";
    std::cout << "Recording folder: " << cold_snapshot.recording_folder << "\n";
    std::cout << "Recording sample rate: " << cold_snapshot.recording_sample_rate << "\n";
    std::cout << "Recording frames written: " << recording.frames_written << "\n";
    std::cout << "Recording dropped frames: " << recording.dropped_frames << "\n";
    std::cout << "Recording drop events: " << recording.drop_events << "\n";
    std::cout << "Recording queue depth frames: " << recording.queue_depth_frames << "\n";
    std::cout << "Recording queue capacity frames: " << recording.queue_capacity_frames << "\n";
    std::cout << "Recording writer errors: " << recording.writer_errors << "\n";
    std::cout << "Playback prefilled: " << (engine_snapshot.playback_prefilled ? "yes" : "no") << "\n";
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
    std::cout << "Audio control metronome: " << (engine_snapshot.metronome_enabled ? "on" : "off") << "\n";
    std::cout << "Audio control BPM: " << engine_snapshot.metronome_pattern.bpm << "\n";
    std::cout << "Audio control metronome level: " << gain_from_ppm(engine_snapshot.metronome_level_ppm) << "\n";
    std::cout << "Audio control remote playback level: " << gain_from_ppm(engine_snapshot.remote_level_ppm) << "\n";
    std::cout << "Audio control metronome mode: " << metronome_mode_text(static_cast<int>(engine_snapshot.metronome_mode)) << "\n";
    std::cout << "Audio control metronome epoch sample time: " << engine_snapshot.metronome_epoch_frame << "\n";
    std::cout << "Audio control metronome epoch valid: " << (engine_snapshot.metronome_epoch_valid ? "yes" : "no") << "\n";
    std::cout << "Audio control resampler ratio: " << (static_cast<double>(engine_snapshot.playback_ratio_ppm) / 1000000.0) << "\n";
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

    // Local is the finite or signal-terminated audio engine without network
    // bootstrap, socket creation, or an interactive command channel.
    auto audio = start_optional_audio(options, true);
    if (!audio.engine || !audio.engine->snapshot().frame_clock_active) {
        throw std::runtime_error("local failed to start an audio stream");
    }

    CommandThread commands(options, true);
    start_startup_recording(audio, options, commands.state);

    std::cout << "{\"event\":\"startup\",\"mode\":\"local\",\"stage\":\"running\"}\n";
    std::cout.flush();
    const std::uint64_t started_us = jam2::monotonic_us();
    while (!commands.state.quit.load(std::memory_order_relaxed) &&
           (options.stream_ms <= 0 ||
            jam2::monotonic_us() - started_us < static_cast<std::uint64_t>(options.stream_ms) * 1000ULL)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    finalize_active_recording(audio, options, commands.state);
    return 0;
}

int run_network_session(Options options, Jam2RuntimeHost& runtime_host)
{
    // This function is the universal direct-mesh UDP packet worker. Keep the
    // process High but elevate only this thread to Time Critical/MMCSS when
    // realtime was requested, leaving Qt and file workers out of realtime.
    OsPriorityScope os_priority_scope(options);
    if (!options.session_id || !options.session_key) {
        throw std::runtime_error("network session requires --session-id and --session-key");
    }
    if (!options.mesh_peers_configured) {
        throw std::runtime_error("network session requires --peers, use --peers \"\" for an empty initial peer list");
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

    std::optional<jam2::Endpoint> public_candidate;
    if (options.bootstrap_role == jam2::SessionBootstrapRole::Creator) {
        if (options.public_endpoint) {
            public_candidate = options.public_endpoint;
        } else if (!options.no_stun) {
            public_candidate = jam2::stun::discover_public_endpoint(
                socket,
                options.stun_server,
                options.stun_timeout_ms,
                options.stun_retries);
        }
    }

    std::cout << "Mode: network session\n";
    std::cout << "Local UDP bind: " << jam2::endpoint_to_string(local) << "\n";
    if (public_candidate) {
        std::cout << "Public UDP candidate: " << jam2::endpoint_to_string(*public_candidate) << "\n";
    }
    std::cout << "Mesh peers: " << options.mesh_peers.size() << "\n";
    for (std::size_t i = 0; i < options.mesh_peers.size(); ++i) {
        std::cout << "  peer" << (i + 1) << ": " << jam2::endpoint_to_string(options.mesh_peers[i]);
        if (i < options.mesh_peer_ids.size() && options.mesh_peer_ids[i] != 0) {
            std::cout << " id=" << options.mesh_peer_ids[i];
        }
        std::cout << "\n";
    }
    print_socket_options(socket);
    auto audio = start_optional_audio(options, false, &runtime_host);
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
            make_csv_context(
                options.bootstrap_role == jam2::SessionBootstrapRole::Creator
                    ? "jam2 network create (typed runtime)"
                    : "jam2 network join (typed runtime)",
                "mesh", options, socket, local, peer_context, "coordinator-membership"));
        std::cout << "Stats CSV: " << csv_log->path().string() << "\n";
    }
    if (runtime_host.startup) {
        runtime_host.startup(Jam2RuntimeStartup{
            local,
            public_candidate,
            csv_log ? std::optional<std::filesystem::path>(csv_log->path()) : std::nullopt,
        });
    }
    const int recording_sample_rate = audio.engine
        ? static_cast<int>(std::lround(audio.engine->snapshot().sample_rate))
        : options.sample_rate;
    CommandThread commands(options, false);
    EngineControlMirror engine_control_mirror;
    hold_shared_grid_at_start(commands.state, audio.engine.get());
    auto apply_runtime_host_commands = [&] {
        std::size_t processed = 0;
        while (processed < Jam2RuntimeHost::kCommandCapacity) {
            const auto next = runtime_host.takeCommand();
            if (!next) {
                break;
            }
            const jam2::EngineCommand& command = *next;
            switch (command.type) {
            case jam2::EngineCommandType::SetMetronomeEnabled: {
                const bool previous = commands.state.metronome.exchange(command.enabled, std::memory_order_relaxed);
                if (command.enabled && !previous) {
                    begin_metronome_epoch(commands.state, audio.engine.get(), recording_sample_rate);
                }
                request_grid_revision(commands.state);
                break;
            }
            case jam2::EngineCommandType::SetMetronomePattern: {
                const auto previous = metronome_pattern_from_runtime(commands.state);
                store_metronome_pattern(commands.state, command.pattern);
                request_grid_revision(commands.state);
                if (previous.bpm != command.pattern.bpm ||
                    previous.beats_per_bar != command.pattern.beats_per_bar ||
                    previous.division != command.pattern.division) {
                    commands.state.metronome_epoch_revision.fetch_add(1, std::memory_order_relaxed);
                }
                break;
            }
            case jam2::EngineCommandType::SetMetronomeLevel:
                commands.state.metronome_level_ppm.store(command.value, std::memory_order_relaxed);
                break;
            case jam2::EngineCommandType::SetRemoteLevel:
                commands.state.remote_level_ppm.store(command.value, std::memory_order_relaxed);
                break;
            case jam2::EngineCommandType::SetSendLevel:
                commands.state.send_level_ppm.store(command.value, std::memory_order_relaxed);
                break;
            case jam2::EngineCommandType::SetLocalMonitorEnabled:
                commands.state.local_monitor.store(command.enabled, std::memory_order_relaxed);
                break;
            case jam2::EngineCommandType::SetLocalMonitorLevel:
                commands.state.local_monitor_level_ppm.store(command.value, std::memory_order_relaxed);
                break;
            case jam2::EngineCommandType::SetMetronomeMode:
                commands.state.metronome_mode.store(std::clamp(command.value, 0, 2), std::memory_order_relaxed);
                commands.state.metronome_render_offset_frames.store(0, std::memory_order_relaxed);
                commands.state.metronome_epoch_revision.fetch_add(1, std::memory_order_relaxed);
                request_grid_revision(commands.state);
                break;
            case jam2::EngineCommandType::SetLeaderAudioLocalClick:
                commands.state.leader_audio_local_click.store(command.enabled, std::memory_order_relaxed);
                commands.state.metronome_local_authority.store(command.enabled, std::memory_order_relaxed);
                break;
            case jam2::EngineCommandType::SetMetronomeEpoch:
                commands.state.metronome_epoch_sample_time.store(command.frame, std::memory_order_relaxed);
                commands.state.metronome_epoch_valid.store(command.enabled, std::memory_order_relaxed);
                break;
            case jam2::EngineCommandType::SetMetronomeRenderOffset:
                commands.state.metronome_render_offset_frames.store(command.signed_value, std::memory_order_relaxed);
                break;
            case jam2::EngineCommandType::ScheduleTransport: {
                std::lock_guard<std::mutex> lock(commands.state.transport_mutex);
                commands.state.transport_revision.fetch_add(1, std::memory_order_relaxed);
                commands.state.transport_target_raw_frame.store(command.transport_target_frame, std::memory_order_relaxed);
                commands.state.transport_target_musical_frame.store(command.transport_musical_frame, std::memory_order_relaxed);
                commands.state.transport_countdown_start_frame.store(command.transport_countdown_start_frame, std::memory_order_relaxed);
                commands.state.transport_action.store(
                    static_cast<int>(command.transport_action),
                    std::memory_order_relaxed);
                commands.state.transport_pending.store(true, std::memory_order_relaxed);
                commands.state.transport_network_revision.store(
                    runtime_host.nextTransportEventId(),
                    std::memory_order_relaxed);
                commands.state.transport_network_target_raw_frame.store(command.transport_target_frame, std::memory_order_relaxed);
                commands.state.transport_network_action.store(
                    commands.state.transport_action.load(std::memory_order_relaxed),
                    std::memory_order_relaxed);
                break;
            }
            default:
                break;
            }
            ++processed;
        }
    };
    std::cout << "Embedded network controls ready\n";
    start_startup_recording(audio, options, commands.state);

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
    for (std::size_t peer_index = 0; peer_index < options.mesh_peers.size(); ++peer_index) {
        const auto& peer = options.mesh_peers[peer_index];
        const std::uint64_t configured_id = peer_index < options.mesh_peer_ids.size()
            ? options.mesh_peer_ids[peer_index]
            : 0ULL;
        const auto inserted = peers.emplace(
            endpoint_key(peer),
            MeshPeerState{peer, configured_id != 0 ? jam2::PeerId{configured_id} : compatibility_peer_id(peer)});
        if (!inserted.second) {
            throw std::runtime_error("mesh peer list contains a duplicate endpoint");
        }
    }

    const jam2::PeerId local_peer_id = options.local_peer_id
        ? jam2::PeerId{*options.local_peer_id}
        : compatibility_peer_id(local);
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
    std::uint64_t last_single_remote_peer_id = peers.size() == 1
        ? peers.begin()->second.peer_id.value
        : 0ULL;

    const jam2::PeerStreamConfig peer_stream_config = make_peer_stream_config(
        options,
        options.stats_enabled && (csv_log.has_value() || options.stats_interval_ms > 0));
    CliPeerStreamPlayback mesh_playback(audio.engine.get());
    jam2::NetworkSession network_session(
        std::move(socket),
        session,
        make_network_session_contract(options),
        options.bootstrap_role,
        local_peer_id,
        peer_descriptors,
        peer_stream_config,
        audio.engine ? &mesh_playback : nullptr,
        options.headless_clock_drift_ppm);
    auto& packet_schedule = network_session.schedule();
    std::uint64_t bootstrap_coordinator_peer_id = options.bootstrap_coordinator_peer_id.value_or(0);
    if (bootstrap_coordinator_peer_id == 0) {
        bootstrap_coordinator_peer_id = local_peer_id.value;
        for (const auto& descriptor : peer_descriptors) {
            bootstrap_coordinator_peer_id = std::min(
                bootstrap_coordinator_peer_id,
                descriptor.peer_id.value);
        }
    } else if (bootstrap_coordinator_peer_id != local_peer_id.value &&
               std::none_of(peer_descriptors.begin(), peer_descriptors.end(), [&](const auto& descriptor) {
                   return descriptor.peer_id.value == bootstrap_coordinator_peer_id;
               })) {
        throw std::runtime_error("--bootstrap-coordinator-peer-id is not the local peer or a configured remote peer");
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
    std::uint64_t last_local_grid_request_sequence =
        commands.state.grid_request_sequence.load(std::memory_order_acquire);
    std::optional<jam2::GridProposal> pending_local_grid_proposal;
    std::uint64_t next_grid_proposal_send_us = 0;
    std::uint64_t next_grid_assignment_send_us = 0;
    std::uint64_t sending_transport_revision = 0;
    std::uint64_t next_transport_send = 0;
    bool sent_current_transport = false;
    const std::uint64_t start_time = packet_schedule.startTimeUs();
    std::uint64_t next_stats = options.stats_enabled && options.stats_interval_ms > 0
        ? start_time + static_cast<std::uint64_t>(options.stats_interval_ms) * 1000ULL
        : 0;
    // Public create/join validation measures established UDP audio, not
    // TCP/audio bootstrap time. Standalone internal lifecycle runs retain a
    // finite local deadline even when their intentional peer list is empty.
    const bool wait_for_first_peer = options.stream_ms > 0 && options.arm_stream_on_first_peer;
    std::uint64_t send_deadline = options.stream_ms > 0 && !wait_for_first_peer
        ? start_time + static_cast<std::uint64_t>(options.stream_ms) * 1000ULL
        : UINT64_MAX;
    std::uint64_t receive_deadline = options.stream_ms > 0 && !wait_for_first_peer
        ? send_deadline + static_cast<std::uint64_t>(options.stream_linger_ms) * 1000ULL
        : UINT64_MAX;
    bool timed_stream_clock_armed = options.stream_ms > 0 && !wait_for_first_peer;
    const std::uint64_t playout_delay_frames = options.jitter_buffer_frames > 0
        ? static_cast<std::uint64_t>(options.jitter_buffer_frames)
        : static_cast<std::uint64_t>(options.playout_delay_frames);

    std::uint64_t mesh_grid_proposals_sent = 0;
    std::uint64_t mesh_grid_assignments_sent = 0;
    std::uint64_t mesh_grid_authority_states_sent = 0;
    std::uint64_t mesh_transport_source_peer_id = 0;
    std::uint64_t mesh_transport_event_counter = 0;
    std::uint64_t mesh_transport_grid_revision = 0;
    jam2::EngineTransportAction mesh_transport_action = jam2::EngineTransportAction::None;
    std::uint64_t mesh_leader_audio_source_peer_id = 0;
    std::uint64_t mesh_leader_audio_injected_packets = 0;
    std::uint64_t mesh_transport_source_frame = 0;
    std::uint64_t mesh_transport_requested_target_frame = 0;
    std::uint64_t mesh_transport_applied_target_frame = 0;
    std::uint64_t mesh_compensation_stale_events = 0;
    bool mesh_compensation_was_stale = false;
    bool timed_stream_audio_detached = false;
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
        std::uint64_t max_rtt_us = 0;
        for (const auto& entry : peers) {
            max_rtt_us = std::max(
                max_rtt_us,
                network_session.peerStream(entry.second.peer_id).stats().rtt_min_us);
        }
        const std::uint64_t rtt_frames = max_rtt_us *
            static_cast<std::uint64_t>(options.sample_rate) / 1000000ULL;
        const std::uint64_t lead_frames = std::clamp(
            rtt_frames + static_cast<std::uint64_t>(options.sample_rate) / 5ULL,
            static_cast<std::uint64_t>(options.sample_rate) / 10ULL,
            static_cast<std::uint64_t>(options.sample_rate) / 2ULL);
        // Every explicit local start/revision gets a fresh bounded epoch. Do
        // not quantize from an earlier membership's epoch: a past epoch can
        // otherwise defer the requested start by almost a full later bar.
        return current_engine_frame(audio.engine.get()) + lead_frames;
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
        const std::uint64_t packet_frame = current_engine_frame(audio.engine.get());
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
    auto clear_departed_authority_state = [&]() {
        commands.state.metronome_epoch_sample_time.store(0, std::memory_order_relaxed);
        commands.state.metronome_epoch_valid.store(false, std::memory_order_relaxed);
        commands.state.metronome_render_offset_frames.store(0, std::memory_order_relaxed);
        commands.state.metronome_local_authority.store(false, std::memory_order_relaxed);
        commands.state.leader_audio_local_click.store(false, std::memory_order_relaxed);
        pending_local_grid_proposal.reset();
        last_authority_state_received_us = 0;
        remote_authority_epoch_frame = 0;
        mesh_grid_target_offset_frames = 0;
        mesh_grid_target_valid = false;
        mesh_grid_last_update_us = 0;
        {
            std::lock_guard<std::mutex> lock(commands.state.transport_mutex);
            commands.state.transport_pending.store(false, std::memory_order_release);
            commands.state.transport_network_revision.store(0, std::memory_order_relaxed);
            commands.state.transport_network_target_raw_frame.store(0, std::memory_order_relaxed);
            commands.state.transport_network_action.store(0, std::memory_order_relaxed);
        }
        mesh_transport_source_peer_id = 0;
        mesh_transport_event_counter = 0;
        mesh_transport_grid_revision = 0;
        mesh_transport_action = jam2::EngineTransportAction::None;
        mesh_transport_source_frame = 0;
        mesh_transport_requested_target_frame = 0;
        mesh_transport_applied_target_frame = 0;
        if (audio.engine != nullptr) {
            jam2::EngineCommand cancel;
            cancel.type = jam2::EngineCommandType::CancelTransport;
            (void)audio.engine->submit(cancel);
        }
    };
    auto align_to_authority_bar = [&](const MetronomePayload& metronome,
                                      std::uint64_t authority_packet_frame,
                                      const jam2::PeerStream& stream) {
        if (stream.stats().rtt_min_us == 0 || audio.engine == nullptr) {
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
            current_engine_frame(audio.engine.get()) + frames_until_bar,
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
                runtime_host.nextGridRequestId(),
                grid_run_state_from_runtime(),
                static_cast<std::uint8_t>(
                    commands.state.metronome_mode.load(std::memory_order_relaxed)),
                0,
            })) {
            (void)activate_local_grid();
        }
    }

    AudioPacketStats retired_peer_stats;
    auto aggregate_stats = [&]() {
        AudioPacketStats stats = retired_peer_stats;
        stats.os_scheduling = os_priority_scope.status();
        stats.local_peer_id = network_session.localPeerId().value;
        switch (network_session.bootstrapRole()) {
        case jam2::SessionBootstrapRole::Creator: stats.bootstrap_role = "creator"; break;
        case jam2::SessionBootstrapRole::Joiner: stats.bootstrap_role = "joiner"; break;
        }
        stats.session_protocol_version = network_session.contract().protocol_version;
        stats.session_audio_format = "pcm24-mono";
        stats.session_sample_rate = network_session.contract().sample_rate;
        stats.session_frames_per_packet = network_session.contract().frames_per_packet;
        stats.network_peer_count = network_session.peerCount();
        if (peers.size() == 1) {
            stats.remote_peer_id = peers.begin()->second.peer_id.value;
        } else if (peers.empty()) {
            stats.remote_peer_id = last_single_remote_peer_id;
        }
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
        stats.metronome_sent = mesh_grid_authority_states_sent;
        stats.metronome_received = authority_stats.grid_authority_states_accepted;
        stats.grid_mapping_error_frames = mesh_grid_target_valid
            ? mesh_grid_target_offset_frames -
                commands.state.metronome_render_offset_frames.load(std::memory_order_relaxed)
            : 0;
        stats.transport_source_peer_id = mesh_transport_source_peer_id;
        stats.transport_event_counter = mesh_transport_event_counter;
        stats.transport_grid_revision = mesh_transport_grid_revision;
        stats.transport_action = static_cast<std::uint64_t>(mesh_transport_action);
        stats.leader_audio_source_peer_id = mesh_leader_audio_source_peer_id;
        stats.leader_audio_injected_packets = mesh_leader_audio_injected_packets;
        stats.transport_source_frame = mesh_transport_source_frame;
        stats.transport_requested_target_frame = mesh_transport_requested_target_frame;
        stats.transport_applied_target_frame = mesh_transport_applied_target_frame;
        stats.metronome_compensation_stale_events = mesh_compensation_stale_events;
        stats.final_metronome_enabled = commands.state.metronome.load(std::memory_order_relaxed);
        stats.final_bpm = commands.state.bpm.load(std::memory_order_relaxed);
        stats.final_metronome_level = gain_from_ppm(
            commands.state.metronome_level_ppm.load(std::memory_order_relaxed));
        stats.final_remote_level = gain_from_ppm(
            commands.state.remote_level_ppm.load(std::memory_order_relaxed));
        stats.final_send_level = gain_from_ppm(
            commands.state.send_level_ppm.load(std::memory_order_relaxed));
        stats.final_local_monitor_enabled =
            commands.state.local_monitor.load(std::memory_order_relaxed);
        stats.final_local_monitor_level = gain_from_ppm(
            commands.state.local_monitor_level_ppm.load(std::memory_order_relaxed));
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
        const auto pattern = metronome_pattern_from_runtime(commands.state);
        const std::uint64_t beat_frames = jam2::metronome::step_interval_samples(
            static_cast<double>(options.sample_rate), pattern.bpm, pattern.division) *
            static_cast<std::uint64_t>(std::max(1, pattern.division));
        const bool epoch_valid = commands.state.metronome_epoch_valid.load(std::memory_order_relaxed);
        const std::uint64_t mapped_epoch =
            commands.state.metronome_epoch_sample_time.load(std::memory_order_relaxed);
        stats.metronome_epoch_sample_time = mapped_epoch;
        if (epoch_valid && beat_frames > 0 && grid.run_state == jam2::GridRunState::Running) {
            const std::uint64_t local_frame = current_engine_frame(audio.engine.get());
            stats.local_metronome_beat = local_frame >= mapped_epoch
                ? (local_frame - mapped_epoch) / beat_frames
                : 0ULL;
            if (authority.localIsGridAuthority()) {
                stats.remote_metronome_beat = stats.local_metronome_beat;
                stats.metronome_alignment_valid = true;
            } else {
                for (const auto& entry : peers) {
                    const auto& peer = entry.second;
                    if (peer.peer_id.value != grid.authority_peer_id) {
                        continue;
                    }
                    const auto& peer_stats = network_session.peerStream(peer.peer_id).stats();
                    if (peer_stats.last_received_sample_time >= grid.authority_epoch_frame) {
                        stats.remote_metronome_beat =
                            (peer_stats.last_received_sample_time - grid.authority_epoch_frame) / beat_frames;
                        const std::uint64_t difference = stats.local_metronome_beat > stats.remote_metronome_beat
                            ? stats.local_metronome_beat - stats.remote_metronome_beat
                            : stats.remote_metronome_beat - stats.local_metronome_beat;
                        stats.metronome_alignment_valid = difference <= 1ULL;
                    }
                    break;
                }
            }
        }
        stats.metronome_compensation_offset_frames =
            commands.state.metronome_render_offset_frames.load(std::memory_order_relaxed);
        stats.metronome_compensation_target_frames = mesh_grid_target_valid
            ? mesh_grid_target_offset_frames
            : stats.metronome_compensation_offset_frames;
        stats.metronome_compensation_active =
            commands.state.metronome_mode.load(std::memory_order_relaxed) ==
                metronome_mode_id(MetronomeMode::ListenerCompensated) &&
            !authority.localIsGridAuthority() && epoch_valid && mesh_grid_target_valid;
        copy_peer_mixer_stats(stats, network_session.mixStats());
        stats.udp_work_budget_yields += network_session.mixStats().work_budget_yields;
        return stats;
    };

    auto print_mesh_stats = [&](std::uint64_t now_us, const CsvStatsLog::AudioSnapshot* provided_audio_snapshot = nullptr) {
        const std::uint64_t elapsed_ms = (now_us - start_time) / 1000ULL;
        const AudioPacketStats stats = aggregate_stats();
        CsvStatsLog::AudioSnapshot current_audio_snapshot;
        if (provided_audio_snapshot == nullptr) {
            current_audio_snapshot = make_audio_snapshot(audio.engine.get());
            provided_audio_snapshot = &current_audio_snapshot;
        }
        const auto& audio_snapshot = *provided_audio_snapshot;
        const auto engine_snapshot = audio.engine ? audio.engine->snapshot() : jam2::EngineSnapshot{};
        {
            std::cout << "mesh_stats elapsed_ms=" << elapsed_ms
                      << " peer_count=" << peers.size()
                      << " metronome=" << (commands.state.metronome.load(std::memory_order_relaxed) ? "on" : "off")
                      << " bpm=" << commands.state.bpm.load(std::memory_order_relaxed)
                      << " metronome_beats_per_bar=" << commands.state.metronome_beats_per_bar.load(std::memory_order_relaxed)
                      << " metronome_division=" << commands.state.metronome_division.load(std::memory_order_relaxed)
                      << " metronome_epoch_sample_frame=" << commands.state.metronome_epoch_sample_time.load(std::memory_order_relaxed)
                      << " engine_frame=" << engine_snapshot.engine_frame
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
        if (runtime_host.network_snapshot) {
            runtime_host.network_snapshot(network_session.snapshot());
        }
    };

    auto apply_membership_update = [&]() {
        auto update = runtime_host.takePeerUpdate();
        if (!update) {
            return;
        }
        try {
        std::string joined;
        for (const std::string& peer : *update) {
            if (!joined.empty()) {
                joined.push_back(',');
            }
            joined += peer;
        }
        ParsedPeerList parsed = parse_peer_list(joined, local);
        std::map<std::uint64_t, jam2::Endpoint> desired;
        for (std::size_t index = 0; index < parsed.endpoints.size(); ++index) {
            const jam2::Endpoint endpoint = jam2::resolve_udp_endpoint(parsed.endpoints[index]);
            const std::uint64_t configured_id = index < parsed.peer_ids.size()
                ? parsed.peer_ids[index]
                : 0ULL;
            const std::uint64_t peer_id = configured_id != 0
                ? configured_id
                : compatibility_peer_id(endpoint).value;
            if (peer_id == local_peer_id.value || !desired.emplace(peer_id, endpoint).second) {
                throw std::runtime_error("embedded peer update contains a duplicate/local peer identity");
            }
        }

        std::size_t removed = 0;
        std::size_t endpoint_updates = 0;
        for (auto it = peers.begin(); it != peers.end();) {
            const std::uint64_t peer_id = it->second.peer_id.value;
            const auto wanted = desired.find(peer_id);
            if (wanted == desired.end()) {
                const auto& retired = it->second;
                retired_peer_stats.sent_packets += retired.sent_packets;
                retired_peer_stats.sent_bytes += retired.sent_bytes;
                retired_peer_stats.recv_packets += retired.recv_packets;
                retired_peer_stats.recv_bytes += retired.recv_bytes;
                retired_peer_stats.ignored_packets += retired.ignored_packets;
                retired_peer_stats.udp_parse.add(retired.udp_parse);
                retired_peer_stats.sent_pings += retired.sent_pings;
                retired_peer_stats.sent_pongs += retired.sent_pongs;
                retired_peer_stats.recv_pongs += retired.recv_pongs;
                add_peer_stream_stats(
                    retired_peer_stats,
                    network_session.peerStream(retired.peer_id).stats());
                const bool invalidates_scheduling =
                    peer_id == authority.grid().authority_peer_id ||
                    peer_id == authority.arrangementAuthorityPeerId();
                (void)authority.markPeerInactive(peer_id);
                if (invalidates_scheduling) {
                    clear_departed_authority_state();
                }
                (void)network_session.removePeer(it->second.peer_id);
                it = peers.erase(it);
                ++removed;
                continue;
            }
            if (it->second.endpoint.host != wanted->second.host ||
                it->second.endpoint.port != wanted->second.port) {
                if (!network_session.updatePeerEndpoint(
                        it->second.peer_id,
                        wanted->second,
                        jam2::PeerEndpointState::Candidate)) {
                    throw std::runtime_error("embedded peer endpoint update was rejected");
                }
                auto node = peers.extract(it++);
                node.key() = endpoint_key(wanted->second);
                node.mapped().endpoint = wanted->second;
                node.mapped().endpoint_proof = EndpointProofState::Candidate;
                node.mapped().probe_challenges = {};
                node.mapped().proof_attempts = 0;
                node.mapped().proof_deadline_us = 0;
                node.mapped().next_probe_us = 0;
                if (!peers.insert(std::move(node)).inserted) {
                    throw std::runtime_error("embedded peer endpoint update collided with another endpoint");
                }
                ++endpoint_updates;
                continue;
            }
            ++it;
        }

        std::size_t added = 0;
        for (const auto& [peer_id, endpoint] : desired) {
            if (network_session.peer(jam2::PeerId{peer_id}) != nullptr) {
                continue;
            }
            if (!network_session.addPeer(
                    {jam2::PeerId{peer_id}, endpoint, jam2::PeerEndpointState::Candidate},
                    peer_stream_config)) {
                throw std::runtime_error("embedded peer add was rejected");
            }
            if (!peers.emplace(
                    endpoint_key(endpoint),
                    MeshPeerState{endpoint, jam2::PeerId{peer_id}}).second) {
                (void)network_session.removePeer(jam2::PeerId{peer_id});
                throw std::runtime_error("embedded peer add collided with another endpoint");
            }
            ++added;
        }
        if (peers.size() == 1) {
            last_single_remote_peer_id = peers.begin()->second.peer_id.value;
        }
        std::cout << "Embedded mesh peer update: added=" << added
                  << " removed=" << removed
                  << " endpoint_updates=" << endpoint_updates
                  << " peers=" << peers.size() << "\n";
        } catch (const std::exception& error) {
            // This runs outside the real-time callback and is intentionally
            // retained: a rejected coordinator membership update must leave a
            // useful cause in CLI/stress logs instead of only exit code 1.
            std::cout << "Embedded mesh peer update failed: " << error.what() << "\n";
            throw;
        }
    };

    while (jam2::monotonic_us() < receive_deadline &&
           !commands.state.quit.load(std::memory_order_relaxed) &&
           !runtime_host.stop_requested.load(std::memory_order_acquire)) {
        apply_runtime_host_commands();
        apply_membership_update();
        const std::uint64_t now = jam2::monotonic_us();
        if (!timed_stream_clock_armed && options.stream_ms > 0 && network_session.activePeerCount() > 0) {
            send_deadline = now + static_cast<std::uint64_t>(options.stream_ms) * 1000ULL;
            receive_deadline = send_deadline +
                static_cast<std::uint64_t>(options.stream_linger_ms) * 1000ULL;
            timed_stream_clock_armed = true;
        }
        const std::uint64_t local_grid_request_sequence =
            commands.state.grid_request_sequence.load(std::memory_order_acquire);
        if (local_grid_request_sequence != last_local_grid_request_sequence) {
            last_local_grid_request_sequence = local_grid_request_sequence;
            jam2::GridProposal proposal{
                local_peer_id.value,
                runtime_host.nextGridRequestId(),
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
                const std::uint64_t local_frame = current_engine_frame(audio.engine.get());
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
        commit_due_transport(commands.state, audio.engine.get());
        sync_engine_control(commands.state, audio.engine.get(), 1.0, engine_control_mirror);
        if (!timed_stream_audio_detached && now >= send_deadline) {
            detach_network_capture(audio);
            mesh_playback.detach();
            timed_stream_audio_detached = true;
        }
        network_session.advance(now);

        int sends_this_loop = 0;
        while (now >= packet_schedule.nextAudioSendUs() &&
               packet_schedule.nextAudioSendUs() < receive_deadline &&
               sends_this_loop < 8) {
            std::span<const std::uint8_t> payload = silence_payload;
            if (audio.engine != nullptr && !timed_stream_audio_detached) {
                const auto captured = audio.engine->popNetworkCapture(audio.network_capture, asio_frames);
                if (captured.frames < static_cast<std::size_t>(options.frame_size)) {
                    break;
                }
                for (std::size_t i = 0; i < asio_frames.size(); ++i) {
                    network_frames[i] = asio_frames[i] / 256;
                }
                apply_send_level(network_frames, commands.state.send_level_ppm.load(std::memory_order_relaxed));
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
                    current_engine_frame(audio.engine.get()),
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
                    current_engine_frame(audio.engine.get()),
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
            sent_current_transport = false;
        }
        const bool sending_track_transport =
            transport_action == static_cast<int>(jam2::EngineTransportAction::TrackRestart) ||
            transport_action == static_cast<int>(jam2::EngineTransportAction::TrackStop) ||
            transport_action == static_cast<int>(jam2::EngineTransportAction::TrackPlay);
        const bool may_send_transport = sending_track_transport
            ? runtime_host.track_sync_enabled.load(std::memory_order_acquire)
            : authority.localIsArrangementAuthority();
        if (may_send_transport &&
            authority.grid().revision <= (std::numeric_limits<std::uint32_t>::max)() &&
            sending_transport_revision != 0 &&
            sending_transport_revision <= (std::numeric_limits<std::uint32_t>::max)() &&
            now >= next_transport_send &&
            (!sent_current_transport || current_engine_frame(audio.engine.get()) <= transport_target) &&
            now < send_deadline) {
            const std::uint64_t engine_now = current_engine_frame(audio.engine.get());
            const auto payload = encode_transport_payload({
                static_cast<jam2::EngineTransportAction>(
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
            mesh_transport_action = static_cast<jam2::EngineTransportAction>(transport_action);
            mesh_transport_source_frame = engine_now;
            mesh_transport_requested_target_frame = transport_target;
            mesh_transport_applied_target_frame = transport_target;
            sent_current_transport = true;
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
                    make_audio_snapshot(audio.engine.get()));
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
                                    const std::uint64_t local_frame = current_engine_frame(audio.engine.get());
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
                    const bool track_transport =
                        transport.action == jam2::EngineTransportAction::TrackRestart ||
                        transport.action == jam2::EngineTransportAction::TrackStop ||
                        transport.action == jam2::EngineTransportAction::TrackPlay;
                    const bool accept_track_transport =
                        runtime_host.track_sync_enabled.load(std::memory_order_acquire);
                    const bool track_transport_ready =
                        !track_transport || !accept_track_transport ||
                        (peer.recv_pongs > 0 &&
                         peer_stream.stats().rtt_min_us > 0 &&
                         audio.engine != nullptr);
                    if (!track_transport_ready) {
                        // Transport packets are repeated until their target.
                        // Do not consume this source event counter before the
                        // rejoined edge has the clock mapping needed to apply
                        // it; a later repeat can then schedule the same action.
                        ++peer.ignored_packets;
                        continue;
                    }
                    const bool accepted_transport = authority.acceptTransportEvent(
                        peer.peer_id.value,
                        transport.event_counter,
                        transport.grid_revision,
                        !track_transport);
                    if (accepted_transport) {
                        mesh_transport_source_peer_id = peer.peer_id.value;
                        mesh_transport_event_counter = transport.event_counter;
                        mesh_transport_grid_revision = transport.grid_revision;
                        mesh_transport_action = transport.action;
                        mesh_transport_source_frame = header.sample_time;
                        mesh_transport_requested_target_frame = transport.target_sender_frame;
                    }
                    if (track_transport && accepted_transport && !accept_track_transport) {
                        ++peer.ignored_packets;
                    }
                    if (track_transport && accept_track_transport &&
                        accepted_transport &&
                        peer.recv_pongs > 0 &&
                        peer_stream.stats().rtt_min_us > 0 &&
                        audio.engine != nullptr) {
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
                            current_engine_frame(audio.engine.get()) + frames_until_target;
                        mesh_transport_applied_target_frame = target_raw_frame;
                        const std::int64_t offset =
                            commands.state.metronome_render_offset_frames.load(std::memory_order_relaxed);
                        const QuantizedSchedule schedule{
                            target_raw_frame,
                            target_raw_frame,
                            musical_frame_from_raw(target_raw_frame, offset),
                        };
                        bool scheduled = false;
                        if (transport.action == jam2::EngineTransportAction::TrackRestart) {
                            scheduled = enqueue_prepared_restart(audio.engine.get(), schedule.target_raw_frame);
                        } else {
                            jam2::EngineCommand command;
                            command.type = transport.action == jam2::EngineTransportAction::TrackPlay
                                ? jam2::EngineCommandType::PreparedPlay
                                : jam2::EngineCommandType::PreparedStop;
                            command.frame = schedule.target_raw_frame;
                            scheduled = audio.engine->submit(command);
                        }
                        if (scheduled) {
                            publish_transport_schedule(
                                commands.state,
                                transport.action,
                                schedule,
                                false);
                        }
                    }
                } else if (header.type == jam2::protocol::PacketType::Bye) {
                    (void)network_session.peerStream(peer.peer_id).acceptReplay(
                        jam2::PeerReplayChannel::Bye,
                        header.sequence);
                    const bool invalidates_scheduling =
                        peer.peer_id.value == authority.grid().authority_peer_id ||
                        peer.peer_id.value == authority.arrangementAuthorityPeerId();
                    (void)authority.markPeerInactive(peer.peer_id.value);
                    if (invalidates_scheduling && !timed_stream_audio_detached) {
                        clear_departed_authority_state();
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
    // Stop callback consumption before flushing final reorder/mix state. A
    // trailing partial mix block is diagnostic state, not live playback, and
    // must not manufacture a shutdown-only playback underrun.
    detach_network_capture(audio);
    network_session.finish(now);
    network_session.sendToActive(
        jam2::protocol::PacketType::Bye,
        packet_schedule.takeControlSequence(),
        0,
        now,
        {});
    const auto final_audio_snapshot = make_audio_snapshot(audio.engine.get());
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
        if (has_help_argument(argc, argv, 2)) {
            print_device_help(command);
            return 0;
        }
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
        if (has_help_argument(argc, argv, 2)) {
            print_device_help(command);
            return 0;
        }
        return run_test_device(argc, argv);
    }

    if (command == "meter-device") {
        if (has_help_argument(argc, argv, 2)) {
            print_device_help(command);
            return 0;
        }
        return run_meter_device(argc, argv);
    }

    if (command == "ring-device") {
        if (has_help_argument(argc, argv, 2)) {
            print_device_help(command);
            return 0;
        }
        return run_ring_device(argc, argv);
    }

    if (command == "local") {
        if (has_help_argument(argc, argv, 2)) {
            print_local_help();
            return 0;
        }
        return run_local(argc, argv);
    }

    if (command == "network") {
        if (argc < 3 || is_help_argument(argv[2])) {
            std::cout << kNetworkUsage;
            return 0;
        }
        const std::string_view operation{argv[2]};
        if (operation == "create") {
            if (has_help_argument(argc, argv, 3)) {
                print_network_create_help();
                return 0;
            }
            throw std::runtime_error("network create bootstrap is owned by the unified Jam2 application");
        }
        if (operation == "join") {
            if (has_help_argument(argc, argv, 3)) {
                print_network_join_help();
                return 0;
            }
            throw std::runtime_error("network join bootstrap is owned by the unified Jam2 application");
        }
        std::cerr << "Unknown network subcommand: " << operation << "\n\n" << kNetworkUsage;
        return 2;
    }

    std::cerr << "Unknown command: " << command << "\n\n" << kUsage;
    return 2;
}

} // namespace

bool Jam2RuntimeHost::submitCommand(const jam2::EngineCommand& command) noexcept
{
    if (engine == nullptr) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(command_mutex_);
        if (commands_.size() >= kCommandCapacity) {
            return false;
        }
        commands_.push_back(command);
    }
    if (!engine->submit(command)) {
        std::lock_guard<std::mutex> lock(command_mutex_);
        if (!commands_.empty()) {
            commands_.pop_back();
        }
        return false;
    }
    return true;
}

bool Jam2RuntimeHost::submitPeerUpdate(const std::vector<std::string>& peers)
{
    if (std::any_of(peers.begin(), peers.end(), [](const std::string& peer) {
            return peer.empty() || peer.size() > 512;
        })) {
        return false;
    }
    std::lock_guard<std::mutex> lock(peer_mutex_);
    peer_update_ = peers;
    return true;
}

std::optional<std::vector<std::string>> Jam2RuntimeHost::takePeerUpdate()
{
    std::lock_guard<std::mutex> lock(peer_mutex_);
    auto result = std::move(peer_update_);
    peer_update_.reset();
    return result;
}

std::optional<jam2::EngineCommand> Jam2RuntimeHost::takeCommand()
{
    std::lock_guard<std::mutex> lock(command_mutex_);
    if (commands_.empty()) {
        return std::nullopt;
    }
    jam2::EngineCommand result = commands_.front();
    commands_.pop_front();
    return result;
}

std::uint64_t Jam2RuntimeHost::nextGridRequestId() noexcept
{
    std::uint64_t current = grid_request_id_.load(std::memory_order_relaxed);
    for (;;) {
        const std::uint64_t next = current == (std::numeric_limits<std::uint64_t>::max)()
            ? 1ULL
            : current + 1ULL;
        if (grid_request_id_.compare_exchange_weak(
                current,
                next,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return next;
        }
    }
}

std::uint64_t Jam2RuntimeHost::nextTransportEventId() noexcept
{
    std::uint64_t current = transport_event_id_.load(std::memory_order_relaxed);
    for (;;) {
        const std::uint64_t next = current == (std::numeric_limits<std::uint64_t>::max)()
            ? 1ULL
            : current + 1ULL;
        if (transport_event_id_.compare_exchange_weak(
                current,
                next,
                std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return next;
        }
    }
}

void Jam2RuntimeHost::reset() noexcept
{
    stop_requested.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(peer_mutex_);
        peer_update_.reset();
    }
    {
        std::lock_guard<std::mutex> lock(command_mutex_);
        commands_.clear();
    }
}

Jam2RuntimeOptions jam2_parse_runtime_options(int argc, char** argv, int start_index)
{
    return parse_options(argc, argv, start_index);
}

jam2::EngineConfig jam2_make_engine_config(
    const Jam2RuntimeOptions& options,
    bool leader_audio_local_click)
{
    return make_engine_config_impl(options, leader_audio_local_click);
}

bool jam2_engine_restart_required(
    const jam2::EngineConfig& active,
    const jam2::EngineConfig& requested) noexcept
{
    return engine_restart_required(active, requested);
}

int jam2_run_network_runtime(Jam2RuntimeOptions options, Jam2RuntimeHost& host)
{
    try {
        return run_network_session(std::move(options), host);
    } catch (const std::exception& exception) {
        if (host.error) {
            host.error(exception.what());
        }
        return 1;
    } catch (...) {
        if (host.error) {
            host.error("unknown native network runtime failure");
        }
        return 1;
    }
}

int jam2_cli_main(int argc, char** argv)
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

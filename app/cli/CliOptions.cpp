#include "CliOptions.hpp"
#include "runtime_limits.hpp"

#include "common.hpp"
#include "tuning_profile.hpp"

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace jam2::cli {

const std::string_view kUsage = R"(Jam2 - direct low-latency music streaming

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

const std::string_view kNetworkUsage = R"(Usage:
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
  --network-audio-format <pcm16|pcm24>  Session wire quality (default: pcm24)
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
  --adaptive-playback-ratio-ramp-ms <0..60000>
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
    options.adaptive_playback_ratio_ramp_ms = profile.adaptive_playback_ratio_ramp_ms;
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
            if (!jam2::limits::valid_sample_rate(options.sample_rate)) {
                throw std::runtime_error(
                    "--sample-rate must be from " +
                    std::to_string(jam2::limits::kMinimumSampleRate) + " through " +
                    std::to_string(jam2::limits::kMaximumSampleRate));
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
        } else if (arg == "--network-audio-format") {
            const auto parsed = jam2::protocol::parse_audio_format(
                require_value(argc, argv, i, arg));
            if (!parsed) {
                throw std::runtime_error("--network-audio-format must be pcm16 or pcm24");
            }
            options.network_audio_format = *parsed;
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
        } else if (arg == "--adaptive-playback-ratio-ramp-ms") {
            options.adaptive_playback_ratio_ramp_ms = std::stoi(
                std::string(require_value(argc, argv, i, arg)));
            if (options.adaptive_playback_ratio_ramp_ms < 0 ||
                options.adaptive_playback_ratio_ramp_ms > 60000) {
                throw std::runtime_error(
                    "--adaptive-playback-ratio-ramp-ms must be 0..60000");
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
            throw std::runtime_error("a runtime peer endpoint must not equal the local bind endpoint");
        }
    }
    return options;
}

} // namespace jam2::cli

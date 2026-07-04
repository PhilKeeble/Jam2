#include <exception>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "audio_device.hpp"
#include "common.hpp"
#include "protocol.hpp"
#include "stun.hpp"
#include "udp_socket.hpp"

namespace {

constexpr std::string_view kUsage = R"(jam2 - two-person low-latency music streaming tool

Usage:
  jam2 --help
  jam2 list-devices
  jam2 test-device <id> [--sample-rate n]
  jam2 meter-device <id> [--sample-rate n] [--buffer-size n] [--duration-ms n]
  jam2 ring-device <id> [--sample-rate n] [--buffer-size n] [--duration-ms n] [--ring-frames n]
  jam2 listen [--bind ip:port] [--stun host:port] [--no-stun] [--public-endpoint ip:port] [--wait-ms n] [--stream-ms n] [--stream-linger-ms n] [--stats enabled|disabled] [--stats-interval-ms n] [--stats-warmup-ms n] [--log-stats folder] [--metronome on|off] [--bpm n] [--metronome-level n] [--remote-level n] [--metronome-mode shared-grid|leader-audio|symmetric-delay|listener-compensated] [--sample-time-playout on|off] [--playout-delay-frames n] [--adaptive-playback-cushion on|off] [--adaptive-playback-target-frames n] [--adaptive-playback-min-frames n] [--adaptive-playback-max-frames n] [--adaptive-playback-release-ppm n] [--session-id hex] [--session-key hex32] [--machine-readable-startup on|off] [--status-format text|jsonl] [--socket-send-buffer n] [--socket-recv-buffer n] [--input-channels n[,n...]] [--output-channels n[,n...]] [--playback-prefill-frames n] [--playback-max-frames n] [--drift-smoothing n] [--drift-deadband-ppm n] [--drift-max-correction-ppm n]
  jam2 connect <jam2-url> [options]

Stage status:
  UDP HELLO/HELLO_ACK session setup, jam2 URL parsing, and STUN endpoint discovery are implemented.
  UDP audio streaming, Windows ASIO, drift stats/correction, and metronome controls are implemented for the MVP slice.
)";

enum class MetronomeMode {
    SharedGrid,
    LeaderAudio,
    SymmetricDelay,
    ListenerCompensated,
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
    double metronome_level = 0.20;
    MetronomeMode metronome_mode = MetronomeMode::SharedGrid;
    double remote_level = 1.0;
    bool sample_time_playout = true;
    std::size_t playout_delay_frames = 0;
    bool adaptive_playback_cushion = false;
    std::size_t adaptive_playback_target_frames = 0;
    std::size_t adaptive_playback_min_frames = 0;
    std::size_t adaptive_playback_max_frames = 0;
    int adaptive_playback_release_ppm = 1000;
    std::optional<std::uint64_t> session_id;
    std::optional<std::array<std::uint8_t, 16>> session_key;
    bool machine_readable_startup = false;
    bool status_jsonl = false;
    std::optional<int> audio_device_id;
    long audio_buffer_size = 0;
    jam2::audio::InputChannels input_channels = jam2::audio::InputChannels::Mono;
    jam2::audio::ChannelSelection channel_selection;
    std::size_t capture_ring_frames = 4096;
    std::size_t playback_ring_frames = 4096;
    std::size_t playback_prefill_frames = 0;
    std::size_t playback_max_frames = 0;
};

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
    if (value == "symmetric-delay") {
        return MetronomeMode::SymmetricDelay;
    }
    if (value == "listener-compensated") {
        return MetronomeMode::ListenerCompensated;
    }
    throw std::runtime_error("--metronome-mode must be shared-grid, leader-audio, symmetric-delay, or listener-compensated");
}

std::string_view metronome_mode_text(MetronomeMode mode)
{
    switch (mode) {
    case MetronomeMode::SharedGrid:
        return "shared-grid";
    case MetronomeMode::LeaderAudio:
        return "leader-audio";
    case MetronomeMode::SymmetricDelay:
        return "symmetric-delay";
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
        return "symmetric-delay";
    case 3:
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
    case MetronomeMode::SymmetricDelay:
        return 2;
    case MetronomeMode::ListenerCompensated:
        return 3;
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

Options parse_options(int argc, char** argv, int start)
{
    Options options;
    for (int i = start; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        if (arg == "--bind") {
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
            if (options.metronome_level < 0.0 || options.metronome_level > 1.0) {
                throw std::runtime_error("--metronome-level must be 0..1");
            }
        } else if (arg == "--metronome-mode") {
            options.metronome_mode = parse_metronome_mode(require_value(argc, argv, i, arg));
        } else if (arg == "--remote-level") {
            options.remote_level = std::stod(std::string(require_value(argc, argv, i, arg)));
            if (options.remote_level < 0.0 || options.remote_level > 1.0) {
                throw std::runtime_error("--remote-level must be 0..1");
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
    if (options.session_id.has_value() != options.session_key.has_value()) {
        throw std::runtime_error("--session-id and --session-key must be provided together");
    }
    if (options.playout_delay_frames == 0) {
        options.playout_delay_frames = options.playback_prefill_frames;
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
    if (options.adaptive_playback_max_frames > options.playback_ring_frames) {
        throw std::runtime_error("--adaptive-playback-max-frames must fit within playback ring capacity");
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

std::vector<std::uint8_t> make_control_packet(
    jam2::protocol::PacketType type,
    const jam2::SessionInfo& session,
    std::uint32_t sequence)
{
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
    return jam2::protocol::encode_packet(header, {}, session.key);
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

struct AudioPacketStats {
    std::uint64_t sent_packets = 0;
    std::uint64_t sent_bytes = 0;
    std::uint64_t recv_packets = 0;
    std::uint64_t recv_bytes = 0;
    std::uint64_t ignored_packets = 0;
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
    std::uint64_t elapsed_ms = 0;
    bool final_metronome_enabled = false;
    int final_bpm = 120;
    double final_metronome_level = 0.20;
    double final_remote_level = 1.0;
};

struct RuntimeState {
    std::atomic<bool> quit{false};
    std::atomic<bool> stats_enabled{false};
    std::atomic<bool> print_stats{false};
    std::atomic<bool> print_status{false};
    std::atomic<bool> metronome{false};
    std::atomic<int> bpm{120};
    std::atomic<int> metronome_level_ppm{200000};
    std::atomic<int> remote_level_ppm{1000000};
    std::atomic<int> metronome_mode{0};
    std::atomic<std::uint64_t> metronome_epoch_sample_time{0};
    std::atomic<bool> metronome_epoch_valid{false};
    std::atomic<std::uint64_t> metronome_revision{0};
};

int ppm_from_unit(double value)
{
    return static_cast<int>(std::clamp(value, 0.0, 1.0) * 1000000.0);
}

int ratio_to_ppm(double ratio)
{
    return static_cast<int>(std::clamp(ratio, 0.5, 2.0) * 1000000.0);
}

double unit_from_ppm(int value)
{
    return static_cast<double>(std::clamp(value, 0, 1000000)) / 1000000.0;
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
    control->metronome_level_ppm.store(runtime.metronome_level_ppm.load(std::memory_order_relaxed), std::memory_order_relaxed);
    control->remote_level_ppm.store(runtime.remote_level_ppm.load(std::memory_order_relaxed), std::memory_order_relaxed);
    control->playback_ratio_ppm.store(ratio_to_ppm(playback_ratio), std::memory_order_relaxed);
    control->metronome_mode.store(runtime.metronome_mode.load(std::memory_order_relaxed), std::memory_order_relaxed);
    control->metronome_epoch_sample_time.store(runtime.metronome_epoch_sample_time.load(std::memory_order_relaxed), std::memory_order_relaxed);
    control->metronome_epoch_valid.store(runtime.metronome_epoch_valid.load(std::memory_order_relaxed), std::memory_order_relaxed);
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
              << "  metro mode <mode>   set shared-grid|leader-audio|symmetric-delay|listener-compensated\n"
              << "  metro level <0..1>  set local metronome level\n"
              << "  metro level +/-n    adjust local metronome level\n"
              << "  metro mute          set local metronome level to 0\n"
              << "  metro unmute        restore local metronome level to 0.20\n"
              << "  remote level <0..1> set peer playback level\n"
              << "  remote level +/-n   adjust peer playback level\n"
              << "  remote mute         set peer playback level to 0\n"
              << "  remote unmute       restore peer playback level to 1\n"
              << "  bpm <1..400>        set metronome tempo\n"
              << "  quit                stop the stream and exit\n"
              << "  exit                stop the stream and exit\n";
}

void print_prompt()
{
    std::cout << "> ";
    std::cout.flush();
}

bool apply_level_token(std::string_view token, std::atomic<int>& target_ppm)
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
        if (value < 0.0 || value > 1.0) {
            return false;
        }
        ppm = ppm_from_unit(value);
    }
    target_ppm.store(std::clamp(ppm, 0, 1000000), std::memory_order_relaxed);
    return true;
}

void stdin_command_loop(RuntimeState& state)
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
                state.metronome.store(true, std::memory_order_relaxed);
                state.metronome_revision.fetch_add(1, std::memory_order_relaxed);
            } else if (value == "off") {
                state.metronome.store(false, std::memory_order_relaxed);
                state.metronome_revision.fetch_add(1, std::memory_order_relaxed);
            } else if (value == "mode") {
                std::string mode;
                in >> mode;
                try {
                    state.metronome_mode.store(metronome_mode_id(parse_metronome_mode(mode)), std::memory_order_relaxed);
                    state.metronome_revision.fetch_add(1, std::memory_order_relaxed);
                } catch (const std::exception& error) {
                    std::cerr << error.what() << "\n";
                }
            } else if (value == "level") {
                std::string level;
                in >> level;
                if (!apply_level_token(level, state.metronome_level_ppm)) {
                    std::cerr << "metro level must be 0..1 or a relative +/- adjustment\n";
                }
            } else if (value == "mute") {
                state.metronome_level_ppm.store(0, std::memory_order_relaxed);
            } else if (value == "unmute") {
                state.metronome_level_ppm.store(200000, std::memory_order_relaxed);
            } else {
                std::cerr << "unknown metro command; use: metro on|off|mode|level|mute|unmute\n";
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
                if (!apply_level_token(level, state.remote_level_ppm)) {
                    std::cerr << "remote level must be 0..1 or a relative +/- adjustment\n";
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
        if (command == "bpm") {
            int value = 0;
            in >> value;
            if (value > 0 && value <= 400) {
                state.bpm.store(value, std::memory_order_relaxed);
                state.metronome_revision.fetch_add(1, std::memory_order_relaxed);
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

std::array<std::uint8_t, 20> encode_metronome_payload(int bpm, std::uint64_t beat, std::uint64_t epoch_sample_time)
{
    std::array<std::uint8_t, 20> payload{};
    payload[0] = static_cast<std::uint8_t>(bpm & 0xff);
    payload[1] = static_cast<std::uint8_t>((bpm >> 8) & 0xff);
    payload[2] = static_cast<std::uint8_t>((bpm >> 16) & 0xff);
    payload[3] = static_cast<std::uint8_t>((bpm >> 24) & 0xff);
    for (int i = 0; i < 8; ++i) {
        payload[4 + i] = static_cast<std::uint8_t>((beat >> (i * 8)) & 0xffU);
        payload[12 + i] = static_cast<std::uint8_t>((epoch_sample_time >> (i * 8)) & 0xffU);
    }
    return payload;
}

struct MetronomePayload {
    int bpm = 120;
    std::uint64_t beat = 0;
    std::uint64_t epoch_sample_time = 0;
};

MetronomePayload decode_metronome_payload(std::span<const std::uint8_t> payload)
{
    if (payload.size() != 12 && payload.size() != 20) {
        throw std::runtime_error("metronome payload size mismatch");
    }
    const int bpm = static_cast<int>(payload[0]) |
        (static_cast<int>(payload[1]) << 8) |
        (static_cast<int>(payload[2]) << 16) |
        (static_cast<int>(payload[3]) << 24);
    std::uint64_t beat = 0;
    for (int i = 7; i >= 0; --i) {
        beat = (beat << 8) | payload[4 + i];
    }
    std::uint64_t epoch = 0;
    if (payload.size() == 20) {
        for (int i = 7; i >= 0; --i) {
            epoch = (epoch << 8) | payload[12 + i];
        }
    }
    return MetronomePayload{bpm, beat, epoch};
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

std::string command_line_text(int argc, char** argv)
{
    std::ostringstream out;
    for (int i = 0; i < argc; ++i) {
        if (i > 0) {
            out << ' ';
        }
        out << argv[i];
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
        << " --metronome " << bool_on_off(options.metronome)
        << " --bpm " << options.bpm
        << " --metronome-level " << options.metronome_level
        << " --remote-level " << options.remote_level
        << " --metronome-mode " << metronome_mode_text(options.metronome_mode)
        << " --sample-time-playout " << bool_on_off(options.sample_time_playout)
        << " --playout-delay-frames " << options.playout_delay_frames
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
    return network_ms + playback_depth_avg_ms(stats, options) + audio_buffer_ms;
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
              << ",\"input_channels\":\"" << mono_mix_mode_text(options.channel_selection.input.size()) << "\""
              << ",\"channel_selection\":\"" << json_escape(channel_selection_text(options.channel_selection)) << "\""
              << ",\"metronome\":\"" << (options.metronome ? "on" : "off") << "\""
              << ",\"bpm\":" << options.bpm
              << ",\"metronome_level\":" << options.metronome_level
              << ",\"metronome_mode\":\"" << metronome_mode_text(options.metronome_mode) << "\""
              << ",\"remote_level\":" << options.remote_level
              << ",\"sample_time_playout\":" << (options.sample_time_playout ? "true" : "false")
              << ",\"playout_delay_frames\":" << options.playout_delay_frames
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

std::filesystem::path make_stats_csv_path(const std::filesystem::path& folder)
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    const std::tm local = local_time_from(now_time);
    std::ostringstream name;
    name << "jam2_stats_"
         << std::put_time(&local, "%Y%m%d_%H%M%S")
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
        std::size_t playback_ring_readable = 0;
        jam2::audio::CallbackTimingStats callback_timing;
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
                "adaptive_playback_longest_above_target_ms,adaptive_playback_longest_under_target_ms\n";
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
             << static_cast<double>(stats.adaptive_playback_longest_under_target_us) / 1000.0 << '\n';
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
        std::vector<std::string> fields(209);
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
    const jam2::audio::MonoRingBuffer* playback_ring)
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
        snapshot.playback_ring_readable = playback_ring->available_read();
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
    context.remote_level = options.remote_level;
    context.audio_device_id = options.audio_device_id.value_or(-1);
    context.requested_input_mode = mono_mix_mode_text(options.channel_selection.input.size());
    context.requested_channels = channel_selection_text(options.channel_selection);
    return context;
}

void print_periodic_stream_stats(const AudioPacketStats& stats, const Options& options, std::uint64_t elapsed_ms)
{
    std::cout << "stats elapsed_ms=" << elapsed_ms
              << " sent=" << stats.sent_packets
              << " recv=" << stats.recv_packets
              << " sequence_lost=" << stats.sequence.lost
              << " sequence_loss_percent=" << sequence_loss_percent(stats)
              << " out_of_order=" << stats.sequence.out_of_order
              << " reordered_recovered=" << stats.reordered_recovered
              << " reordered_lost=" << stats.reordered_lost
              << " late=" << stats.sequence.late
              << " playback_depth_avg_ms=" << playback_depth_avg_ms(stats, options)
              << " jitter_avg_ms="
              << (stats.jitter_samples > 0 ?
                  static_cast<double>(stats.jitter_sum_us) / static_cast<double>(stats.jitter_samples) / 1000.0 :
                  0.0)
              << " jitter_max_ms=" << (stats.jitter_samples > 0 ? static_cast<double>(stats.jitter_max_us) / 1000.0 : 0.0)
              << " estimated_one_way_ms=" << estimated_one_way_ms(stats, options)
              << " rtt_avg_ms=" << rtt_avg_ms(stats)
              << " drift_ppm=" << stats.drift_ppm
              << " resampler_ratio=" << stats.resampler_ratio
              << " playback_dropped_frames=" << stats.playback_dropped_frames
              << "\n";
}

void print_compact_status(
    const AudioPacketStats& stats,
    const Options& options,
    const RuntimeState& runtime,
    const jam2::audio::DeviceStream* audio_stream,
    const jam2::audio::MonoRingBuffer* playback_ring,
    std::uint64_t elapsed_ms)
{
    const std::size_t playback_depth = playback_ring != nullptr ? playback_ring->available_read() : 0;
    const double playback_depth_ms = frames_to_ms(playback_depth, options.sample_rate);
    const double metro_level = unit_from_ppm(runtime.metronome_level_ppm.load(std::memory_order_relaxed));
    const double remote_level = unit_from_ppm(runtime.remote_level_ppm.load(std::memory_order_relaxed));
    const int live_metronome_mode = runtime.metronome_mode.load(std::memory_order_relaxed);
    const bool playback_prefilled = audio_stream != nullptr && audio_stream->playback_prefilled();
    if (options.status_jsonl) {
        std::cout << "{\"event\":\"status\""
                  << ",\"elapsed_ms\":" << elapsed_ms
                  << ",\"metronome\":\"" << (runtime.metronome.load(std::memory_order_relaxed) ? "on" : "off") << "\""
                  << ",\"bpm\":" << runtime.bpm.load(std::memory_order_relaxed)
                  << ",\"metronome_level\":" << metro_level
                  << ",\"remote_level\":" << remote_level
                  << ",\"metronome_mode\":\"" << metronome_mode_text(live_metronome_mode) << "\""
                  << ",\"sample_time_playout\":" << (options.sample_time_playout ? "true" : "false")
                  << ",\"playout_delay_frames\":" << options.playout_delay_frames
                  << ",\"playback_prefilled\":" << (playback_prefilled ? "true" : "false")
                  << ",\"playback_depth_frames\":" << playback_depth
                  << ",\"playback_depth_ms\":" << playback_depth_ms
                  << ",\"expected_remote_sample_time\":" << stats.expected_remote_sample_time
                  << ",\"last_received_sample_time\":" << stats.last_received_sample_time
                  << ",\"last_played_remote_sample_time\":" << stats.last_played_remote_sample_time
                  << ",\"missing_audio_frames_inserted\":" << stats.missing_audio_frames_inserted
                  << ",\"late_audio_frames_dropped\":" << stats.late_audio_frames_dropped
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
                  << ",\"sequence_lost\":" << stats.sequence.lost
                  << ",\"sequence_loss_percent\":" << sequence_loss_percent(stats)
                  << ",\"drift_ppm\":" << stats.drift_ppm
                  << ",\"resampler_ratio\":" << stats.resampler_ratio
                  << "}\n";
        return;
    }
    std::cout << "status elapsed_ms=" << elapsed_ms
              << " metro=" << (runtime.metronome.load(std::memory_order_relaxed) ? "on" : "off")
              << " bpm=" << runtime.bpm.load(std::memory_order_relaxed)
              << " metro_level=" << metro_level
              << " remote_level=" << remote_level
              << " metronome_mode=" << metronome_mode_text(live_metronome_mode)
              << " sample_time_playout=" << (options.sample_time_playout ? "on" : "off")
              << " playout_delay_frames=" << options.playout_delay_frames
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
              << " sequence_lost=" << stats.sequence.lost
              << " sequence_loss_percent=" << sequence_loss_percent(stats)
              << " drift_ppm=" << stats.drift_ppm
              << " resampler_ratio=" << stats.resampler_ratio
              << "\n";
}

void mix_leader_click_into_packet(
    std::span<std::int32_t> samples,
    std::uint64_t packet_sample_time,
    int sample_rate,
    int bpm,
    double level,
    std::uint64_t epoch_sample_time)
{
    if (sample_rate <= 0 || bpm <= 0 || samples.empty()) {
        return;
    }
    const std::uint64_t beat_interval =
        static_cast<std::uint64_t>((60.0 * static_cast<double>(sample_rate)) / static_cast<double>(bpm));
    if (beat_interval == 0) {
        return;
    }
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const std::uint64_t absolute_sample = packet_sample_time + static_cast<std::uint64_t>(i);
        if (absolute_sample < epoch_sample_time) {
            continue;
        }
        const std::uint64_t grid_sample = absolute_sample - epoch_sample_time;
        const std::uint64_t beat_phase = grid_sample % beat_interval;
        const double click_frame_count = static_cast<double>(sample_rate) * 0.010;
        const std::uint64_t click_frames =
            static_cast<std::uint64_t>(click_frame_count < 1.0 ? 1.0 : click_frame_count);
        if (beat_phase >= click_frames) {
            continue;
        }
        const std::uint64_t beat_index = grid_sample / beat_interval;
        const bool accent = (beat_index % 4ULL) == 0ULL;
        const double frequency = accent ? 1800.0 : 1200.0;
        const double phase = 2.0 * 3.14159265358979323846 * frequency *
            static_cast<double>(beat_phase) / static_cast<double>(sample_rate);
        const double envelope = 1.0 - (static_cast<double>(beat_phase) / static_cast<double>(click_frames));
        const double click_level = std::clamp(level * (accent ? 1.6 : 1.0), 0.0, 1.0);
        const double click = std::sin(phase) * envelope * click_level * 8388607.0;
        const double mixed = static_cast<double>(samples[i]) + click;
        samples[i] = static_cast<std::int32_t>(std::clamp(mixed, -8388608.0, 8388607.0));
    }
}

AudioPacketStats run_audio_packet_exchange(
    jam2::UdpSocket& socket,
    const jam2::SessionInfo& session,
    const jam2::Endpoint& peer,
    const Options& options,
    RuntimeState& runtime,
    jam2::audio::StreamControl* audio_control,
    jam2::audio::MonoRingBuffer* capture_ring,
    jam2::audio::MonoRingBuffer* playback_ring,
    jam2::audio::DeviceStream* audio_stream,
    std::uint64_t startup_drained_packets,
    CsvStatsLog* csv_log,
    bool listener_side)
{
    AudioPacketStats stats;
    stats.startup_drained_packets = startup_drained_packets;
    stats.sample_time_playout_enabled = options.sample_time_playout;
    stats.playout_delay_frames = static_cast<std::uint64_t>(options.playout_delay_frames);
    stats.adaptive_playback_cushion_enabled = options.adaptive_playback_cushion;
    stats.adaptive_playback_target_frames = static_cast<std::uint64_t>(options.adaptive_playback_target_frames);
    stats.adaptive_playback_min_frames = static_cast<std::uint64_t>(options.adaptive_playback_min_frames);
    stats.adaptive_playback_max_frames = static_cast<std::uint64_t>(options.adaptive_playback_max_frames);
    const bool bounded_stream = options.stream_ms > 0;
    const bool collect_stats = true;
    const bool collect_diagnostics = collect_stats && (csv_log != nullptr || options.stats_interval_ms > 0);

    std::vector<std::int32_t> asio_frames(static_cast<std::size_t>(options.frame_size), 0);
    std::vector<std::int32_t> network_frames(static_cast<std::size_t>(options.frame_size), 0);
    std::vector<std::int32_t> silence_frames(static_cast<std::size_t>(options.frame_size), 0);
    const auto silence_payload = jam2::protocol::pack_pcm24(network_frames);
    const std::uint16_t audio_payload_size = static_cast<std::uint16_t>(silence_payload.size());
    std::uint32_t audio_sequence = 1;
    std::uint32_t control_sequence = 1;
    std::uint64_t sample_time = 0;
    const std::uint64_t interval_numerator_us = static_cast<std::uint64_t>(options.frame_size) * 1000000ULL;
    const std::uint64_t interval_denominator = static_cast<std::uint64_t>(options.sample_rate);
    const std::uint64_t interval_us = interval_numerator_us / interval_denominator;
    const std::uint64_t interval_remainder_us = interval_numerator_us % interval_denominator;
    std::uint64_t interval_remainder_accumulator = 0;
    std::uint64_t next_send = jam2::monotonic_us();
    std::uint64_t next_ping = next_send;
    std::uint64_t next_metronome = next_send;
    std::uint64_t local_beat = 0;
    std::uint64_t last_metronome_revision = runtime.metronome_revision.load(std::memory_order_relaxed);
    std::uint64_t last_audio_receive_us = 0;
    std::uint64_t last_audio_gap_receive_us = 0;
    bool drift_started = false;
    bool drift_smoothed = false;
    double smoothed_drift_ppm = 0.0;
    double previous_resampler_ratio = 1.0;
    std::uint64_t previous_resampler_ratio_time_us = 0;
    std::uint64_t first_remote_sample_time = 0;
    std::uint64_t first_receive_time_us = 0;
    bool playout_sample_time_initialized = false;
    std::uint64_t next_playout_remote_sample_time = 0;
    std::uint64_t adaptive_target_frames = static_cast<std::uint64_t>(options.adaptive_playback_target_frames);
    std::uint64_t adaptive_last_update_us = 0;
    std::uint64_t adaptive_under_start_us = 0;
    std::uint64_t adaptive_above_start_us = 0;
    bool adaptive_under_active = false;
    bool adaptive_above_active = false;
    std::uint64_t last_send_time_us = 0;
    std::uint64_t last_stream_loop_us = 0;
    const std::uint64_t start_time = next_send;
    std::uint64_t next_stats = collect_stats && options.stats_interval_ms > 0 ?
        start_time + static_cast<std::uint64_t>(options.stats_interval_ms) * 1000ULL :
        0;
    const std::uint64_t send_deadline = bounded_stream ?
        next_send + static_cast<std::uint64_t>(options.stream_ms) * 1000ULL :
        UINT64_MAX;
    const std::uint64_t receive_deadline = bounded_stream ?
        send_deadline + static_cast<std::uint64_t>(options.stream_linger_ms) * 1000ULL :
        UINT64_MAX;
    constexpr std::uint32_t kReorderWindowPackets = 4;
    struct PendingAudioPacket {
        std::uint32_t sequence = 0;
        std::uint64_t sample_time = 0;
        std::uint64_t receive_time = 0;
        bool reordered = false;
        std::vector<std::int32_t> samples;
    };
    std::map<std::uint32_t, PendingAudioPacket> pending_audio;
    bool expected_sequence_set = false;
    std::uint32_t expected_sequence = 0;
    std::uint32_t highest_sequence = 0;
    const std::uint64_t depth_under_2ms_frames =
        static_cast<std::uint64_t>(std::ceil(static_cast<double>(options.sample_rate) * 2.0 / 1000.0)) > 0 ?
            static_cast<std::uint64_t>(std::ceil(static_cast<double>(options.sample_rate) * 2.0 / 1000.0)) : 1;
    const std::uint64_t depth_under_5ms_frames =
        static_cast<std::uint64_t>(std::ceil(static_cast<double>(options.sample_rate) * 5.0 / 1000.0)) > 0 ?
            static_cast<std::uint64_t>(std::ceil(static_cast<double>(options.sample_rate) * 5.0 / 1000.0)) : 1;
    const std::uint64_t depth_under_10ms_frames =
        static_cast<std::uint64_t>(std::ceil(static_cast<double>(options.sample_rate) * 10.0 / 1000.0)) > 0 ?
            static_cast<std::uint64_t>(std::ceil(static_cast<double>(options.sample_rate) * 10.0 / 1000.0)) : 1;
    struct LowDepthTracker {
        bool active = false;
        std::uint64_t start_us = 0;
    };
    LowDepthTracker low_depth_2ms;
    LowDepthTracker low_depth_5ms;
    LowDepthTracker low_depth_10ms;

    auto observe_low_depth_event = [](
        bool below,
        std::uint64_t now_us,
        LowDepthTracker& tracker,
        std::uint64_t& event_count,
        std::uint64_t& max_duration_us) {
        if (below) {
            if (!tracker.active) {
                tracker.active = true;
                tracker.start_us = now_us;
                ++event_count;
            }
            return;
        }
        if (!tracker.active) {
            return;
        }
        const std::uint64_t duration = now_us >= tracker.start_us ? now_us - tracker.start_us : 0;
        if (duration > max_duration_us) {
            max_duration_us = duration;
        }
        tracker.active = false;
    };

    auto observe_target_duration = [](
        bool active,
        std::uint64_t now_us,
        bool& tracker_active,
        std::uint64_t& tracker_start_us,
        std::uint64_t& total_us,
        std::uint64_t& longest_us) {
        if (active) {
            if (!tracker_active) {
                tracker_active = true;
                tracker_start_us = now_us;
            }
            return;
        }
        if (!tracker_active) {
            return;
        }
        const std::uint64_t duration = now_us >= tracker_start_us ? now_us - tracker_start_us : 0;
        total_us += duration;
        if (duration > longest_us) {
            longest_us = duration;
        }
        tracker_active = false;
    };

    auto current_playout_delay_frames = [&]() -> std::uint64_t {
        return options.adaptive_playback_cushion ?
            adaptive_target_frames :
            static_cast<std::uint64_t>(options.playout_delay_frames);
    };

    auto process_audio_packet = [&](const PendingAudioPacket& packet) {
        const std::uint64_t warmup_end_time =
            start_time + static_cast<std::uint64_t>(options.stats_warmup_ms) * 1000ULL;
        const bool past_stats_warmup = packet.receive_time >= warmup_end_time;
        const bool collect_tuning_stats = collect_stats && past_stats_warmup;
        const bool collect_deep_diagnostics = collect_diagnostics && past_stats_warmup;
        if (collect_stats && !collect_tuning_stats) {
            ++stats.stats_warmup_skipped_packets;
        }
        if (playback_ring != nullptr) {
            stats.sample_time_playout_enabled = options.sample_time_playout;
            stats.last_received_sample_time = packet.sample_time;
            std::size_t pushed_frames = 0;
            bool inserted_missing_samples_for_packet = false;
            if (options.sample_time_playout) {
                if (!playout_sample_time_initialized) {
                    playout_sample_time_initialized = true;
                    next_playout_remote_sample_time = packet.sample_time;
                    stats.expected_remote_sample_time = next_playout_remote_sample_time;
                }
                if (packet.sample_time > next_playout_remote_sample_time) {
                    std::uint64_t missing = packet.sample_time - next_playout_remote_sample_time;
                    inserted_missing_samples_for_packet = true;
                    ++stats.missing_sample_ranges;
                    stats.missing_audio_frames_inserted += missing;
                    while (missing > 0) {
                        const std::size_t chunk = static_cast<std::size_t>(std::min<std::uint64_t>(
                            missing,
                            static_cast<std::uint64_t>(silence_frames.size())));
                        pushed_frames += playback_ring->push(std::span<const std::int32_t>(silence_frames.data(), chunk));
                        missing -= chunk;
                    }
                    next_playout_remote_sample_time = packet.sample_time;
                }
                const std::uint64_t packet_end =
                    packet.sample_time + static_cast<std::uint64_t>(packet.samples.size());
                if (packet_end <= next_playout_remote_sample_time) {
                    stats.late_audio_frames_dropped += static_cast<std::uint64_t>(packet.samples.size());
                    return;
                }
                std::span<const std::int32_t> packet_samples(packet.samples.data(), packet.samples.size());
                if (packet.sample_time < next_playout_remote_sample_time) {
                    const std::uint64_t late = next_playout_remote_sample_time - packet.sample_time;
                    const std::size_t skip = static_cast<std::size_t>(std::min<std::uint64_t>(
                        late,
                        static_cast<std::uint64_t>(packet.samples.size())));
                    stats.late_audio_frames_dropped += static_cast<std::uint64_t>(skip);
                    packet_samples = packet_samples.subspan(skip);
                }
                pushed_frames += playback_ring->push(packet_samples);
                next_playout_remote_sample_time += static_cast<std::uint64_t>(packet_samples.size());
                stats.expected_remote_sample_time = next_playout_remote_sample_time;
                stats.last_played_remote_sample_time = next_playout_remote_sample_time;
                stats.remote_sample_lag_frames =
                    next_playout_remote_sample_time >= packet.sample_time ?
                        next_playout_remote_sample_time - packet.sample_time :
                        0;
            } else {
                pushed_frames = playback_ring->push(packet.samples);
                stats.expected_remote_sample_time = packet.sample_time + static_cast<std::uint64_t>(packet.samples.size());
                stats.last_played_remote_sample_time = stats.expected_remote_sample_time;
            }
            stats.playout_delay_frames = current_playout_delay_frames();
            const std::int64_t depth_after_push = static_cast<std::int64_t>(playback_ring->available_read());
            stats.playout_delay_error_frames = depth_after_push - static_cast<std::int64_t>(stats.playout_delay_frames);
            if (collect_deep_diagnostics) {
                observe_timing(
                    static_cast<std::uint64_t>(pushed_frames),
                    stats.playback_push_min_frames,
                    stats.playback_push_sum_frames,
                    stats.playback_push_max_frames);
                ++stats.playback_push_batches;
            }
            if (options.playback_max_frames > 0) {
                const std::size_t depth = playback_ring->available_read();
                if (depth > options.playback_max_frames) {
                    const std::uint64_t dropped =
                        playback_ring->drop_oldest(depth - options.playback_max_frames);
                    if (collect_stats && dropped > 0) {
                        stats.playback_dropped_frames += dropped;
                        ++stats.playback_drop_events;
                        if (dropped > stats.playback_drop_event_max_frames) {
                            stats.playback_drop_event_max_frames = dropped;
                        }
                    }
                }
            }
            if (options.adaptive_playback_cushion && collect_tuning_stats) {
                const std::uint64_t now_us = packet.receive_time;
                std::uint64_t depth = static_cast<std::uint64_t>(playback_ring->available_read());
                const bool burst_evidence =
                    depth < static_cast<std::uint64_t>(options.adaptive_playback_min_frames) ||
                    inserted_missing_samples_for_packet;
                if (burst_evidence && adaptive_target_frames < static_cast<std::uint64_t>(options.adaptive_playback_max_frames)) {
                    const std::uint64_t previous_target = adaptive_target_frames;
                    adaptive_target_frames = std::min<std::uint64_t>(
                        static_cast<std::uint64_t>(options.adaptive_playback_max_frames),
                        std::max<std::uint64_t>(
                            adaptive_target_frames + static_cast<std::uint64_t>(options.frame_size),
                            static_cast<std::uint64_t>(options.adaptive_playback_min_frames)));
                    if (adaptive_target_frames > previous_target) {
                        ++stats.adaptive_playback_raise_events;
                        ++stats.adaptive_playback_burst_events;
                    }
                } else if (!burst_evidence &&
                    adaptive_target_frames > static_cast<std::uint64_t>(options.adaptive_playback_min_frames) &&
                    options.adaptive_playback_release_ppm > 0) {
                    const std::uint64_t elapsed_us =
                        adaptive_last_update_us != 0 && now_us > adaptive_last_update_us ? now_us - adaptive_last_update_us : 0;
                    const double release_frames =
                        static_cast<double>(adaptive_target_frames) *
                        static_cast<double>(options.adaptive_playback_release_ppm) *
                        (static_cast<double>(elapsed_us) / 1000000.0) / 1000000.0;
                    const std::uint64_t release = static_cast<std::uint64_t>(release_frames);
                    if (release > 0) {
                        const std::uint64_t previous_target = adaptive_target_frames;
                        adaptive_target_frames = previous_target > release ?
                            std::max<std::uint64_t>(
                                static_cast<std::uint64_t>(options.adaptive_playback_min_frames),
                                previous_target - release) :
                            static_cast<std::uint64_t>(options.adaptive_playback_min_frames);
                        if (adaptive_target_frames < previous_target) {
                            ++stats.adaptive_playback_release_events;
                        }
                    }
                }
                adaptive_last_update_us = now_us;
                depth = static_cast<std::uint64_t>(playback_ring->available_read());
                if (depth < adaptive_target_frames) {
                    std::uint64_t padding = adaptive_target_frames - depth;
                    stats.adaptive_playback_padding_frames += padding;
                    while (padding > 0) {
                        const std::size_t chunk = static_cast<std::size_t>(std::min<std::uint64_t>(
                            padding,
                            static_cast<std::uint64_t>(silence_frames.size())));
                        (void)playback_ring->push(std::span<const std::int32_t>(silence_frames.data(), chunk));
                        padding -= chunk;
                    }
                }
                depth = static_cast<std::uint64_t>(playback_ring->available_read());
                observe_target_duration(
                    depth < adaptive_target_frames,
                    now_us,
                    adaptive_under_active,
                    adaptive_under_start_us,
                    stats.adaptive_playback_time_under_target_us,
                    stats.adaptive_playback_longest_under_target_us);
                observe_target_duration(
                    depth > adaptive_target_frames,
                    now_us,
                    adaptive_above_active,
                    adaptive_above_start_us,
                    stats.adaptive_playback_time_above_target_us,
                    stats.adaptive_playback_longest_above_target_us);
                stats.adaptive_playback_target_frames = adaptive_target_frames;
                stats.playout_delay_frames = adaptive_target_frames;
                stats.playout_delay_error_frames =
                    static_cast<std::int64_t>(depth) - static_cast<std::int64_t>(adaptive_target_frames);
            }
            if (collect_tuning_stats) {
                const std::uint64_t depth_after = playback_ring->available_read();
                if (collect_deep_diagnostics) {
                    observe_low_depth_event(
                        depth_after < depth_under_2ms_frames,
                        packet.receive_time,
                        low_depth_2ms,
                        stats.playback_depth_under_2ms_events,
                        stats.playback_depth_under_2ms_max_duration_us);
                    observe_low_depth_event(
                        depth_after < depth_under_5ms_frames,
                        packet.receive_time,
                        low_depth_5ms,
                        stats.playback_depth_under_5ms_events,
                        stats.playback_depth_under_5ms_max_duration_us);
                    observe_low_depth_event(
                        depth_after < depth_under_10ms_frames,
                        packet.receive_time,
                        low_depth_10ms,
                        stats.playback_depth_under_10ms_events,
                        stats.playback_depth_under_10ms_max_duration_us);
                }
                observe_timing(
                    depth_after,
                    stats.playback_depth_min_frames,
                    stats.playback_depth_sum_frames,
                    stats.playback_depth_max_frames);
                ++stats.playback_depth_samples;
            }
        }
        if (past_stats_warmup) {
            if (!drift_started) {
                drift_started = true;
                first_remote_sample_time = packet.sample_time;
                first_receive_time_us = packet.receive_time;
            } else if (packet.receive_time > first_receive_time_us && packet.sample_time > first_remote_sample_time) {
                const double remote_elapsed_samples = static_cast<double>(packet.sample_time - first_remote_sample_time);
                const double remote_elapsed_us = remote_elapsed_samples * 1000000.0 / static_cast<double>(options.sample_rate);
                const double local_elapsed_us = static_cast<double>(packet.receive_time - first_receive_time_us);
                stats.raw_drift_ppm = ((remote_elapsed_us / local_elapsed_us) - 1.0) * 1000000.0;
                if (!drift_smoothed || options.drift_smoothing >= 1.0) {
                    smoothed_drift_ppm = stats.raw_drift_ppm;
                    drift_smoothed = true;
                } else if (options.drift_smoothing > 0.0) {
                    smoothed_drift_ppm += (stats.raw_drift_ppm - smoothed_drift_ppm) * options.drift_smoothing;
                }
                stats.drift_ppm = smoothed_drift_ppm;
                const double max_ratio_delta = static_cast<double>(options.drift_max_correction_ppm) / 1000000.0;
                const double raw_ratio = 1.0 + stats.drift_ppm / 1000000.0;
                const bool inside_deadband =
                    std::abs(stats.drift_ppm) <= static_cast<double>(options.drift_deadband_ppm);
                stats.resampler_ratio = options.drift_correction && !inside_deadband ?
                    std::clamp(raw_ratio, 1.0 - max_ratio_delta, 1.0 + max_ratio_delta) :
                    1.0;
                if (collect_deep_diagnostics) {
                    if (stats.resampler_ratio_samples == 0) {
                        stats.resampler_ratio_min = stats.resampler_ratio;
                        stats.resampler_ratio_max = stats.resampler_ratio;
                    } else {
                        if (stats.resampler_ratio < stats.resampler_ratio_min) {
                            stats.resampler_ratio_min = stats.resampler_ratio;
                        }
                        if (stats.resampler_ratio > stats.resampler_ratio_max) {
                            stats.resampler_ratio_max = stats.resampler_ratio;
                        }
                    }
                    stats.resampler_ratio_sum += stats.resampler_ratio;
                    ++stats.resampler_ratio_samples;
                    if (stats.resampler_ratio != 1.0) {
                        ++stats.drift_correction_active_samples;
                    }
                    if (options.drift_correction && !inside_deadband &&
                        (stats.resampler_ratio == 1.0 - max_ratio_delta || stats.resampler_ratio == 1.0 + max_ratio_delta)) {
                        ++stats.drift_correction_clamped_samples;
                    }
                    if (previous_resampler_ratio_time_us != 0 && packet.receive_time > previous_resampler_ratio_time_us) {
                        const double delta_ppm = std::abs(stats.resampler_ratio - previous_resampler_ratio) * 1000000.0;
                        const double delta_seconds =
                            static_cast<double>(packet.receive_time - previous_resampler_ratio_time_us) / 1000000.0;
                        const double ppm_per_second = delta_seconds > 0.0 ? delta_ppm / delta_seconds : 0.0;
                        if (ppm_per_second > stats.resampler_ratio_change_max_ppm_per_second) {
                            stats.resampler_ratio_change_max_ppm_per_second = ppm_per_second;
                        }
                    }
                    previous_resampler_ratio = stats.resampler_ratio;
                    previous_resampler_ratio_time_us = packet.receive_time;
                }
                sync_audio_control(runtime, audio_control, stats.resampler_ratio);
                stats.drift_valid = true;
            }
        }
        if (collect_tuning_stats && last_audio_receive_us != 0) {
            const std::uint64_t interval =
                packet.receive_time >= last_audio_receive_us ? packet.receive_time - last_audio_receive_us : 0;
            const std::uint64_t jitter =
                interval > interval_us ? interval - interval_us : interval_us - interval;
            observe_timing(jitter, stats.jitter_min_us, stats.jitter_sum_us, stats.jitter_max_us);
            ++stats.jitter_samples;
        }
        if (collect_deep_diagnostics && last_audio_gap_receive_us != 0) {
            const std::uint64_t interval =
                packet.receive_time >= last_audio_gap_receive_us ? packet.receive_time - last_audio_gap_receive_us : 0;
            observe_timing(
                interval,
                stats.audio_packet_gap_min_us,
                stats.audio_packet_gap_sum_us,
                stats.audio_packet_gap_max_us);
            ++stats.audio_packet_gap_samples;
            if (interval_us > 0 && interval > interval_us * 2) {
                ++stats.audio_packet_gap_over_2x_count;
            }
            if (interval_us > 0 && interval > interval_us * 4) {
                ++stats.audio_packet_gap_over_4x_count;
            }
        }
        if (collect_tuning_stats) {
            last_audio_receive_us = packet.receive_time;
            if (collect_deep_diagnostics) {
                last_audio_gap_receive_us = packet.receive_time;
            }
        } else {
            last_audio_receive_us = 0;
            last_audio_gap_receive_us = 0;
        }
    };

    auto drain_reorder_buffer = [&]() {
        for (;;) {
            const auto next = pending_audio.find(expected_sequence);
            if (next != pending_audio.end()) {
                if (collect_stats && next->second.reordered) {
                    ++stats.reordered_recovered;
                }
                process_audio_packet(next->second);
                pending_audio.erase(next);
                ++expected_sequence;
                continue;
            }
            if (expected_sequence_set && highest_sequence > expected_sequence + kReorderWindowPackets) {
                if (collect_stats) {
                    ++stats.sequence.lost;
                    ++stats.reordered_lost;
                    ++stats.reordered_lost_events;
                }
                ++expected_sequence;
                continue;
            }
            break;
        }
    };

    while (jam2::monotonic_us() < receive_deadline && !stats.received_bye && !runtime.quit.load(std::memory_order_relaxed)) {
        const std::uint64_t now = jam2::monotonic_us();
        if (collect_diagnostics && last_stream_loop_us != 0 && now >= last_stream_loop_us) {
            observe_timing(now - last_stream_loop_us, stats.receive_loop_gap_min_us, stats.receive_loop_gap_sum_us, stats.receive_loop_gap_max_us);
            ++stats.receive_loop_gap_samples;
        }
        last_stream_loop_us = now;
        const bool metronome_enabled = runtime.metronome.load(std::memory_order_relaxed);
        const std::uint64_t metronome_revision = runtime.metronome_revision.load(std::memory_order_relaxed);
        if (metronome_enabled && (!runtime.metronome_epoch_valid.load(std::memory_order_relaxed) ||
            metronome_revision != last_metronome_revision)) {
            runtime.metronome_epoch_sample_time.store(
                sample_time + current_playout_delay_frames(),
                std::memory_order_relaxed);
            runtime.metronome_epoch_valid.store(true, std::memory_order_relaxed);
        }
        sync_audio_control(runtime, audio_control, stats.resampler_ratio);
        int sends_this_loop = 0;
        while (now >= next_send && next_send < send_deadline && sends_this_loop < 8) {
            std::vector<std::uint8_t> payload;
            if (capture_ring != nullptr) {
                (void)capture_ring->pop(asio_frames);
                for (std::size_t i = 0; i < asio_frames.size(); ++i) {
                    network_frames[i] = asio_frames[i] / 256;
                }
                const int live_metronome_mode = runtime.metronome_mode.load(std::memory_order_relaxed);
                if (live_metronome_mode == metronome_mode_id(MetronomeMode::LeaderAudio) && listener_side && metronome_enabled) {
                    mix_leader_click_into_packet(
                        network_frames,
                        sample_time,
                        options.sample_rate,
                        runtime.bpm.load(std::memory_order_relaxed),
                        unit_from_ppm(runtime.metronome_level_ppm.load(std::memory_order_relaxed)),
                        runtime.metronome_epoch_sample_time.load(std::memory_order_relaxed));
                }
                payload = jam2::protocol::pack_pcm24(network_frames);
            } else {
                const int live_metronome_mode = runtime.metronome_mode.load(std::memory_order_relaxed);
                if (live_metronome_mode == metronome_mode_id(MetronomeMode::LeaderAudio) && listener_side && metronome_enabled) {
                    std::fill(network_frames.begin(), network_frames.end(), 0);
                    mix_leader_click_into_packet(
                        network_frames,
                        sample_time,
                        options.sample_rate,
                        runtime.bpm.load(std::memory_order_relaxed),
                        unit_from_ppm(runtime.metronome_level_ppm.load(std::memory_order_relaxed)),
                        runtime.metronome_epoch_sample_time.load(std::memory_order_relaxed));
                    payload = jam2::protocol::pack_pcm24(network_frames);
                } else {
                    payload = silence_payload;
                }
            }
            const jam2::protocol::Header header{
                jam2::protocol::PacketType::Audio,
                0,
                session.session_id,
                audio_sequence++,
                sample_time,
                now,
                0,
                0,
            };
            const auto packet = jam2::protocol::encode_packet(header, payload, session.key);
            const std::uint64_t actual_send_time = jam2::monotonic_us();
            socket.send_to(peer, packet);
            if (collect_stats) {
                ++stats.sent_packets;
                stats.sent_bytes += packet.size();
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
                    actual_send_time >= next_send ? actual_send_time - next_send : next_send - actual_send_time;
                observe_timing(
                    schedule_error,
                    stats.send_schedule_error_min_us,
                    stats.send_schedule_error_sum_us,
                    stats.send_schedule_error_max_us);
                ++stats.send_schedule_error_samples;
                last_send_time_us = actual_send_time;
            }
            sample_time += static_cast<std::uint64_t>(options.frame_size);
            std::uint64_t next_send_step = interval_us == 0 ? 1 : interval_us;
            interval_remainder_accumulator += interval_remainder_us;
            if (interval_remainder_accumulator >= interval_denominator) {
                ++next_send_step;
                interval_remainder_accumulator -= interval_denominator;
            }
            next_send += next_send_step;
            ++sends_this_loop;
        }
        if (collect_diagnostics && sends_this_loop > 1) {
            ++stats.send_catchup_events;
            if (static_cast<std::uint64_t>(sends_this_loop) > stats.send_catchup_max_packets) {
                stats.send_catchup_max_packets = static_cast<std::uint64_t>(sends_this_loop);
            }
        }
        if (now >= next_ping && now < send_deadline) {
            const jam2::protocol::Header ping{
                jam2::protocol::PacketType::Ping,
                0,
                session.session_id,
                control_sequence++,
                0,
                now,
                0,
                0,
            };
            const auto packet = jam2::protocol::encode_packet(ping, {}, session.key);
            socket.send_to(peer, packet);
            if (collect_stats) {
                ++stats.sent_pings;
            }
            next_ping += 100000ULL;
        }
        if (((metronome_enabled && now >= next_metronome) || metronome_revision != last_metronome_revision) &&
            now < send_deadline) {
            const int current_bpm = runtime.bpm.load(std::memory_order_relaxed);
            const std::uint64_t epoch = runtime.metronome_epoch_sample_time.load(std::memory_order_relaxed);
            const auto metro_payload = encode_metronome_payload(metronome_enabled ? current_bpm : -current_bpm, local_beat++, epoch);
            const jam2::protocol::Header metro{
                jam2::protocol::PacketType::MetronomeState,
                0,
                session.session_id,
                control_sequence++,
                sample_time,
                now,
                0,
                0,
            };
            const auto packet = jam2::protocol::encode_packet(metro, metro_payload, session.key);
            socket.send_to(peer, packet);
            if (collect_stats) {
                ++stats.metronome_sent;
            }
            stats.local_metronome_beat = local_beat;
            stats.metronome_epoch_sample_time = epoch;
            stats.metronome_alignment_valid = runtime.metronome_epoch_valid.load(std::memory_order_relaxed);
            last_metronome_revision = metronome_revision;
            if (metronome_enabled) {
                const std::uint64_t beat_interval_us = 60000000ULL / static_cast<std::uint64_t>(current_bpm);
                next_metronome += beat_interval_us == 0 ? 1 : beat_interval_us;
            } else {
                next_metronome = now + 1000000ULL;
            }
        }
        if (collect_stats && runtime.print_stats.exchange(false, std::memory_order_relaxed)) {
            const std::uint64_t elapsed_ms = (now - start_time) / 1000ULL;
            print_periodic_stream_stats(stats, options, elapsed_ms);
        }
        if (runtime.print_status.exchange(false, std::memory_order_relaxed)) {
            const std::uint64_t elapsed_ms = (now - start_time) / 1000ULL;
            print_compact_status(stats, options, runtime, audio_stream, playback_ring, elapsed_ms);
        }
        if (!stats.sent_bye && now >= send_deadline) {
            const jam2::protocol::Header bye{
                jam2::protocol::PacketType::Bye,
                0,
                session.session_id,
                control_sequence++,
                0,
                now,
                0,
                0,
            };
            const auto packet = jam2::protocol::encode_packet(bye, {}, session.key);
            socket.send_to(peer, packet);
            stats.sent_bye = true;
        }
        if (next_stats != 0 && now >= next_stats) {
            const std::uint64_t elapsed_ms = (now - start_time) / 1000ULL;
            if (runtime.stats_enabled.load(std::memory_order_relaxed)) {
                print_periodic_stream_stats(stats, options, elapsed_ms);
            }
            if (csv_log != nullptr) {
                csv_log->write_periodic(
                    elapsed_ms,
                    stats,
                    options,
                    make_audio_snapshot(audio_stream, capture_ring, playback_ring));
            }
            next_stats += static_cast<std::uint64_t>(options.stats_interval_ms) * 1000ULL;
        }

        bool received_any = false;
        std::uint64_t received_this_loop = 0;
        for (;;) {
            const auto received = socket.recv_from(received_any ? 0 : 1);
            if (!received) {
                break;
            }
            received_any = true;
            if (collect_diagnostics) {
                ++received_this_loop;
            }
            const auto& [from, bytes] = *received;
            if (from.host != peer.host || from.port != peer.port) {
                if (collect_stats) {
                    ++stats.ignored_packets;
                }
                continue;
            }
            try {
                const auto header = jam2::protocol::decode_packet(bytes, session.key, session.session_id);
                if (header.type == jam2::protocol::PacketType::Audio) {
                    if (header.payload_length != audio_payload_size) {
                        if (collect_stats) {
                            ++stats.ignored_packets;
                        }
                        continue;
                    }
                    if (collect_stats) {
                        ++stats.recv_packets;
                        stats.recv_bytes += bytes.size();
                    }
                    const std::uint64_t receive_time = jam2::monotonic_us();
                    if (!expected_sequence_set) {
                        expected_sequence_set = true;
                        expected_sequence = header.sequence;
                        highest_sequence = header.sequence;
                    }
                    if (header.sequence < expected_sequence) {
                        if (collect_stats) {
                            ++stats.sequence.late;
                            ++stats.ignored_packets;
                        }
                        continue;
                    }
                    if (pending_audio.find(header.sequence) != pending_audio.end()) {
                        if (collect_stats) {
                            ++stats.sequence.duplicate;
                            ++stats.ignored_packets;
                        }
                        continue;
                    }
                    const bool reordered = header.sequence > expected_sequence;
                    if (reordered && collect_stats) {
                        ++stats.sequence.out_of_order;
                        if (collect_diagnostics) {
                            const std::uint64_t distance =
                                static_cast<std::uint64_t>(header.sequence - expected_sequence);
                            if (distance > stats.reordered_max_distance_packets) {
                                stats.reordered_max_distance_packets = distance;
                            }
                        }
                    }
                    if (header.sequence > highest_sequence) {
                        highest_sequence = header.sequence;
                    }
                    const auto received_payload = std::span<const std::uint8_t>(
                        bytes.data() + jam2::protocol::kHeaderSize,
                        header.payload_length);
                    auto decoded = jam2::protocol::unpack_pcm24(received_payload);
                    for (auto& sample : decoded) {
                        sample *= 256;
                    }
                    pending_audio.emplace(
                        header.sequence,
                        PendingAudioPacket{header.sequence, header.sample_time, receive_time, reordered, std::move(decoded)});
                    drain_reorder_buffer();
                } else if (header.type == jam2::protocol::PacketType::Ping) {
                    const jam2::protocol::Header pong{
                        jam2::protocol::PacketType::Pong,
                        0,
                        session.session_id,
                        header.sequence,
                        0,
                        header.send_time_us,
                        0,
                        0,
                    };
                    const auto packet = jam2::protocol::encode_packet(pong, {}, session.key);
                    socket.send_to(peer, packet);
                    if (collect_stats) {
                        ++stats.sent_pongs;
                    }
                } else if (header.type == jam2::protocol::PacketType::Pong) {
                    const std::uint64_t receive_time = jam2::monotonic_us();
                    if (collect_stats && receive_time >= header.send_time_us) {
                        observe_timing(
                            receive_time - header.send_time_us,
                            stats.rtt_min_us,
                            stats.rtt_sum_us,
                            stats.rtt_max_us);
                    }
                    if (collect_stats) {
                        ++stats.recv_pongs;
                    }
                } else if (header.type == jam2::protocol::PacketType::Bye) {
                    stats.received_bye = true;
                    break;
                } else if (header.type == jam2::protocol::PacketType::MetronomeState) {
                    const auto payload = std::span<const std::uint8_t>(
                        bytes.data() + jam2::protocol::kHeaderSize,
                        header.payload_length);
                    const auto metronome_payload = decode_metronome_payload(payload);
                    const int remote_bpm = metronome_payload.bpm;
                    if (runtime.metronome_revision.load(std::memory_order_relaxed) == 0) {
                        if (remote_bpm < 0 && remote_bpm >= -400) {
                            runtime.metronome.store(false, std::memory_order_relaxed);
                            runtime.bpm.store(-remote_bpm, std::memory_order_relaxed);
                            runtime.metronome_epoch_valid.store(false, std::memory_order_relaxed);
                        } else if (remote_bpm > 0 && remote_bpm <= 400) {
                            runtime.metronome.store(true, std::memory_order_relaxed);
                            runtime.bpm.store(remote_bpm, std::memory_order_relaxed);
                            runtime.metronome_epoch_sample_time.store(
                                metronome_payload.epoch_sample_time + current_playout_delay_frames(),
                                std::memory_order_relaxed);
                            runtime.metronome_epoch_valid.store(true, std::memory_order_relaxed);
                        }
                    }
                    stats.last_remote_beat = metronome_payload.beat;
                    stats.remote_metronome_beat = metronome_payload.beat;
                    stats.metronome_epoch_sample_time = runtime.metronome_epoch_sample_time.load(std::memory_order_relaxed);
                    stats.metronome_alignment_valid = runtime.metronome_epoch_valid.load(std::memory_order_relaxed);
                    if (collect_stats) {
                        ++stats.metronome_received;
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
            session.session_id,
            control_sequence++,
            0,
            now,
            0,
            0,
        };
        const auto packet = jam2::protocol::encode_packet(bye, {}, session.key);
        socket.send_to(peer, packet);
        stats.sent_bye = true;
    }

    const std::uint64_t finish_time = jam2::monotonic_us();
    if (collect_diagnostics) {
        observe_low_depth_event(
            false,
            finish_time,
            low_depth_2ms,
            stats.playback_depth_under_2ms_events,
            stats.playback_depth_under_2ms_max_duration_us);
        observe_low_depth_event(
            false,
            finish_time,
            low_depth_5ms,
            stats.playback_depth_under_5ms_events,
            stats.playback_depth_under_5ms_max_duration_us);
        observe_low_depth_event(
            false,
            finish_time,
            low_depth_10ms,
            stats.playback_depth_under_10ms_events,
            stats.playback_depth_under_10ms_max_duration_us);
    }
    if (options.adaptive_playback_cushion) {
        observe_target_duration(
            false,
            finish_time,
            adaptive_under_active,
            adaptive_under_start_us,
            stats.adaptive_playback_time_under_target_us,
            stats.adaptive_playback_longest_under_target_us);
        observe_target_duration(
            false,
            finish_time,
            adaptive_above_active,
            adaptive_above_start_us,
            stats.adaptive_playback_time_above_target_us,
            stats.adaptive_playback_longest_above_target_us);
    }

    stats.elapsed_ms = (finish_time - start_time) / 1000ULL;
    stats.final_metronome_enabled = runtime.metronome.load(std::memory_order_relaxed);
    stats.final_bpm = runtime.bpm.load(std::memory_order_relaxed);
    stats.final_metronome_level = unit_from_ppm(runtime.metronome_level_ppm.load(std::memory_order_relaxed));
    stats.final_remote_level = unit_from_ppm(runtime.remote_level_ppm.load(std::memory_order_relaxed));
    stats.metronome_epoch_sample_time = runtime.metronome_epoch_sample_time.load(std::memory_order_relaxed);
    stats.metronome_alignment_valid = runtime.metronome_epoch_valid.load(std::memory_order_relaxed);
    stats.adaptive_playback_cushion_enabled = options.adaptive_playback_cushion;
    stats.adaptive_playback_target_frames = adaptive_target_frames;
    stats.adaptive_playback_min_frames = static_cast<std::uint64_t>(options.adaptive_playback_min_frames);
    stats.adaptive_playback_max_frames = static_cast<std::uint64_t>(options.adaptive_playback_max_frames);
    if (csv_log != nullptr) {
        csv_log->write(
            "final",
            stats.elapsed_ms,
            stats,
            options,
            make_audio_snapshot(audio_stream, capture_ring, playback_ring));
    }
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
    std::cout << "Stream duration ms: " << stream_ms << "\n";
    std::cout << "Sample rate: " << options.sample_rate << "\n";
    std::cout << "Frame size samples: " << options.frame_size << "\n";
    std::cout << "Frame interval ms: " << frame_interval_ms << "\n";
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
    std::cout << "Remote playback level: " << options.remote_level << "\n";
    std::cout << "Sample-time playout: " << (options.sample_time_playout ? "on" : "off") << "\n";
    std::cout << "Playout delay frames: " << options.playout_delay_frames << "\n";
    std::cout << "Playout delay ms: " << frames_to_ms(options.playout_delay_frames, options.sample_rate) << "\n";
    std::cout << "Adaptive playback cushion requested: " << (options.adaptive_playback_cushion ? "on" : "off") << "\n";
    std::cout << "Adaptive playback target frames requested: " << options.adaptive_playback_target_frames << "\n";
    std::cout << "Adaptive playback min frames requested: " << options.adaptive_playback_min_frames << "\n";
    std::cout << "Adaptive playback max frames requested: " << options.adaptive_playback_max_frames << "\n";
    std::cout << "Adaptive playback release ppm: " << options.adaptive_playback_release_ppm << "\n";
    std::cout << "Final metronome: " << (stats.final_metronome_enabled ? "on" : "off") << "\n";
    std::cout << "Final BPM: " << stats.final_bpm << "\n";
    std::cout << "Final metronome level: " << stats.final_metronome_level << "\n";
    std::cout << "Final remote playback level: " << stats.final_remote_level << "\n";
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
    if (stats.recv_pongs > 0 && stats.playback_depth_samples > 0) {
        std::cout << "Estimated one-way latency ms: " << estimated_one_way_ms(stats, options) << "\n";
        std::cout << "Estimated one-way latency note: RTT/2 + playback depth avg + audio buffer\n";
    }
    if (stats.drift_valid) {
        std::cout << "Raw drift ppm estimate: " << stats.raw_drift_ppm << "\n";
        std::cout << "Smoothed drift ppm estimate: " << stats.drift_ppm << "\n";
        std::cout << "Resampler ratio: " << stats.resampler_ratio << "\n";
    }
}

struct OptionalAudioStream {
    std::unique_ptr<jam2::audio::StreamControl> control;
    std::unique_ptr<jam2::audio::MonoRingBuffer> capture_ring;
    std::unique_ptr<jam2::audio::MonoRingBuffer> playback_ring;
    std::unique_ptr<jam2::audio::DeviceStream> stream;
};

OptionalAudioStream start_optional_audio(const Options& options, bool leader_audio_local_click)
{
    OptionalAudioStream audio;
    if (!options.audio_device_id) {
        return audio;
    }
    audio.control = std::make_unique<jam2::audio::StreamControl>();
    audio.control->metronome_enabled.store(options.metronome, std::memory_order_relaxed);
    audio.control->metronome_bpm.store(options.bpm, std::memory_order_relaxed);
    audio.control->metronome_level_ppm.store(ppm_from_unit(options.metronome_level), std::memory_order_relaxed);
    audio.control->remote_level_ppm.store(ppm_from_unit(options.remote_level), std::memory_order_relaxed);
    audio.control->playback_ratio_ppm.store(1000000, std::memory_order_relaxed);
    audio.control->metronome_mode.store(metronome_mode_id(options.metronome_mode), std::memory_order_relaxed);
    audio.control->leader_audio_local_click.store(leader_audio_local_click, std::memory_order_relaxed);
    audio.control->metronome_epoch_sample_time.store(0, std::memory_order_relaxed);
    audio.control->metronome_epoch_valid.store(options.metronome, std::memory_order_relaxed);
    audio.capture_ring = std::make_unique<jam2::audio::MonoRingBuffer>(options.capture_ring_frames);
    audio.playback_ring = std::make_unique<jam2::audio::MonoRingBuffer>(options.playback_ring_frames);
    const bool diagnostics_enabled =
        options.stats_enabled && (options.log_stats_dir.has_value() || options.stats_interval_ms > 0);
    audio.capture_ring->set_diagnostics_enabled(diagnostics_enabled);
    audio.playback_ring->set_diagnostics_enabled(diagnostics_enabled);
    audio.playback_ring->set_depth_bucket_thresholds(static_cast<double>(options.sample_rate));
    audio.stream = jam2::audio::start_duplex_stream(
        *options.audio_device_id,
        static_cast<double>(options.sample_rate),
        options.audio_buffer_size,
        options.input_channels,
        options.channel_selection,
        *audio.capture_ring,
        *audio.playback_ring,
        options.playback_prefill_frames,
        *audio.control);
    return audio;
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

    explicit CommandThread(const Options& options)
    {
        state.metronome.store(options.metronome, std::memory_order_relaxed);
        state.bpm.store(options.bpm, std::memory_order_relaxed);
        state.metronome_level_ppm.store(ppm_from_unit(options.metronome_level), std::memory_order_relaxed);
        state.remote_level_ppm.store(ppm_from_unit(options.remote_level), std::memory_order_relaxed);
        state.metronome_mode.store(metronome_mode_id(options.metronome_mode), std::memory_order_relaxed);
        state.metronome_epoch_sample_time.store(0, std::memory_order_relaxed);
        state.metronome_epoch_valid.store(options.metronome, std::memory_order_relaxed);
        state.stats_enabled.store(options.stats_enabled, std::memory_order_relaxed);
        thread = std::thread([this] { stdin_command_loop(state); });
    }

    ~CommandThread()
    {
        state.quit.store(true, std::memory_order_relaxed);
        if (thread.joinable()) {
            thread.detach();
        }
    }
};

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
    std::cout << "Playback prefilled: " << (audio.stream->playback_prefilled() ? "yes" : "no") << "\n";
    std::cout << "Playback prefill frames: " << options.playback_prefill_frames << "\n";
    std::cout << "Playback prefill ms: " << frames_to_ms(options.playback_prefill_frames, options.sample_rate) << "\n";
    std::cout << "Capture ring overruns frames: " << capture_stats.overruns << "\n";
    std::cout << "Capture ring underruns frames: " << capture_stats.underruns << "\n";
    std::cout << "Capture ring underrun events: " << capture_stats.underrun_events << "\n";
    std::cout << "Capture ring readable frames: " << capture_readable << "\n";
    std::cout << "Capture ring readable ms: " << frames_to_ms(capture_readable, options.sample_rate) << "\n";
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
                  << unit_from_ppm(audio.control->metronome_level_ppm.load(std::memory_order_relaxed)) << "\n";
        std::cout << "Audio control remote playback level: "
                  << unit_from_ppm(audio.control->remote_level_ppm.load(std::memory_order_relaxed)) << "\n";
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
    const std::uint64_t deadline = options.wait_ms > 0 ?
        jam2::monotonic_us() + static_cast<std::uint64_t>(options.wait_ms) * 1000ULL :
        UINT64_MAX;
    while (jam2::monotonic_us() < deadline) {
        const auto received = socket.recv_from(250);
        if (!received) {
            continue;
        }
        const auto& [from, bytes] = *received;
        if (locked_peer && (from.host != locked_peer->host || from.port != locked_peer->port)) {
            ++ignored_wrong_endpoint;
            continue;
        }
        jam2::protocol::Header header{};
        try {
            header = jam2::protocol::decode_packet(bytes, session.key, session.session_id);
        } catch (const std::exception&) {
            ++ignored_malformed;
            continue;
        }
        if (header.type != jam2::protocol::PacketType::Hello) {
            ++ignored_malformed;
            continue;
        }
        locked_peer = from;
        socket.send_to(from, make_control_packet(jam2::protocol::PacketType::HelloAck, session, header.sequence));
        std::cout << "Peer locked: " << jam2::endpoint_to_string(from) << "\n";
        std::cout << "Handshake complete\n";
        std::cout << "Ignored malformed/auth/session packets: " << ignored_malformed << "\n";
        std::cout << "Ignored wrong-endpoint packets: " << ignored_wrong_endpoint << "\n";
        print_startup_json("listen", "connected", options, local, from, endpoint_mode, connection_url);
        auto audio = start_optional_audio(options, true);
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
        CommandThread commands(options);
        auto audio_stats = run_audio_packet_exchange(
            socket,
            session,
            from,
            options,
            commands.state,
            audio.control.get(),
            audio.capture_ring.get(),
            audio.playback_ring.get(),
            audio.stream.get(),
            static_cast<std::uint64_t>(drained_startup_packets),
            csv_log ? &*csv_log : nullptr,
            true);
        audio_stats.startup_drained_packets = static_cast<std::uint64_t>(drained_startup_packets);
        if (commands.state.stats_enabled.load(std::memory_order_relaxed)) {
            print_audio_packet_stats(audio_stats, options);
            print_optional_audio_stats(audio, options);
        }
        std::cout.flush();
        return 0;
    }
    std::cerr << "Timed out waiting for authenticated peer\n";
    std::cerr << "Ignored malformed/auth/session packets: " << ignored_malformed << "\n";
    std::cerr << "Ignored wrong-endpoint packets: " << ignored_wrong_endpoint << "\n";
    print_startup_json("listen", "error", options, local, std::nullopt, endpoint_mode, connection_url, "timed out waiting for authenticated peer");
    return 3;
}

int run_connect(int argc, char** argv)
{
    if (argc < 3) {
        throw std::runtime_error("connect requires a jam2 URL");
    }
    const auto session = jam2::parse_jam_url(argv[2]);
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

    const auto hello = make_control_packet(jam2::protocol::PacketType::Hello, session, 1);
    const std::uint64_t deadline = options.wait_ms > 0 ?
        jam2::monotonic_us() + static_cast<std::uint64_t>(options.wait_ms) * 1000ULL :
        UINT64_MAX;
    int attempts = 0;
    int ignored = 0;
    while (jam2::monotonic_us() < deadline) {
        ++attempts;
        socket.send_to(session.endpoint, hello);
        const auto received = socket.recv_from(500);
        if (!received) {
            continue;
        }
        const auto& [from, bytes] = *received;
        if (from.host != session.endpoint.host || from.port != session.endpoint.port) {
            ++ignored;
            continue;
        }
        try {
            const auto header = jam2::protocol::decode_packet(bytes, session.key, session.session_id);
            if (header.type == jam2::protocol::PacketType::HelloAck) {
                std::cout << "Handshake complete\n";
                std::cout << "Attempts: " << attempts << "\n";
                std::cout << "Ignored packets: " << ignored << "\n";
                print_startup_json("connect", "connected", options, socket.local_endpoint(), session.endpoint, "jam2-url", "");
                auto audio = start_optional_audio(options, false);
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
                CommandThread commands(options);
                auto audio_stats = run_audio_packet_exchange(
                    socket,
                    session,
                    session.endpoint,
                    options,
                    commands.state,
                    audio.control.get(),
                    audio.capture_ring.get(),
                    audio.playback_ring.get(),
                    audio.stream.get(),
                    static_cast<std::uint64_t>(drained_startup_packets),
                    csv_log ? &*csv_log : nullptr,
                    false);
                audio_stats.startup_drained_packets = static_cast<std::uint64_t>(drained_startup_packets);
                if (commands.state.stats_enabled.load(std::memory_order_relaxed)) {
                    print_audio_packet_stats(audio_stats, options);
                    print_optional_audio_stats(audio, options);
                }
                std::cout.flush();
                return 0;
            }
            ++ignored;
        } catch (const std::exception&) {
            ++ignored;
        }
    }
    std::cerr << "Timed out waiting for HELLO_ACK\n";
    std::cerr << "Attempts: " << attempts << "\n";
    std::cerr << "Ignored packets: " << ignored << "\n";
    print_startup_json("connect", "error", options, socket.local_endpoint(), session.endpoint, "jam2-url", "", "timed out waiting for HELLO_ACK");
    return 3;
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

    if (command == "listen") {
        return run_listen(argc, argv);
    }

    if (command == "connect") {
        return run_connect(argc, argv);
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

#include <exception>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <sstream>
#include <span>
#include <string_view>
#include <thread>
#include <utility>

#include "audio_device.hpp"
#include "common.hpp"
#include "protocol.hpp"
#include "stun.hpp"
#include "udp_socket.hpp"

namespace {

constexpr std::string_view kUsage = R"(jam2 - two-person low-latency music streaming tool

Usage:
  jam2 --help
  jam2 --list-devices
  jam2 probe-device <id> [--sample-rate n]
  jam2 meter-device <id> [--sample-rate n] [--buffer-size n] [--duration-ms n]
  jam2 ring-device <id> [--sample-rate n] [--buffer-size n] [--duration-ms n] [--ring-frames n]
  jam2 listen [--bind ip:port] [--stun host:port] [--no-stun] [--public-endpoint ip:port] [--wait-ms n] [--stream-ms n] [--stream-linger-ms n] [--stats-interval-ms n] [--metronome on|off] [--bpm n] [--metronome-level n] [--socket-send-buffer n] [--socket-recv-buffer n] [--input-channels mono|stereo] [--playback-prefill-frames n] [--playback-max-frames n] [--drift-smoothing n] [--drift-max-correction-ppm n]
  jam2 connect <jam2-url> [options]

Stage status:
  UDP HELLO/HELLO_ACK session setup, jam2 URL parsing, and STUN endpoint discovery are implemented.
  UDP audio streaming, Windows ASIO, drift stats/correction, and metronome controls are implemented for the MVP slice.
)";

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
    int stats_interval_ms = 0;
    std::optional<int> socket_send_buffer;
    std::optional<int> socket_recv_buffer;
    int sample_rate = 48000;
    int frame_size = 128;
    bool drift_correction = true;
    double drift_smoothing = 0.02;
    int drift_max_correction_ppm = 500;
    bool metronome = false;
    int bpm = 120;
    double metronome_level = 0.20;
    std::optional<int> audio_device_id;
    long audio_buffer_size = 0;
    jam2::audio::InputChannels input_channels = jam2::audio::InputChannels::Mono;
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
        } else if (arg == "--stats-interval-ms") {
            options.stats_interval_ms = std::stoi(std::string(require_value(argc, argv, i, arg)));
            if (options.stats_interval_ms < 0) {
                throw std::runtime_error("--stats-interval-ms must be non-negative");
            }
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
            if (options.frame_size != 32 && options.frame_size != 64 && options.frame_size != 128 && options.frame_size != 256) {
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
            if (value == "mono" || value == "1") {
                options.input_channels = jam2::audio::InputChannels::Mono;
            } else if (value == "stereo" || value == "2") {
                options.input_channels = jam2::audio::InputChannels::Stereo;
            } else {
                throw std::runtime_error("--input-channels must be mono or stereo");
            }
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
    std::uint64_t playback_dropped_frames = 0;
    std::uint64_t playback_depth_min_frames = 0;
    std::uint64_t playback_depth_sum_frames = 0;
    std::uint64_t playback_depth_max_frames = 0;
    std::uint64_t playback_depth_samples = 0;
    jam2::protocol::SequenceStats sequence;
    std::uint64_t audio_delay_min_us = 0;
    std::uint64_t audio_delay_sum_us = 0;
    std::uint64_t audio_delay_max_us = 0;
    std::uint64_t audio_delay_samples = 0;
    std::uint64_t jitter_min_us = 0;
    std::uint64_t jitter_sum_us = 0;
    std::uint64_t jitter_max_us = 0;
    std::uint64_t jitter_samples = 0;
    std::uint64_t rtt_min_us = 0;
    std::uint64_t rtt_sum_us = 0;
    std::uint64_t rtt_max_us = 0;
    bool drift_valid = false;
    double raw_drift_ppm = 0.0;
    double drift_ppm = 0.0;
    double resampler_ratio = 1.0;
    std::uint64_t metronome_sent = 0;
    std::uint64_t metronome_received = 0;
    std::uint64_t last_remote_beat = 0;
    std::uint64_t elapsed_ms = 0;
    bool final_metronome_enabled = false;
    int final_bpm = 120;
};

struct RuntimeState {
    std::atomic<bool> quit{false};
    std::atomic<bool> print_stats{false};
    std::atomic<bool> metronome{false};
    std::atomic<int> bpm{120};
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

void sync_audio_control(
    const RuntimeState& runtime,
    jam2::audio::StreamControl* control,
    double metronome_level,
    double playback_ratio)
{
    if (control == nullptr) {
        return;
    }
    control->metronome_enabled.store(runtime.metronome.load(std::memory_order_relaxed), std::memory_order_relaxed);
    control->metronome_bpm.store(runtime.bpm.load(std::memory_order_relaxed), std::memory_order_relaxed);
    control->metronome_level_ppm.store(ppm_from_unit(metronome_level), std::memory_order_relaxed);
    control->playback_ratio_ppm.store(ratio_to_ppm(playback_ratio), std::memory_order_relaxed);
}

void print_interactive_help()
{
    std::cout << "Commands:\n"
              << "  help          show this command list\n"
              << "  stats         print current stream stats\n"
              << "  metro on      enable local metronome\n"
              << "  metro off     disable local metronome\n"
              << "  bpm <1..400>  set metronome tempo\n"
              << "  quit          stop the stream and exit\n"
              << "  exit          stop the stream and exit\n";
}

void print_prompt()
{
    std::cout << "> ";
    std::cout.flush();
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
            print_interactive_help();
            print_prompt();
            continue;
        }
        if (command == "stats") {
            state.print_stats.store(true, std::memory_order_relaxed);
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
            } else {
                std::cerr << "unknown metro command; use: metro on|off\n";
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

std::array<std::uint8_t, 12> encode_metronome_payload(int bpm, std::uint64_t beat)
{
    std::array<std::uint8_t, 12> payload{};
    payload[0] = static_cast<std::uint8_t>(bpm & 0xff);
    payload[1] = static_cast<std::uint8_t>((bpm >> 8) & 0xff);
    payload[2] = static_cast<std::uint8_t>((bpm >> 16) & 0xff);
    payload[3] = static_cast<std::uint8_t>((bpm >> 24) & 0xff);
    for (int i = 0; i < 8; ++i) {
        payload[4 + i] = static_cast<std::uint8_t>((beat >> (i * 8)) & 0xffU);
    }
    return payload;
}

std::pair<int, std::uint64_t> decode_metronome_payload(std::span<const std::uint8_t> payload)
{
    if (payload.size() != 12) {
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
    return {bpm, beat};
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

void print_periodic_stream_stats(const AudioPacketStats& stats, std::uint64_t elapsed_ms)
{
    const double delay_avg_ms = stats.audio_delay_samples > 0 ?
        (static_cast<double>(stats.audio_delay_sum_us) / static_cast<double>(stats.audio_delay_samples) / 1000.0) :
        0.0;
    const double rtt_avg_ms = stats.recv_pongs > 0 ?
        (static_cast<double>(stats.rtt_sum_us) / static_cast<double>(stats.recv_pongs) / 1000.0) :
        0.0;
    const std::uint64_t estimated_missing = stats.sent_packets > stats.recv_packets ? stats.sent_packets - stats.recv_packets : 0;
    const double loss_percent = stats.sent_packets > 0 ?
        (static_cast<double>(estimated_missing) * 100.0 / static_cast<double>(stats.sent_packets)) :
        0.0;
    std::cout << "stats elapsed_ms=" << elapsed_ms
              << " sent=" << stats.sent_packets
              << " recv=" << stats.recv_packets
              << " missing_now_percent=" << loss_percent
              << " delay_avg_ms=" << delay_avg_ms
              << " rtt_avg_ms=" << rtt_avg_ms
              << "\n";
}

AudioPacketStats run_audio_packet_exchange(
    jam2::UdpSocket& socket,
    const jam2::SessionInfo& session,
    const jam2::Endpoint& peer,
    const Options& options,
    RuntimeState& runtime,
    jam2::audio::StreamControl* audio_control,
    jam2::audio::MonoRingBuffer* capture_ring,
    jam2::audio::MonoRingBuffer* playback_ring)
{
    AudioPacketStats stats;
    const bool bounded_stream = options.stream_ms > 0;

    std::vector<std::int32_t> asio_frames(static_cast<std::size_t>(options.frame_size), 0);
    std::vector<std::int32_t> network_frames(static_cast<std::size_t>(options.frame_size), 0);
    const auto silence_payload = jam2::protocol::pack_pcm24(network_frames);
    const std::uint16_t audio_payload_size = static_cast<std::uint16_t>(silence_payload.size());
    jam2::protocol::SequenceTracker tracker;
    std::uint32_t audio_sequence = 1;
    std::uint32_t control_sequence = 1;
    std::uint64_t sample_time = 0;
    const std::uint64_t interval_us =
        (static_cast<std::uint64_t>(options.frame_size) * 1000000ULL) / static_cast<std::uint64_t>(options.sample_rate);
    std::uint64_t next_send = jam2::monotonic_us();
    std::uint64_t next_ping = next_send;
    std::uint64_t next_metronome = next_send;
    std::uint64_t local_beat = 0;
    std::uint64_t last_metronome_revision = runtime.metronome_revision.load(std::memory_order_relaxed);
    std::uint64_t last_audio_receive_us = 0;
    bool drift_started = false;
    bool drift_smoothed = false;
    double smoothed_drift_ppm = 0.0;
    std::uint64_t first_remote_sample_time = 0;
    std::uint64_t first_receive_time_us = 0;
    const std::uint64_t start_time = next_send;
    std::uint64_t next_stats = options.stats_interval_ms > 0 ?
        start_time + static_cast<std::uint64_t>(options.stats_interval_ms) * 1000ULL :
        0;
    const std::uint64_t send_deadline = bounded_stream ?
        next_send + static_cast<std::uint64_t>(options.stream_ms) * 1000ULL :
        UINT64_MAX;
    const std::uint64_t receive_deadline = bounded_stream ?
        send_deadline + static_cast<std::uint64_t>(options.stream_linger_ms) * 1000ULL :
        UINT64_MAX;

    while (jam2::monotonic_us() < receive_deadline && !stats.received_bye && !runtime.quit.load(std::memory_order_relaxed)) {
        const std::uint64_t now = jam2::monotonic_us();
        sync_audio_control(runtime, audio_control, options.metronome_level, stats.resampler_ratio);
        int sends_this_loop = 0;
        while (now >= next_send && next_send < send_deadline && sends_this_loop < 8) {
            std::vector<std::uint8_t> payload;
            if (capture_ring != nullptr) {
                (void)capture_ring->pop(asio_frames);
                for (std::size_t i = 0; i < asio_frames.size(); ++i) {
                    network_frames[i] = asio_frames[i] / 256;
                }
                payload = jam2::protocol::pack_pcm24(network_frames);
            } else {
                payload = silence_payload;
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
            socket.send_to(peer, packet);
            ++stats.sent_packets;
            stats.sent_bytes += packet.size();
            sample_time += static_cast<std::uint64_t>(options.frame_size);
            next_send += interval_us == 0 ? 1 : interval_us;
            ++sends_this_loop;
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
            ++stats.sent_pings;
            next_ping += 100000ULL;
        }
        const bool metronome_enabled = runtime.metronome.load(std::memory_order_relaxed);
        const std::uint64_t metronome_revision = runtime.metronome_revision.load(std::memory_order_relaxed);
        if (((metronome_enabled && now >= next_metronome) || metronome_revision != last_metronome_revision) &&
            now < send_deadline) {
            const int current_bpm = runtime.bpm.load(std::memory_order_relaxed);
            const auto metro_payload = encode_metronome_payload(metronome_enabled ? current_bpm : -current_bpm, local_beat++);
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
            ++stats.metronome_sent;
            last_metronome_revision = metronome_revision;
            if (metronome_enabled) {
                const std::uint64_t beat_interval_us = 60000000ULL / static_cast<std::uint64_t>(current_bpm);
                next_metronome += beat_interval_us == 0 ? 1 : beat_interval_us;
            } else {
                next_metronome = now + 1000000ULL;
            }
        }
        if (runtime.print_stats.exchange(false, std::memory_order_relaxed)) {
            print_periodic_stream_stats(stats, (now - start_time) / 1000ULL);
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
            print_periodic_stream_stats(stats, (now - start_time) / 1000ULL);
            next_stats += static_cast<std::uint64_t>(options.stats_interval_ms) * 1000ULL;
        }

        bool received_any = false;
        for (;;) {
            const auto received = socket.recv_from(received_any ? 0 : 1);
            if (!received) {
                break;
            }
            received_any = true;
            const auto& [from, bytes] = *received;
            if (from.host != peer.host || from.port != peer.port) {
                ++stats.ignored_packets;
                continue;
            }
            try {
                const auto header = jam2::protocol::decode_packet(bytes, session.key, session.session_id);
                if (header.type == jam2::protocol::PacketType::Audio) {
                    if (header.payload_length != audio_payload_size) {
                        ++stats.ignored_packets;
                        continue;
                    }
                    const auto sequence_result = tracker.observe(header.sequence);
                    ++stats.recv_packets;
                    stats.recv_bytes += bytes.size();
                    if (sequence_result != jam2::protocol::SequenceResult::InOrder) {
                        ++stats.ignored_packets;
                        continue;
                    }
                    if (playback_ring != nullptr) {
                        const auto received_payload = std::span<const std::uint8_t>(
                            bytes.data() + jam2::protocol::kHeaderSize,
                            header.payload_length);
                        auto decoded = jam2::protocol::unpack_pcm24(received_payload);
                        for (auto& sample : decoded) {
                            sample *= 256;
                        }
                        (void)playback_ring->push(decoded);
                        if (options.playback_max_frames > 0) {
                            const std::size_t depth = playback_ring->available_read();
                            if (depth > options.playback_max_frames) {
                                stats.playback_dropped_frames +=
                                    playback_ring->drop_oldest(depth - options.playback_max_frames);
                            }
                        }
                        const std::uint64_t depth_after = playback_ring->available_read();
                        observe_timing(
                            depth_after,
                            stats.playback_depth_min_frames,
                            stats.playback_depth_sum_frames,
                            stats.playback_depth_max_frames);
                        ++stats.playback_depth_samples;
                    }
                    const std::uint64_t receive_time = jam2::monotonic_us();
                    if (!drift_started) {
                        drift_started = true;
                        first_remote_sample_time = header.sample_time;
                        first_receive_time_us = receive_time;
                    } else if (receive_time > first_receive_time_us && header.sample_time > first_remote_sample_time) {
                        const double remote_elapsed_samples = static_cast<double>(header.sample_time - first_remote_sample_time);
                        const double remote_elapsed_us = remote_elapsed_samples * 1000000.0 / static_cast<double>(options.sample_rate);
                        const double local_elapsed_us = static_cast<double>(receive_time - first_receive_time_us);
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
                        stats.resampler_ratio = options.drift_correction ?
                            std::clamp(raw_ratio, 1.0 - max_ratio_delta, 1.0 + max_ratio_delta) :
                            1.0;
                        sync_audio_control(runtime, audio_control, options.metronome_level, stats.resampler_ratio);
                        stats.drift_valid = true;
                    }
                    if (last_audio_receive_us != 0) {
                        const std::uint64_t interval =
                            receive_time >= last_audio_receive_us ? receive_time - last_audio_receive_us : 0;
                        const std::uint64_t jitter =
                            interval > interval_us ? interval - interval_us : interval_us - interval;
                        observe_timing(jitter, stats.jitter_min_us, stats.jitter_sum_us, stats.jitter_max_us);
                        ++stats.jitter_samples;
                    }
                    last_audio_receive_us = receive_time;
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
                    ++stats.sent_pongs;
                } else if (header.type == jam2::protocol::PacketType::Pong) {
                    const std::uint64_t receive_time = jam2::monotonic_us();
                    if (receive_time >= header.send_time_us) {
                        observe_timing(
                            receive_time - header.send_time_us,
                            stats.rtt_min_us,
                            stats.rtt_sum_us,
                            stats.rtt_max_us);
                    }
                    ++stats.recv_pongs;
                } else if (header.type == jam2::protocol::PacketType::Bye) {
                    stats.received_bye = true;
                    break;
                } else if (header.type == jam2::protocol::PacketType::MetronomeState) {
                    const auto payload = std::span<const std::uint8_t>(
                        bytes.data() + jam2::protocol::kHeaderSize,
                        header.payload_length);
                    const auto [remote_bpm, remote_beat] = decode_metronome_payload(payload);
                    if (runtime.metronome_revision.load(std::memory_order_relaxed) == 0) {
                        if (remote_bpm < 0 && remote_bpm >= -400) {
                            runtime.metronome.store(false, std::memory_order_relaxed);
                            runtime.bpm.store(-remote_bpm, std::memory_order_relaxed);
                        } else if (remote_bpm > 0 && remote_bpm <= 400) {
                            runtime.metronome.store(true, std::memory_order_relaxed);
                            runtime.bpm.store(remote_bpm, std::memory_order_relaxed);
                        }
                    }
                    stats.last_remote_beat = remote_beat;
                    ++stats.metronome_received;
                } else {
                    ++stats.ignored_packets;
                }
            } catch (const std::exception&) {
                ++stats.ignored_packets;
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

    stats.sequence = tracker.stats();
    stats.elapsed_ms = (jam2::monotonic_us() - start_time) / 1000ULL;
    stats.final_metronome_enabled = runtime.metronome.load(std::memory_order_relaxed);
    stats.final_bpm = runtime.bpm.load(std::memory_order_relaxed);
    return stats;
}

double frames_to_ms(std::size_t frames, int sample_rate)
{
    return sample_rate > 0 ? (static_cast<double>(frames) * 1000.0 / static_cast<double>(sample_rate)) : 0.0;
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
    const std::uint64_t loss_denominator = stats.recv_packets + stats.sequence.lost;
    const double loss_percent = loss_denominator > 0 ?
        (static_cast<double>(stats.sequence.lost) * 100.0 / static_cast<double>(loss_denominator)) :
        0.0;
    std::cout << "Stream duration ms: " << stream_ms << "\n";
    std::cout << "Sample rate: " << options.sample_rate << "\n";
    std::cout << "Frame size samples: " << options.frame_size << "\n";
    std::cout << "Frame interval ms: " << frame_interval_ms << "\n";
    std::cout << "Drift correction: " << (options.drift_correction ? "on" : "off") << "\n";
    std::cout << "Drift smoothing alpha: " << options.drift_smoothing << "\n";
    std::cout << "Drift max correction ppm: " << options.drift_max_correction_ppm << "\n";
    std::cout << "Metronome: " << (options.metronome ? "on" : "off") << "\n";
    std::cout << "BPM: " << options.bpm << "\n";
    std::cout << "Final metronome: " << (stats.final_metronome_enabled ? "on" : "off") << "\n";
    std::cout << "Final BPM: " << stats.final_bpm << "\n";
    if (options.stream_ms > 0) {
        std::cout << "Expected audio packets: " << expected_packets << "\n";
    }
    std::cout << "Audio packets sent: " << stats.sent_packets << "\n";
    std::cout << "Audio packets received: " << stats.recv_packets << "\n";
    std::cout << "Audio send packet rate pps: " << sent_rate << "\n";
    std::cout << "Audio receive packet rate pps: " << recv_rate << "\n";
    std::cout << "Send bitrate bps: " << send_bitrate << "\n";
    std::cout << "Send bitrate kbps: " << (send_bitrate / 1000.0) << "\n";
    std::cout << "Receive bitrate bps: " << recv_bitrate << "\n";
    std::cout << "Receive bitrate kbps: " << (recv_bitrate / 1000.0) << "\n";
    std::cout << "Ignored audio packets: " << stats.ignored_packets << "\n";
    if (stats.playback_depth_samples > 0) {
        std::cout << "Playback dropped frames: " << stats.playback_dropped_frames << "\n";
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
    if (stats.metronome_received > 0) {
        std::cout << "Metronome last remote beat: " << stats.last_remote_beat << "\n";
    }
    if (stats.recv_pongs > 0) {
        std::cout << "RTT ms min: " << (static_cast<double>(stats.rtt_min_us) / 1000.0) << "\n";
        std::cout << "RTT ms avg: "
                  << (static_cast<double>(stats.rtt_sum_us) / static_cast<double>(stats.recv_pongs) / 1000.0) << "\n";
        std::cout << "RTT ms max: " << (static_cast<double>(stats.rtt_max_us) / 1000.0) << "\n";
    }
    std::cout << "Sequence lost: " << stats.sequence.lost << "\n";
    std::cout << "Sequence loss percent: " << loss_percent << "\n";
    std::cout << "Sequence duplicate: " << stats.sequence.duplicate << "\n";
    std::cout << "Sequence out_of_order: " << stats.sequence.out_of_order << "\n";
    std::cout << "Sequence late: " << stats.sequence.late << "\n";
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

OptionalAudioStream start_optional_audio(const Options& options)
{
    OptionalAudioStream audio;
    if (!options.audio_device_id) {
        return audio;
    }
    audio.control = std::make_unique<jam2::audio::StreamControl>();
    audio.control->metronome_enabled.store(options.metronome, std::memory_order_relaxed);
    audio.control->metronome_bpm.store(options.bpm, std::memory_order_relaxed);
    audio.control->metronome_level_ppm.store(ppm_from_unit(options.metronome_level), std::memory_order_relaxed);
    audio.control->playback_ratio_ppm.store(1000000, std::memory_order_relaxed);
    audio.capture_ring = std::make_unique<jam2::audio::MonoRingBuffer>(options.capture_ring_frames);
    audio.playback_ring = std::make_unique<jam2::audio::MonoRingBuffer>(options.playback_ring_frames);
    audio.stream = jam2::audio::start_duplex_stream(
        *options.audio_device_id,
        static_cast<double>(options.sample_rate),
        options.audio_buffer_size,
        options.input_channels,
        *audio.capture_ring,
        *audio.playback_ring,
        options.playback_prefill_frames,
        *audio.control);
    return audio;
}

struct CommandThread {
    RuntimeState state;
    std::thread thread;

    explicit CommandThread(const Options& options)
    {
        state.metronome.store(options.metronome, std::memory_order_relaxed);
        state.bpm.store(options.bpm, std::memory_order_relaxed);
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
    const std::size_t capture_readable = audio.capture_ring->available_read();
    const std::size_t playback_readable = audio.playback_ring->available_read();
    std::cout << "Audio callbacks: " << audio.stream->callbacks() << "\n";
    std::cout << "Input channels: "
              << (options.input_channels == jam2::audio::InputChannels::Stereo ? "stereo" : "mono") << "\n";
    std::cout << "Output channels: duplicated mono to channels 1/2 when available\n";
    std::cout << "Playback prefilled: " << (audio.stream->playback_prefilled() ? "yes" : "no") << "\n";
    std::cout << "Playback prefill frames: " << options.playback_prefill_frames << "\n";
    std::cout << "Playback prefill ms: " << frames_to_ms(options.playback_prefill_frames, options.sample_rate) << "\n";
    std::cout << "Capture ring overruns frames: " << capture_stats.overruns << "\n";
    std::cout << "Capture ring underruns frames: " << capture_stats.underruns << "\n";
    std::cout << "Capture ring readable frames: " << capture_readable << "\n";
    std::cout << "Capture ring readable ms: " << frames_to_ms(capture_readable, options.sample_rate) << "\n";
    std::cout << "Playback ring overruns frames: " << playback_stats.overruns << "\n";
    std::cout << "Playback ring underruns frames: " << playback_stats.underruns << "\n";
    std::cout << "Playback ring readable frames: " << playback_readable << "\n";
    std::cout << "Playback ring readable ms: " << frames_to_ms(playback_readable, options.sample_rate) << "\n";
    if (audio.control) {
        std::cout << "Audio control metronome: "
                  << (audio.control->metronome_enabled.load(std::memory_order_relaxed) ? "on" : "off") << "\n";
        std::cout << "Audio control BPM: " << audio.control->metronome_bpm.load(std::memory_order_relaxed) << "\n";
        std::cout << "Audio control resampler ratio: "
                  << (static_cast<double>(audio.control->playback_ratio_ppm.load(std::memory_order_relaxed)) / 1000000.0) << "\n";
    }
}

int run_probe_device(int argc, char** argv)
{
    if (argc < 3) {
        throw std::runtime_error("probe-device requires a device id");
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

    jam2::SessionInfo session{advertised, jam2::random_u64(), jam2::random_key()};
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
    std::cout << "Connection string:\n" << jam2::make_jam_url(session) << "\n\n";
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
        auto audio = start_optional_audio(options);
        CommandThread commands(options);
        const auto audio_stats = run_audio_packet_exchange(
            socket,
            session,
            from,
            options,
            commands.state,
            audio.control.get(),
            audio.capture_ring.get(),
            audio.playback_ring.get());
            print_audio_packet_stats(audio_stats, options);
            print_optional_audio_stats(audio, options);
            std::cout.flush();
            return 0;
    }
    std::cerr << "Timed out waiting for authenticated peer\n";
    std::cerr << "Ignored malformed/auth/session packets: " << ignored_malformed << "\n";
    std::cerr << "Ignored wrong-endpoint packets: " << ignored_wrong_endpoint << "\n";
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
                auto audio = start_optional_audio(options);
                CommandThread commands(options);
                const auto audio_stats = run_audio_packet_exchange(
                    socket,
                    session,
                    session.endpoint,
                    options,
                    commands.state,
                    audio.control.get(),
                    audio.capture_ring.get(),
                    audio.playback_ring.get());
                print_audio_packet_stats(audio_stats, options);
                print_optional_audio_stats(audio, options);
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

    if (command == "--list-devices") {
        const auto devices = jam2::audio::list_devices();
        if (devices.empty()) {
            std::cout << "No audio devices found for this MVP backend.\n";
            return 0;
        }
        for (const auto& device : devices) {
            std::cout << "[" << device.id << "] " << device.backend << " " << device.name << "\n";
            if (!device.clsid.empty()) {
                std::cout << "    clsid: " << device.clsid << "\n";
            }
            if (!device.driver_path.empty()) {
                std::cout << "    path: " << device.driver_path << "\n";
            }
        }
        return 0;
    }

    if (command == "probe-device") {
        return run_probe_device(argc, argv);
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

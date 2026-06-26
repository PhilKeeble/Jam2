#include <exception>
#include <chrono>
#include <iostream>
#include <memory>
#include <span>
#include <string_view>
#include <thread>

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
  jam2 listen [--bind ip:port] [--stun host:port] [--no-stun] [--public-endpoint ip:port] [--wait-ms n] [--stream-ms n] [--playback-prefill-frames n]
  jam2 connect <jam2-url> [options]

Stage status:
  UDP HELLO/HELLO_ACK session setup, jam2 URL parsing, and STUN endpoint discovery are implemented.
  Audio streaming, device backends, stats, and metronome are still planned.
)";

struct Options {
    jam2::Endpoint bind{"0.0.0.0", 49000};
    jam2::Endpoint stun_server{"stun.l.google.com", jam2::stun::kDefaultPort};
    std::optional<jam2::Endpoint> public_endpoint;
    bool no_stun = false;
    int stun_timeout_ms = 1000;
    int stun_retries = 3;
    int wait_ms = 30000;
    int stream_ms = 0;
    int sample_rate = 48000;
    int frame_size = 128;
    std::optional<int> audio_device_id;
    long audio_buffer_size = 0;
    std::size_t capture_ring_frames = 4096;
    std::size_t playback_ring_frames = 4096;
    std::size_t playback_prefill_frames = 0;
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
            if (options.wait_ms <= 0) {
                throw std::runtime_error("--wait-ms must be positive");
            }
        } else if (arg == "--stream-ms") {
            options.stream_ms = std::stoi(std::string(require_value(argc, argv, i, arg)));
            if (options.stream_ms < 0) {
                throw std::runtime_error("--stream-ms must be non-negative");
            }
        } else if (arg == "--sample-rate") {
            options.sample_rate = std::stoi(std::string(require_value(argc, argv, i, arg)));
            if (options.sample_rate <= 0) {
                throw std::runtime_error("--sample-rate must be positive");
            }
        } else if (arg == "--frame-size") {
            options.frame_size = std::stoi(std::string(require_value(argc, argv, i, arg)));
            if (options.frame_size != 32 && options.frame_size != 64 && options.frame_size != 128 && options.frame_size != 256) {
                throw std::runtime_error("--frame-size must be 32, 64, 128, or 256");
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
        } else if (arg == "--device") {
            (void)require_value(argc, argv, i, arg);
        } else {
            throw std::runtime_error("unknown option: " + std::string(arg));
        }
    }
    return options;
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

struct AudioPacketStats {
    std::uint64_t sent_packets = 0;
    std::uint64_t sent_bytes = 0;
    std::uint64_t recv_packets = 0;
    std::uint64_t recv_bytes = 0;
    std::uint64_t ignored_packets = 0;
    std::uint64_t sent_pings = 0;
    std::uint64_t recv_pongs = 0;
    std::uint64_t sent_pongs = 0;
    jam2::protocol::SequenceStats sequence;
    std::uint64_t audio_delay_min_us = 0;
    std::uint64_t audio_delay_sum_us = 0;
    std::uint64_t audio_delay_max_us = 0;
    std::uint64_t jitter_min_us = 0;
    std::uint64_t jitter_sum_us = 0;
    std::uint64_t jitter_max_us = 0;
    std::uint64_t jitter_samples = 0;
    std::uint64_t rtt_min_us = 0;
    std::uint64_t rtt_sum_us = 0;
    std::uint64_t rtt_max_us = 0;
};

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

AudioPacketStats run_audio_packet_exchange(
    jam2::UdpSocket& socket,
    const jam2::SessionInfo& session,
    const jam2::Endpoint& peer,
    const Options& options,
    jam2::audio::MonoRingBuffer* capture_ring,
    jam2::audio::MonoRingBuffer* playback_ring)
{
    AudioPacketStats stats;
    if (options.stream_ms <= 0) {
        return stats;
    }

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
    std::uint64_t last_audio_receive_us = 0;
    const std::uint64_t deadline = next_send + static_cast<std::uint64_t>(options.stream_ms) * 1000ULL;

    while (jam2::monotonic_us() < deadline) {
        const std::uint64_t now = jam2::monotonic_us();
        if (now >= next_send) {
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
        }
        if (now >= next_ping) {
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
                    (void)tracker.observe(header.sequence);
                    ++stats.recv_packets;
                    stats.recv_bytes += bytes.size();
                    if (playback_ring != nullptr) {
                        const auto received_payload = std::span<const std::uint8_t>(
                            bytes.data() + jam2::protocol::kHeaderSize,
                            header.payload_length);
                        auto decoded = jam2::protocol::unpack_pcm24(received_payload);
                        for (auto& sample : decoded) {
                            sample *= 256;
                        }
                        (void)playback_ring->push(decoded);
                    }
                    const std::uint64_t receive_time = jam2::monotonic_us();
                    if (last_audio_receive_us != 0) {
                        const std::uint64_t interval =
                            receive_time >= last_audio_receive_us ? receive_time - last_audio_receive_us : 0;
                        const std::uint64_t jitter =
                            interval > interval_us ? interval - interval_us : interval_us - interval;
                        observe_timing(jitter, stats.jitter_min_us, stats.jitter_sum_us, stats.jitter_max_us);
                        ++stats.jitter_samples;
                    }
                    last_audio_receive_us = receive_time;
                    if (receive_time >= header.send_time_us) {
                        observe_timing(
                            receive_time - header.send_time_us,
                            stats.audio_delay_min_us,
                            stats.audio_delay_sum_us,
                            stats.audio_delay_max_us);
                    }
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

    stats.sequence = tracker.stats();
    return stats;
}

void print_audio_packet_stats(const AudioPacketStats& stats, int stream_ms)
{
    if (stream_ms <= 0) {
        return;
    }
    const double seconds = static_cast<double>(stream_ms) / 1000.0;
    const double send_bitrate = seconds > 0.0 ? (static_cast<double>(stats.sent_bytes) * 8.0 / seconds) : 0.0;
    const double recv_bitrate = seconds > 0.0 ? (static_cast<double>(stats.recv_bytes) * 8.0 / seconds) : 0.0;
    std::cout << "Audio packets sent: " << stats.sent_packets << "\n";
    std::cout << "Audio packets received: " << stats.recv_packets << "\n";
    std::cout << "Send bitrate bps: " << send_bitrate << "\n";
    std::cout << "Receive bitrate bps: " << recv_bitrate << "\n";
    std::cout << "Ignored audio packets: " << stats.ignored_packets << "\n";
    if (stats.recv_packets > 0) {
        std::cout << "Audio receive delay ms min: " << (static_cast<double>(stats.audio_delay_min_us) / 1000.0) << "\n";
        std::cout << "Audio receive delay ms avg: "
                  << (static_cast<double>(stats.audio_delay_sum_us) / static_cast<double>(stats.recv_packets) / 1000.0) << "\n";
        std::cout << "Audio receive delay ms max: " << (static_cast<double>(stats.audio_delay_max_us) / 1000.0) << "\n";
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
    if (stats.recv_pongs > 0) {
        std::cout << "RTT ms min: " << (static_cast<double>(stats.rtt_min_us) / 1000.0) << "\n";
        std::cout << "RTT ms avg: "
                  << (static_cast<double>(stats.rtt_sum_us) / static_cast<double>(stats.recv_pongs) / 1000.0) << "\n";
        std::cout << "RTT ms max: " << (static_cast<double>(stats.rtt_max_us) / 1000.0) << "\n";
    }
    std::cout << "Sequence lost: " << stats.sequence.lost << "\n";
    std::cout << "Sequence duplicate: " << stats.sequence.duplicate << "\n";
    std::cout << "Sequence out_of_order: " << stats.sequence.out_of_order << "\n";
    std::cout << "Sequence late: " << stats.sequence.late << "\n";
}

struct OptionalAudioStream {
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
    audio.capture_ring = std::make_unique<jam2::audio::MonoRingBuffer>(options.capture_ring_frames);
    audio.playback_ring = std::make_unique<jam2::audio::MonoRingBuffer>(options.playback_ring_frames);
    audio.stream = jam2::audio::start_duplex_stream(
        *options.audio_device_id,
        static_cast<double>(options.sample_rate),
        options.audio_buffer_size,
        *audio.capture_ring,
        *audio.playback_ring,
        options.playback_prefill_frames);
    return audio;
}

double frames_to_ms(std::size_t frames, int sample_rate)
{
    return sample_rate > 0 ? (static_cast<double>(frames) * 1000.0 / static_cast<double>(sample_rate)) : 0.0;
}

void print_optional_audio_stats(const OptionalAudioStream& audio, const Options& options)
{
    if (!audio.stream || !audio.capture_ring || !audio.playback_ring) {
        return;
    }
    const auto capture_stats = audio.capture_ring->stats();
    const auto playback_stats = audio.playback_ring->stats();
    const std::size_t capture_readable = audio.capture_ring->available_read();
    const std::size_t playback_readable = audio.playback_ring->available_read();
    std::cout << "ASIO callbacks: " << audio.stream->callbacks() << "\n";
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
    const auto deadline = jam2::monotonic_us() + static_cast<std::uint64_t>(options.wait_ms) * 1000ULL;
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
        const auto audio_stats = run_audio_packet_exchange(
            socket,
            session,
            from,
            options,
            audio.capture_ring.get(),
            audio.playback_ring.get());
            print_audio_packet_stats(audio_stats, options.stream_ms);
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
    socket.bind({"0.0.0.0", 0});

    std::cout << "Mode: connect\n";
    std::cout << "Local UDP bind: " << jam2::endpoint_to_string(socket.local_endpoint()) << "\n";
    std::cout << "Peer endpoint: " << jam2::endpoint_to_string(session.endpoint) << "\n";

    const auto hello = make_control_packet(jam2::protocol::PacketType::Hello, session, 1);
    const auto deadline = jam2::monotonic_us() + static_cast<std::uint64_t>(options.wait_ms) * 1000ULL;
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
                const auto audio_stats = run_audio_packet_exchange(
                    socket,
                    session,
                    session.endpoint,
                    options,
                    audio.capture_ring.get(),
                    audio.playback_ring.get());
                print_audio_packet_stats(audio_stats, options.stream_ms);
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

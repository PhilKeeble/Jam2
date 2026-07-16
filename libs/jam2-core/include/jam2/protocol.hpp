#pragma once

#include "common.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace jam2 {

enum class NetworkAudioFormat : std::uint8_t {
    Pcm16Mono = 1,
    Pcm24Mono = 2,
};

namespace protocol {

constexpr std::uint8_t kProtocolVersion = 2;

enum class PacketType : std::uint8_t {
    Hello = 1,
    HelloAck = 2,
    Audio = 3,
    Ping = 4,
    Pong = 5,
    MetronomeState = 6,
    Bye = 7,
    TransportState = 8,
};

struct Header {
    PacketType type{};
    std::uint64_t session_id{};
    std::uint32_t sequence{};
    // Audio/metronome/transport packets carry a sample-time value. Ping/pong
    // packets carry their monotonic timing token. No packet needs both.
    std::uint64_t timing_value{};
    std::uint16_t payload_length{};
    std::uint64_t auth_tag{};
};

constexpr std::size_t kHeaderSize = 36;
constexpr std::size_t kMaxAudioFramesPerPacket = 256;
constexpr std::size_t kMaxDatagramSize = kHeaderSize + kMaxAudioFramesPerPacket * 3;

enum class ParseError : std::uint8_t {
    None = 0,
    ShortPacket,
    WrongMagic,
    WrongVersion,
    UnknownType,
    WrongSession,
    InvalidPayloadSize,
    AuthenticationFailed,
};

struct ParseResult {
    Header header{};
    ParseError error = ParseError::None;

    explicit operator bool() const { return error == ParseError::None; }
};

// RFC 1982-style serial arithmetic. Comparisons are defined for distances
// smaller than half the uint32 sequence space; the exact half-range is treated
// as unordered/invalid by both helpers.
constexpr bool sequence_after(std::uint32_t candidate, std::uint32_t reference)
{
    const std::uint32_t distance = candidate - reference;
    return distance != 0 && distance < 0x80000000U;
}

constexpr bool sequence_before(std::uint32_t candidate, std::uint32_t reference)
{
    return sequence_after(reference, candidate);
}

constexpr std::uint32_t sequence_forward_distance(std::uint32_t later, std::uint32_t earlier)
{
    return later - earlier;
}

struct SequenceStats {
    std::uint64_t lost = 0;
    std::uint64_t loss_events = 0;
    std::uint64_t loss_max_gap = 0;
    std::uint64_t duplicate = 0;
    std::uint64_t out_of_order = 0;
    std::uint64_t late = 0;
};

enum class ReplayResult {
    New,
    Duplicate,
    TooOld,
    Ambiguous,
};

class ReplayWindow {
public:
    ReplayResult observe(std::uint32_t sequence);
    void reset();

private:
    bool initialized_ = false;
    std::uint32_t highest_ = 0;
    std::uint64_t bitmap_ = 0;
};

std::vector<std::uint8_t> encode_packet(
    const Header& header,
    std::span<const std::uint8_t> payload,
    const std::array<std::uint8_t, 16>& key,
    NetworkAudioFormat audio_format);

std::size_t encode_packet_into(
    const Header& header,
    std::span<const std::uint8_t> payload,
    const std::array<std::uint8_t, 16>& key,
    NetworkAudioFormat audio_format,
    std::span<std::uint8_t> output);

ParseResult parse_packet(
    std::span<const std::uint8_t> packet,
    const std::array<std::uint8_t, 16>& key,
    std::uint64_t expected_session_id,
    NetworkAudioFormat audio_format);

const char* parse_error_text(ParseError error);
const char* audio_format_text(NetworkAudioFormat format) noexcept;
std::optional<NetworkAudioFormat> parse_audio_format(std::string_view text) noexcept;
std::size_t audio_bytes_per_sample(NetworkAudioFormat format) noexcept;
std::size_t audio_payload_size(NetworkAudioFormat format, std::size_t frames) noexcept;

std::vector<std::uint8_t> pack_pcm16(std::span<const std::int32_t> samples);
std::vector<std::int32_t> unpack_pcm16(std::span<const std::uint8_t> bytes);
bool pack_pcm16_into(std::span<const std::int32_t> samples, std::span<std::uint8_t> output) noexcept;
bool unpack_pcm16_into(std::span<const std::uint8_t> bytes, std::span<std::int32_t> output) noexcept;
std::vector<std::uint8_t> pack_pcm24(std::span<const std::int32_t> samples);
std::vector<std::int32_t> unpack_pcm24(std::span<const std::uint8_t> bytes);
bool pack_pcm24_into(std::span<const std::int32_t> samples, std::span<std::uint8_t> output) noexcept;
bool unpack_pcm24_into(std::span<const std::uint8_t> bytes, std::span<std::int32_t> output) noexcept;
bool pack_audio_into(
    NetworkAudioFormat format,
    std::span<const std::int32_t> samples,
    std::span<std::uint8_t> output) noexcept;
bool unpack_audio_into(
    NetworkAudioFormat format,
    std::span<const std::uint8_t> bytes,
    std::span<std::int32_t> output) noexcept;

} // namespace protocol
} // namespace jam2

#pragma once

#include "common.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace jam2::protocol {

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
    std::uint16_t flags{};
    std::uint64_t session_id{};
    std::uint32_t sequence{};
    std::uint64_t sample_time{};
    std::uint64_t send_time_us{};
    std::uint16_t payload_length{};
    std::uint64_t auth_tag{};
};

constexpr std::size_t kHeaderSize = 48;
constexpr std::size_t kMaxAudioFramesPerPacket = 256;
constexpr std::size_t kMaxDatagramSize = kHeaderSize + kMaxAudioFramesPerPacket * 3;

enum class ParseError : std::uint8_t {
    None = 0,
    ShortPacket,
    WrongMagic,
    WrongVersion,
    UnknownType,
    InvalidFlags,
    InvalidReserved,
    WrongSession,
    InvalidPayloadSize,
    AuthenticationFailed,
};

struct ParseResult {
    Header header{};
    ParseError error = ParseError::None;

    explicit operator bool() const { return error == ParseError::None; }
};

enum class SequenceResult {
    InOrder,
    Duplicate,
    OutOfOrder,
    Late,
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

class SequenceTracker {
public:
    SequenceResult observe(std::uint32_t sequence);
    const SequenceStats& stats() const { return stats_; }

private:
    bool initialized_ = false;
    std::uint32_t highest_ = 0;
    std::uint64_t recent_window_ = 0;
    SequenceStats stats_{};
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
    const std::array<std::uint8_t, 16>& key);

std::size_t encode_packet_into(
    const Header& header,
    std::span<const std::uint8_t> payload,
    const std::array<std::uint8_t, 16>& key,
    std::span<std::uint8_t> output);

ParseResult parse_packet(
    std::span<const std::uint8_t> packet,
    const std::array<std::uint8_t, 16>& key,
    std::uint64_t expected_session_id);

const char* parse_error_text(ParseError error);

std::vector<std::uint8_t> pack_pcm24(std::span<const std::int32_t> samples);
std::vector<std::int32_t> unpack_pcm24(std::span<const std::uint8_t> bytes);
bool pack_pcm24_into(std::span<const std::int32_t> samples, std::span<std::uint8_t> output) noexcept;
bool unpack_pcm24_into(std::span<const std::uint8_t> bytes, std::span<std::int32_t> output) noexcept;

} // namespace jam2::protocol

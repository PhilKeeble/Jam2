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

enum class SequenceResult {
    InOrder,
    Duplicate,
    OutOfOrder,
    Late,
};

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

std::vector<std::uint8_t> encode_packet(
    const Header& header,
    std::span<const std::uint8_t> payload,
    const std::array<std::uint8_t, 16>& key);

Header decode_packet(
    std::span<const std::uint8_t> packet,
    const std::array<std::uint8_t, 16>& key,
    std::uint64_t expected_session_id);

std::vector<std::uint8_t> pack_pcm24(std::span<const std::int32_t> samples);
std::vector<std::int32_t> unpack_pcm24(std::span<const std::uint8_t> bytes);

} // namespace jam2::protocol

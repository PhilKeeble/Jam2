#include "protocol.hpp"

#include <array>
#include <cstring>
#include <stdexcept>

namespace jam2::protocol {
namespace {

constexpr std::uint32_t kMagic = 0x324d414aU; // JAM2 little-endian.
constexpr std::uint8_t kVersion = 1;

void put_u16(std::vector<std::uint8_t>& out, std::size_t offset, std::uint16_t value)
{
    out[offset] = static_cast<std::uint8_t>(value & 0xffU);
    out[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
}

void put_u32(std::vector<std::uint8_t>& out, std::size_t offset, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i) {
        out[offset + i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xffU);
    }
}

void put_u64(std::vector<std::uint8_t>& out, std::size_t offset, std::uint64_t value)
{
    for (int i = 0; i < 8; ++i) {
        out[offset + i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xffU);
    }
}

std::uint16_t read_u16(std::span<const std::uint8_t> in, std::size_t offset)
{
    return static_cast<std::uint16_t>(in[offset] | (static_cast<std::uint16_t>(in[offset + 1]) << 8));
}

std::uint32_t read_u32(std::span<const std::uint8_t> in, std::size_t offset)
{
    std::uint32_t value = 0;
    for (int i = 3; i >= 0; --i) {
        value = (value << 8) | in[offset + i];
    }
    return value;
}

std::uint64_t read_u64(std::span<const std::uint8_t> in, std::size_t offset)
{
    std::uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | in[offset + i];
    }
    return value;
}

std::uint64_t siphash24(std::span<const std::uint8_t> data, const std::array<std::uint8_t, 16>& key)
{
    auto rotl = [](std::uint64_t value, int bits) {
        return (value << bits) | (value >> (64 - bits));
    };
    auto read_key = [&](std::size_t offset) {
        std::uint64_t value = 0;
        for (int i = 7; i >= 0; --i) {
            value = (value << 8) | key[offset + i];
        }
        return value;
    };
    auto round = [&]() {};

    std::uint64_t v0 = 0x736f6d6570736575ULL ^ read_key(0);
    std::uint64_t v1 = 0x646f72616e646f6dULL ^ read_key(8);
    std::uint64_t v2 = 0x6c7967656e657261ULL ^ read_key(0);
    std::uint64_t v3 = 0x7465646279746573ULL ^ read_key(8);

    auto sip_round = [&]() {
        v0 += v1;
        v1 = rotl(v1, 13);
        v1 ^= v0;
        v0 = rotl(v0, 32);
        v2 += v3;
        v3 = rotl(v3, 16);
        v3 ^= v2;
        v0 += v3;
        v3 = rotl(v3, 21);
        v3 ^= v0;
        v2 += v1;
        v1 = rotl(v1, 17);
        v1 ^= v2;
        v2 = rotl(v2, 32);
    };
    (void)round;

    std::size_t offset = 0;
    while (offset + 8 <= data.size()) {
        const std::uint64_t m = read_u64(data, offset);
        v3 ^= m;
        sip_round();
        sip_round();
        v0 ^= m;
        offset += 8;
    }

    std::uint64_t b = static_cast<std::uint64_t>(data.size()) << 56;
    for (std::size_t i = 0; offset + i < data.size(); ++i) {
        b |= static_cast<std::uint64_t>(data[offset + i]) << (8 * i);
    }
    v3 ^= b;
    sip_round();
    sip_round();
    v0 ^= b;
    v2 ^= 0xff;
    sip_round();
    sip_round();
    sip_round();
    sip_round();
    return v0 ^ v1 ^ v2 ^ v3;
}

} // namespace

std::vector<std::uint8_t> encode_packet(
    const Header& header,
    std::span<const std::uint8_t> payload,
    const std::array<std::uint8_t, 16>& key)
{
    if (payload.size() > 65535) {
        throw std::runtime_error("payload too large");
    }
    std::vector<std::uint8_t> out(kHeaderSize + payload.size());
    put_u32(out, 0, kMagic);
    out[4] = kVersion;
    out[5] = static_cast<std::uint8_t>(header.type);
    put_u16(out, 6, header.flags);
    put_u64(out, 8, header.session_id);
    put_u32(out, 16, header.sequence);
    put_u64(out, 20, header.sample_time);
    put_u64(out, 28, header.send_time_us);
    put_u16(out, 36, static_cast<std::uint16_t>(payload.size()));
    put_u64(out, 38, 0);
    put_u16(out, 46, 0);
    std::memcpy(out.data() + kHeaderSize, payload.data(), payload.size());
    put_u64(out, 38, siphash24(out, key));
    return out;
}

Header decode_packet(
    std::span<const std::uint8_t> packet,
    const std::array<std::uint8_t, 16>& key,
    std::uint64_t expected_session_id)
{
    if (packet.size() < kHeaderSize) {
        throw std::runtime_error("packet shorter than header");
    }
    if (read_u32(packet, 0) != kMagic || packet[4] != kVersion) {
        throw std::runtime_error("wrong packet magic/version");
    }
    Header header;
    header.type = static_cast<PacketType>(packet[5]);
    header.flags = read_u16(packet, 6);
    header.session_id = read_u64(packet, 8);
    header.sequence = read_u32(packet, 16);
    header.sample_time = read_u64(packet, 20);
    header.send_time_us = read_u64(packet, 28);
    header.payload_length = read_u16(packet, 36);
    header.auth_tag = read_u64(packet, 38);
    if (header.session_id != expected_session_id) {
        throw std::runtime_error("wrong session id");
    }
    if (packet.size() != kHeaderSize + header.payload_length) {
        throw std::runtime_error("packet payload length mismatch");
    }
    std::vector<std::uint8_t> auth_copy(packet.begin(), packet.end());
    put_u64(auth_copy, 38, 0);
    if (siphash24(auth_copy, key) != header.auth_tag) {
        throw std::runtime_error("packet authentication failed");
    }
    return header;
}

std::vector<std::uint8_t> pack_pcm24(std::span<const std::int32_t> samples)
{
    std::vector<std::uint8_t> out(samples.size() * 3);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const std::int32_t clamped = samples[i] < -8388608 ? -8388608 : (samples[i] > 8388607 ? 8388607 : samples[i]);
        const std::uint32_t value = static_cast<std::uint32_t>(clamped) & 0x00ffffffU;
        out[i * 3] = static_cast<std::uint8_t>(value & 0xffU);
        out[i * 3 + 1] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
        out[i * 3 + 2] = static_cast<std::uint8_t>((value >> 16) & 0xffU);
    }
    return out;
}

std::vector<std::int32_t> unpack_pcm24(std::span<const std::uint8_t> bytes)
{
    if ((bytes.size() % 3) != 0) {
        throw std::runtime_error("24-bit PCM byte count must be divisible by 3");
    }
    std::vector<std::int32_t> out(bytes.size() / 3);
    for (std::size_t i = 0; i < out.size(); ++i) {
        std::uint32_t value = static_cast<std::uint32_t>(bytes[i * 3]) |
            (static_cast<std::uint32_t>(bytes[i * 3 + 1]) << 8) |
            (static_cast<std::uint32_t>(bytes[i * 3 + 2]) << 16);
        if ((value & 0x00800000U) != 0) {
            value |= 0xff000000U;
        }
        out[i] = static_cast<std::int32_t>(value);
    }
    return out;
}

SequenceResult SequenceTracker::observe(std::uint32_t sequence)
{
    if (!initialized_) {
        initialized_ = true;
        highest_ = sequence;
        recent_window_ = 1;
        return SequenceResult::InOrder;
    }

    if (sequence > highest_) {
        const std::uint32_t gap = sequence - highest_;
        if (gap > 1) {
            stats_.lost += gap - 1;
        }
        recent_window_ = gap >= 64 ? 1 : ((recent_window_ << gap) | 1U);
        highest_ = sequence;
        return SequenceResult::InOrder;
    }

    const std::uint32_t delta = highest_ - sequence;
    if (delta >= 64) {
        ++stats_.late;
        return SequenceResult::Late;
    }

    const std::uint64_t mask = 1ULL << delta;
    if ((recent_window_ & mask) != 0) {
        ++stats_.duplicate;
        return SequenceResult::Duplicate;
    }

    recent_window_ |= mask;
    ++stats_.out_of_order;
    return SequenceResult::OutOfOrder;
}

} // namespace jam2::protocol

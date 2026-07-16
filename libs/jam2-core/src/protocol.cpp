#include "protocol.hpp"

#include <array>
#include <cstring>
#include <stdexcept>

namespace jam2::protocol {
namespace {

constexpr std::uint32_t kMagic = 0x324d414aU; // JAM2 little-endian.
constexpr std::size_t kAuthTagOffset = 28;
constexpr std::size_t kAuthTagSize = 8;

void put_u16(std::span<std::uint8_t> out, std::size_t offset, std::uint16_t value)
{
    out[offset] = static_cast<std::uint8_t>(value & 0xffU);
    out[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
}

void put_u32(std::span<std::uint8_t> out, std::size_t offset, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i) {
        out[offset + i] = static_cast<std::uint8_t>((value >> (i * 8)) & 0xffU);
    }
}

void put_u64(std::span<std::uint8_t> out, std::size_t offset, std::uint64_t value)
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

std::uint64_t siphash24(
    std::span<const std::uint8_t> data,
    const std::array<std::uint8_t, 16>& key,
    bool zero_auth_tag = false)
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
    auto data_byte = [&](std::size_t index) {
        if (zero_auth_tag && index >= kAuthTagOffset && index < kAuthTagOffset + kAuthTagSize) {
            return static_cast<std::uint8_t>(0);
        }
        return data[index];
    };

    std::size_t offset = 0;
    while (offset + 8 <= data.size()) {
        std::uint64_t m = 0;
        for (std::size_t i = 0; i < 8; ++i) {
            m |= static_cast<std::uint64_t>(data_byte(offset + i)) << (8 * i);
        }
        v3 ^= m;
        sip_round();
        sip_round();
        v0 ^= m;
        offset += 8;
    }

    std::uint64_t b = static_cast<std::uint64_t>(data.size()) << 56;
    for (std::size_t i = 0; offset + i < data.size(); ++i) {
        b |= static_cast<std::uint64_t>(data_byte(offset + i)) << (8 * i);
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

bool known_packet_type(std::uint8_t value)
{
    return value >= static_cast<std::uint8_t>(PacketType::Hello) &&
        value <= static_cast<std::uint8_t>(PacketType::TransportState);
}

bool valid_payload_size(PacketType type, std::size_t size, NetworkAudioFormat audio_format)
{
    switch (type) {
    case PacketType::Hello:
    case PacketType::HelloAck:
        return size == 8;
    case PacketType::Audio:
        return audio_bytes_per_sample(audio_format) != 0 && size > 0 &&
            size <= kMaxAudioFramesPerPacket * audio_bytes_per_sample(audio_format) &&
            (size % audio_bytes_per_sample(audio_format)) == 0;
    case PacketType::Ping:
    case PacketType::Pong:
    case PacketType::Bye:
        return size == 0;
    case PacketType::MetronomeState:
        return size == 56;
    case PacketType::TransportState:
        return size == 20;
    }
    return false;
}

} // namespace

std::vector<std::uint8_t> encode_packet(
    const Header& header,
    std::span<const std::uint8_t> payload,
    const std::array<std::uint8_t, 16>& key,
    NetworkAudioFormat audio_format)
{
    if (payload.size() > 65535) {
        throw std::runtime_error("payload too large");
    }
    std::vector<std::uint8_t> out(kHeaderSize + payload.size());
    if (encode_packet_into(header, payload, key, audio_format, out) == 0) {
        throw std::runtime_error("packet output buffer is too small");
    }
    return out;
}

std::size_t encode_packet_into(
    const Header& header,
    std::span<const std::uint8_t> payload,
    const std::array<std::uint8_t, 16>& key,
    NetworkAudioFormat audio_format,
    std::span<std::uint8_t> output)
{
    if (payload.size() > 65535 || !valid_payload_size(header.type, payload.size(), audio_format) ||
        output.size() < kHeaderSize + payload.size()) {
        return 0;
    }
    const std::size_t packet_size = kHeaderSize + payload.size();
    std::span<std::uint8_t> out = output.first(packet_size);
    put_u32(out, 0, kMagic);
    out[4] = kProtocolVersion;
    out[5] = static_cast<std::uint8_t>(header.type);
    put_u16(out, 6, static_cast<std::uint16_t>(payload.size()));
    put_u64(out, 8, header.session_id);
    put_u32(out, 16, header.sequence);
    put_u64(out, 20, header.timing_value);
    put_u64(out, kAuthTagOffset, 0);
    if (!payload.empty()) {
        std::memcpy(out.data() + kHeaderSize, payload.data(), payload.size());
    }
    put_u64(out, kAuthTagOffset, siphash24(out, key));
    return packet_size;
}

ParseResult parse_packet(
    std::span<const std::uint8_t> packet,
    const std::array<std::uint8_t, 16>& key,
    std::uint64_t expected_session_id,
    NetworkAudioFormat audio_format)
{
    if (packet.size() < kHeaderSize) {
        return {{}, ParseError::ShortPacket};
    }
    if (read_u32(packet, 0) != kMagic) {
        return {{}, ParseError::WrongMagic};
    }
    if (packet[4] != kProtocolVersion) {
        return {{}, ParseError::WrongVersion};
    }
    if (!known_packet_type(packet[5])) {
        return {{}, ParseError::UnknownType};
    }
    Header header;
    header.type = static_cast<PacketType>(packet[5]);
    header.payload_length = read_u16(packet, 6);
    header.session_id = read_u64(packet, 8);
    header.sequence = read_u32(packet, 16);
    header.timing_value = read_u64(packet, 20);
    header.auth_tag = read_u64(packet, kAuthTagOffset);
    if (header.session_id != expected_session_id) {
        return {header, ParseError::WrongSession};
    }
    if (packet.size() != kHeaderSize + header.payload_length ||
        !valid_payload_size(header.type, header.payload_length, audio_format)) {
        return {header, ParseError::InvalidPayloadSize};
    }
    if (siphash24(packet, key, true) != header.auth_tag) {
        return {header, ParseError::AuthenticationFailed};
    }
    return {header, ParseError::None};
}

const char* parse_error_text(ParseError error)
{
    switch (error) {
    case ParseError::None: return "none";
    case ParseError::ShortPacket: return "short_packet";
    case ParseError::WrongMagic: return "wrong_magic";
    case ParseError::WrongVersion: return "wrong_version";
    case ParseError::UnknownType: return "unknown_type";
    case ParseError::WrongSession: return "wrong_session";
    case ParseError::InvalidPayloadSize: return "invalid_payload_size";
    case ParseError::AuthenticationFailed: return "authentication_failed";
    }
    return "unknown";
}

const char* audio_format_text(NetworkAudioFormat format) noexcept
{
    switch (format) {
    case NetworkAudioFormat::Pcm16Mono: return "pcm16-mono";
    case NetworkAudioFormat::Pcm24Mono: return "pcm24-mono";
    }
    return "unknown";
}

std::optional<NetworkAudioFormat> parse_audio_format(std::string_view text) noexcept
{
    if (text == "pcm16" || text == "pcm16-mono") {
        return NetworkAudioFormat::Pcm16Mono;
    }
    if (text == "pcm24" || text == "pcm24-mono") {
        return NetworkAudioFormat::Pcm24Mono;
    }
    return std::nullopt;
}

std::size_t audio_bytes_per_sample(NetworkAudioFormat format) noexcept
{
    switch (format) {
    case NetworkAudioFormat::Pcm16Mono: return 2;
    case NetworkAudioFormat::Pcm24Mono: return 3;
    }
    return 0;
}

std::size_t audio_payload_size(NetworkAudioFormat format, std::size_t frames) noexcept
{
    const std::size_t bytes = audio_bytes_per_sample(format);
    return bytes != 0 && frames <= kMaxAudioFramesPerPacket ? frames * bytes : 0;
}

std::vector<std::uint8_t> pack_pcm16(std::span<const std::int32_t> samples)
{
    std::vector<std::uint8_t> out(samples.size() * 2);
    (void)pack_pcm16_into(samples, out);
    return out;
}

bool pack_pcm16_into(std::span<const std::int32_t> samples, std::span<std::uint8_t> out) noexcept
{
    if (out.size() != samples.size() * 2) {
        return false;
    }
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const std::int32_t clamped = samples[i] < -8388608 ? -8388608 :
            (samples[i] > 8388607 ? 8388607 : samples[i]);
        const std::int32_t quantized = clamped >= 0
            ? clamped / 256
            : -static_cast<std::int32_t>(
                (static_cast<std::uint32_t>(-clamped) + 255U) / 256U);
        const std::uint16_t value = static_cast<std::uint16_t>(quantized & 0xffff);
        out[i * 2] = static_cast<std::uint8_t>(value & 0xffU);
        out[i * 2 + 1] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
    }
    return true;
}

std::vector<std::int32_t> unpack_pcm16(std::span<const std::uint8_t> bytes)
{
    if ((bytes.size() % 2) != 0) {
        throw std::runtime_error("16-bit PCM byte count must be divisible by 2");
    }
    std::vector<std::int32_t> out(bytes.size() / 2);
    (void)unpack_pcm16_into(bytes, out);
    return out;
}

bool unpack_pcm16_into(std::span<const std::uint8_t> bytes, std::span<std::int32_t> out) noexcept
{
    if ((bytes.size() % 2) != 0 || out.size() != bytes.size() / 2) {
        return false;
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        const std::uint16_t encoded = static_cast<std::uint16_t>(bytes[i * 2]) |
            (static_cast<std::uint16_t>(bytes[i * 2 + 1]) << 8);
        const std::int32_t signed_value = (encoded & 0x8000U) != 0
            ? static_cast<std::int32_t>(encoded) - 65536
            : static_cast<std::int32_t>(encoded);
        // Return the same signed-24 domain used by the PCM24 decoder so the
        // receive path keeps one fixed internal conversion.
        out[i] = signed_value * 256;
    }
    return true;
}

std::vector<std::uint8_t> pack_pcm24(std::span<const std::int32_t> samples)
{
    std::vector<std::uint8_t> out(samples.size() * 3);
    (void)pack_pcm24_into(samples, out);
    return out;
}

bool pack_pcm24_into(std::span<const std::int32_t> samples, std::span<std::uint8_t> out) noexcept
{
    if (out.size() != samples.size() * 3) {
        return false;
    }
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const std::int32_t clamped = samples[i] < -8388608 ? -8388608 : (samples[i] > 8388607 ? 8388607 : samples[i]);
        const std::uint32_t value = static_cast<std::uint32_t>(clamped) & 0x00ffffffU;
        out[i * 3] = static_cast<std::uint8_t>(value & 0xffU);
        out[i * 3 + 1] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
        out[i * 3 + 2] = static_cast<std::uint8_t>((value >> 16) & 0xffU);
    }
    return true;
}

std::vector<std::int32_t> unpack_pcm24(std::span<const std::uint8_t> bytes)
{
    if ((bytes.size() % 3) != 0) {
        throw std::runtime_error("24-bit PCM byte count must be divisible by 3");
    }
    std::vector<std::int32_t> out(bytes.size() / 3);
    (void)unpack_pcm24_into(bytes, out);
    return out;
}

bool unpack_pcm24_into(std::span<const std::uint8_t> bytes, std::span<std::int32_t> out) noexcept
{
    if ((bytes.size() % 3) != 0 || out.size() != bytes.size() / 3) {
        return false;
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        std::uint32_t value = static_cast<std::uint32_t>(bytes[i * 3]) |
            (static_cast<std::uint32_t>(bytes[i * 3 + 1]) << 8) |
            (static_cast<std::uint32_t>(bytes[i * 3 + 2]) << 16);
        if ((value & 0x00800000U) != 0) {
            value |= 0xff000000U;
        }
        out[i] = static_cast<std::int32_t>(value);
    }
    return true;
}

bool pack_audio_into(
    NetworkAudioFormat format,
    std::span<const std::int32_t> samples,
    std::span<std::uint8_t> output) noexcept
{
    switch (format) {
    case NetworkAudioFormat::Pcm16Mono: return pack_pcm16_into(samples, output);
    case NetworkAudioFormat::Pcm24Mono: return pack_pcm24_into(samples, output);
    }
    return false;
}

bool unpack_audio_into(
    NetworkAudioFormat format,
    std::span<const std::uint8_t> bytes,
    std::span<std::int32_t> output) noexcept
{
    switch (format) {
    case NetworkAudioFormat::Pcm16Mono: return unpack_pcm16_into(bytes, output);
    case NetworkAudioFormat::Pcm24Mono: return unpack_pcm24_into(bytes, output);
    }
    return false;
}

ReplayResult ReplayWindow::observe(std::uint32_t sequence)
{
    if (!initialized_) {
        initialized_ = true;
        highest_ = sequence;
        bitmap_ = 1;
        return ReplayResult::New;
    }
    if (sequence == highest_) {
        return ReplayResult::Duplicate;
    }
    if (sequence_after(sequence, highest_)) {
        const std::uint32_t distance = sequence_forward_distance(sequence, highest_);
        bitmap_ = distance >= 64 ? 1 : ((bitmap_ << distance) | 1ULL);
        highest_ = sequence;
        return ReplayResult::New;
    }
    if (!sequence_before(sequence, highest_)) {
        return ReplayResult::Ambiguous;
    }
    const std::uint32_t distance = sequence_forward_distance(highest_, sequence);
    if (distance >= 64) {
        return ReplayResult::TooOld;
    }
    const std::uint64_t mask = 1ULL << distance;
    if ((bitmap_ & mask) != 0) {
        return ReplayResult::Duplicate;
    }
    bitmap_ |= mask;
    return ReplayResult::New;
}

void ReplayWindow::reset()
{
    initialized_ = false;
    highest_ = 0;
    bitmap_ = 0;
}

} // namespace jam2::protocol

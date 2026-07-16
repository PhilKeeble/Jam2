#include "stun.hpp"

#include <array>
#include <stdexcept>

namespace jam2::stun {
namespace {

constexpr std::uint16_t kBindingRequest = 0x0001;
constexpr std::uint16_t kBindingSuccessResponse = 0x0101;
constexpr std::uint16_t kXorMappedAddress = 0x0020;
constexpr std::uint32_t kMagicCookie = 0x2112a442U;

void put_u16(std::vector<std::uint8_t>& out, std::size_t offset, std::uint16_t value)
{
    out[offset] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
    out[offset + 1] = static_cast<std::uint8_t>(value & 0xffU);
}

void put_u32(std::vector<std::uint8_t>& out, std::size_t offset, std::uint32_t value)
{
    out[offset] = static_cast<std::uint8_t>((value >> 24) & 0xffU);
    out[offset + 1] = static_cast<std::uint8_t>((value >> 16) & 0xffU);
    out[offset + 2] = static_cast<std::uint8_t>((value >> 8) & 0xffU);
    out[offset + 3] = static_cast<std::uint8_t>(value & 0xffU);
}

std::uint16_t read_u16(std::span<const std::uint8_t> in, std::size_t offset)
{
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(in[offset]) << 8) | in[offset + 1]);
}

std::uint32_t read_u32(std::span<const std::uint8_t> in, std::size_t offset)
{
    return (static_cast<std::uint32_t>(in[offset]) << 24) |
        (static_cast<std::uint32_t>(in[offset + 1]) << 16) |
        (static_cast<std::uint32_t>(in[offset + 2]) << 8) |
        static_cast<std::uint32_t>(in[offset + 3]);
}

std::string ipv4_to_string(std::uint32_t value)
{
    return std::to_string((value >> 24) & 0xffU) + "." +
        std::to_string((value >> 16) & 0xffU) + "." +
        std::to_string((value >> 8) & 0xffU) + "." +
        std::to_string(value & 0xffU);
}

} // namespace

BindingRequest make_binding_request()
{
    BindingRequest request;
    const auto key = random_key();
    for (std::size_t i = 0; i < request.transaction_id.size(); ++i) {
        request.transaction_id[i] = key[i];
    }

    request.bytes.resize(20);
    put_u16(request.bytes, 0, kBindingRequest);
    put_u16(request.bytes, 2, 0);
    put_u32(request.bytes, 4, kMagicCookie);
    for (std::size_t i = 0; i < request.transaction_id.size(); ++i) {
        request.bytes[8 + i] = request.transaction_id[i];
    }
    return request;
}

Endpoint parse_binding_response(
    std::span<const std::uint8_t> packet,
    const std::array<std::uint8_t, 12>& transaction_id)
{
    if (packet.size() < 20) {
        throw std::runtime_error("STUN response shorter than header");
    }
    if (read_u16(packet, 0) != kBindingSuccessResponse) {
        throw std::runtime_error("STUN response is not a Binding Success Response");
    }
    const std::uint16_t length = read_u16(packet, 2);
    if (packet.size() != static_cast<std::size_t>(20 + length)) {
        throw std::runtime_error("STUN response length mismatch");
    }
    if (read_u32(packet, 4) != kMagicCookie) {
        throw std::runtime_error("STUN response has wrong magic cookie");
    }
    for (std::size_t i = 0; i < transaction_id.size(); ++i) {
        if (packet[8 + i] != transaction_id[i]) {
            throw std::runtime_error("STUN response transaction id mismatch");
        }
    }

    std::size_t offset = 20;
    while (offset + 4 <= packet.size()) {
        const std::uint16_t type = read_u16(packet, offset);
        const std::uint16_t attr_length = read_u16(packet, offset + 2);
        const std::size_t value_offset = offset + 4;
        const std::size_t next_offset = value_offset + ((attr_length + 3U) & ~3U);
        if (value_offset + attr_length > packet.size() || next_offset > packet.size()) {
            throw std::runtime_error("STUN attribute length exceeds packet");
        }
        if (type == kXorMappedAddress) {
            if (attr_length < 8) {
                throw std::runtime_error("STUN XOR-MAPPED-ADDRESS is too short");
            }
            if (packet[value_offset + 1] != 0x01) {
                throw std::runtime_error("STUN XOR-MAPPED-ADDRESS is not IPv4");
            }
            const std::uint16_t xport = read_u16(packet, value_offset + 2);
            const std::uint32_t xaddr = read_u32(packet, value_offset + 4);
            const std::uint16_t port = static_cast<std::uint16_t>(xport ^ (kMagicCookie >> 16));
            const std::uint32_t addr = xaddr ^ kMagicCookie;
            return Endpoint{ipv4_to_string(addr), port};
        }
        offset = next_offset;
    }

    throw std::runtime_error("STUN response did not contain XOR-MAPPED-ADDRESS");
}

Endpoint discover_public_endpoint(
    UdpSocket& socket,
    const Endpoint& stun_server,
    int timeout_ms,
    int retries)
{
    if (timeout_ms <= 0 || retries <= 0) {
        throw std::runtime_error("STUN timeout and retries must be positive");
    }
    const ResolvedUdpEndpoint resolved_server = resolve_udp_endpoint(stun_server);
    std::array<std::uint8_t, 2048> receive_buffer{};
    for (int attempt = 0; attempt < retries; ++attempt) {
        const BindingRequest request = make_binding_request();
        const UdpSendResult send = socket.send_to(resolved_server, request.bytes);
        if (send.outcome == UdpSendOutcome::Fatal) {
            throw std::runtime_error(
                "failed to send STUN binding request (socket error " +
                std::to_string(send.error_code) + ")");
        }
        if (send.outcome != UdpSendOutcome::Sent) {
            continue;
        }
        const auto deadline = monotonic_us() + static_cast<std::uint64_t>(timeout_ms) * 1000ULL;
        while (monotonic_us() < deadline) {
            const auto remaining_us = deadline - monotonic_us();
            const int wait_ms = static_cast<int>(remaining_us / 1000ULL) + 1;
            const auto received = socket.recv_from(
                receive_buffer,
                wait_ms > timeout_ms ? timeout_ms : wait_ms);
            if (!received) {
                continue;
            }
            try {
                return parse_binding_response(
                    std::span<const std::uint8_t>(receive_buffer.data(), received->size),
                    request.transaction_id);
            } catch (const std::exception&) {
                continue;
            }
        }
    }
    throw std::runtime_error("STUN discovery timed out; try --no-stun --public-endpoint ip:port for manual mode");
}

} // namespace jam2::stun

#pragma once

#include "common.hpp"
#include "udp_socket.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace jam2::stun {

constexpr std::uint16_t kDefaultPort = 19302;

struct BindingRequest {
    std::array<std::uint8_t, 12> transaction_id{};
    std::vector<std::uint8_t> bytes;
};

BindingRequest make_binding_request();
Endpoint parse_binding_response(
    std::span<const std::uint8_t> packet,
    const std::array<std::uint8_t, 12>& transaction_id);
Endpoint discover_public_endpoint(
    UdpSocket& socket,
    const Endpoint& stun_server,
    int timeout_ms,
    int retries);

} // namespace jam2::stun

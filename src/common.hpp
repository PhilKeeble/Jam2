#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jam2 {

struct Endpoint {
    std::string host;
    std::uint16_t port{};
};

struct SessionInfo {
    Endpoint endpoint;
    std::uint64_t session_id{};
    std::array<std::uint8_t, 16> key{};
};

std::string endpoint_to_string(const Endpoint& endpoint);
Endpoint parse_endpoint(std::string_view text);
Endpoint parse_bind_endpoint(std::string_view text);

std::string hex_encode(const std::uint8_t* data, std::size_t size);
std::vector<std::uint8_t> hex_decode(std::string_view text);
std::uint64_t parse_hex_u64(std::string_view text);

std::array<std::uint8_t, 16> random_key();
std::uint64_t random_u64();
std::uint64_t monotonic_us();

std::string make_jam_url(const SessionInfo& info);
SessionInfo parse_jam_url(std::string_view url);

} // namespace jam2

#include "common.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>

namespace jam2 {
namespace {

int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::string query_value(std::string_view query, std::string_view name)
{
    std::size_t pos = 0;
    while (pos <= query.size()) {
        const std::size_t next = query.find('&', pos);
        const std::string_view part = query.substr(pos, next == std::string_view::npos ? query.size() - pos : next - pos);
        const std::size_t eq = part.find('=');
        if (eq != std::string_view::npos && part.substr(0, eq) == name) {
            return std::string(part.substr(eq + 1));
        }
        if (next == std::string_view::npos) {
            break;
        }
        pos = next + 1;
    }
    throw std::runtime_error("missing jam2 URL field: " + std::string(name));
}

} // namespace

std::string endpoint_to_string(const Endpoint& endpoint)
{
    return endpoint.host + ":" + std::to_string(endpoint.port);
}

Endpoint parse_endpoint_impl(std::string_view text, bool allow_zero_port)
{
    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= text.size()) {
        throw std::runtime_error("endpoint must be host:port");
    }
    const std::string host{text.substr(0, colon)};
    const std::string port_text{text.substr(colon + 1)};
    std::size_t consumed = 0;
    const unsigned long port = std::stoul(port_text, &consumed, 10);
    if (consumed != port_text.size() || port > 65535 || (!allow_zero_port && port == 0)) {
        throw std::runtime_error(allow_zero_port ? "endpoint port must be 0..65535" : "endpoint port must be 1..65535");
    }
    return Endpoint{host, static_cast<std::uint16_t>(port)};
}

Endpoint parse_endpoint(std::string_view text)
{
    return parse_endpoint_impl(text, false);
}

Endpoint parse_bind_endpoint(std::string_view text)
{
    return parse_endpoint_impl(text, true);
}

std::string hex_encode(const std::uint8_t* data, std::size_t size)
{
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < size; ++i) {
        out << std::setw(2) << static_cast<unsigned int>(data[i]);
    }
    return out.str();
}

std::vector<std::uint8_t> hex_decode(std::string_view text)
{
    if ((text.size() % 2) != 0) {
        throw std::runtime_error("hex value has odd length");
    }
    std::vector<std::uint8_t> out(text.size() / 2);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int hi = hex_value(text[i * 2]);
        const int lo = hex_value(text[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            throw std::runtime_error("hex value contains a non-hex character");
        }
        out[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return out;
}

std::uint64_t parse_hex_u64(std::string_view text)
{
    if (text.empty() || text.size() > 16) {
        throw std::runtime_error("session id must be 1..16 hex characters");
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        const int digit = hex_value(c);
        if (digit < 0) {
            throw std::runtime_error("session id contains a non-hex character");
        }
        value = (value << 4) | static_cast<std::uint64_t>(digit);
    }
    return value;
}

std::array<std::uint8_t, 16> random_key()
{
    std::array<std::uint8_t, 16> key{};
    std::random_device rd;
    for (auto& byte : key) {
        byte = static_cast<std::uint8_t>(rd());
    }
    return key;
}

std::uint64_t random_u64()
{
    std::random_device rd;
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<std::uint64_t>(rd() & 0xffU);
    }
    return value == 0 ? 1 : value;
}

std::uint64_t monotonic_us()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

std::string make_jam_url(const SessionInfo& info)
{
    std::array<std::uint8_t, 8> session_bytes{};
    for (std::size_t i = 0; i < session_bytes.size(); ++i) {
        session_bytes[session_bytes.size() - 1 - i] = static_cast<std::uint8_t>((info.session_id >> (i * 8)) & 0xffU);
    }
    return "jam2://v1?endpoint=" + endpoint_to_string(info.endpoint) + "&session=" +
        hex_encode(session_bytes.data(), session_bytes.size()) + "&key=" +
        hex_encode(info.key.data(), info.key.size());
}

SessionInfo parse_jam_url(std::string_view url)
{
    constexpr std::string_view prefix = "jam2://v1?";
    if (url.substr(0, prefix.size()) != prefix) {
        throw std::runtime_error("jam2 URL must start with jam2://v1?");
    }
    const std::string_view query = url.substr(prefix.size());
    const auto endpoint = parse_endpoint(query_value(query, "endpoint"));
    const auto session = parse_hex_u64(query_value(query, "session"));
    const auto key_bytes = hex_decode(query_value(query, "key"));
    if (key_bytes.size() != 16) {
        throw std::runtime_error("jam2 URL key must be 16 bytes encoded as 32 hex characters");
    }
    SessionInfo info;
    info.endpoint = endpoint;
    info.session_id = session;
    std::copy(key_bytes.begin(), key_bytes.end(), info.key.begin());
    return info;
}

} // namespace jam2

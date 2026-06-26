#pragma once

#include "common.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <netinet/in.h>
#endif

namespace jam2 {

class NetworkRuntime {
public:
    NetworkRuntime();
    ~NetworkRuntime();
    NetworkRuntime(const NetworkRuntime&) = delete;
    NetworkRuntime& operator=(const NetworkRuntime&) = delete;
};

class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;

    void bind(const Endpoint& endpoint);
    Endpoint local_endpoint() const;
    void send_to(const Endpoint& endpoint, std::span<const std::uint8_t> bytes) const;
    std::optional<std::pair<Endpoint, std::vector<std::uint8_t>>> recv_from(int timeout_ms) const;

private:
#if defined(_WIN32)
    SOCKET handle_{INVALID_SOCKET};
#else
    int handle_{-1};
#endif
};

} // namespace jam2

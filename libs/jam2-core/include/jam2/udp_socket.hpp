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

Endpoint resolve_udp_endpoint(const Endpoint& endpoint);

class NetworkRuntime {
public:
    NetworkRuntime();
    ~NetworkRuntime();
    NetworkRuntime(const NetworkRuntime&) = delete;
    NetworkRuntime& operator=(const NetworkRuntime&) = delete;
};

class UdpSocket {
public:
    struct ReceivedDatagram {
        Endpoint endpoint;
        std::size_t size = 0;
    };

    UdpSocket();
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;

    void bind(const Endpoint& endpoint);
    Endpoint local_endpoint() const;
    void set_send_buffer_size(int bytes);
    void set_recv_buffer_size(int bytes);
    int send_buffer_size() const;
    int recv_buffer_size() const;
    void send_to(const Endpoint& endpoint, std::span<const std::uint8_t> bytes) const;
    std::optional<ReceivedDatagram> recv_from(std::span<std::uint8_t> buffer, int timeout_ms) const;
    std::optional<ReceivedDatagram> recv_from_for(
        std::span<std::uint8_t> buffer,
        std::uint64_t timeout_us) const;
    std::optional<std::pair<Endpoint, std::vector<std::uint8_t>>> recv_from(int timeout_ms) const;

private:
#if defined(_WIN32)
    SOCKET handle_{INVALID_SOCKET};
#else
    int handle_{-1};
#endif
};

} // namespace jam2

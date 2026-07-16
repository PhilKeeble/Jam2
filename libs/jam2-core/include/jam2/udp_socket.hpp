#pragma once

#include "common.hpp"

#include <cstdint>
#include <optional>
#include <span>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <netinet/in.h>
#endif

namespace jam2 {

// Numeric IPv4 endpoint used by the UDP data plane. Text parsing and DNS are
// deliberately confined to resolve_udp_endpoint(), before an endpoint enters
// a packet-rate send/receive path.
struct ResolvedUdpEndpoint {
    std::uint32_t address = 0; // Network byte order.
    std::uint16_t port = 0;    // Host byte order.

    friend constexpr bool operator==(ResolvedUdpEndpoint, ResolvedUdpEndpoint) = default;
};

ResolvedUdpEndpoint resolve_udp_endpoint(const Endpoint& endpoint);
Endpoint format_udp_endpoint(const ResolvedUdpEndpoint& endpoint);

enum class UdpSendOutcome : std::uint8_t {
    Sent,
    WouldBlock,
    NoBufferSpace,
    Unreachable,
    Refused,
    Fatal,
};

struct UdpSendResult {
    UdpSendOutcome outcome = UdpSendOutcome::Sent;
    int error_code = 0;
};

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
        ResolvedUdpEndpoint endpoint;
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
    UdpSendResult send_to(
        const ResolvedUdpEndpoint& endpoint,
        std::span<const std::uint8_t> bytes) const noexcept;
    std::optional<ReceivedDatagram> recv_from(std::span<std::uint8_t> buffer, int timeout_ms) const;
    std::optional<ReceivedDatagram> recv_from_for(
        std::span<std::uint8_t> buffer,
        std::uint64_t timeout_us) const;
private:
#if defined(_WIN32)
    SOCKET handle_{INVALID_SOCKET};
#else
    int handle_{-1};
#endif
};

} // namespace jam2

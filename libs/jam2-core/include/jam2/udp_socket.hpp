#pragma once

#include "common.hpp"

#include <atomic>
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

// A pre-created, coalescing notification that may be signalled by a real-time
// audio callback. signal() performs no allocation, locking, or blocking; the
// network worker consumes the notification while waiting for UDP input.
class RealtimeWakeSignal {
public:
    RealtimeWakeSignal();
    ~RealtimeWakeSignal();
    RealtimeWakeSignal(const RealtimeWakeSignal&) = delete;
    RealtimeWakeSignal& operator=(const RealtimeWakeSignal&) = delete;

    void requestWake() noexcept;
    void cancelWakeRequest() noexcept;
    void signal(std::uint64_t ready_time_us) noexcept;
    std::uint64_t signalCount() const noexcept;
    std::uint64_t consumptionCount() const noexcept;
    std::uint64_t lastSignalTimeUs() const noexcept;
    // Wait for queued transport input or the existing audio-capture notification.
    void waitFor(std::uint64_t timeout_us, RealtimeWakeSignal* additional = nullptr);

private:
    friend class UdpSocket;
    void consume() noexcept;
    void signalNative() noexcept;

    std::atomic<bool> pending_{false};
    std::atomic<bool> wake_requested_{false};
    std::atomic<std::uint64_t> request_generation_{0};
    std::atomic<std::uint64_t> signal_count_{0};
    std::atomic<std::uint64_t> consumption_count_{0};
    std::atomic<std::uint64_t> last_signal_time_us_{0};
#if defined(_WIN32)
    WSAEVENT event_{WSA_INVALID_EVENT};
#else
    int read_handle_{-1};
    int write_handle_{-1};
#endif
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
    // Discover the outbound interface without sending a probe or connecting this media socket.
    Endpoint local_endpoint_for(const ResolvedUdpEndpoint& destination) const;
    static void waitForAny(std::span<const UdpSocket* const> sockets,
        std::uint64_t timeout_us, RealtimeWakeSignal* command_wake = nullptr);
    // Runtime receive deadline, matching the direct socket's select path when
    // there is no capture event. Does not read any datagram.
    static void waitForDatagrams(std::span<const UdpSocket* const> sockets,
        std::uint64_t timeout_us, RealtimeWakeSignal* capture_wake = nullptr);
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
        std::uint64_t timeout_us,
        RealtimeWakeSignal* wake_signal = nullptr) const;
private:
#if defined(_WIN32)
    SOCKET handle_{INVALID_SOCKET};
    WSAEVENT receive_event_{WSA_INVALID_EVENT};
#else
    int handle_{-1};
#endif
};

// Network-only socket routing seam. Legacy sessions keep their owned UdpSocket;
// Server mode supplies a link transport whose selected socket is per peer.
class NetworkDatagramTransport {
public:
    // Multiplex control fragments can exceed the native audio packet maximum.
    static constexpr std::size_t kReceiveBufferBytes = 1500;
    virtual ~NetworkDatagramTransport() = default;
    // NetworkSession owns one receiver registration. Failed attachment must
    // leave this transport detached; replacement preserves the old receiver.
    virtual void receiverAttached() {}
    virtual void receiverDetached() noexcept {}
    // Optional Server-only receipt after the control owner accepts/schedules
    // a native transport intent. This is never an audio callback.
    virtual void transportIntentAccepted(std::uint64_t, std::span<const std::uint8_t>) noexcept {}
    virtual UdpSendResult sendToPeer(
        std::uint64_t peer_id, const ResolvedUdpEndpoint& endpoint,
        std::span<const std::uint8_t> bytes) noexcept = 0;
    // The receiving session supplies at least kReceiveBufferBytes bytes.
    virtual std::optional<UdpSocket::ReceivedDatagram> receiveFor(
        std::span<std::uint8_t> buffer, std::uint64_t timeout_us,
        RealtimeWakeSignal* capture_wake) = 0;
};

} // namespace jam2

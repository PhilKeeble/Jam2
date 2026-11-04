#pragma once
#if defined(__APPLE__)
#include <cstdint>
#include <memory>
#include <span>

namespace jam2 {
class UdpSocket;
class RealtimeWakeSignal;
enum class LegacyUdpWaitStyle { Select, Poll };
struct MacSocketWaitResult {
    bool readiness = false, deadline = false, interrupted = false;
    bool datagram = false, primary_wake = false, secondary_wake = false;
};

// Non-real-time, single-thread-owned readiness and deadline queue. Readable
// descriptors remain untouched: the socket/wake owner performs consumption.
class MacSocketWaiter {
public:
    MacSocketWaiter();
    ~MacSocketWaiter();
    MacSocketWaiter(const MacSocketWaiter&) = delete;
    MacSocketWaiter& operator=(const MacSocketWaiter&) = delete;
    MacSocketWaitResult waitForRead(std::span<const int> descriptors, std::uint64_t timeout_us,
        LegacyUdpWaitStyle legacy_style = LegacyUdpWaitStyle::Select);
    bool readable(int descriptor) const noexcept;
    MacSocketWaitResult waitUdp(std::span<const UdpSocket* const> sockets, std::uint64_t timeout_us,
        RealtimeWakeSignal* primary = nullptr, RealtimeWakeSignal* secondary = nullptr,
        LegacyUdpWaitStyle legacy_style = LegacyUdpWaitStyle::Select);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
MacSocketWaiter& macos_thread_socket_waiter();
} // namespace jam2
#endif

#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

namespace jam2 {
enum class UdpWaitMode { PlatformDefault, Legacy };
std::string_view udp_wait_mode_text(UdpWaitMode mode) noexcept;

struct UdpWaitSnapshot {
    std::uint64_t calls = 0, requested_us = 0, elapsed_us = 0;
    std::uint64_t max_elapsed_us = 0, max_overshoot_us = 0, late_over_1ms = 0;
    std::uint64_t readiness_wakes = 0, deadline_wakes = 0, interruptions = 0, errors = 0;
};

// One network-thread producer; cold diagnostic readers use relaxed snapshots.
class UdpWaitDiagnostics {
public:
    void record(std::uint64_t requested, std::uint64_t elapsed,
        bool readiness, bool deadline, bool interrupted, bool error) noexcept;
    UdpWaitSnapshot snapshot() const noexcept;
private:
    std::atomic<std::uint64_t> calls_{0}, requested_{0}, elapsed_{0};
    std::atomic<std::uint64_t> max_elapsed_{0}, max_overshoot_{0}, late_{0};
    std::atomic<std::uint64_t> readiness_{0}, deadlines_{0}, interrupted_{0}, errors_{0};
};

// Scoped to a non-real-time network reader. The macOS waiter itself is owned
// by that thread and reused; changing comparison mode creates no audio work.
class UdpWaitScope {
public:
    explicit UdpWaitScope(UdpWaitMode mode, UdpWaitDiagnostics* diagnostics = nullptr);
    ~UdpWaitScope();
    UdpWaitScope(const UdpWaitScope&) = delete;
    UdpWaitScope& operator=(const UdpWaitScope&) = delete;
private:
    UdpWaitMode previous_mode_;
    UdpWaitDiagnostics* previous_diagnostics_;
};
UdpWaitMode current_udp_wait_mode() noexcept;
UdpWaitDiagnostics* current_udp_wait_diagnostics() noexcept;
} // namespace jam2

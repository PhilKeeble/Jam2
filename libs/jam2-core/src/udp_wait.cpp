#include "udp_wait.hpp"
#include <stdexcept>

namespace jam2 {
namespace {
thread_local UdpWaitMode mode = UdpWaitMode::PlatformDefault;
thread_local UdpWaitDiagnostics* diagnostics = nullptr;
}
std::string_view udp_wait_mode_text(UdpWaitMode value) noexcept
{
#if defined(__APPLE__)
    return value == UdpWaitMode::Legacy ? "legacy-select-poll" : "kqueue-critical";
#else
    (void)value;
    return "native";
#endif
}
UdpWaitScope::UdpWaitScope(UdpWaitMode value, UdpWaitDiagnostics* target)
    : previous_mode_(mode), previous_diagnostics_(diagnostics)
{
    if (value != UdpWaitMode::PlatformDefault && value != UdpWaitMode::Legacy)
        throw std::invalid_argument("invalid UDP wait mode");
    mode = value; diagnostics = target;
}
UdpWaitScope::~UdpWaitScope() { mode = previous_mode_; diagnostics = previous_diagnostics_; }
UdpWaitMode current_udp_wait_mode() noexcept { return mode; }
UdpWaitDiagnostics* current_udp_wait_diagnostics() noexcept { return diagnostics; }
void UdpWaitDiagnostics::record(std::uint64_t requested, std::uint64_t elapsed,
    bool readiness, bool deadline, bool interrupted, bool error) noexcept
{
    constexpr auto relaxed = std::memory_order_relaxed;
    const auto overshoot = elapsed > requested ? elapsed - requested : 0;
    calls_.fetch_add(1, relaxed); requested_.fetch_add(requested, relaxed); elapsed_.fetch_add(elapsed, relaxed);
    if (elapsed > max_elapsed_.load(relaxed)) max_elapsed_.store(elapsed, relaxed);
    if (overshoot > max_overshoot_.load(relaxed)) max_overshoot_.store(overshoot, relaxed);
    if (overshoot >= 1000) late_.fetch_add(1, relaxed);
    if (readiness) readiness_.fetch_add(1, relaxed);
    if (deadline) deadlines_.fetch_add(1, relaxed);
    if (interrupted) interrupted_.fetch_add(1, relaxed);
    if (error) errors_.fetch_add(1, relaxed);
}
UdpWaitSnapshot UdpWaitDiagnostics::snapshot() const noexcept
{
    constexpr auto relaxed = std::memory_order_relaxed;
    return {calls_.load(relaxed), requested_.load(relaxed), elapsed_.load(relaxed),
        max_elapsed_.load(relaxed), max_overshoot_.load(relaxed), late_.load(relaxed),
        readiness_.load(relaxed), deadlines_.load(relaxed), interrupted_.load(relaxed), errors_.load(relaxed)};
}
} // namespace jam2

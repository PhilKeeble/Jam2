#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>

namespace jam2 {
// Diagnostic clock only: never read from a real-time audio callback.
std::uint64_t receive_diagnostic_time_us();
#if defined(__APPLE__)
std::uint64_t receive_diagnostic_mach_us(std::uint64_t ticks);
#endif
struct ReceiveClockSample {
    std::uint64_t wall_us = 0, cpu_us = 0;
    bool cpu_valid = false;
};
ReceiveClockSample receive_clock_sample();

enum class ReceiveServicePhase : std::uint8_t {
    Unknown, Advance, Maintenance, CapturePack, AudioSend, ControlSend,
    ReceiveDispatch, SocketRead, SocketWait, Classification, DecodeMix,
    ReceiveFinish, RuntimeCommands, Telemetry, Shutdown, Count
};
std::string_view receive_service_phase_text(ReceiveServicePhase phase) noexcept;
inline constexpr std::size_t kReceivePhaseCount = static_cast<std::size_t>(ReceiveServicePhase::Count);

struct ReceiveReadinessEvidence {
    std::uint64_t last_would_block_begin_us = 0, last_would_block_end_us = 0;
    std::uint64_t datagrams_since_would_block = 0;
};
struct ReceiveCallCpu { std::uint64_t us = 0; bool valid = false; };

struct SlowReceiveServiceSpan {
    std::uint64_t begin_us = 0, end_us = 0, cpu_us = 0;
    bool cpu_valid = false;
};

struct SlowReceiveEvent {
    std::uint64_t id = 0, peer_id = 0;
    std::uint64_t kernel_us = 0, read_begin_us = 0, read_end_us = 0, observed_us = 0, trace_us = 0;
    std::uint64_t before_read_us = 0, receive_call_us = 0, after_read_us = 0, after_observation_us = 0;
    std::uint64_t covered_us = 0;
    std::array<std::uint64_t, kReceivePhaseCount> phase_wall_us{};
    ReceiveServicePhase dominant_phase = ReceiveServicePhase::Unknown;
    std::uint64_t dominant_span_wall_us = 0, dominant_span_cpu_us = 0, dominant_overlap_us = 0;
    bool dominant_cpu_valid = false;
    ReceiveReadinessEvidence readiness;
    std::uint64_t would_block_after_kernel_us = 0;
    ReceiveCallCpu read_cpu;
};

// Single network-worker producer. Only the bounded event copy is shared with
// cold telemetry readers. Phase history and packet observation allocate nothing.
class ReceiveServiceDiagnostics {
public:
    static constexpr std::uint64_t kSlowUs = 1000, kEventSpacingUs = 5000;
    static constexpr std::size_t kHistorySize = 512, kEventCount = 32;
    struct Snapshot {
        std::uint64_t slow_packets = 0, coalesced_packets = 0, overwritten_events = 0;
        std::uint64_t invalid_samples = 0, publish_max_us = 0;
        std::size_t count = 0;
        std::array<SlowReceiveEvent, kEventCount> events{};
        // Worst completed span per phase, including empty socket reads and
        // waits before any packet exists. Packet residence cannot see those.
        std::array<SlowReceiveServiceSpan, kReceivePhaseCount> worst_phase_spans{};
    };
    void phase(ReceiveServicePhase value, ReceiveClockSample sample) noexcept;
    void observe(std::uint64_t peer, std::uint64_t kernel, std::uint64_t read_begin,
        std::uint64_t read_end, std::uint64_t observed, ReceiveClockSample sample,
        ReceiveReadinessEvidence readiness = {}, ReceiveCallCpu read_cpu = {});
    Snapshot snapshot() const;
private:
    struct Span {
        ReceiveServicePhase phase = ReceiveServicePhase::Unknown;
        std::uint64_t begin = 0, end = 0, cpu = 0;
        bool cpu_valid = false;
    };
    std::array<Span, kHistorySize> history_{};
    std::size_t history_next_ = 0, history_count_ = 0;
    ReceiveServicePhase phase_ = ReceiveServicePhase::Unknown;
    ReceiveClockSample previous_{};
    std::uint64_t last_event_us_ = 0, last_event_delay_us_ = 0, next_id_ = 0;
    mutable std::mutex events_mutex_;
    std::array<SlowReceiveEvent, kEventCount> events_{};
    std::array<std::uint64_t, kReceivePhaseCount> phase_max_wall_us_{};
    std::array<SlowReceiveServiceSpan, kReceivePhaseCount> worst_phase_spans_{};
    std::size_t event_next_ = 0, event_count_ = 0;
    std::atomic<std::uint64_t> slow_{0}, coalesced_{0}, overwritten_{0}, invalid_{0}, publish_max_{0};
};
} // namespace jam2

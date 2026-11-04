#include "receive_diagnostics.hpp"
#include "common.hpp"

#include <algorithm>
#include <stdexcept>
#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <time.h>
#endif

namespace jam2 {
#if defined(__APPLE__)
std::uint64_t receive_diagnostic_mach_us(std::uint64_t ticks)
{
    static const auto base = [] {
        mach_timebase_info_data_t value{};
        if (mach_timebase_info(&value) != KERN_SUCCESS || !value.denom)
            throw std::runtime_error("failed to read receive timing timebase");
        return value;
    }();
    return static_cast<std::uint64_t>(static_cast<long double>(ticks) * base.numer / base.denom / 1000.0L);
}
#endif
std::uint64_t receive_diagnostic_time_us()
{
#if defined(__APPLE__)
    return receive_diagnostic_mach_us(mach_absolute_time());
#else
    return monotonic_us();
#endif
}
ReceiveClockSample receive_clock_sample()
{
    ReceiveClockSample sample;
#if defined(_WIN32)
    FILETIME created{}, exited{}, kernel{}, user{};
    sample.cpu_valid = GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user) != 0;
    if (sample.cpu_valid) {
        const auto ticks = [](FILETIME t) { return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
        sample.cpu_us = (ticks(kernel) + ticks(user)) / 10;
    }
#else
    timespec value{};
    sample.cpu_valid = clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) == 0;
    if (sample.cpu_valid) sample.cpu_us = static_cast<std::uint64_t>(value.tv_sec) * 1000000 + value.tv_nsec / 1000;
#endif
    sample.wall_us = receive_diagnostic_time_us();
    return sample;
}
std::string_view receive_service_phase_text(ReceiveServicePhase phase) noexcept
{
    constexpr std::array<std::string_view, kReceivePhaseCount> names{
        "unknown", "advance", "maintenance", "capture_pack", "audio_send", "control_send",
        "receive_dispatch", "socket_read", "socket_wait", "classification", "decode_mix",
        "receive_finish", "runtime_commands", "telemetry", "shutdown"};
    const auto index = static_cast<std::size_t>(phase);
    return index < names.size() ? names[index] : names[0];
}
void ReceiveServiceDiagnostics::phase(ReceiveServicePhase value, ReceiveClockSample sample) noexcept
{
    if (previous_.wall_us && sample.wall_us >= previous_.wall_us) {
        auto& span = history_[history_next_];
        span = {phase_, previous_.wall_us, sample.wall_us, 0,
            sample.cpu_valid && previous_.cpu_valid && sample.cpu_us >= previous_.cpu_us};
        if (span.cpu_valid) span.cpu = sample.cpu_us - previous_.cpu_us;
        const auto wall_us = span.end - span.begin;
        const auto raw_index = static_cast<std::size_t>(span.phase);
        const auto index = raw_index < kReceivePhaseCount ? raw_index : 0;
        if (wall_us >= kSlowUs && wall_us > phase_max_wall_us_[index]) {
            phase_max_wall_us_[index] = wall_us;
            std::lock_guard lock(events_mutex_);
            worst_phase_spans_[index] = {span.begin, span.end, span.cpu, span.cpu_valid};
        }
        history_next_ = (history_next_ + 1) % kHistorySize;
        history_count_ = (std::min)(history_count_ + 1, kHistorySize);
    } else if (previous_.wall_us) ++invalid_;
    previous_ = sample;
    phase_ = value;
}
void ReceiveServiceDiagnostics::observe(std::uint64_t peer, std::uint64_t kernel,
    std::uint64_t read_begin, std::uint64_t read_end, std::uint64_t observed, ReceiveClockSample sample,
    ReceiveReadinessEvidence readiness, ReceiveCallCpu read_cpu)
{
    phase(phase_, sample);
    if (!kernel || !read_begin || read_end < read_begin || observed < read_end || sample.wall_us < observed || kernel > observed) {
        ++invalid_; return;
    }
    if (sample.wall_us - kernel < kSlowUs) return;
    ++slow_;
    bool replace = false;
    if (last_event_us_ && sample.wall_us >= last_event_us_ && sample.wall_us - last_event_us_ < kEventSpacingUs) {
        ++coalesced_;
        if (sample.wall_us - kernel <= last_event_delay_us_) return;
        replace = true;
    }
    if (!replace) last_event_us_ = sample.wall_us;
    last_event_delay_us_ = sample.wall_us - kernel;
    SlowReceiveEvent event;
    event.read_cpu = read_cpu;
    event.id = replace ? next_id_ : ++next_id_; event.peer_id = peer;
    event.kernel_us = kernel; event.read_begin_us = read_begin; event.read_end_us = read_end;
    event.observed_us = observed; event.trace_us = sample.wall_us;
    event.before_read_us = read_begin > kernel ? read_begin - kernel : 0;
    event.receive_call_us = read_end - read_begin;
    event.after_read_us = observed > (std::max)(read_end, kernel) ? observed - (std::max)(read_end, kernel) : 0;
    event.after_observation_us = sample.wall_us - observed;
    if (readiness.last_would_block_begin_us <= readiness.last_would_block_end_us &&
        readiness.last_would_block_end_us <= read_begin) {
        event.readiness = readiness;
        if (readiness.last_would_block_begin_us > kernel)
            event.would_block_after_kernel_us = readiness.last_would_block_begin_us - kernel;
    } else ++invalid_;
    for (std::size_t i = 0; i < history_count_; ++i) {
        const auto& span = history_[(history_next_ + kHistorySize - history_count_ + i) % kHistorySize];
        const auto begin = (std::max)(kernel, span.begin), end = (std::min)(sample.wall_us, span.end);
        if (end <= begin) continue;
        const auto overlap = end - begin;
        event.covered_us += overlap;
        const auto index = static_cast<std::size_t>(span.phase);
        event.phase_wall_us[index < kReceivePhaseCount ? index : 0] += overlap;
        if (overlap > event.dominant_overlap_us) {
            event.dominant_phase = span.phase; event.dominant_overlap_us = overlap;
            event.dominant_span_wall_us = span.end - span.begin;
            event.dominant_span_cpu_us = span.cpu; event.dominant_cpu_valid = span.cpu_valid;
        }
    }
    const auto publish_begin = receive_diagnostic_time_us();
    {
        std::lock_guard lock(events_mutex_);
        if (replace) events_[(event_next_ + kEventCount - 1) % kEventCount] = event;
        else {
            if (event_count_ == kEventCount) ++overwritten_;
            events_[event_next_] = event;
            event_next_ = (event_next_ + 1) % kEventCount;
            event_count_ = (std::min)(event_count_ + 1, kEventCount);
        }
    }
    const auto publish_us = receive_diagnostic_time_us() - publish_begin;
    if (publish_us > publish_max_.load(std::memory_order_relaxed)) publish_max_.store(publish_us, std::memory_order_relaxed);
}
ReceiveServiceDiagnostics::Snapshot ReceiveServiceDiagnostics::snapshot() const
{
    Snapshot result;
    std::lock_guard lock(events_mutex_);
    result.slow_packets = slow_; result.coalesced_packets = coalesced_; result.overwritten_events = overwritten_;
    result.invalid_samples = invalid_; result.publish_max_us = publish_max_;
    result.count = event_count_;
    result.worst_phase_spans = worst_phase_spans_;
    for (std::size_t i = 0; i < event_count_; ++i)
        result.events[i] = events_[(event_next_ + kEventCount - event_count_ + i) % kEventCount];
    return result;
}
} // namespace jam2

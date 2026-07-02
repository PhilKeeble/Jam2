#include "audio_ring.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace jam2::audio {

namespace {

void atomic_update_max(std::atomic<std::uint64_t>& target, std::uint64_t value)
{
    std::uint64_t current = target.load(std::memory_order_relaxed);
    while (value > current &&
           !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

std::size_t ms_to_frames(double sample_rate, double ms)
{
    return static_cast<std::size_t>(std::max(1.0, std::ceil(sample_rate * ms / 1000.0)));
}

} // namespace

MonoRingBuffer::MonoRingBuffer(std::size_t capacity_frames)
    : buffer_(capacity_frames), capacity_(capacity_frames)
{
    if (capacity_frames == 0) {
        throw std::runtime_error("audio ring capacity must be positive");
    }
}

std::size_t MonoRingBuffer::available_read() const
{
    const std::uint64_t read = read_.load(std::memory_order_acquire);
    const std::uint64_t write = write_.load(std::memory_order_acquire);
    return static_cast<std::size_t>(write - read);
}

std::size_t MonoRingBuffer::available_write() const
{
    return capacity_ - available_read();
}

std::size_t MonoRingBuffer::push(std::span<const std::int32_t> frames)
{
    const std::uint64_t read = read_.load(std::memory_order_acquire);
    const std::uint64_t write = write_.load(std::memory_order_relaxed);
    const std::size_t used = static_cast<std::size_t>(write - read);
    const std::size_t writable = capacity_ - used;
    const std::size_t count = std::min(writable, frames.size());
    for (std::size_t i = 0; i < count; ++i) {
        buffer_[static_cast<std::size_t>((write + i) % capacity_)] = frames[i];
    }
    if (count < frames.size()) {
        const std::uint64_t dropped = static_cast<std::uint64_t>(frames.size() - count);
        overruns_.fetch_add(dropped, std::memory_order_relaxed);
        if (diagnostics_enabled_.load(std::memory_order_relaxed)) {
            overrun_events_.fetch_add(1, std::memory_order_relaxed);
            atomic_update_max(overrun_event_max_frames_, dropped);
        }
    }
    write_.store(write + count, std::memory_order_release);
    return count;
}

std::size_t MonoRingBuffer::pop(std::span<std::int32_t> frames, bool observe_depth)
{
    const std::uint64_t read = read_.load(std::memory_order_relaxed);
    const std::uint64_t write = write_.load(std::memory_order_acquire);
    const std::size_t readable = static_cast<std::size_t>(write - read);
    const std::size_t count = std::min(readable, frames.size());
    const bool diagnostics_enabled = diagnostics_enabled_.load(std::memory_order_relaxed);
    if (observe_depth && diagnostics_enabled) {
        observe_depth_for_pop(readable, frames.size());
    }
    for (std::size_t i = 0; i < count; ++i) {
        frames[i] = buffer_[static_cast<std::size_t>((read + i) % capacity_)];
    }
    if (count < frames.size()) {
        const std::uint64_t missing = static_cast<std::uint64_t>(frames.size() - count);
        std::fill(frames.begin() + static_cast<std::ptrdiff_t>(count), frames.end(), 0);
        underruns_.fetch_add(missing, std::memory_order_relaxed);
        underrun_events_.fetch_add(1, std::memory_order_relaxed);
        if (diagnostics_enabled) {
            atomic_update_max(underrun_event_max_frames_, missing);
            const std::uint64_t previous_burst =
                underrun_burst_current_frames_.fetch_add(missing, std::memory_order_relaxed);
            if (previous_burst == 0) {
                underrun_burst_events_.fetch_add(1, std::memory_order_relaxed);
            }
            atomic_update_max(underrun_burst_max_frames_, previous_burst + missing);
        }
    } else if (diagnostics_enabled) {
        underrun_burst_current_frames_.store(0, std::memory_order_relaxed);
    }
    read_.store(read + count, std::memory_order_release);
    return count;
}

std::size_t MonoRingBuffer::drop_oldest(std::size_t frames)
{
    const std::uint64_t read = read_.load(std::memory_order_relaxed);
    const std::uint64_t write = write_.load(std::memory_order_acquire);
    const std::size_t readable = static_cast<std::size_t>(write - read);
    const std::size_t count = std::min(readable, frames);
    read_.store(read + count, std::memory_order_release);
    return count;
}

void MonoRingBuffer::set_depth_bucket_thresholds(double sample_rate)
{
    if (sample_rate <= 0.0) {
        return;
    }
    depth_under_2ms_threshold_frames_ = ms_to_frames(sample_rate, 2.0);
    depth_under_5ms_threshold_frames_ = ms_to_frames(sample_rate, 5.0);
    depth_under_10ms_threshold_frames_ = ms_to_frames(sample_rate, 10.0);
}

void MonoRingBuffer::set_diagnostics_enabled(bool enabled)
{
    diagnostics_enabled_.store(enabled, std::memory_order_relaxed);
}

RingStats MonoRingBuffer::stats() const
{
    return RingStats{
        overruns_.load(std::memory_order_relaxed),
        overrun_events_.load(std::memory_order_relaxed),
        overrun_event_max_frames_.load(std::memory_order_relaxed),
        underruns_.load(std::memory_order_relaxed),
        underrun_events_.load(std::memory_order_relaxed),
        underrun_event_max_frames_.load(std::memory_order_relaxed),
        underrun_burst_events_.load(std::memory_order_relaxed),
        underrun_burst_max_frames_.load(std::memory_order_relaxed),
        underrun_burst_current_frames_.load(std::memory_order_relaxed),
        depth_under_2ms_frames_.load(std::memory_order_relaxed),
        depth_under_5ms_frames_.load(std::memory_order_relaxed),
        depth_under_10ms_frames_.load(std::memory_order_relaxed),
        depth_10ms_plus_frames_.load(std::memory_order_relaxed),
        depth_observed_frames_.load(std::memory_order_relaxed),
    };
}

void MonoRingBuffer::reset()
{
    read_.store(0, std::memory_order_release);
    write_.store(0, std::memory_order_release);
    overruns_.store(0, std::memory_order_relaxed);
    overrun_events_.store(0, std::memory_order_relaxed);
    overrun_event_max_frames_.store(0, std::memory_order_relaxed);
    underruns_.store(0, std::memory_order_relaxed);
    underrun_events_.store(0, std::memory_order_relaxed);
    underrun_event_max_frames_.store(0, std::memory_order_relaxed);
    underrun_burst_events_.store(0, std::memory_order_relaxed);
    underrun_burst_max_frames_.store(0, std::memory_order_relaxed);
    underrun_burst_current_frames_.store(0, std::memory_order_relaxed);
    depth_under_2ms_frames_.store(0, std::memory_order_relaxed);
    depth_under_5ms_frames_.store(0, std::memory_order_relaxed);
    depth_under_10ms_frames_.store(0, std::memory_order_relaxed);
    depth_10ms_plus_frames_.store(0, std::memory_order_relaxed);
    depth_observed_frames_.store(0, std::memory_order_relaxed);
}

void MonoRingBuffer::observe_depth_for_pop(std::size_t readable, std::size_t output_frames)
{
    if (output_frames == 0) {
        return;
    }
    const std::uint64_t frames = static_cast<std::uint64_t>(output_frames);
    depth_observed_frames_.fetch_add(frames, std::memory_order_relaxed);
    if (readable < depth_under_2ms_threshold_frames_) {
        depth_under_2ms_frames_.fetch_add(frames, std::memory_order_relaxed);
    } else if (readable < depth_under_5ms_threshold_frames_) {
        depth_under_5ms_frames_.fetch_add(frames, std::memory_order_relaxed);
    } else if (readable < depth_under_10ms_threshold_frames_) {
        depth_under_10ms_frames_.fetch_add(frames, std::memory_order_relaxed);
    } else {
        depth_10ms_plus_frames_.fetch_add(frames, std::memory_order_relaxed);
    }
}

} // namespace jam2::audio

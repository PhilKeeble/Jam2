#include "audio_ring.hpp"

#include <algorithm>
#include <stdexcept>

namespace jam2::audio {

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
        overruns_.fetch_add(frames.size() - count, std::memory_order_relaxed);
    }
    write_.store(write + count, std::memory_order_release);
    return count;
}

std::size_t MonoRingBuffer::pop(std::span<std::int32_t> frames)
{
    const std::uint64_t read = read_.load(std::memory_order_relaxed);
    const std::uint64_t write = write_.load(std::memory_order_acquire);
    const std::size_t readable = static_cast<std::size_t>(write - read);
    const std::size_t count = std::min(readable, frames.size());
    for (std::size_t i = 0; i < count; ++i) {
        frames[i] = buffer_[static_cast<std::size_t>((read + i) % capacity_)];
    }
    if (count < frames.size()) {
        std::fill(frames.begin() + static_cast<std::ptrdiff_t>(count), frames.end(), 0);
        underruns_.fetch_add(frames.size() - count, std::memory_order_relaxed);
        underrun_events_.fetch_add(1, std::memory_order_relaxed);
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

RingStats MonoRingBuffer::stats() const
{
    return RingStats{
        overruns_.load(std::memory_order_relaxed),
        underruns_.load(std::memory_order_relaxed),
        underrun_events_.load(std::memory_order_relaxed),
    };
}

void MonoRingBuffer::reset()
{
    read_.store(0, std::memory_order_release);
    write_.store(0, std::memory_order_release);
    overruns_.store(0, std::memory_order_relaxed);
    underruns_.store(0, std::memory_order_relaxed);
    underrun_events_.store(0, std::memory_order_relaxed);
}

} // namespace jam2::audio

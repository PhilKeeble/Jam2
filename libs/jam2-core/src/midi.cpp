#include "midi.hpp"

#include <algorithm>
#include <limits>

namespace jam2::midi {

bool valid_mpe_zone(const MpeZone& zone) noexcept
{
    return zone.master_channel < 16U &&
        zone.member_begin_channel < 16U &&
        zone.member_end_channel < 16U &&
        zone.member_begin_channel <= zone.member_end_channel &&
        (zone.master_channel < zone.member_begin_channel ||
         zone.master_channel > zone.member_end_channel) &&
        zone.pitch_bend_range_semitones > 0U &&
        zone.pitch_bend_range_semitones <= 96U;
}

bool EventQueue::push(const Event& event) noexcept
{
    const std::uint32_t write = write_.load(std::memory_order_relaxed);
    const std::uint32_t next = (write + 1U) % static_cast<std::uint32_t>(events_.size());
    if (next == read_.load(std::memory_order_acquire)) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    events_[write] = event;
    write_.store(next, std::memory_order_release);
    const auto current_depth = static_cast<std::uint32_t>(depth());
    std::uint32_t high = high_water_.load(std::memory_order_relaxed);
    while (current_depth > high &&
           !high_water_.compare_exchange_weak(
               high, current_depth, std::memory_order_relaxed)) {
    }
    return true;
}

bool EventQueue::pop(Event& event) noexcept
{
    const std::uint32_t read = read_.load(std::memory_order_relaxed);
    if (read == write_.load(std::memory_order_acquire)) {
        return false;
    }
    event = events_[read];
    read_.store(
        (read + 1U) % static_cast<std::uint32_t>(events_.size()),
        std::memory_order_release);
    return true;
}

void EventQueue::clear() noexcept
{
    read_.store(write_.load(std::memory_order_acquire), std::memory_order_release);
}

std::size_t EventQueue::depth() const noexcept
{
    const std::uint32_t write = write_.load(std::memory_order_acquire);
    const std::uint32_t read = read_.load(std::memory_order_acquire);
    return write >= read ? write - read : events_.size() - read + write;
}

std::size_t EventQueue::high_water() const noexcept
{
    return high_water_.load(std::memory_order_relaxed);
}

std::uint64_t EventQueue::dropped() const noexcept
{
    return dropped_.load(std::memory_order_relaxed);
}

BlockResult BlockBuilder::build(
    EventQueue& queue,
    std::uint64_t block_start_us,
    std::uint64_t block_end_us,
    std::uint32_t block_frames,
    std::span<Event> output) noexcept
{
    BlockResult result;
    if (block_frames == 0 || output.empty() || block_end_us <= block_start_us) {
        return result;
    }
    while (result.count < output.size()) {
        Event event;
        if (pending_valid_) {
            event = pending_;
            pending_valid_ = false;
        } else if (!queue.pop(event)) {
            break;
        }
        if (event.monotonic_us >= block_end_us) {
            pending_ = event;
            pending_valid_ = true;
            result.deferred = 1;
            break;
        }
        if (event.monotonic_us <= block_start_us) {
            event.sample_offset = 0;
            if (event.monotonic_us < block_start_us) {
                ++result.late;
            }
        } else {
            const std::uint64_t elapsed = event.monotonic_us - block_start_us;
            const std::uint64_t duration = block_end_us - block_start_us;
            const std::uint64_t offset = elapsed * block_frames / duration;
            event.sample_offset = static_cast<std::uint16_t>(std::min<std::uint64_t>(
                offset, static_cast<std::uint64_t>(block_frames - 1U)));
        }
        output[result.count++] = event;
    }
    return result;
}

} // namespace jam2::midi

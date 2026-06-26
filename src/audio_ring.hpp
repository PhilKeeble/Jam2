#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace jam2::audio {

struct RingStats {
    std::uint64_t overruns = 0;
    std::uint64_t underruns = 0;
};

class MonoRingBuffer {
public:
    explicit MonoRingBuffer(std::size_t capacity_frames);

    MonoRingBuffer(const MonoRingBuffer&) = delete;
    MonoRingBuffer& operator=(const MonoRingBuffer&) = delete;

    std::size_t capacity() const { return capacity_; }
    std::size_t available_read() const;
    std::size_t available_write() const;

    std::size_t push(std::span<const std::int32_t> frames);
    std::size_t pop(std::span<std::int32_t> frames);
    RingStats stats() const;
    void reset();

private:
    std::vector<std::int32_t> buffer_;
    std::size_t capacity_ = 0;
    std::atomic<std::uint64_t> read_{0};
    std::atomic<std::uint64_t> write_{0};
    std::atomic<std::uint64_t> overruns_{0};
    std::atomic<std::uint64_t> underruns_{0};
};

} // namespace jam2::audio

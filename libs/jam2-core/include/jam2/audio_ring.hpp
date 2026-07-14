#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace jam2::audio {

struct RingStats {
    std::uint64_t overruns = 0;
    std::uint64_t overrun_events = 0;
    std::uint64_t overrun_event_max_frames = 0;
    std::uint64_t underruns = 0;
    std::uint64_t underrun_events = 0;
    std::uint64_t underrun_event_max_frames = 0;
    std::uint64_t underrun_burst_events = 0;
    std::uint64_t underrun_burst_max_frames = 0;
    std::uint64_t underrun_burst_current_frames = 0;
    std::uint64_t depth_under_2ms_frames = 0;
    std::uint64_t depth_under_5ms_frames = 0;
    std::uint64_t depth_under_10ms_frames = 0;
    std::uint64_t depth_10ms_plus_frames = 0;
    std::uint64_t depth_observed_frames = 0;
    std::uint64_t drop_requested_frames = 0;
    std::uint64_t drop_applied_frames = 0;
    std::uint64_t drop_coalesced_requests = 0;
    std::uint64_t drop_pending_frames = 0;
    std::uint64_t drop_max_batch_frames = 0;
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
    std::size_t pop(std::span<std::int32_t> frames, bool observe_depth = true);
    void request_drop_oldest(std::size_t frames);
    void set_depth_bucket_thresholds(double sample_rate);
    void set_diagnostics_enabled(bool enabled);
    RingStats stats() const;
    // Used only at a producer/consumer attachment boundary where the consumer
    // is known to be inactive. Preserves cumulative diagnostics.
    std::size_t discard_all() noexcept;
    void reset();

private:
    std::size_t apply_requested_drop(std::uint64_t read, std::uint64_t write);
    void observe_depth_for_pop(std::size_t readable, std::size_t output_frames);

    std::vector<std::int32_t> buffer_;
    std::size_t capacity_ = 0;
    std::size_t depth_under_2ms_threshold_frames_ = 96;
    std::size_t depth_under_5ms_threshold_frames_ = 240;
    std::size_t depth_under_10ms_threshold_frames_ = 480;
    std::atomic<std::uint64_t> read_{0};
    std::atomic<std::uint64_t> write_{0};
    std::atomic<bool> diagnostics_enabled_{false};
    std::atomic<std::uint64_t> overruns_{0};
    std::atomic<std::uint64_t> overrun_events_{0};
    std::atomic<std::uint64_t> overrun_event_max_frames_{0};
    std::atomic<std::uint64_t> underruns_{0};
    std::atomic<std::uint64_t> underrun_events_{0};
    std::atomic<std::uint64_t> underrun_event_max_frames_{0};
    std::atomic<std::uint64_t> underrun_burst_events_{0};
    std::atomic<std::uint64_t> underrun_burst_max_frames_{0};
    std::atomic<std::uint64_t> underrun_burst_current_frames_{0};
    std::atomic<std::uint64_t> depth_under_2ms_frames_{0};
    std::atomic<std::uint64_t> depth_under_5ms_frames_{0};
    std::atomic<std::uint64_t> depth_under_10ms_frames_{0};
    std::atomic<std::uint64_t> depth_10ms_plus_frames_{0};
    std::atomic<std::uint64_t> depth_observed_frames_{0};
    std::atomic<std::uint64_t> drop_requested_frames_{0};
    std::atomic<std::uint64_t> drop_applied_frames_{0};
    std::atomic<std::uint64_t> drop_coalesced_requests_{0};
    std::atomic<std::uint64_t> drop_target_read_{0};
    std::atomic<std::uint64_t> drop_max_batch_frames_{0};
};

} // namespace jam2::audio

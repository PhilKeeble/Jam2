#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace jam2::audio {

struct TrackTakeRecorderStats {
    bool armed = false;
    bool recording = false;
    bool finalized = false;
    std::string take_id;
    std::string output_path;
    std::string last_error;
    int sample_rate = 0;
    std::uint64_t start_frame = 0;
    std::uint64_t stop_frame = 0;
    std::uint64_t frames_queued = 0;
    std::uint64_t frames_written = 0;
    std::uint64_t dropped_frames = 0;
    std::uint64_t drop_events = 0;
    std::uint64_t writer_errors = 0;
    std::size_t queue_depth_frames = 0;
    std::size_t queue_capacity_frames = 0;
};

struct TrackTakeRecorderCompletion {
    bool available = false;
    bool ok = false;
    std::string take_id;
    std::string output_path;
    std::string error;
    int sample_rate = 0;
    std::uint64_t start_frame = 0;
    std::uint64_t stop_frame = 0;
    std::uint64_t frames_written = 0;
    std::uint64_t dropped_frames = 0;
    std::uint64_t writer_errors = 0;
};

class TrackTakeRecorder {
public:
    explicit TrackTakeRecorder(std::size_t queue_capacity_frames = 262144);
    ~TrackTakeRecorder();

    TrackTakeRecorder(const TrackTakeRecorder&) = delete;
    TrackTakeRecorder& operator=(const TrackTakeRecorder&) = delete;

    bool arm(
        std::string take_id,
        const std::filesystem::path& output_path,
        int sample_rate,
        std::string& error);
    bool start_at(std::uint64_t frame, std::string& error);
    bool stop_at(std::uint64_t frame, std::string& error);
    void cancel() noexcept;
    void record(std::uint64_t audio_frame_start, std::span<const std::int32_t> input) noexcept;

    TrackTakeRecorderStats stats() const;
    TrackTakeRecorderCompletion consume_completion();

private:
    struct SampleQueue;
    struct WavWriter;

    void writer_loop() noexcept;
    void reset_queue() noexcept;
    void close_writer() noexcept;
    void finalize_from_audio_thread(std::uint64_t frame) noexcept;
    void join_writer_if_needed();

    const std::size_t queue_capacity_frames_;
    std::unique_ptr<SampleQueue> queue_;
    std::unique_ptr<WavWriter> writer_;
    std::thread writer_thread_;

    std::atomic<bool> armed_{false};
    std::atomic<bool> recording_{false};
    std::atomic<bool> start_requested_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> writer_stop_{false};
    std::atomic<bool> finalized_{false};
    std::atomic<bool> completion_pending_{false};
    std::atomic<bool> canceled_{false};
    std::atomic<std::uint64_t> start_frame_{0};
    std::atomic<std::uint64_t> stop_frame_{0};
    std::atomic<std::uint64_t> frames_queued_{0};
    std::atomic<std::uint64_t> frames_written_{0};
    std::atomic<std::uint64_t> dropped_frames_{0};
    std::atomic<std::uint64_t> drop_events_{0};
    std::atomic<std::uint64_t> writer_errors_{0};

    mutable std::mutex state_mutex_;
    std::string take_id_;
    std::filesystem::path output_path_;
    std::string last_error_;
    int sample_rate_ = 0;
};

} // namespace jam2::audio

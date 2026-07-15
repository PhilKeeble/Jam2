#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>

namespace jam2::audio {

enum class RecordStem : std::size_t {
    Mix = 0,
    MyInput,
    TheirInput,
    InputsMix,
    Metronome,
    Count,
};

struct RecordBlock {
    std::uint64_t audio_frame_start = 0;
    std::span<const std::int32_t> mix;
    std::span<const std::int32_t> my_input;
    std::span<const std::int32_t> their_input;
    std::span<const std::int32_t> inputs_mix;
    std::span<const std::int32_t> metronome;
};

struct OutputRecorderStats {
    bool active = false;
    std::string folder;
    int sample_rate = 0;
    std::size_t queue_capacity_frames = 0;
    std::uint64_t start_audio_frame = 0;
    std::uint64_t stop_audio_frame = 0;
    std::uint64_t frames_queued = 0;
    std::uint64_t frames_written = 0;
    std::uint64_t dropped_frames = 0;
    std::uint64_t drop_events = 0;
    std::uint64_t writer_errors = 0;
    std::size_t queue_depth_frames = 0;
    std::string last_error;
};

struct OutputRecorderSnapshot {
    bool active = false;
    std::size_t queue_capacity_frames = 0;
    std::uint64_t start_audio_frame = 0;
    std::uint64_t stop_audio_frame = 0;
    std::uint64_t frames_queued = 0;
    std::uint64_t frames_written = 0;
    std::uint64_t dropped_frames = 0;
    std::uint64_t drop_events = 0;
    std::uint64_t writer_errors = 0;
    std::size_t queue_depth_frames = 0;
};

class OutputRecorder {
public:
    explicit OutputRecorder(std::size_t queue_capacity_frames = 262144);
    ~OutputRecorder();

    OutputRecorder(const OutputRecorder&) = delete;
    OutputRecorder& operator=(const OutputRecorder&) = delete;

    bool start(const std::filesystem::path& folder, int sample_rate, std::string& error);
    bool stop(std::string& error);
    void record(const RecordBlock& block) noexcept;
    OutputRecorderSnapshot snapshot() const noexcept;
    OutputRecorderStats stats() const;

    static constexpr std::array<const char*, static_cast<std::size_t>(RecordStem::Count)> stem_file_names{
        "mix.wav",
        "my-input.wav",
        "their-input.wav",
        "inputs-mix.wav",
        "metronome.wav",
    };

private:
    struct StemQueue;
    struct WavWriter;

    void writer_loop() noexcept;
    void close_writers() noexcept;
    void reset_queues();

    std::array<std::unique_ptr<StemQueue>, static_cast<std::size_t>(RecordStem::Count)> queues_;
    std::array<std::unique_ptr<WavWriter>, static_cast<std::size_t>(RecordStem::Count)> writers_;
    const std::size_t queue_capacity_frames_;
    std::thread writer_thread_;
    std::atomic<bool> active_{false};
    std::atomic<bool> writer_stop_{false};
    std::atomic<bool> first_frame_set_{false};
    std::atomic<std::uint64_t> start_audio_frame_{0};
    std::atomic<std::uint64_t> stop_audio_frame_{0};
    std::atomic<std::uint64_t> frames_queued_{0};
    std::atomic<std::uint64_t> frames_written_{0};
    std::atomic<std::uint64_t> dropped_frames_{0};
    std::atomic<std::uint64_t> drop_events_{0};
    std::atomic<std::uint64_t> writer_errors_{0};
    std::filesystem::path folder_;
    int sample_rate_ = 0;
    std::string last_error_;
    mutable std::mutex error_mutex_;
};

} // namespace jam2::audio

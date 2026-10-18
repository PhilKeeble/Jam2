#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace jam2::audio {
struct StreamControl;
// Shared by CoreAudio, ASIO and the deterministic backend. Device calibration
// is separate from plugin processing delay and explicit manual adjustment.
void set_recording_calibration(StreamControl& control, std::size_t channel, std::int64_t adjustment) noexcept;
std::int64_t applied_recording_calibration_frames(const StreamControl& control) noexcept;
std::uint64_t input_take_compensation_frames(const StreamControl& control) noexcept;

class RecordingCalibrationCapture final {
public:
    enum class State { Idle, Requested, Capturing, Complete, Analysing, Finished, Cancelled };
    struct Result {
        bool ok = false;
        std::string error;
        double measured_frames = 0;
        double reported_frames = 0;
        double spread_frames = 0;
        std::int64_t adjustment_frames = 0;
        int detected_clicks = 0;
        double peak = 0;
    };
    explicit RecordingCalibrationCapture(int sample_rate);
    bool start(std::size_t input_channel) noexcept;
    void cancel() noexcept;
    State state() const noexcept { return state_.load(std::memory_order_acquire); }
    std::size_t input_channel() const noexcept { return channel_.load(std::memory_order_relaxed); }
    // Device callback only: raw physical input before plugins, then final output
    // after the normal mix. Storage and test signal are allocated off that path.
    void capture_input(std::span<const std::int32_t> input) noexcept;
    void render(std::span<std::int32_t> output, std::uint64_t reported_frames) noexcept;
    Result analyse(); // non-real-time worker only
private:
    int rate_;
    std::vector<float> input_, reference_;
    std::vector<std::size_t> clicks_;
    std::atomic<State> state_{State::Idle};
    std::atomic<std::size_t> channel_{0};
    std::atomic<bool> cancelled_{false};
    std::size_t cursor_ = 0, captured_frames_ = 0;
    std::uint64_t reported_sum_ = 0, reported_count_ = 0;
    std::uint64_t reported_min_ = 0, reported_max_ = 0;
};
} // namespace jam2::audio

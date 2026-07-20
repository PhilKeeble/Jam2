#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "audio_ring.hpp"
#include "metronome.hpp"
#include "output_recorder.hpp"
#include "prepared_track_source.hpp"
#include "track_take_recorder.hpp"

namespace jam2::audio {

// Selected channels whose block peak is more than 30 dB below the loudest
// selected channel are treated as inactive for that block. Active channels
// are averaged, preserving a mono source present on only one selected input
// without adding gain to genuinely active stereo material.
inline constexpr double kMonoDownmixRelativeActivity = 0.03162277660168379;

inline bool mono_downmix_channel_active(double peak, double loudestPeak) noexcept
{
    return loudestPeak > 0.0 &&
        peak >= loudestPeak * kMonoDownmixRelativeActivity;
}

inline std::size_t mono_downmix_active_channels(std::span<const double> peaks) noexcept
{
    double loudest = 0.0;
    for (double peak : peaks) {
        loudest = std::max(loudest, std::abs(peak));
    }
    std::size_t active = 0;
    for (double peak : peaks) {
        if (mono_downmix_channel_active(std::abs(peak), loudest)) {
            ++active;
        }
    }
    return active;
}

class PlaybackRatioSmoother final {
public:
    void reset() noexcept
    {
        currentPpm_ = 1000000.0;
        stepPpm_ = 0.0;
        remainingFrames_ = 0;
        targetPpm_ = 1000000;
    }

    void setTargetPpm(int targetPpm, std::uint64_t rampFrames) noexcept
    {
        targetPpm = std::clamp(targetPpm, 500000, 2000000);
        if (targetPpm == targetPpm_) {
            return;
        }
        targetPpm_ = targetPpm;
        if (rampFrames == 0) {
            currentPpm_ = static_cast<double>(targetPpm_);
            stepPpm_ = 0.0;
            remainingFrames_ = 0;
            return;
        }
        remainingFrames_ = rampFrames;
        stepPpm_ = (static_cast<double>(targetPpm_) - currentPpm_) /
            static_cast<double>(remainingFrames_);
    }

    double nextRatio() noexcept
    {
        if (remainingFrames_ > 0) {
            currentPpm_ += stepPpm_;
            --remainingFrames_;
            if (remainingFrames_ == 0) {
                currentPpm_ = static_cast<double>(targetPpm_);
                stepPpm_ = 0.0;
            }
        }
        return currentPpm_ / 1000000.0;
    }

    bool steadyUnity() const noexcept
    {
        return remainingFrames_ == 0 && targetPpm_ == 1000000 &&
            std::abs(currentPpm_ - 1000000.0) < 0.5;
    }

    bool ramping() const noexcept { return remainingFrames_ != 0; }

    int appliedPpm() const noexcept
    {
        return std::clamp(
            static_cast<int>(std::llround(currentPpm_)), 500000, 2000000);
    }

private:
    double currentPpm_ = 1000000.0;
    double stepPpm_ = 0.0;
    std::uint64_t remainingFrames_ = 0;
    int targetPpm_ = 1000000;
};

struct DeviceInfo {
    int id = 0;
    std::string backend;
    std::string name;
    std::string clsid;
    std::string driver_path;
};

struct DeviceTestResult {
    DeviceInfo device;
    double current_sample_rate = 0.0;
    std::array<bool, 2> sample_rate_supported{};
    std::array<bool, 4> buffer_size_supported{};
};

inline constexpr std::array<int, 2> kTestSampleRates{44100, 48000};
inline constexpr std::array<long, 4> kTestBufferSizes{32, 64, 128, 256};

struct MetronomeConfig {
    bool enabled = false;
    int bpm = 120;
    double level = 0.20;
};

struct StreamControl {
    std::atomic<bool> metronome_enabled{false};
    std::atomic<int> metronome_bpm{120};
    std::atomic<int> metronome_beats_per_bar{4};
    std::atomic<int> metronome_division{1};
    std::atomic<int> metronome_step_count{4};
    std::atomic<std::uint64_t> metronome_play_mask_low{0x0fULL};
    std::atomic<std::uint64_t> metronome_play_mask_high{0};
    std::atomic<std::uint64_t> metronome_accent_mask_low{0x01ULL};
    std::atomic<std::uint64_t> metronome_accent_mask_high{0};
    std::atomic<int> metronome_level_ppm{200000};
    std::atomic<int> remote_level_ppm{1000000};
    std::atomic<bool> local_monitor_enabled{false};
    std::atomic<int> local_monitor_level_ppm{250000};
    std::atomic<int> send_level_ppm{1000000};
    std::atomic<int> output_level_ppm{1000000};
    std::atomic<int> playback_ratio_ppm{1000000};
    std::atomic<int> playback_ratio_applied_ppm{1000000};
    std::atomic<bool> playback_ratio_ramping{false};
    std::atomic<std::uint64_t> playback_ratio_ramp_frames{0};
    std::atomic<int> metronome_mode{0};
    std::atomic<bool> leader_audio_local_click{false};
    std::atomic<std::uint64_t> metronome_epoch_sample_time{0};
    std::atomic<bool> metronome_epoch_valid{false};
    std::atomic<std::int64_t> metronome_render_offset_frames{0};
    std::atomic<bool> recording_count_in_active{false};
    std::atomic<std::uint64_t> recording_count_in_start_frame{0};
    std::atomic<std::uint64_t> recording_count_in_target_frame{0};
    std::atomic<std::uint64_t> engine_frame_counter{0};
    // The callback is the capture-ring producer and the only side allowed to
    // reset it. A network attachment publishes a generation; the callback
    // acknowledges that generation at a frame boundary, discards stale local
    // data, and publishes the first authoritative capture frame.
    std::atomic<bool> network_capture_requested_enabled{false};
    std::atomic<bool> network_capture_enabled{false};
    std::atomic<std::uint64_t> network_capture_generation_requested{0};
    std::atomic<std::uint64_t> network_capture_generation_applied{0};
    std::atomic<std::uint64_t> network_capture_epoch_frame{0};
    std::atomic<std::uint64_t> network_capture_stale_frames_discarded{0};
    std::atomic<bool> network_playback_enabled{false};
    std::atomic<bool> network_playback_enabled_applied{false};
    std::atomic<bool> pitch_analysis_enabled{false};
    std::atomic<std::uint32_t> input_latency_frames{0};
    std::atomic<std::uint32_t> output_latency_frames{0};
    std::atomic<std::int64_t> recording_latency_adjustment_frames{0};
    std::atomic<std::uint64_t> recording_latency_compensation_frames{0};
    std::atomic<int> test_input_mode{0};
    std::atomic<int> test_input_level_ppm{125000};
    std::atomic<int> input_peak_ppm{0};
    std::atomic<int> send_peak_ppm{0};
    std::atomic<int> monitor_peak_ppm{0};
    std::atomic<int> remote_peak_ppm{0};
    std::atomic<int> prepared_track_peak_ppm{0};
    std::atomic<int> metronome_peak_ppm{0};
    std::atomic<int> output_peak_ppm{0};
    std::atomic<int> gui_input_peak_ppm{0};
    std::atomic<int> gui_send_peak_ppm{0};
    std::atomic<int> gui_monitor_peak_ppm{0};
    std::atomic<int> gui_remote_peak_ppm{0};
    std::atomic<int> gui_prepared_track_peak_ppm{0};
    std::atomic<int> gui_metronome_peak_ppm{0};
    std::atomic<int> gui_output_peak_ppm{0};
    std::atomic<std::uint64_t> output_clipped_samples{0};
    PreparedTrackSource* prepared_source = nullptr;
    std::atomic<std::uint64_t> prepared_source_frame{0};
    std::atomic<std::uint64_t> prepared_source_scheduled_start_frame{0};
    std::atomic<std::uint64_t> prepared_source_actual_start_frame{0};
    std::atomic<std::uint64_t> prepared_source_underruns{0};
    std::atomic<std::uint64_t> prepared_source_busy_events{0};
};

enum class InputChannels {
    Mono = 1,
    Stereo = 2,
};

struct ChannelSelection {
    std::vector<int> input{0};
    std::vector<int> output{0, 1};
};

struct StreamInfo {
    DeviceInfo device;
    double sample_rate = 0.0;
    long buffer_size = 0;
    long input_latency_frames = 0;
    long output_latency_frames = 0;
    InputChannels input_channels = InputChannels::Mono;
    ChannelSelection channels;
    std::string sample_format;
};

struct CallbackTimingStats {
    std::uint64_t interval_min_us = 0;
    std::uint64_t interval_sum_us = 0;
    std::uint64_t interval_max_us = 0;
    std::uint64_t interval_samples = 0;
    std::uint64_t gap_over_1_1x_count = 0;
    std::uint64_t gap_over_1_5x_count = 0;
    std::uint64_t gap_over_2x_count = 0;
};

class DeviceStream {
public:
    virtual ~DeviceStream() = default;
    DeviceStream(const DeviceStream&) = delete;
    DeviceStream& operator=(const DeviceStream&) = delete;

    virtual long callbacks() const = 0;
    virtual bool playback_prefilled() const = 0;
    virtual StreamInfo info() const = 0;
    virtual CallbackTimingStats callback_timing_stats() const = 0;

protected:
    DeviceStream() = default;
};

std::vector<DeviceInfo> list_devices();
DeviceTestResult test_device(int id);
std::unique_ptr<DeviceStream> start_duplex_stream(
    int id,
    double requested_sample_rate,
    long buffer_size,
    InputChannels input_channels,
    ChannelSelection channels,
    MonoRingBuffer& capture_ring,
    MonoRingBuffer& pitch_ring,
    MonoRingBuffer& playback_ring,
    std::size_t playback_prefill_frames,
    StreamControl& control,
    OutputRecorder* recorder,
    TrackTakeRecorder* track_take_recorder = nullptr);

// Called once per device callback before capture is pushed. This is fixed-work,
// allocation-free, lock-free, and is shared by real and headless devices.
bool prepare_network_capture_callback(
    StreamControl& control,
    MonoRingBuffer& capture_ring,
    std::uint64_t callback_frame) noexcept;

inline void push_pitch_analysis_callback(
    StreamControl& control,
    MonoRingBuffer& pitch_ring,
    std::span<const std::int32_t> input) noexcept
{
    if (control.pitch_analysis_enabled.load(std::memory_order_acquire)) {
        (void)pitch_ring.push(input);
    }
}

} // namespace jam2::audio

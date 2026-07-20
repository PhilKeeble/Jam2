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

// A binary active-channel decision at each audio block creates a discontinuity
// whenever the divisor changes. This stateful mixer instead gives each selected
// input its own slowly adapting noise floor and ramps channel weights sample by
// sample. Its normalized weighted average preserves unity gain for one sounding
// channel and cannot exceed the selected input sample range.
class SmoothedMonoDownmix final {
public:
    // These relative constants remain the policy for the offline block downmix.
    static constexpr double kRelativeOpen = 0.03162277660168379; // -30 dB
    static constexpr double kRelativeClose = 0.01584893192461114; // -36 dB
    static constexpr double kAbsoluteOpen = 0.000251188643150958; // -72 dBFS
    static constexpr double kAbsoluteClose = 0.000125892541179417; // -78 dBFS
    static constexpr double kInitialNoiseFloor = 0.000251188643150958; // -72 dBFS peak
    static constexpr double kMinimumNoiseFloor = 0.000015848931924611; // -96 dBFS peak
    static constexpr double kMaximumNoiseFloor = 0.003981071705534969; // -48 dBFS peak
    static constexpr double kNoiseOpenRatio = 5.011872336272722; // +14 dB
    static constexpr double kNoiseCloseRatio = 2.51188643150958; // +8 dB
    static constexpr double kWeightAttackSeconds = 0.004;
    static constexpr double kWeightReleaseSeconds = 0.045;
    static constexpr double kEnvelopeReleaseSeconds = 0.035;
    static constexpr double kNoiseFloorRiseSeconds = 0.750;
    static constexpr double kNoiseFloorFallSeconds = 3.000;

    void configure(std::size_t channels, double sampleRate, std::size_t blockFrames)
    {
        const double safeRate = sampleRate > 1.0 ? sampleRate : 48000.0;
        blockFrames_ = std::max<std::size_t>(blockFrames, 1U);
        const double blockSeconds = static_cast<double>(blockFrames_) / safeRate;
        weightAttack_ = 1.0 - std::exp(-blockSeconds / kWeightAttackSeconds);
        weightRelease_ = 1.0 - std::exp(-blockSeconds / kWeightReleaseSeconds);
        envelopeRelease_ = 1.0 - std::exp(-blockSeconds / kEnvelopeReleaseSeconds);
        noiseFloorRise_ = 1.0 - std::exp(-blockSeconds / kNoiseFloorRiseSeconds);
        noiseFloorFall_ = 1.0 - std::exp(-blockSeconds / kNoiseFloorFallSeconds);
        envelopes_.assign(channels, 0.0);
        noiseFloors_.assign(channels, kInitialNoiseFloor);
        weights_.assign(channels, 0.0);
        blockStarts_.assign(channels, 0.0);
        blockSteps_.assign(channels, 0.0);
        active_.assign(channels, 0U);
        blockStartWeightSum_ = 0.0;
        blockWeightStepSum_ = 0.0;
        effectiveWeight_ = 0.0;
        normalizationGain_ = 1.0;
        transitionCount_ = 0;
        maxGainChangePerBlock_ = 0.0;
    }

    void beginBlock(std::span<const double> peaks) noexcept
    {
        if (peaks.size() != weights_.size() || weights_.empty()) {
            return;
        }
        for (std::size_t channel = 0; channel < peaks.size(); ++channel) {
            const double peak = std::isfinite(peaks[channel]) ?
                std::abs(peaks[channel]) : 0.0;
            double& envelope = envelopes_[channel];
            envelope = peak >= envelope ?
                peak : envelope + (peak - envelope) * envelopeRelease_;
        }

        blockStartWeightSum_ = 0.0;
        double endWeightSum = 0.0;
        blockWeightStepSum_ = 0.0;
        for (std::size_t channel = 0; channel < weights_.size(); ++channel) {
            const bool wasActive = active_[channel] != 0U;
            const double envelope = envelopes_[channel];
            const double noiseFloor = noiseFloors_[channel];
            const double openThreshold = std::max(
                kAbsoluteOpen, noiseFloor * kNoiseOpenRatio);
            const double closeThreshold = std::max(
                kAbsoluteClose, noiseFloor * kNoiseCloseRatio);
            const bool active = wasActive ?
                envelope >= closeThreshold :
                envelope >= openThreshold;
            if (active != wasActive) {
                active_[channel] = active ? 1U : 0U;
                ++transitionCount_;
            }

            // A sounding channel never teaches the estimator that a held note
            // is its noise floor. Closed channels track persistent interface or
            // microphone noise without coupling their decision to louder inputs.
            if (!active) {
                const double peak = std::isfinite(peaks[channel]) ?
                    std::abs(peaks[channel]) : 0.0;
                const double targetFloor = std::clamp(
                    peak, kMinimumNoiseFloor, kMaximumNoiseFloor);
                const double coefficient = targetFloor > noiseFloor ?
                    noiseFloorRise_ : noiseFloorFall_;
                noiseFloors_[channel] = std::clamp(
                    noiseFloor + (targetFloor - noiseFloor) * coefficient,
                    kMinimumNoiseFloor,
                    kMaximumNoiseFloor);
            }

            const double start = weights_[channel];
            const double target = active ? 1.0 : 0.0;
            const double coefficient = target > start ? weightAttack_ : weightRelease_;
            const double end = std::clamp(
                start + (target - start) * coefficient, 0.0, 1.0);
            blockStarts_[channel] = start;
            blockSteps_[channel] = (end - start) / static_cast<double>(blockFrames_);
            weights_[channel] = end;
            blockStartWeightSum_ += start;
            endWeightSum += end;
            blockWeightStepSum_ += blockSteps_[channel];
        }

        const double startGain = 1.0 / std::max(1.0, blockStartWeightSum_);
        normalizationGain_ = 1.0 / std::max(1.0, endWeightSum);
        maxGainChangePerBlock_ = std::max(
            maxGainChangePerBlock_, std::abs(normalizationGain_ - startGain));
        effectiveWeight_ = endWeightSum;
    }

    double weightAt(std::size_t channel, std::size_t frame) const noexcept
    {
        if (channel >= blockStarts_.size()) {
            return 0.0;
        }
        const double offset = static_cast<double>(
            std::min(frame + 1U, blockFrames_));
        return std::clamp(
            blockStarts_[channel] + blockSteps_[channel] * offset,
            0.0,
            1.0);
    }

    double normalizationAt(std::size_t frame) const noexcept
    {
        const double offset = static_cast<double>(
            std::min(frame + 1U, blockFrames_));
        const double weightSum = blockStartWeightSum_ + blockWeightStepSum_ * offset;
        return 1.0 / std::max(1.0, weightSum);
    }

    std::size_t channelCount() const noexcept { return weights_.size(); }
    double effectiveWeight() const noexcept { return effectiveWeight_; }
    double normalizationGain() const noexcept { return normalizationGain_; }
    std::uint64_t transitionCount() const noexcept { return transitionCount_; }
    double maxGainChangePerBlock() const noexcept { return maxGainChangePerBlock_; }
    double channelWeight(std::size_t channel) const noexcept
    {
        return channel < weights_.size() ? weights_[channel] : 0.0;
    }
    double channelNoiseFloor(std::size_t channel) const noexcept
    {
        return channel < noiseFloors_.size() ? noiseFloors_[channel] : 0.0;
    }

private:
    std::vector<double> envelopes_;
    std::vector<double> noiseFloors_;
    std::vector<double> weights_;
    std::vector<double> blockStarts_;
    std::vector<double> blockSteps_;
    std::vector<std::uint8_t> active_;
    std::size_t blockFrames_ = 1;
    double weightAttack_ = 1.0;
    double weightRelease_ = 1.0;
    double envelopeRelease_ = 1.0;
    double noiseFloorRise_ = 1.0;
    double noiseFloorFall_ = 1.0;
    double blockStartWeightSum_ = 0.0;
    double blockWeightStepSum_ = 0.0;
    double effectiveWeight_ = 0.0;
    double normalizationGain_ = 1.0;
    std::uint64_t transitionCount_ = 0;
    double maxGainChangePerBlock_ = 0.0;
};

// Offline system-loopback capture retains its existing block fold-down. The
// real-time device input path must use SmoothedMonoDownmix instead.
inline bool block_downmix_channel_active(double peak, double loudestPeak) noexcept
{
    return loudestPeak > 0.0 &&
        peak >= loudestPeak * SmoothedMonoDownmix::kRelativeOpen;
}

inline std::size_t block_downmix_active_channels(
    std::span<const double> peaks) noexcept
{
    double loudest = 0.0;
    for (double peak : peaks) {
        loudest = std::max(loudest, std::abs(peak));
    }
    std::size_t active = 0;
    for (double peak : peaks) {
        if (block_downmix_channel_active(std::abs(peak), loudest)) {
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
    std::atomic<int> input_downmix_selected_channels{1};
    std::atomic<int> input_downmix_effective_weight_ppm{1000000};
    std::atomic<int> input_downmix_normalization_gain_ppm{1000000};
    std::atomic<std::uint64_t> input_downmix_transition_count{0};
    std::atomic<int> input_downmix_max_gain_change_per_block_ppm{0};
    std::array<std::atomic<int>, 4> input_downmix_channel_weight_ppm{
        std::atomic<int>{1000000},
        std::atomic<int>{0},
        std::atomic<int>{0},
        std::atomic<int>{0},
    };
    std::array<std::atomic<int>, 4> input_downmix_channel_noise_floor_ppm{
        std::atomic<int>{251},
        std::atomic<int>{251},
        std::atomic<int>{251},
        std::atomic<int>{251},
    };
    PreparedTrackSource* prepared_source = nullptr;
    std::atomic<std::uint64_t> prepared_source_frame{0};
    std::atomic<std::uint64_t> prepared_source_scheduled_start_frame{0};
    std::atomic<std::uint64_t> prepared_source_actual_start_frame{0};
    std::atomic<std::uint64_t> prepared_source_underruns{0};
    std::atomic<std::uint64_t> prepared_source_busy_events{0};
};

inline int downmix_unit_to_ppm(double value) noexcept
{
    return std::clamp(
        static_cast<int>(std::llround(value * 1000000.0)),
        0,
        4000000);
}

inline void publish_downmix_diagnostics(
    StreamControl& control,
    const SmoothedMonoDownmix& downmix) noexcept
{
    control.input_downmix_selected_channels.store(
        static_cast<int>(downmix.channelCount()), std::memory_order_relaxed);
    control.input_downmix_effective_weight_ppm.store(
        downmix_unit_to_ppm(downmix.effectiveWeight()), std::memory_order_relaxed);
    control.input_downmix_normalization_gain_ppm.store(
        downmix_unit_to_ppm(downmix.normalizationGain()), std::memory_order_relaxed);
    control.input_downmix_transition_count.store(
        downmix.transitionCount(), std::memory_order_relaxed);
    control.input_downmix_max_gain_change_per_block_ppm.store(
        downmix_unit_to_ppm(downmix.maxGainChangePerBlock()), std::memory_order_relaxed);
    for (std::size_t channel = 0;
         channel < control.input_downmix_channel_weight_ppm.size();
         ++channel) {
        control.input_downmix_channel_weight_ppm[channel].store(
            downmix_unit_to_ppm(downmix.channelWeight(channel)),
            std::memory_order_relaxed);
        control.input_downmix_channel_noise_floor_ppm[channel].store(
            downmix_unit_to_ppm(downmix.channelNoiseFloor(channel)),
            std::memory_order_relaxed);
    }
}

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

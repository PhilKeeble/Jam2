#pragma once

#include "audio_device.hpp"

#include <span>
#include <vector>

namespace jam2::audio::wasapi {

enum class SampleEncoding : std::uint8_t { Float32, Pcm16, Pcm24, Pcm32 };

struct SampleFormat {
    SampleEncoding encoding = SampleEncoding::Float32;
    std::size_t channels = 0;
    unsigned valid_bits = 32;
    std::uint32_t channel_mask = 0;
    std::size_t bytes_per_sample() const noexcept;
    std::size_t frame_bytes() const noexcept { return channels * bytes_per_sample(); }
    bool valid() const noexcept;
};

std::int32_t decode_sample(const std::uint8_t* data, SampleFormat format) noexcept;
// Mono to front L/R (or the single mono channel), every other channel silent.
void encode_output(std::span<const std::int32_t> mono, SampleFormat format,
    std::span<std::uint8_t> output) noexcept;

// Used by the live WASAPI thread. Single owner; no allocation after configure.
// Windows performs nominal sample-rate conversion; this bridge only corrects
// the remaining independent capture/render oscillator difference.
class CaptureBuffer final {
public:
    void configure(std::size_t channels, int rate, const WasapiConfig& config);
    void push(std::span<const std::uint8_t> input, std::size_t frames,
        SampleFormat format, bool silent, bool discontinuity,
        std::uint64_t firstFrameTimeUs = 0) noexcept;
    void pull(std::span<std::int32_t* const> output, std::size_t frames) noexcept;
    void reset() noexcept;
    DeviceStreamStatus status() const noexcept;
    std::size_t target_frames() const noexcept { return target_; }
    std::size_t capacity_frames() const noexcept { return capacity_; }
    std::uint64_t source_time_us() const noexcept { return sourceTimeUs_; }
private:
    std::vector<std::int32_t> samples_;
    std::vector<std::uint64_t> times_;
    std::size_t channels_ = 0, capacity_ = 0, target_ = 0;
    std::size_t read_ = 0, depth_ = 0;
    int rate_ = 48000, maximumPpm_ = 2000, appliedPpm_ = 0;
    double phase_ = 0, filteredError_ = 0;
    bool started_ = false;
    std::uint64_t underruns_ = 0, overruns_ = 0, resets_ = 0, discontinuities_ = 0;
    std::uint64_t sourceTimeUs_ = 0;
};

} // namespace jam2::audio::wasapi

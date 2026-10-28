#include "wasapi_processing.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace jam2::audio::wasapi {

std::size_t SampleFormat::bytes_per_sample() const noexcept
{
    switch (encoding) {
    case SampleEncoding::Float32: case SampleEncoding::Pcm32: return 4;
    case SampleEncoding::Pcm16: return 2;
    case SampleEncoding::Pcm24: return 3;
    }
    return 0;
}

bool SampleFormat::valid() const noexcept
{
    const auto bits = bytes_per_sample() * 8;
    return channels > 0 && channels <= 64 && bits > 0 && valid_bits > 0 &&
        valid_bits <= bits && (encoding != SampleEncoding::Float32 || valid_bits == 32);
}

std::int32_t decode_sample(const std::uint8_t* data, SampleFormat format) noexcept
{
    if (!data || !format.valid()) return 0;
    if (format.encoding == SampleEncoding::Float32) {
        float value = 0;
        std::memcpy(&value, data, sizeof(value));
        if (!std::isfinite(value)) return 0;
        return static_cast<std::int32_t>(std::clamp(
            static_cast<double>(value) * 2147483648., -2147483648., 2147483647.));
    }
    const auto bytes = format.bytes_per_sample();
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < bytes; ++i) value |= std::uint32_t(data[i]) << (i * 8);
    // WAVEFORMATEXTENSIBLE PCM valid bits are left-aligned in their container.
    value <<= (4 - bytes) * 8;
    const unsigned padding = 32 - format.valid_bits;
    if (padding) value &= ~((std::uint32_t{1} << padding) - 1);
    return std::bit_cast<std::int32_t>(value);
}

void encode_output(std::span<const std::int32_t> mono, SampleFormat format,
    std::span<std::uint8_t> output) noexcept
{
    std::fill(output.begin(), output.end(), std::uint8_t{0});
    if (!format.valid() || mono.size() > output.size() / format.frame_bytes()) return;
    const auto bytes = format.bytes_per_sample();
    for (std::size_t frame = 0; frame < mono.size(); ++frame) {
        for (std::size_t channel = 0; channel < format.channels; ++channel) {
            bool front = format.channels == 1;
            if (!front && format.channel_mask == 0) front = channel < 2;
            if (!front && format.channel_mask != 0) {
                unsigned ordinal = 0;
                for (unsigned bit = 0; bit < 32; ++bit) {
                    if ((format.channel_mask & (std::uint32_t{1} << bit)) == 0) continue;
                    if (ordinal++ == channel) { front = bit == 0 || bit == 1; break; }
                }
            }
            if (!front) continue;
            auto* sample = output.data() + (frame * format.channels + channel) * bytes;
            if (format.encoding == SampleEncoding::Float32) {
                const auto value = static_cast<float>(mono[frame] / 2147483648.);
                std::memcpy(sample, &value, sizeof(value));
            } else {
                auto value = std::bit_cast<std::uint32_t>(mono[frame]);
                const unsigned padding = 32 - format.valid_bits;
                if (padding) value &= ~((std::uint32_t{1} << padding) - 1);
                value >>= (4 - bytes) * 8;
                for (std::size_t i = 0; i < bytes; ++i)
                    sample[i] = static_cast<std::uint8_t>(value >> (i * 8));
            }
        }
    }
}

void CaptureBuffer::configure(std::size_t channels, int rate, const WasapiConfig& config)
{
    if (channels == 0 || channels > 64 || rate < 8000 || rate > 384000 ||
        config.capture_target_ms < 1 || config.capture_target_ms > 500 ||
        config.capture_capacity_ms < config.capture_target_ms * 2 ||
        config.capture_capacity_ms > 2000 || config.capture_max_correction_ppm < 0 ||
        config.capture_max_correction_ppm > 5000)
        throw std::invalid_argument("invalid WASAPI capture buffer configuration");
    channels_ = channels; rate_ = rate; maximumPpm_ = config.capture_max_correction_ppm;
    target_ = static_cast<std::size_t>(rate) * config.capture_target_ms / 1000;
    capacity_ = static_cast<std::size_t>(rate) * config.capture_capacity_ms / 1000;
    if (capacity_ * channels > 8U * 1024U * 1024U)
        throw std::invalid_argument("WASAPI capture buffer exceeds 32 MiB");
    samples_.assign(capacity_ * channels_, 0);
    times_.assign(capacity_, 0);
    underruns_ = overruns_ = resets_ = discontinuities_ = 0;
    reset(); resets_ = 0;
}

void CaptureBuffer::reset() noexcept
{
    read_ = depth_ = 0; phase_ = filteredError_ = 0; appliedPpm_ = 0; started_ = false;
    sourceTimeUs_ = 0;
    ++resets_;
}

void CaptureBuffer::push(std::span<const std::uint8_t> input, std::size_t frames,
    SampleFormat format, bool silent, bool discontinuity, std::uint64_t firstFrameTimeUs) noexcept
{
    if (!capacity_ || format.channels != channels_ || !format.valid() ||
        (!silent && frames > input.size() / format.frame_bytes())) return;
    if (discontinuity) { ++discontinuities_; reset(); }
    const auto skip = frames > capacity_ ? frames - capacity_ : 0;
    frames -= skip;
    if (skip) { overruns_ += skip + depth_; reset(); }
    if (frames > capacity_ - depth_) {
        const auto drop = frames - (capacity_ - depth_);
        read_ = (read_ + drop) % capacity_; depth_ -= drop;
        overruns_ += drop; phase_ = 0;
    }
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto index = ((read_ + depth_ + frame) % capacity_) * channels_;
        times_[index / channels_] = firstFrameTimeUs == 0 ? 0 :
            firstFrameTimeUs + (frame + skip) * 1000000ULL / static_cast<std::uint64_t>(rate_);
        for (std::size_t channel = 0; channel < channels_; ++channel)
            samples_[index + channel] = silent ? 0 : decode_sample(input.data() +
                ((frame + skip) * channels_ + channel) * format.bytes_per_sample(), format);
    }
    depth_ += frames;
}

void CaptureBuffer::pull(std::span<std::int32_t* const> output, std::size_t frames) noexcept
{
    if (output.size() != channels_ || !capacity_) return;
    for (auto* channel : output) if (!channel) return;
    // The queue is sampled at render service boundaries. Low-pass its occupancy
    // so capture packet batching is not mistaken for oscillator drift.
    const double smoothing = std::min(1., static_cast<double>(frames) / rate_);
    filteredError_ += smoothing * (static_cast<double>(depth_) - target_ - filteredError_);
    appliedPpm_ = static_cast<int>(std::clamp(filteredError_ * 10000. / target_,
        -static_cast<double>(maximumPpm_), static_cast<double>(maximumPpm_)));
    if (!started_ && depth_ >= target_ + 2) started_ = true;
    sourceTimeUs_ = started_ && depth_ >= 2 ? times_[read_] : 0;
    const auto ratio = 1. + appliedPpm_ / 1000000.;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        if (!started_ || depth_ < 2) {
            for (auto* channel : output) channel[frame] = 0;
            ++underruns_; started_ = false;
            continue;
        }
        const auto first = read_ * channels_, second = ((read_ + 1) % capacity_) * channels_;
        for (std::size_t channel = 0; channel < channels_; ++channel) {
            const double a = samples_[first + channel], b = samples_[second + channel];
            output[channel][frame] = static_cast<std::int32_t>(std::clamp(
                a + (b - a) * phase_, -2147483648., 2147483647.));
        }
        phase_ += ratio;
        const auto advance = std::min(static_cast<std::size_t>(phase_), depth_);
        phase_ -= advance; read_ = (read_ + advance) % capacity_; depth_ -= advance;
    }
}

DeviceStreamStatus CaptureBuffer::status() const noexcept
{
    DeviceStreamStatus result;
    result.capture_discontinuities = discontinuities_;
    result.capture_underrun_frames = underruns_; result.capture_overrun_frames = overruns_;
    result.capture_resets = resets_; result.capture_queued_frames = depth_;
    result.capture_correction_ppm = appliedPpm_;
    return result;
}

} // namespace jam2::audio::wasapi

namespace jam2::audio {
ChannelSelection automatic_wasapi_channels(const WasapiConfig& config, std::span<const DeviceInfo> devices)
{
    const auto find = [&](const std::string& id, EndpointDirection direction) -> const DeviceInfo& {
        const auto found = std::find_if(devices.begin(), devices.end(), [&](const auto& device) {
            return device.backend == "WASAPI" && device.endpoint_id == id && device.direction == direction;
        });
        if (found == devices.end() || found->channel_count == 0 || found->channel_count > 64)
            throw std::runtime_error(direction == EndpointDirection::Input ?
                "selected WASAPI input is unavailable" : "selected WASAPI output is unavailable");
        return *found;
    };
    const auto& output = find(config.output_endpoint_id, EndpointDirection::Output);
    ChannelSelection selection;
    selection.input.clear();
    selection.output = output.channel_count == 1 ? std::vector<int>{0} : std::vector<int>{0, 1};
    if (!config.input_endpoint_id.empty()) {
        const auto& input = find(config.input_endpoint_id, EndpointDirection::Input);
        for (std::size_t i = 0; i < input.channel_count; ++i) selection.input.push_back(static_cast<int>(i));
    }
    return selection;
}
} // namespace jam2::audio

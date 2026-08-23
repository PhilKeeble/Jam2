#pragma once

#if defined(JAM2_PLATFORM_MACOS)

#include <CoreAudio/CoreAudio.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace jam2::audio::coreaudio_processing {

struct ConstFloatChannelView {
    const float* samples = nullptr;
    std::size_t stride = 0;
    std::size_t frames = 0;

    [[nodiscard]] bool available() const noexcept
    {
        return samples != nullptr && stride > 0;
    }
};

struct FloatChannelView {
    float* samples = nullptr;
    std::size_t stride = 0;
    std::size_t frames = 0;

    [[nodiscard]] bool available() const noexcept
    {
        return samples != nullptr && stride > 0;
    }
};

inline std::size_t buffer_frames(const AudioBufferList* buffers) noexcept
{
    if (buffers == nullptr || buffers->mNumberBuffers == 0) {
        return 0;
    }
    std::size_t frames = static_cast<std::size_t>(-1);
    for (UInt32 buffer_index = 0; buffer_index < buffers->mNumberBuffers; ++buffer_index) {
        const AudioBuffer& buffer = buffers->mBuffers[buffer_index];
        if (buffer.mNumberChannels == 0) {
            continue;
        }
        frames = std::min(
            frames,
            static_cast<std::size_t>(buffer.mDataByteSize) /
                (sizeof(float) * static_cast<std::size_t>(buffer.mNumberChannels)));
    }
    return frames == static_cast<std::size_t>(-1) ? 0 : frames;
}

inline ConstFloatChannelView input_channel(
    const AudioBufferList* buffers,
    UInt32 channel) noexcept
{
    if (buffers == nullptr) {
        return {};
    }
    UInt32 base_channel = 0;
    for (UInt32 buffer_index = 0; buffer_index < buffers->mNumberBuffers; ++buffer_index) {
        const AudioBuffer& buffer = buffers->mBuffers[buffer_index];
        const UInt32 channel_count = buffer.mNumberChannels;
        if (channel >= base_channel && channel < base_channel + channel_count) {
            if (buffer.mData == nullptr || channel_count == 0) {
                return {};
            }
            const std::size_t stride = static_cast<std::size_t>(channel_count);
            const std::size_t local_channel = static_cast<std::size_t>(channel - base_channel);
            return ConstFloatChannelView{
                static_cast<const float*>(buffer.mData) + local_channel,
                stride,
                static_cast<std::size_t>(buffer.mDataByteSize) / (sizeof(float) * stride),
            };
        }
        base_channel += channel_count;
    }
    return {};
}

inline FloatChannelView output_channel(
    AudioBufferList* buffers,
    UInt32 channel) noexcept
{
    if (buffers == nullptr) {
        return {};
    }
    UInt32 base_channel = 0;
    for (UInt32 buffer_index = 0; buffer_index < buffers->mNumberBuffers; ++buffer_index) {
        AudioBuffer& buffer = buffers->mBuffers[buffer_index];
        const UInt32 channel_count = buffer.mNumberChannels;
        if (channel >= base_channel && channel < base_channel + channel_count) {
            if (buffer.mData == nullptr || channel_count == 0) {
                return {};
            }
            const std::size_t stride = static_cast<std::size_t>(channel_count);
            const std::size_t local_channel = static_cast<std::size_t>(channel - base_channel);
            return FloatChannelView{
                static_cast<float*>(buffer.mData) + local_channel,
                stride,
                static_cast<std::size_t>(buffer.mDataByteSize) / (sizeof(float) * stride),
            };
        }
        base_channel += channel_count;
    }
    return {};
}

inline std::int32_t float_to_i32(float sample) noexcept
{
    const double clamped = std::clamp(static_cast<double>(sample), -1.0, 1.0);
    return static_cast<std::int32_t>(clamped * 2147483647.0);
}

inline float i32_to_float(std::int32_t sample) noexcept
{
    return static_cast<float>(
        std::clamp(static_cast<double>(sample) / 2147483648.0, -1.0, 1.0));
}

inline void copy_input_channel(
    ConstFloatChannelView source,
    std::span<std::int32_t> destination) noexcept
{
    const std::size_t copy_frames = std::min(source.frames, destination.size());
    if (!source.available()) {
        std::fill(destination.begin(), destination.end(), 0);
        return;
    }
    for (std::size_t frame = 0; frame < copy_frames; ++frame) {
        destination[frame] = float_to_i32(source.samples[frame * source.stride]);
    }
    std::fill(destination.begin() + copy_frames, destination.end(), 0);
}

inline void write_mono_output(
    std::span<const std::int32_t> source,
    std::span<const FloatChannelView> destinations) noexcept
{
    for (std::size_t frame = 0; frame < source.size(); ++frame) {
        const float sample = i32_to_float(source[frame]);
        for (const FloatChannelView& destination : destinations) {
            if (destination.available() && frame < destination.frames) {
                destination.samples[frame * destination.stride] = sample;
            }
        }
    }
}

} // namespace jam2::audio::coreaudio_processing

#endif

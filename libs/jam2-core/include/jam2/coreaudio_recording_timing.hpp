#pragma once

#if defined(JAM2_PLATFORM_MACOS)

#include "audio_device.hpp"
#include "recording_calibration.hpp"

#include <CoreAudio/CoreAudio.h>
#include <CoreAudio/HostTime.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>

namespace jam2::audio::coreaudio_processing {

struct RecordingLatencyProperties {
    std::uint32_t device_frames = 0;
    std::uint32_t stream_frames = 0;
    std::uint32_t safety_frames = 0;
    bool enabled = true;
};

// The timestamp interval already includes HAL buffering and the safety offset.
// Device and stream hardware latency must be added separately. Missing or
// invalid timestamps fall back to the queried safety offset and callback size.
inline std::uint32_t recording_latency_frames(
    RecordingLatencyProperties properties,
    std::size_t callback_frames,
    const AudioTimeStamp* earlier,
    const AudioTimeStamp* later,
    double sample_rate) noexcept
{
    if (!properties.enabled) return 0;
    constexpr auto maximum = (std::numeric_limits<std::uint32_t>::max)();
    double interval = -1.0;
    if (earlier != nullptr && later != nullptr) {
        if ((earlier->mFlags & later->mFlags & kAudioTimeStampSampleTimeValid) != 0) {
            interval = later->mSampleTime - earlier->mSampleTime;
        }
        if ((!std::isfinite(interval) || interval < 0.0 || interval > maximum) &&
            (earlier->mFlags & later->mFlags & kAudioTimeStampHostTimeValid) != 0 &&
            later->mHostTime >= earlier->mHostTime &&
            std::isfinite(sample_rate) && sample_rate > 0.0) {
            interval = static_cast<double>(AudioConvertHostTimeToNanos(
                later->mHostTime - earlier->mHostTime)) * sample_rate / 1000000000.0;
        }
    }
    const std::uint64_t scheduling = std::isfinite(interval) &&
            interval >= 0.0 && interval <= maximum
        ? static_cast<std::uint64_t>(std::llround(interval))
        : static_cast<std::uint64_t>(properties.safety_frames) +
            std::min<std::size_t>(callback_frames, maximum);
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(maximum,
        static_cast<std::uint64_t>(properties.device_frames) +
        properties.stream_frames + scheduling));
}

inline void publish_recording_latency(
    StreamControl& control,
    std::uint32_t input_frames,
    std::uint32_t output_frames) noexcept
{
    control.input_latency_frames.store(input_frames, std::memory_order_relaxed);
    control.output_latency_frames.store(output_frames, std::memory_order_relaxed);
    const std::int64_t adjustment = control.recording_latency_adjustment_frames.load(
        std::memory_order_relaxed);
    control.recording_latency_compensation_frames.store(
        static_cast<std::uint64_t>(std::max<std::int64_t>(0,
            static_cast<std::int64_t>(input_frames) + output_frames + adjustment)),
        std::memory_order_relaxed);
}

inline void record_input_take(
    TrackTakeRecorder& recorder,
    const StreamControl* control,
    std::uint64_t audio_frame_start,
    std::span<const std::int32_t> input) noexcept
{
    const std::uint64_t compensation = control != nullptr
        ? input_take_compensation_frames(*control) : 0;
    recorder.record(audio_frame_start > compensation ? audio_frame_start - compensation : 0,
        input);
}

} // namespace jam2::audio::coreaudio_processing

#endif

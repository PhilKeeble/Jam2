#pragma once

namespace jam2::limits {

inline constexpr int kMinimumSampleRate = 8000;
inline constexpr int kMaximumSampleRate = 384000;

constexpr bool valid_sample_rate(int sample_rate) noexcept
{
    return sample_rate >= kMinimumSampleRate && sample_rate <= kMaximumSampleRate;
}

} // namespace jam2::limits

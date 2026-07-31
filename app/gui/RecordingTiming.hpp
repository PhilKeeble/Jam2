#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace jam2::gui {

inline std::uint64_t recording_frames_for_bars(
    int bars,
    int beatsPerBar,
    double bpm,
    int sampleRate,
    int tempoPulseUnits = 1) noexcept
{
    if (bars <= 0 || beatsPerBar <= 0 || !std::isfinite(bpm) || bpm <= 0.0 ||
        sampleRate <= 0 ||
        (tempoPulseUnits != 1 && tempoPulseUnits != 3)) {
        return 0;
    }
    const long double frames =
        static_cast<long double>(bars) *
        static_cast<long double>(beatsPerBar) *
        60.0L *
        static_cast<long double>(sampleRate) /
        static_cast<long double>(bpm) /
        static_cast<long double>(tempoPulseUnits);
    if (frames >= static_cast<long double>((std::numeric_limits<std::uint64_t>::max)())) {
        return (std::numeric_limits<std::uint64_t>::max)();
    }
    return static_cast<std::uint64_t>(std::round(frames));
}

} // namespace jam2::gui

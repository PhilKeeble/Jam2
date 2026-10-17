#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace jam2 {

// The device/session frame clock always runs at the device sample rate. This
// mapping advances musical source time at the selected listening rate instead.
struct PlaybackTempo {
    int rate_ppm = 1000000;
    std::uint64_t anchor_frame = 0;
    double anchor_position = 0.0;

    double rate() const noexcept { return std::clamp(rate_ppm, 100000, 2000000) / 1000000.0; }
    double position(std::uint64_t frame) const noexcept {
        const double delta = frame >= anchor_frame
            ? static_cast<double>(frame - anchor_frame)
            : -static_cast<double>(anchor_frame - frame);
        return std::max(0.0, anchor_position + delta * rate());
    }
    std::uint64_t frame(double sourcePosition) const noexcept {
        const long double result = static_cast<long double>(anchor_frame) +
            (static_cast<long double>(sourcePosition) - anchor_position) / rate();
        if (!std::isfinite(result) || result <= 0.0L) return 0;
        if (result >= static_cast<long double>((std::numeric_limits<std::uint64_t>::max)()))
            return (std::numeric_limits<std::uint64_t>::max)();
        return static_cast<std::uint64_t>(std::floor(result + 0.5L));
    }
};

} // namespace jam2

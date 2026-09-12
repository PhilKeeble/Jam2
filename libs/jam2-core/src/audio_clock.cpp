#include "audio_clock.hpp"

#include <cmath>

namespace jam2::audio {
void DeviceClockEstimator::observe(DeviceClockSample sample) noexcept
{
    if (sample.time_us == 0 || nominal_rate_ <= 0 ||
        (sample.time_us == last_.time_us && sample.frame == last_.frame)) return;
    const bool backward = sample.time_us <= last_.time_us || sample.frame <= last_.frame;
    const bool discontinuity = last_.time_us != 0 && !backward &&
        std::abs(static_cast<long double>(sample.time_us - last_.time_us) -
            static_cast<long double>(sample.frame - last_.frame) * 1000000.0L / nominal_rate_) > 1000000.0L;
    if (last_.time_us != 0 && (backward || discontinuity)) {
        const auto previous = stats_;
        *this = DeviceClockEstimator(nominal_rate_);
        stats_.resets = previous.resets + 1;
        stats_.observations = previous.observations;
        stats_.rejected_observations = previous.rejected_observations;
    }
    last_ = sample;
    const auto offset = static_cast<long double>(sample.time_us) -
        static_cast<long double>(sample.frame) * 1000000.0L / nominal_rate_;
    if (window_start_ == 0) {
        window_start_ = sample.time_us;
        best_ = sample;
        best_offset_ = offset;
        return;
    }
    if (sample.time_us - window_start_ >= observation_window_us) {
        if (!calibrated_) {
            baseline_ = best_;
            calibrated_ = true;
        } else if (best_.time_us > baseline_.time_us && best_.frame > baseline_.frame &&
            best_.time_us - baseline_.time_us >= minimum_observation_us) {
            const long double ratio = static_cast<long double>(best_.frame - baseline_.frame) * 1000000.0L /
                (static_cast<long double>(nominal_rate_) * (best_.time_us - baseline_.time_us));
            stats_.raw_ppm = static_cast<double>((ratio - 1.0L) * 1000000.0L);
            if (std::isfinite(stats_.raw_ppm) && std::abs(stats_.raw_ppm) <= maximum_ppm) {
                stats_.ppm = stats_.valid ? stats_.ppm + (stats_.raw_ppm - stats_.ppm) * 0.25 : stats_.raw_ppm;
                stats_.valid = true;
                ++stats_.observations;
            } else {
                ++stats_.rejected_observations;
            }
        }
        window_start_ = sample.time_us;
        best_ = sample;
        best_offset_ = offset;
    } else if (offset < best_offset_) {
        best_ = sample;
        best_offset_ = offset;
    }
}
} // namespace jam2::audio

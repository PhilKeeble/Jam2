#pragma once

#include <cstdint>

namespace jam2::audio {

struct DeviceClockSample {
    std::uint64_t frame = 0;
    std::uint64_t time_us = 0;
};

struct DeviceClockStats {
    bool valid = false;
    double raw_ppm = 0;
    double ppm = 0;
    std::uint64_t observations = 0;
    std::uint64_t resets = 0;
    std::uint64_t rejected_observations = 0;
};

// Non-real-time estimator of device frames / monotonic time. Use a low-delay
// callback observation per window, so batched/delayed callback dispatch does
// not become an instantaneous device-rate change.
class DeviceClockEstimator {
public:
    static constexpr std::uint64_t observation_window_us = 250000;
    static constexpr std::uint64_t minimum_observation_us = 1000000;
    static constexpr double maximum_ppm = 5000;
    explicit DeviceClockEstimator(int nominal_rate) noexcept : nominal_rate_(nominal_rate) {}
    void observe(DeviceClockSample sample) noexcept;
    const DeviceClockStats& stats() const noexcept { return stats_; }
    double rateRatio() const noexcept { return stats_.valid ? 1.0 + stats_.ppm / 1000000.0 : 1.0; }
private:
    int nominal_rate_;
    DeviceClockStats stats_;
    DeviceClockSample last_, baseline_, best_;
    std::uint64_t window_start_ = 0;
    long double best_offset_ = 0;
    bool calibrated_ = false;
};
} // namespace jam2::audio

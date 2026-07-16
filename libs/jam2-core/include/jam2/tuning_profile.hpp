#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace jam2 {

struct TuningProfile {
    std::string_view name;
    std::string_view label;
    int sample_rate;
    long audio_buffer_size;
    int frame_size;
    std::size_t playback_prefill_frames;
    std::size_t playback_ring_frames;
    std::size_t playback_max_frames;
    std::size_t capture_ring_frames;
    bool drift_correction;
    double drift_smoothing;
    int drift_deadband_ppm;
    int drift_max_correction_ppm;
    bool sample_time_playout;
    std::size_t playout_delay_frames;
    std::size_t jitter_buffer_frames;
    std::size_t jitter_buffer_max_frames;
    bool adaptive_playback_cushion;
    std::size_t adaptive_playback_target_frames;
    std::size_t adaptive_playback_min_frames;
    std::size_t adaptive_playback_max_frames;
    int adaptive_playback_release_ppm;
    int adaptive_playback_ratio_ramp_ms;
};

std::span<const TuningProfile> tuning_profiles();
const TuningProfile* find_tuning_profile(std::string_view name);
const TuningProfile& default_tuning_profile();
std::string tuning_profile_names();

} // namespace jam2

#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace jam2 {

// Local receive/playback tuning. These values belong to one participant and
// may differ between peers in the same session.
struct JoinProfile {
    std::string_view name;
    std::string_view label;
    long audio_buffer_size;
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

// Creator-owned session packetization plus the creator's local tuning.
struct CreateProfile {
    std::string_view name;
    std::string_view label;
    int sample_rate;
    int frame_size;
    const JoinProfile* local;
};

std::span<const JoinProfile> join_profiles();
std::span<const CreateProfile> create_profiles();
const JoinProfile* find_join_profile(std::string_view name);
const CreateProfile* find_create_profile(std::string_view name);
const JoinProfile& default_join_profile();
const CreateProfile& default_create_profile();
std::string tuning_profile_names();

} // namespace jam2

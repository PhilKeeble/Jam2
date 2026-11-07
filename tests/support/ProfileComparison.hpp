#pragma once

#include "tuning_profile.hpp"

#include <stdexcept>
#include <string_view>

namespace jam2::test {

// Experiment-only values. Never register these as application presets.
struct ProfileComparison {
    JoinProfile local;
    int packet_frames;
};

inline ProfileComparison comparisonProfile(std::string_view name)
{
    const auto* preset = find_create_profile(name == "candidate" ? "standard"
        : name == "fast128-control" ? "fast" : name);
    if (!preset) throw std::invalid_argument("unknown comparison profile");
    ProfileComparison result{*preset->local, preset->frame_size};
    result.local.name = name;
    if (name == "moderate") {
        // Historical control for the recorded experiments, not a public preset.
        result.local.playback_prefill_frames = 256;
        result.local.playback_ring_frames = 8192;
        result.local.playback_max_frames = 4096;
        result.local.playout_delay_frames = 512;
        result.local.jitter_buffer_frames = 1536;
        result.local.adaptive_playback_target_frames = 512;
        result.local.adaptive_playback_min_frames = 512;
        result.local.adaptive_playback_max_frames = 4096;
    }
    if (name == "candidate") {
        result.packet_frames = 128;
        result.local.audio_buffer_size = 64;
        result.local.playback_prefill_frames = 128;
        result.local.playback_ring_frames = 16384;
        result.local.playback_max_frames = 8192;
        result.local.playout_delay_frames = 128;
        result.local.jitter_buffer_frames = 128;
        // Retain Moderate's reorder/deadline bound; vary it separately later.
        result.local.adaptive_playback_target_frames = 128;
        result.local.adaptive_playback_min_frames = 128;
        result.local.adaptive_playback_max_frames = 8192;
    } else if (name == "fast128-control") {
        result.packet_frames = 128;
        result.local.audio_buffer_size = 64;
    }
    return result;
}

} // namespace jam2::test

#pragma once

#include <cstdint>

namespace jam2::metronome {

constexpr int kMaxPatternSteps = 128;

struct PatternSnapshot {
    int bpm = 120;
    int beats_per_bar = 4;
    int division = 1;
    int step_count = 4;
    std::uint64_t play_mask_low = 0x0fULL;
    std::uint64_t play_mask_high = 0;
    std::uint64_t accent_mask_low = 0x01ULL;
    std::uint64_t accent_mask_high = 0;
};

struct AuthorityClockMapping {
    std::uint64_t epoch_sample_time = 0;
    std::int64_t render_offset_frames = 0;
    bool valid = false;
};

enum class ClickVoice {
    Normal,
    CountIn,
};

int clamp_bpm(int bpm);
int clamp_beats_per_bar(int beats);
int clamp_division(int division);
int pattern_step_count(int beats_per_bar, int division);
PatternSnapshot sanitize(PatternSnapshot pattern);

bool mask_enabled(std::uint64_t low, std::uint64_t high, int step);
void set_mask_enabled(std::uint64_t& low, std::uint64_t& high, int step, bool enabled);

std::uint64_t step_interval_samples(double sample_rate, int bpm, int division);
AuthorityClockMapping map_authority_clock(
    std::uint64_t authority_epoch_sample_time,
    std::uint64_t projected_authority_sample_time,
    std::uint64_t local_sample_time);
double render_sample(
    const PatternSnapshot& pattern,
    std::uint64_t grid_sample,
    double sample_rate,
    double level,
    ClickVoice voice = ClickVoice::Normal);
double render_sample(
    const PatternSnapshot& pattern,
    std::uint64_t grid_sample,
    std::uint64_t step_interval,
    double sample_rate,
    double level,
    ClickVoice voice = ClickVoice::Normal);
std::int32_t mix_i32(std::int32_t sample, double normalized_click);
std::int32_t mix_pcm24(std::int32_t sample, double normalized_click);

} // namespace jam2::metronome

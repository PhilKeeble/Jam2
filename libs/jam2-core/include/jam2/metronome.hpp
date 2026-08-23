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
    int beat_unit = 4;
    int tempo_pulse_units = 1;
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

enum class ClickSound : std::uint8_t {
    Classic = 0,
    Woodblock = 1,
    RimClick = 2,
    DigitalTick = 3,
};

int clamp_bpm(int bpm);
int clamp_beats_per_bar(int beats);
int clamp_division(int division);
int clamp_beat_unit(int beat_unit);
int clamp_tempo_pulse_units(int units);
ClickSound sanitize_click_sound(int sound);
int pattern_step_count(int beats_per_bar, int division);
PatternSnapshot sanitize(PatternSnapshot pattern);

bool mask_enabled(std::uint64_t low, std::uint64_t high, int step);
void set_mask_enabled(std::uint64_t& low, std::uint64_t& high, int step, bool enabled);

std::uint64_t step_interval_samples(
    double sample_rate,
    int bpm,
    int division,
    int tempo_pulse_units = 1);
std::uint64_t click_duration_samples(
    double sample_rate,
    bool accent,
    ClickVoice voice,
    ClickSound sound);
double render_click_tone_sample(
    std::uint64_t step_offset,
    double sample_rate,
    bool accent,
    ClickVoice voice,
    ClickSound sound);
double render_pattern_step_sample(
    const PatternSnapshot& pattern,
    int pattern_step,
    std::uint64_t step_offset,
    double sample_rate,
    double level,
    ClickVoice voice,
    ClickSound sound);
AuthorityClockMapping map_authority_clock(
    std::uint64_t authority_epoch_sample_time,
    std::uint64_t projected_authority_sample_time,
    std::uint64_t local_sample_time);
double render_sample(
    const PatternSnapshot& pattern,
    std::uint64_t grid_sample,
    double sample_rate,
    double level,
    ClickVoice voice,
    ClickSound sound);
double render_sample(
    const PatternSnapshot& pattern,
    std::uint64_t grid_sample,
    std::uint64_t step_interval,
    double sample_rate,
    double level,
    ClickVoice voice,
    ClickSound sound);
std::int32_t mix_i32(std::int32_t sample, double normalized_click);
std::int32_t mix_pcm24(std::int32_t sample, double normalized_click);

} // namespace jam2::metronome

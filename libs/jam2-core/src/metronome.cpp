#include "metronome.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace jam2::metronome {

namespace {

constexpr double kPi = 3.14159265358979323846;

double click_tone(std::uint64_t offset, double sample_rate, bool accent)
{
    if (sample_rate <= 0.0) {
        return 0.0;
    }
    const double duration_seconds = accent ? 0.008 : 0.0065;
    const auto duration = static_cast<std::uint64_t>(std::max(1.0, std::round(sample_rate * duration_seconds)));
    if (offset >= duration) {
        return 0.0;
    }

    const double t = static_cast<double>(offset) / sample_rate;
    const double attack_frames = std::max(1.0, std::round(sample_rate * 0.00035));
    const double release_frames = std::max(1.0, std::round(sample_rate * 0.0012));
    const double attack = offset < static_cast<std::uint64_t>(attack_frames)
        ? 0.5 - 0.5 * std::cos(kPi * static_cast<double>(offset) / attack_frames)
        : 1.0;
    const std::uint64_t remaining = duration - offset - 1;
    const double release = remaining < static_cast<std::uint64_t>(release_frames)
        ? 0.5 - 0.5 * std::cos(kPi * static_cast<double>(remaining) / release_frames)
        : 1.0;
    const double decay = std::exp(-t * 360.0);
    const double frequency = accent ? 1320.0 : 880.0;
    const double phase = 2.0 * kPi * frequency * t;
    return std::sin(phase) * attack * decay * release;
}

} // namespace

int clamp_bpm(int bpm)
{
    return std::clamp(bpm, 1, 400);
}

int clamp_beats_per_bar(int beats)
{
    return std::clamp(beats, 1, 16);
}

int clamp_division(int division)
{
    switch (division) {
    case 1:
    case 2:
    case 3:
    case 4:
    case 6:
    case 8:
        return division;
    default:
        return 1;
    }
}

int pattern_step_count(int beats_per_bar, int division)
{
    const int beats = clamp_beats_per_bar(beats_per_bar);
    const int div = clamp_division(division);
    return std::clamp(beats * div, 1, kMaxPatternSteps);
}

PatternSnapshot sanitize(PatternSnapshot pattern)
{
    pattern.bpm = clamp_bpm(pattern.bpm);
    pattern.beats_per_bar = clamp_beats_per_bar(pattern.beats_per_bar);
    pattern.division = clamp_division(pattern.division);
    pattern.step_count = pattern_step_count(pattern.beats_per_bar, pattern.division);
    if (pattern.step_count < 64) {
        const std::uint64_t valid = (1ULL << pattern.step_count) - 1ULL;
        pattern.play_mask_low &= valid;
        pattern.accent_mask_low &= valid;
        pattern.play_mask_high = 0;
        pattern.accent_mask_high = 0;
    } else if (pattern.step_count == 64) {
        pattern.play_mask_high = 0;
        pattern.accent_mask_high = 0;
    } else if (pattern.step_count < kMaxPatternSteps) {
        const int high_bits = pattern.step_count - 64;
        const std::uint64_t valid_high = (1ULL << high_bits) - 1ULL;
        pattern.play_mask_high &= valid_high;
        pattern.accent_mask_high &= valid_high;
    }
    return pattern;
}

bool mask_enabled(std::uint64_t low, std::uint64_t high, int step)
{
    if (step < 0 || step >= kMaxPatternSteps) {
        return false;
    }
    if (step < 64) {
        return ((low >> step) & 1ULL) != 0ULL;
    }
    return ((high >> (step - 64)) & 1ULL) != 0ULL;
}

void set_mask_enabled(std::uint64_t& low, std::uint64_t& high, int step, bool enabled)
{
    if (step < 0 || step >= kMaxPatternSteps) {
        return;
    }
    std::uint64_t& mask = step < 64 ? low : high;
    const int bit = step < 64 ? step : step - 64;
    const std::uint64_t value = 1ULL << bit;
    if (enabled) {
        mask |= value;
    } else {
        mask &= ~value;
    }
}

std::uint64_t step_interval_samples(double sample_rate, int bpm, int division)
{
    if (sample_rate <= 0.0) {
        return 0;
    }
    const double interval = (60.0 * sample_rate) /
        static_cast<double>(clamp_bpm(bpm) * clamp_division(division));
    return static_cast<std::uint64_t>(std::max(1.0, std::round(interval)));
}

AuthorityClockMapping map_authority_clock(
    std::uint64_t authority_epoch_sample_time,
    std::uint64_t projected_authority_sample_time,
    std::uint64_t local_sample_time)
{
    if (projected_authority_sample_time < authority_epoch_sample_time) {
        const std::uint64_t lead =
            authority_epoch_sample_time - projected_authority_sample_time;
        if (local_sample_time > (std::numeric_limits<std::uint64_t>::max)() - lead) {
            return {};
        }
        return {local_sample_time + lead, 0, true};
    }

    const std::uint64_t elapsed =
        projected_authority_sample_time - authority_epoch_sample_time;
    if (local_sample_time >= elapsed) {
        return {local_sample_time - elapsed, 0, true};
    }

    const std::uint64_t offset = elapsed - local_sample_time;
    if (offset > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        return {};
    }
    return {0, static_cast<std::int64_t>(offset), true};
}

double render_sample(const PatternSnapshot& input, std::uint64_t grid_sample, double sample_rate, double level)
{
    const std::uint64_t interval = step_interval_samples(sample_rate, input.bpm, input.division);
    return render_sample(input, grid_sample, interval, sample_rate, level);
}

double render_sample(
    const PatternSnapshot& pattern,
    std::uint64_t grid_sample,
    std::uint64_t step_interval,
    double sample_rate,
    double level)
{
    if (pattern.step_count <= 0) {
        return 0.0;
    }
    const std::uint64_t interval = step_interval;
    if (interval == 0) {
        return 0.0;
    }
    const std::uint64_t step_index = grid_sample / interval;
    const std::uint64_t step_offset = grid_sample % interval;
    const int pattern_step = static_cast<int>(step_index % static_cast<std::uint64_t>(pattern.step_count));
    if (!mask_enabled(pattern.play_mask_low, pattern.play_mask_high, pattern_step)) {
        return 0.0;
    }
    const bool accent = mask_enabled(pattern.accent_mask_low, pattern.accent_mask_high, pattern_step);
    const double click_level = std::clamp(level, 0.0, 1.0) * (accent ? 1.25 : 0.78);
    return std::clamp(click_tone(step_offset, sample_rate, accent) * click_level, -1.0, 1.0);
}

std::int32_t mix_i32(std::int32_t sample, double normalized_click)
{
    const double mixed = static_cast<double>(sample) + (std::clamp(normalized_click, -1.0, 1.0) * 2147483647.0);
    return static_cast<std::int32_t>(std::clamp(mixed, -2147483648.0, 2147483647.0));
}

std::int32_t mix_pcm24(std::int32_t sample, double normalized_click)
{
    const double mixed = static_cast<double>(sample) + (std::clamp(normalized_click, -1.0, 1.0) * 8388607.0);
    return static_cast<std::int32_t>(std::clamp(mixed, -8388608.0, 8388607.0));
}

} // namespace jam2::metronome

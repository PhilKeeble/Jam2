#include "metronome.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace jam2::metronome {

namespace {

constexpr double kPi = 3.14159265358979323846;

double deterministic_noise(std::uint64_t offset, std::uint32_t salt)
{
    std::uint32_t value = static_cast<std::uint32_t>(offset) + salt;
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    value ^= value >> 16;
    return static_cast<double>(value) /
        static_cast<double>((std::numeric_limits<std::uint32_t>::max)()) * 2.0 - 1.0;
}

double shaped_click(
    std::uint64_t offset,
    double sample_rate,
    double duration_seconds,
    double release_seconds,
    double decay_rate,
    double signal)
{
    const auto duration = static_cast<std::uint64_t>(
        std::max(1.0, std::round(sample_rate * duration_seconds)));
    if (offset >= duration) {
        return 0.0;
    }
    const double attack_frames = std::max(1.0, std::round(sample_rate * 0.00025));
    const double release_frames = std::max(1.0, std::round(sample_rate * release_seconds));
    const double attack = offset < static_cast<std::uint64_t>(attack_frames)
        ? 0.5 - 0.5 * std::cos(kPi * static_cast<double>(offset) / attack_frames)
        : 1.0;
    const std::uint64_t remaining = duration - offset - 1;
    const double release = remaining < static_cast<std::uint64_t>(release_frames)
        ? 0.5 - 0.5 * std::cos(kPi * static_cast<double>(remaining) / release_frames)
        : 1.0;
    const double seconds = static_cast<double>(offset) / sample_rate;
    return signal * attack * std::exp(-seconds * decay_rate) * release;
}

double classic_click_tone(
    std::uint64_t offset,
    double sample_rate,
    bool accent,
    ClickVoice voice)
{
    if (sample_rate <= 0.0) {
        return 0.0;
    }
    const bool count_in = voice == ClickVoice::CountIn;
    const double duration_seconds = count_in
        ? (accent ? 0.014 : 0.012)
        : (accent ? 0.008 : 0.0065);
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
    const double decay = std::exp(-t * (count_in ? 180.0 : 360.0));
    const double frequency = count_in
        ? (accent ? 760.0 : 560.0)
        : (accent ? 1320.0 : 880.0);
    const double phase = 2.0 * kPi * frequency * t;
    const double tone = count_in
        ? 0.78 * std::sin(phase) + 0.22 * std::sin(phase * 2.0)
        : std::sin(phase);
    return tone * attack * decay * release;
}

double click_tone(
    std::uint64_t offset,
    double sample_rate,
    bool accent,
    ClickVoice voice,
    ClickSound sound)
{
    if (sample_rate <= 0.0) {
        return 0.0;
    }
    if (sound == ClickSound::Classic) {
        return classic_click_tone(offset, sample_rate, accent, voice);
    }

    const bool count_in = voice == ClickVoice::CountIn;
    const double seconds = static_cast<double>(offset) / sample_rate;

    if (sound == ClickSound::Woodblock) {
        const double frequency = count_in
            ? (accent ? 920.0 : 690.0)
            : (accent ? 1780.0 : 1220.0);
        const double phase = 2.0 * kPi * frequency * seconds;
        const double tone =
            0.72 * std::sin(phase) +
            0.20 * std::sin(phase * 1.58 + 0.35) +
            0.08 * deterministic_noise(offset, 0x36a9U);
        return shaped_click(
            offset,
            sample_rate,
            count_in ? (accent ? 0.042 : 0.036) : (accent ? 0.030 : 0.025),
            0.003,
            count_in ? 82.0 : 118.0,
            tone);
    }

    if (sound == ClickSound::RimClick) {
        const double frequency = count_in
            ? (accent ? 1750.0 : 1350.0)
            : (accent ? 3100.0 : 2350.0);
        const double phase = 2.0 * kPi * frequency * seconds;
        const double noise =
            deterministic_noise(offset, 0x91e1U) -
            0.55 * deterministic_noise(offset > 0 ? offset - 1 : 0, 0x91e1U);
        const double tone =
            0.58 * noise +
            0.30 * std::sin(phase) +
            0.12 * std::sin(phase * 2.17);
        return shaped_click(
            offset,
            sample_rate,
            count_in ? 0.020 : (accent ? 0.014 : 0.011),
            0.0018,
            count_in ? 190.0 : 285.0,
            tone);
    }

    const double frequency = count_in
        ? (accent ? 2450.0 : 1950.0)
        : (accent ? 4300.0 : 3400.0);
    const double phase = 2.0 * kPi * frequency * seconds;
    const double tone =
        0.62 * std::sin(phase) +
        0.27 * std::sin(phase * 2.0) +
        0.11 * std::sin(phase * 3.0);
    return shaped_click(
        offset,
        sample_rate,
        count_in ? 0.012 : (accent ? 0.007 : 0.0055),
        0.001,
        count_in ? 250.0 : 420.0,
        tone);
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

int clamp_beat_unit(int beat_unit)
{
    switch (beat_unit) {
    case 2:
    case 4:
    case 8:
    case 16:
        return beat_unit;
    default:
        return 4;
    }
}

int clamp_tempo_pulse_units(int units)
{
    return units == 3 ? 3 : 1;
}

ClickSound sanitize_click_sound(int sound)
{
    return static_cast<ClickSound>(std::clamp(
        sound,
        static_cast<int>(ClickSound::Classic),
        static_cast<int>(ClickSound::DigitalTick)));
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
    pattern.beat_unit = clamp_beat_unit(pattern.beat_unit);
    pattern.tempo_pulse_units =
        clamp_tempo_pulse_units(pattern.tempo_pulse_units);
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

std::uint64_t step_interval_samples(
    double sample_rate,
    int bpm,
    int division,
    int tempo_pulse_units)
{
    if (sample_rate <= 0.0) {
        return 0;
    }
    const double interval = (60.0 * sample_rate) /
        static_cast<double>(
            clamp_bpm(bpm) * clamp_division(division) *
            clamp_tempo_pulse_units(tempo_pulse_units));
    return static_cast<std::uint64_t>(std::max(1.0, std::round(interval)));
}

std::uint64_t click_duration_samples(
    double sample_rate,
    bool accent,
    ClickVoice voice,
    ClickSound sound)
{
    if (sample_rate <= 0.0) {
        return 0;
    }
    const bool count_in = voice == ClickVoice::CountIn;
    double duration_seconds = 0.0;
    switch (sound) {
    case ClickSound::Classic:
        duration_seconds = count_in
            ? (accent ? 0.014 : 0.012)
            : (accent ? 0.008 : 0.0065);
        break;
    case ClickSound::Woodblock:
        duration_seconds = count_in
            ? (accent ? 0.042 : 0.036)
            : (accent ? 0.030 : 0.025);
        break;
    case ClickSound::RimClick:
        duration_seconds = count_in ? 0.020 : (accent ? 0.014 : 0.011);
        break;
    case ClickSound::DigitalTick:
        duration_seconds = count_in ? 0.012 : (accent ? 0.007 : 0.0055);
        break;
    }
    return static_cast<std::uint64_t>(
        std::max(1.0, std::round(sample_rate * duration_seconds)));
}

double render_click_tone_sample(
    std::uint64_t step_offset,
    double sample_rate,
    bool accent,
    ClickVoice voice,
    ClickSound sound)
{
    return click_tone(step_offset, sample_rate, accent, voice, sound);
}

double render_pattern_step_sample(
    const PatternSnapshot& pattern,
    int pattern_step,
    std::uint64_t step_offset,
    double sample_rate,
    double level,
    ClickVoice voice,
    ClickSound sound)
{
    if (pattern.step_count <= 0 || pattern_step < 0 ||
        pattern_step >= pattern.step_count ||
        !mask_enabled(pattern.play_mask_low, pattern.play_mask_high, pattern_step)) {
        return 0.0;
    }
    const bool accent = mask_enabled(
        pattern.accent_mask_low,
        pattern.accent_mask_high,
        pattern_step);
    const double click_level = std::clamp(level, 0.0, 1.0) *
        (accent ? 1.25 : 0.78);
    return std::clamp(
        click_tone(step_offset, sample_rate, accent, voice, sound) * click_level,
        -1.0,
        1.0);
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

double render_sample(
    const PatternSnapshot& input,
    std::uint64_t grid_sample,
    double sample_rate,
    double level,
    ClickVoice voice,
    ClickSound sound)
{
    const std::uint64_t interval = step_interval_samples(
        sample_rate, input.bpm, input.division, input.tempo_pulse_units);
    return render_sample(input, grid_sample, interval, sample_rate, level, voice, sound);
}

double render_sample(
    const PatternSnapshot& pattern,
    std::uint64_t grid_sample,
    std::uint64_t step_interval,
    double sample_rate,
    double level,
    ClickVoice voice,
    ClickSound sound)
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
    return render_pattern_step_sample(
        pattern,
        pattern_step,
        step_offset,
        sample_rate,
        level,
        voice,
        sound);
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

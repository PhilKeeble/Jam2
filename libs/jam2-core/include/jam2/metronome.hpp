#pragma once

#include <cstdint>
#include <optional>

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

struct GridPhaseObservation {
    std::uint64_t authority_frame = 0, authority_epoch = 0;
    std::uint64_t local_musical_frame = 0, local_epoch = 0, bar_frames = 0;
};
// Signed phase difference on the nearest bar. No clock, I/O or owned state.
std::optional<std::int64_t> grid_phase_error(const GridPhaseObservation& observation) noexcept;

struct AuthorityBeaconState {
    std::uint64_t revision = 0, epoch = 0, active_peers = 0;
    bool operator==(const AuthorityBeaconState&) const noexcept = default;
};
class AuthorityBeaconSchedule {
public:
    explicit AuthorityBeaconSchedule(std::uint64_t interval_us) noexcept : interval_us_(interval_us) {}
    bool due(std::uint64_t now_us, AuthorityBeaconState state) const noexcept;
    void commitAttempt(std::uint64_t now_us, AuthorityBeaconState state) noexcept;
    std::uint64_t intervalUs() const noexcept { return interval_us_; }
    std::uint64_t phaseFreshnessUs() const noexcept;
    bool phaseFresh(std::uint64_t now_us, std::uint64_t received_us) const noexcept;
private:
    std::uint64_t interval_us_, next_us_ = 0;
    std::optional<AuthorityBeaconState> last_;
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
std::uint64_t raw_epoch_phase(std::uint64_t epoch, std::int64_t clockOffset, std::uint64_t interval) noexcept;
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

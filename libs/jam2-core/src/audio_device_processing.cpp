#include "audio_device_processing.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <limits>

namespace jam2::audio::device_processing {
namespace {

constexpr std::uint32_t kPlaybackUnderrunFadeFrames = 32;

constexpr double kPi = 3.14159265358979323846;

bool valid_render_sample_rate(double sampleRate) noexcept
{
    // Metronome interval math multiplies the rate by at most 60 before
    // converting to a frame count. Keep that conversion, and the click
    // renderer's divisions, inside a finite representable domain.
    constexpr double maximum = static_cast<double>(
        (std::numeric_limits<std::uint64_t>::max)()) / 60.0;
    return std::isfinite(sampleRate) && sampleRate >= 1.0 &&
        sampleRate <= maximum;
}

bool interval_exceeds(
    std::uint64_t interval,
    double expected,
    double factor) noexcept
{
    const double threshold = expected * factor;
    const double maximum = static_cast<double>(
        (std::numeric_limits<std::uint64_t>::max)());
    return std::isfinite(threshold) && threshold < maximum &&
        interval > static_cast<std::uint64_t>(threshold);
}

void update_interval_peak(std::atomic<int>& target, int value) noexcept
{
    int current = target.load(std::memory_order_relaxed);
    while (value > current && !target.compare_exchange_weak(
               current,
               value,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

void atomic_update_max(
    std::atomic<std::uint64_t>& target,
    std::uint64_t value) noexcept
{
    std::uint64_t current = target.load(std::memory_order_relaxed);
    while (value > current && !target.compare_exchange_weak(
               current, value, std::memory_order_relaxed)) {
    }
}

void atomic_update_min(
    std::atomic<std::uint64_t>& target,
    std::uint64_t value) noexcept
{
    std::uint64_t current = target.load(std::memory_order_relaxed);
    while ((current == 0 || value < current) &&
           !target.compare_exchange_weak(
               current, value, std::memory_order_relaxed)) {
    }
}

std::int32_t pop_one_frame(
    MonoRingBuffer& ring,
    PlaybackResamplerState& state,
    std::int32_t continuity_fallback) noexcept
{
    std::array<std::int32_t, 1> frame{};
    if (ring.pop(frame, false) == 1) {
        state.underrunConcealmentOrigin = 0;
        state.underrunConcealmentFrames = 0;
        return frame[0];
    }
    if (state.underrunConcealmentFrames == 0) {
        state.underrunConcealmentOrigin = continuity_fallback;
    }
    if (state.underrunConcealmentFrames >= kPlaybackUnderrunFadeFrames) {
        return 0;
    }
    const std::uint32_t remaining =
        kPlaybackUnderrunFadeFrames - state.underrunConcealmentFrames;
    ++state.underrunConcealmentFrames;
    return static_cast<std::int32_t>(
        static_cast<std::int64_t>(state.underrunConcealmentOrigin) *
        static_cast<std::int64_t>(remaining) /
        static_cast<std::int64_t>(kPlaybackUnderrunFadeFrames));
}

} // namespace

void PlaybackResamplerState::reset() noexcept
{
    current = 0;
    next = 0;
    hasCurrent = false;
    hasNext = false;
    phase = 0.0;
    underrunConcealmentOrigin = 0;
    underrunConcealmentFrames = 0;
    ratioSmoother.reset();
}

std::uint64_t callback_now_us() noexcept
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

void observe_callback_interval(
    CallbackIntervalState& state,
    std::uint64_t nowUs,
    std::size_t bufferFrames,
    double sampleRate) noexcept
{
    const std::uint64_t previous = state.lastCallbackUs.exchange(
        nowUs, std::memory_order_relaxed);
    if (previous == 0 || nowUs <= previous || bufferFrames == 0 ||
        !std::isfinite(sampleRate) || sampleRate <= 0.0) {
        return;
    }
    const std::uint64_t interval = nowUs - previous;
    atomic_update_min(state.minimumUs, interval);
    state.sumUs.fetch_add(interval, std::memory_order_relaxed);
    atomic_update_max(state.maximumUs, interval);
    state.samples.fetch_add(1, std::memory_order_relaxed);
    const double expected = static_cast<double>(bufferFrames) * 1000000.0 /
        sampleRate;
    if (interval_exceeds(interval, expected, 1.1)) {
        state.gapsOver1_1x.fetch_add(1, std::memory_order_relaxed);
    }
    if (interval_exceeds(interval, expected, 1.5)) {
        state.gapsOver1_5x.fetch_add(1, std::memory_order_relaxed);
    }
    if (interval_exceeds(interval, expected, 2.0)) {
        state.gapsOver2x.fetch_add(1, std::memory_order_relaxed);
    }
}

void update_peak(std::atomic<int>& peak, int candidate) noexcept
{
    int current = peak.load(std::memory_order_relaxed);
    while (candidate > current && !peak.compare_exchange_weak(
               current,
               candidate,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

int i32_peak_ppm(std::span<const std::int32_t> samples) noexcept
{
    std::uint32_t peak = 0;
    for (const std::int32_t sample : samples) {
        const std::uint32_t absolute =
            sample == (std::numeric_limits<std::int32_t>::min)()
            ? static_cast<std::uint32_t>(
                (std::numeric_limits<std::int32_t>::max)())
            : static_cast<std::uint32_t>(std::abs(sample));
        peak = (std::max)(peak, absolute);
    }
    const double normalized = static_cast<double>(peak) / 2147483647.0;
    return static_cast<int>(
        std::clamp(normalized, 0.0, 1.0) * 1000000.0);
}

std::int32_t scale_i32_sample(std::int32_t sample, double level) noexcept
{
    if (!std::isfinite(level)) {
        return 0;
    }
    const double scaled = static_cast<double>(sample) * level;
    return static_cast<std::int32_t>(
        std::clamp(scaled, -2147483648.0, 2147483647.0));
}

void observe_peak(
    std::atomic<int>& peak,
    std::span<const std::int32_t> samples) noexcept
{
    update_peak(peak, i32_peak_ppm(samples));
}

void observe_input_peaks(
    StreamControl* control,
    std::span<const std::int32_t> samples) noexcept
{
    if (control == nullptr) {
        return;
    }
    const int inputPeak = i32_peak_ppm(samples);
    update_peak(control->input_peak_ppm, inputPeak);
    update_peak(control->gui_input_peak_ppm, inputPeak);
    const int sendPeak = static_cast<int>(
        static_cast<std::int64_t>(inputPeak) *
        std::clamp(
            control->send_level_ppm.load(std::memory_order_relaxed),
            0,
            4000000) /
        1000000LL);
    update_peak(control->send_peak_ppm, sendPeak);
    update_peak(control->gui_send_peak_ppm, sendPeak);
}

bool read_network_playback_timeline(
    const StreamControl& control,
    const MonoRingBuffer& playback,
    std::uint64_t& engineFrame,
    std::size_t& queuedFrames) noexcept
{
    const std::uint64_t before = control.audio_callback_generation.load(
        std::memory_order_acquire);
    if ((before & 1ULL) != 0) {
        return false;
    }
    const std::uint64_t observedFrame = control.engine_frame_counter.load(
        std::memory_order_acquire);
    const std::size_t observedDepth = playback.available_read();
    const std::uint64_t after = control.audio_callback_generation.load(
        std::memory_order_acquire);
    if (before != after || (after & 1ULL) != 0) {
        return false;
    }
    engineFrame = observedFrame;
    queuedFrames = observedDepth;
    return true;
}

std::int32_t mix_i32_samples(std::int32_t a, std::int32_t b) noexcept
{
    const std::int64_t mixed =
        static_cast<std::int64_t>(a) + static_cast<std::int64_t>(b);
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(
        mixed, -2147483648LL, 2147483647LL));
}

void apply_remote_level(
    StreamControl* control,
    std::span<std::int32_t> output) noexcept
{
    if (control == nullptr) {
        return;
    }
    const int levelPpm = control->remote_level_ppm.load(
        std::memory_order_relaxed);
    if (levelPpm == 1000000) {
        return;
    }
    const double level = static_cast<double>(
        std::clamp(levelPpm, 0, 4000000)) / 1000000.0;
    for (std::int32_t& sample : output) {
        sample = scale_i32_sample(sample, level);
    }
}

void apply_output_level(
    StreamControl* control,
    std::span<std::int32_t> output) noexcept
{
    if (control == nullptr) {
        return;
    }
    const int levelPpm = control->output_level_ppm.load(
        std::memory_order_relaxed);
    if (levelPpm == 1000000) {
        return;
    }
    const double level = static_cast<double>(
        std::clamp(levelPpm, 0, 4000000)) / 1000000.0;
    for (std::int32_t& sample : output) {
        sample = scale_i32_sample(sample, level);
    }
}

void mix_local_monitor(
    StreamControl* control,
    std::span<std::int32_t> output,
    std::span<const std::int32_t> input) noexcept
{
    if (control == nullptr ||
        !control->local_monitor_enabled.load(std::memory_order_relaxed) ||
        input.empty()) {
        if (control != nullptr) {
            control->monitor_peak_ppm.store(0, std::memory_order_relaxed);
            control->gui_monitor_peak_ppm.store(0, std::memory_order_relaxed);
        }
        return;
    }
    const int levelPpm = control->local_monitor_level_ppm.load(
        std::memory_order_relaxed);
    if (levelPpm <= 0) {
        control->monitor_peak_ppm.store(0, std::memory_order_relaxed);
        control->gui_monitor_peak_ppm.store(0, std::memory_order_relaxed);
        return;
    }
    const double level = static_cast<double>(
        std::clamp(levelPpm, 0, 4000000)) / 1000000.0;
    std::uint32_t monitorPeak = 0;
    const std::size_t frames = (std::min)(output.size(), input.size());
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::int32_t monitored = scale_i32_sample(input[frame], level);
        const std::uint32_t absolute =
            monitored == (std::numeric_limits<std::int32_t>::min)()
            ? static_cast<std::uint32_t>(
                (std::numeric_limits<std::int32_t>::max)())
            : static_cast<std::uint32_t>(std::abs(monitored));
        monitorPeak = (std::max)(monitorPeak, absolute);
        output[frame] = mix_i32_samples(output[frame], monitored);
    }
    const double normalized = static_cast<double>(monitorPeak) / 2147483647.0;
    const int peakPpm = static_cast<int>(
        std::clamp(normalized, 0.0, 1.0) * 1000000.0);
    control->monitor_peak_ppm.store(peakPpm, std::memory_order_relaxed);
    update_interval_peak(control->gui_monitor_peak_ppm, peakPpm);
}

void mix_prepared_source(
    StreamControl* control,
    std::span<std::int32_t> output,
    std::uint64_t frame,
    std::span<std::int32_t> stem) noexcept
{
    if (control == nullptr || control->prepared_source == nullptr ||
        output.empty()) {
        if (control != nullptr) {
            control->prepared_track_peak_ppm.store(0, std::memory_order_relaxed);
        }
        return;
    }
    const int peak = control->prepared_source->mix(
        output.data(), output.size(), frame, stem);
    control->prepared_track_peak_ppm.store(peak, std::memory_order_relaxed);
    update_interval_peak(control->gui_prepared_track_peak_ppm, peak);
    control->prepared_source_frame.store(
        control->prepared_source->sourceFrame(), std::memory_order_relaxed);
    control->prepared_source_scheduled_start_frame.store(
        control->prepared_source->scheduledStartFrame(),
        std::memory_order_relaxed);
    control->prepared_source_actual_start_frame.store(
        control->prepared_source->actualStartFrame(), std::memory_order_relaxed);
    control->prepared_source_underruns.store(
        control->prepared_source->underruns(), std::memory_order_relaxed);
}

void observe_output_peak(
    StreamControl* control,
    std::span<const std::int32_t> output) noexcept
{
    if (control == nullptr) {
        return;
    }
    observe_peak(control->output_peak_ppm, output);
    observe_peak(control->gui_output_peak_ppm, output);
    for (const std::int32_t sample : output) {
        if (sample == (std::numeric_limits<std::int32_t>::min)() ||
            sample == (std::numeric_limits<std::int32_t>::max)()) {
            control->output_clipped_samples.fetch_add(
                1, std::memory_order_relaxed);
        }
    }
}

void pop_resampled_playback(
    MonoRingBuffer* playback,
    StreamControl* control,
    PlaybackResamplerState& state,
    std::span<std::int32_t> output) noexcept
{
    if (playback == nullptr || control == nullptr) {
        std::fill(output.begin(), output.end(), 0);
        return;
    }
    state.ratioSmoother.setTargetPpm(
        control->playback_ratio_ppm.load(std::memory_order_relaxed),
        control->playback_ratio_ramp_frames.load(std::memory_order_relaxed));
    if (state.ratioSmoother.steadyUnity() &&
        !state.hasCurrent && !state.hasNext) {
        playback->pop(output);
        control->playback_ratio_applied_ppm.store(
            1000000, std::memory_order_relaxed);
        control->playback_ratio_ramping.store(false, std::memory_order_relaxed);
        return;
    }
    if (!state.hasCurrent) {
        state.current = pop_one_frame(*playback, state, 0);
        state.hasCurrent = true;
    }
    if (!state.hasNext) {
        state.next = pop_one_frame(*playback, state, state.current);
        state.hasNext = true;
    }
    for (std::int32_t& sample : output) {
        const double ratio = state.ratioSmoother.nextRatio();
        const double mixed = static_cast<double>(state.current) +
            ((static_cast<double>(state.next) -
              static_cast<double>(state.current)) * state.phase);
        sample = static_cast<std::int32_t>(
            std::clamp(mixed, -2147483648.0, 2147483647.0));
        state.phase += ratio;
        while (state.phase >= 1.0) {
            state.phase -= 1.0;
            state.current = state.next;
            // Preserve the last real sample for an isolated shortage. A
            // literal zero here creates a full-scale discontinuity at a
            // non-unity playback ratio even when the producer refills the
            // ring one frame later. The ring still records the exact underrun.
            state.next = pop_one_frame(*playback, state, state.current);
        }
    }
    control->playback_ratio_applied_ppm.store(
        state.ratioSmoother.appliedPpm(), std::memory_order_relaxed);
    control->playback_ratio_ramping.store(
        state.ratioSmoother.ramping(), std::memory_order_relaxed);
}

std::int32_t render_test_input_sample(
    int mode,
    std::uint64_t sampleTime,
    double sampleRate,
    double level) noexcept
{
    if (mode == 1 || !valid_render_sample_rate(sampleRate) ||
        !std::isfinite(level)) {
        return 0;
    }
    if (mode == 2 || mode == 5) {
        const double frequency = mode == 2 ? 440.0 : 30.867706;
        const double phase = std::fmod(
            static_cast<double>(sampleTime) * frequency / sampleRate, 1.0);
        if (!std::isfinite(phase)) {
            return 0;
        }
        const double rendered =
            std::sin(phase * 2.0 * kPi) * level * 2147483647.0;
        return static_cast<std::int32_t>(
            std::clamp(rendered, -2147483648.0, 2147483647.0));
    }
    if (mode == 3) {
        const std::uint64_t period = static_cast<std::uint64_t>(
            sampleRate > 1.0 ? sampleRate : 1.0);
        const std::uint64_t width = (std::max<std::uint64_t>)(1, period / 100);
        const double rendered = (sampleTime % period) < width
            ? level * 2147483647.0
            : 0.0;
        return static_cast<std::int32_t>(
            std::clamp(rendered, -2147483648.0, 2147483647.0));
    }
    return 0;
}

std::int32_t render_metronome_test_input_sample(
    const StreamControl& control,
    std::uint64_t sampleTime,
    double sampleRate,
    double level) noexcept
{
    if (!valid_render_sample_rate(sampleRate) ||
        !std::isfinite(level) ||
        !control.metronome_enabled.load(std::memory_order_relaxed) ||
        !control.metronome_epoch_valid.load(std::memory_order_relaxed)) {
        return 0;
    }
    const std::uint64_t epoch = control.metronome_epoch_sample_time.load(
        std::memory_order_relaxed);
    if (sampleTime < epoch) {
        return 0;
    }
    const metronome::PatternSnapshot pattern = metronome::sanitize({
        control.metronome_bpm.load(std::memory_order_relaxed),
        control.metronome_beats_per_bar.load(std::memory_order_relaxed),
        control.metronome_division.load(std::memory_order_relaxed),
        control.metronome_step_count.load(std::memory_order_relaxed),
        control.metronome_play_mask_low.load(std::memory_order_relaxed),
        control.metronome_play_mask_high.load(std::memory_order_relaxed),
        control.metronome_accent_mask_low.load(std::memory_order_relaxed),
        control.metronome_accent_mask_high.load(std::memory_order_relaxed),
        control.metronome_beat_unit.load(std::memory_order_relaxed),
        control.metronome_tempo_pulse_units.load(std::memory_order_relaxed),
    });
    const std::uint64_t interval = metronome::step_interval_samples(
        sampleRate,
        pattern.bpm,
        pattern.division,
        pattern.tempo_pulse_units);
    const double rendered = metronome::render_sample(
        pattern,
        sampleTime - epoch,
        interval,
        sampleRate,
        level,
        metronome::ClickVoice::Normal,
        metronome::sanitize_click_sound(
            control.metronome_sound.load(std::memory_order_relaxed)));
    return metronome::mix_i32(0, rendered);
}

void fill_test_input(
    StreamControl* control,
    double sampleRate,
    std::uint64_t& sampleCounter,
    std::span<std::int32_t> output) noexcept
{
    if (control == nullptr) {
        std::fill(output.begin(), output.end(), 0);
        return;
    }
    const int mode = control->test_input_mode.load(std::memory_order_relaxed);
    const double level = static_cast<double>(std::clamp(
        control->test_input_level_ppm.load(std::memory_order_relaxed),
        0,
        1000000)) / 1000000.0;
    for (std::int32_t& sample : output) {
        sample = mode == 4
            ? render_metronome_test_input_sample(
                *control, sampleCounter, sampleRate, level)
            : render_test_input_sample(mode, sampleCounter, sampleRate, level);
        ++sampleCounter;
    }
}

void mix_metronome_click(
    StreamControl* control,
    double sampleRate,
    std::uint64_t engineFrame,
    std::uint64_t& beatIndex,
    std::span<std::int32_t> output,
    std::span<std::int32_t> metronomeStem) noexcept
{
    if (control == nullptr || !valid_render_sample_rate(sampleRate)) {
        return;
    }
    if (metronomeStem.size() == output.size()) {
        std::fill(metronomeStem.begin(), metronomeStem.end(), 0);
    }
    const bool enabled =
        control->metronome_enabled.load(std::memory_order_relaxed) ||
        control->playback_count_in_active.load(std::memory_order_relaxed);
    const bool transportGated = control->metronome_transport_gated.load(
        std::memory_order_relaxed);
    const bool localClickSuppressed =
        control->metronome_mode.load(std::memory_order_relaxed) == 1 &&
        !control->leader_audio_local_click.load(std::memory_order_relaxed);
    if (!metronome_output_allowed(
            enabled,
            localClickSuppressed,
            transportGated,
            control->transport_playback_active.load(std::memory_order_relaxed),
            control->recording_count_in_active.load(std::memory_order_relaxed))) {
        return;
    }
    const double level = static_cast<double>(std::clamp(
        control->metronome_level_ppm.load(std::memory_order_relaxed),
        0,
        4000000)) / 1000000.0;
    const bool epochValid = control->metronome_epoch_valid.load(
        std::memory_order_relaxed);
    const std::uint64_t epoch = control->metronome_epoch_sample_time.load(
        std::memory_order_relaxed);
    const std::int64_t renderOffset = control->metronome_render_offset_frames.load(
        std::memory_order_relaxed);
    const metronome::PatternSnapshot pattern = metronome::sanitize({
        control->metronome_bpm.load(std::memory_order_relaxed),
        control->metronome_beats_per_bar.load(std::memory_order_relaxed),
        control->metronome_division.load(std::memory_order_relaxed),
        control->metronome_step_count.load(std::memory_order_relaxed),
        control->metronome_play_mask_low.load(std::memory_order_relaxed),
        control->metronome_play_mask_high.load(std::memory_order_relaxed),
        control->metronome_accent_mask_low.load(std::memory_order_relaxed),
        control->metronome_accent_mask_high.load(std::memory_order_relaxed),
        control->metronome_beat_unit.load(std::memory_order_relaxed),
        control->metronome_tempo_pulse_units.load(std::memory_order_relaxed),
    });
    const std::uint64_t interval = metronome::step_interval_samples(
        sampleRate,
        pattern.bpm,
        pattern.division,
        pattern.tempo_pulse_units);
    const bool countInActive = control->recording_count_in_active.load(
        std::memory_order_acquire);
    const auto clickSound = metronome::sanitize_click_sound(
        control->metronome_sound.load(std::memory_order_relaxed));
    const std::uint64_t countInStart =
        control->recording_count_in_start_frame.load(std::memory_order_relaxed);
    const std::uint64_t countInTarget =
        control->recording_count_in_target_frame.load(std::memory_order_relaxed);
    for (std::size_t frame = 0; frame < output.size(); ++frame) {
        const std::uint64_t frameOffset = static_cast<std::uint64_t>(frame);
        const std::uint64_t rawFrame = frameOffset >
                (std::numeric_limits<std::uint64_t>::max)() - engineFrame
            ? (std::numeric_limits<std::uint64_t>::max)()
            : engineFrame + frameOffset;
        if (countInActive && rawFrame < countInStart) {
            continue;
        }
        const std::uint64_t musicalFrame = metronome_musical_frame_from_raw(
            rawFrame, renderOffset);
        std::uint64_t position = 0;
        if (!metronome_pattern_position(
                *control,
                rawFrame,
                musicalFrame,
                epochValid,
                epoch,
                position)) {
            continue;
        }
        const double rendered = metronome::render_sample(
            pattern,
            position,
            interval,
            sampleRate,
            level,
            countInActive && rawFrame >= countInStart && rawFrame < countInTarget
                ? metronome::ClickVoice::CountIn
                : metronome::ClickVoice::Normal,
            clickSound);
        if (metronomeStem.size() == output.size()) {
            metronomeStem[frame] = metronome::mix_i32(0, rendered);
        }
        output[frame] = metronome::mix_i32(output[frame], rendered);
        if (interval > 0) {
            const std::uint64_t zeroBasedBeat = position / interval;
            beatIndex = zeroBasedBeat ==
                    (std::numeric_limits<std::uint64_t>::max)()
                ? zeroBasedBeat
                : zeroBasedBeat + 1;
        }
    }
}

} // namespace jam2::audio::device_processing

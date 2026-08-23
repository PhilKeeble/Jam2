#include "audio_device_processing.hpp"
#include "runtime_limits.hpp"

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

void single_writer_update_max(
    std::atomic<std::uint64_t>& target,
    std::uint64_t value) noexcept
{
    if (value > target.load(std::memory_order_relaxed)) {
        target.store(value, std::memory_order_relaxed);
    }
}

void single_writer_update_min(
    std::atomic<std::uint64_t>& target,
    std::uint64_t value) noexcept
{
    const std::uint64_t current = target.load(std::memory_order_relaxed);
    if (current == 0 || value < current) {
        target.store(value, std::memory_order_relaxed);
    }
}

void single_writer_add(
    std::atomic<std::uint64_t>& target,
    std::uint64_t value = 1) noexcept
{
    target.store(
        target.load(std::memory_order_relaxed) + value,
        std::memory_order_relaxed);
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

bool DriverOutputReadyState::shouldNotify() const noexcept
{
    return status == DriverOutputReadyStatus::Active;
}

void DriverOutputReadyState::observe(
    DriverOutputReadyObservation observation,
    long errorCode) noexcept
{
    switch (observation) {
    case DriverOutputReadyObservation::Accepted:
        status = DriverOutputReadyStatus::Active;
        error = 0;
        break;
    case DriverOutputReadyObservation::Unsupported:
        status = DriverOutputReadyStatus::Unsupported;
        error = 0;
        break;
    case DriverOutputReadyObservation::Error:
        status = DriverOutputReadyStatus::Error;
        error = errorCode;
        break;
    }
}

long driver_output_ready_latency_reduction(
    const DriverOutputReadyState& state,
    long beforeFrames,
    long afterFrames) noexcept
{
    if (!state.shouldNotify() || beforeFrames <= afterFrames) {
        return 0;
    }
    return beforeFrames - afterFrames;
}

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

namespace {

std::size_t metronome_wave_index(
    bool accent,
    metronome::ClickVoice voice,
    metronome::ClickSound sound) noexcept
{
    return static_cast<std::size_t>(sound) * 4U +
        (voice == metronome::ClickVoice::CountIn ? 2U : 0U) +
        (accent ? 1U : 0U);
}

} // namespace

MetronomeWaveBank::MetronomeWaveBank(double sampleRate)
{
    prepare(sampleRate);
}

void MetronomeWaveBank::prepare(double sampleRate)
{
    sampleRate_ = 0.0;
    for (auto& wave : waves_) {
        wave.clear();
    }
    if (!valid_render_sample_rate(sampleRate) ||
        sampleRate < static_cast<double>(limits::kMinimumSampleRate) ||
        sampleRate > static_cast<double>(limits::kMaximumSampleRate)) {
        return;
    }
    for (int soundValue = static_cast<int>(metronome::ClickSound::Classic);
         soundValue <= static_cast<int>(metronome::ClickSound::DigitalTick);
         ++soundValue) {
        const auto sound = static_cast<metronome::ClickSound>(soundValue);
        for (const auto voice : {
                 metronome::ClickVoice::Normal,
                 metronome::ClickVoice::CountIn}) {
            for (const bool accent : {false, true}) {
                auto& wave = waves_[metronome_wave_index(accent, voice, sound)];
                wave.resize(static_cast<std::size_t>(
                    metronome::click_duration_samples(
                        sampleRate, accent, voice, sound)));
                for (std::size_t offset = 0; offset < wave.size(); ++offset) {
                    wave[offset] = metronome::render_click_tone_sample(
                        offset, sampleRate, accent, voice, sound);
                }
            }
        }
    }
    sampleRate_ = sampleRate;
}

bool MetronomeWaveBank::preparedFor(double sampleRate) const noexcept
{
    return sampleRate_ == sampleRate && sampleRate_ > 0.0;
}

double MetronomeWaveBank::render(
    const metronome::PatternSnapshot& pattern,
    int patternStep,
    std::uint64_t stepOffset,
    double level,
    metronome::ClickVoice voice,
    metronome::ClickSound sound) const noexcept
{
    if (pattern.step_count <= 0 || patternStep < 0 ||
        patternStep >= pattern.step_count ||
        !metronome::mask_enabled(
            pattern.play_mask_low, pattern.play_mask_high, patternStep)) {
        return 0.0;
    }
    const bool accent = metronome::mask_enabled(
        pattern.accent_mask_low, pattern.accent_mask_high, patternStep);
    sound = metronome::sanitize_click_sound(static_cast<int>(sound));
    const auto& wave = waves_[metronome_wave_index(accent, voice, sound)];
    if (stepOffset >= wave.size()) {
        return 0.0;
    }
    const double clickLevel = std::clamp(level, 0.0, 1.0) *
        (accent ? 1.25 : 0.78);
    return std::clamp(wave[static_cast<std::size_t>(stepOffset)] * clickLevel, -1.0, 1.0);
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
    const std::uint64_t previous = state.lastCallbackUs;
    state.lastCallbackUs = nowUs;
    if (previous == 0 || nowUs <= previous || bufferFrames == 0 ||
        !std::isfinite(sampleRate) || sampleRate <= 0.0) {
        return;
    }
    const std::uint64_t interval = nowUs - previous;
    single_writer_update_min(state.minimumUs, interval);
    single_writer_add(state.sumUs, interval);
    single_writer_update_max(state.maximumUs, interval);
    single_writer_add(state.samples);
    const double expected = static_cast<double>(bufferFrames) * 1000000.0 /
        sampleRate;
    if (interval_exceeds(interval, expected, 1.1)) {
        single_writer_add(state.gapsOver1_1x);
    }
    if (interval_exceeds(interval, expected, 1.5)) {
        single_writer_add(state.gapsOver1_5x);
    }
    if (interval_exceeds(interval, expected, 2.0)) {
        single_writer_add(state.gapsOver2x);
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

void observe_shared_peak(
    std::atomic<int>& currentPeak,
    std::atomic<int>& intervalPeak,
    std::span<const std::int32_t> samples) noexcept
{
    const int peak = i32_peak_ppm(samples);
    update_peak(currentPeak, peak);
    update_peak(intervalPeak, peak);
}

void observe_input_peaks(
    StreamControl* control,
    std::span<const std::int32_t> samples) noexcept
{
    if (control == nullptr) return;
    observe_input_peak_value(control, i32_peak_ppm(samples));
}

void observe_input_peak_value(StreamControl* control, int inputPeak) noexcept
{
    if (control == nullptr) return;
    inputPeak = std::max(0, inputPeak);
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
    std::uint32_t monitorPeak = 0;
    const std::size_t frames = (std::min)(output.size(), input.size());
    if (levelPpm == 1000000) {
        for (std::size_t frame = 0; frame < frames; ++frame) {
            const std::int32_t monitored = input[frame];
            const std::uint32_t absolute =
                monitored == (std::numeric_limits<std::int32_t>::min)()
                ? static_cast<std::uint32_t>(
                    (std::numeric_limits<std::int32_t>::max)())
                : static_cast<std::uint32_t>(std::abs(monitored));
            monitorPeak = (std::max)(monitorPeak, absolute);
            output[frame] = mix_i32_samples(output[frame], monitored);
        }
    } else {
        const double level = static_cast<double>(
            std::clamp(levelPpm, 0, 4000000)) / 1000000.0;
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
    if (!control->prepared_source->needsProcessing()) {
        if (stem.size() == output.size()) {
            std::fill(stem.begin(), stem.end(), 0);
        }
        control->prepared_track_peak_ppm.store(0, std::memory_order_relaxed);
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
    std::uint32_t peak = 0;
    std::uint64_t clipped = 0;
    for (const std::int32_t sample : output) {
        const std::uint32_t magnitude =
            sample == (std::numeric_limits<std::int32_t>::min)()
            ? static_cast<std::uint32_t>(
                (std::numeric_limits<std::int32_t>::max)())
            : static_cast<std::uint32_t>(std::abs(sample));
        peak = (std::max)(peak, magnitude);
        if (sample == (std::numeric_limits<std::int32_t>::min)() ||
            sample == (std::numeric_limits<std::int32_t>::max)()) {
            ++clipped;
        }
    }
    const int peakPpm = static_cast<int>(
        std::clamp(
            static_cast<double>(peak) / 2147483647.0,
            0.0,
            1.0) * 1000000.0);
    update_peak(control->output_peak_ppm, peakPpm);
    update_peak(control->gui_output_peak_ppm, peakPpm);
    if (clipped > 0) {
        control->output_clipped_samples.fetch_add(
            clipped, std::memory_order_relaxed);
    }
}

void observe_callback_work(
    CallbackIntervalState& state,
    std::uint64_t startUs,
    std::uint64_t endUs) noexcept
{
    if (endUs < startUs) {
        return;
    }
    const std::uint64_t duration = endUs - startUs;
    single_writer_update_min(state.workMinimumUs, duration);
    single_writer_add(state.workSumUs, duration);
    single_writer_update_max(state.workMaximumUs, duration);
    single_writer_add(state.workSamples);
}

void publish_callback_begin(
    StreamControl* control,
    std::uint64_t& writerGeneration) noexcept
{
    if (control == nullptr) {
        return;
    }
    writerGeneration += (writerGeneration & 1ULL) == 0 ? 1ULL : 2ULL;
    control->audio_callback_generation.store(
        writerGeneration,
        std::memory_order_release);
}

void publish_callback_end(
    StreamControl* control,
    std::uint64_t& writerGeneration) noexcept
{
    if (control == nullptr) {
        return;
    }
    writerGeneration += (writerGeneration & 1ULL) != 0 ? 1ULL : 2ULL;
    control->audio_callback_generation.store(
        writerGeneration,
        std::memory_order_release);
}

void pop_resampled_playback(
    MonoRingBuffer* playback,
    StreamControl* control,
    PlaybackResamplerState& state,
    std::span<std::int32_t> sourceScratch,
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
    PlaybackRatioSmoother sizingSmoother = state.ratioSmoother;
    double sizingPhase = state.phase;
    std::size_t requiredSourceFrames =
        (state.hasCurrent ? 0U : 1U) + (state.hasNext ? 0U : 1U);
    for (std::size_t frame = 0; frame < output.size(); ++frame) {
        sizingPhase += sizingSmoother.nextRatio();
        while (sizingPhase >= 1.0) {
            sizingPhase -= 1.0;
            ++requiredSourceFrames;
        }
    }
    const bool batchSource = sourceScratch.size() >= requiredSourceFrames;
    std::size_t realSourceFrames = 0;
    std::size_t sourceIndex = 0;
    if (batchSource && requiredSourceFrames > 0) {
        realSourceFrames = playback->pop(
            sourceScratch.first(requiredSourceFrames), false);
    }
    auto nextSource = [&](std::int32_t continuityFallback) noexcept {
        if (!batchSource) {
            return pop_one_frame(*playback, state, continuityFallback);
        }
        if (sourceIndex < realSourceFrames) {
            state.underrunConcealmentOrigin = 0;
            state.underrunConcealmentFrames = 0;
            return sourceScratch[sourceIndex++];
        }
        ++sourceIndex;
        if (state.underrunConcealmentFrames == 0) {
            state.underrunConcealmentOrigin = continuityFallback;
        }
        if (state.underrunConcealmentFrames >= kPlaybackUnderrunFadeFrames) {
            return std::int32_t{0};
        }
        const std::uint32_t remaining =
            kPlaybackUnderrunFadeFrames - state.underrunConcealmentFrames;
        ++state.underrunConcealmentFrames;
        return static_cast<std::int32_t>(
            static_cast<std::int64_t>(state.underrunConcealmentOrigin) *
            static_cast<std::int64_t>(remaining) /
            static_cast<std::int64_t>(kPlaybackUnderrunFadeFrames));
    };
    if (!state.hasCurrent) {
        state.current = nextSource(0);
        state.hasCurrent = true;
    }
    if (!state.hasNext) {
        state.next = nextSource(state.current);
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
            state.next = nextSource(state.current);
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
    std::span<std::int32_t> metronomeStem,
    const MetronomeWaveBank* waveBank) noexcept
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
    // Engine commands are applied before this callback is rendered. Snapshot
    // the click origin once so every sample in the block observes one coherent
    // state and the hot loop does not reload up to seven atomics per frame.
    bool patternOriginValid = control->metronome_pattern_origin_valid.load(
        std::memory_order_relaxed);
    std::uint64_t patternOrigin = patternOriginValid
        ? control->metronome_pattern_origin_frame.load(std::memory_order_relaxed)
        : epoch;
    std::uint64_t scheduledPatternOrigin =
        control->metronome_pattern_scheduled_origin_raw_frame.load(
            std::memory_order_relaxed);
    std::uint32_t clickPeak = 0;
    bool positionSequenceValid = false;
    bool previousInCountIn = false;
    std::uint64_t previousPosition = 0;
    std::uint64_t stepIndex = 0;
    std::uint64_t stepOffset = 0;
    int patternStep = 0;
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
        const bool inCountIn = countInActive &&
            rawFrame >= countInStart && rawFrame < countInTarget;
        if (inCountIn) {
            position = rawFrame - countInStart;
        } else {
            if (scheduledPatternOrigin != 0 &&
                rawFrame >= scheduledPatternOrigin) {
                patternOrigin = metronome_musical_frame_from_raw(
                    scheduledPatternOrigin, renderOffset);
                patternOriginValid = true;
                scheduledPatternOrigin = 0;
                control->metronome_pattern_origin_frame.store(
                    patternOrigin, std::memory_order_relaxed);
                control->metronome_pattern_origin_valid.store(
                    true, std::memory_order_relaxed);
                control->metronome_pattern_scheduled_origin_raw_frame.store(
                    0, std::memory_order_relaxed);
            }
            if ((patternOriginValid || epochValid) &&
                musicalFrame < patternOrigin) {
                continue;
            }
            position = patternOriginValid || epochValid
                ? musicalFrame - patternOrigin
                : musicalFrame;
        }
        double rendered = 0.0;
        if (interval > 0) {
            const bool sequential = positionSequenceValid &&
                previousInCountIn == inCountIn &&
                previousPosition != (std::numeric_limits<std::uint64_t>::max)() &&
                position == previousPosition + 1ULL;
            if (sequential) {
                ++stepOffset;
                if (stepOffset == interval) {
                    stepOffset = 0;
                    ++stepIndex;
                    ++patternStep;
                    if (patternStep == pattern.step_count) {
                        patternStep = 0;
                    }
                }
            } else {
                stepIndex = position / interval;
                stepOffset = position % interval;
                patternStep = static_cast<int>(
                    stepIndex % static_cast<std::uint64_t>(pattern.step_count));
            }
            const auto voice = inCountIn
                ? metronome::ClickVoice::CountIn
                : metronome::ClickVoice::Normal;
            rendered = waveBank != nullptr && waveBank->preparedFor(sampleRate)
                ? waveBank->render(
                    pattern, patternStep, stepOffset, level, voice, clickSound)
                : metronome::render_pattern_step_sample(
                    pattern,
                    patternStep,
                    stepOffset,
                    sampleRate,
                    level,
                    voice,
                    clickSound);
            previousPosition = position;
            previousInCountIn = inCountIn;
            positionSequenceValid = true;
        }
        const std::int32_t clickSample = metronome::mix_i32(0, rendered);
        const std::uint32_t magnitude = clickSample ==
                (std::numeric_limits<std::int32_t>::min)()
            ? static_cast<std::uint32_t>(
                (std::numeric_limits<std::int32_t>::max)())
            : static_cast<std::uint32_t>(std::abs(clickSample));
        clickPeak = (std::max)(clickPeak, magnitude);
        if (metronomeStem.size() == output.size()) {
            metronomeStem[frame] = clickSample;
        }
        output[frame] = metronome::mix_i32(output[frame], rendered);
        if (interval > 0) {
            beatIndex = stepIndex ==
                    (std::numeric_limits<std::uint64_t>::max)()
                ? stepIndex
                : stepIndex + 1;
        }
    }
    const int peakPpm = static_cast<int>(
        static_cast<std::uint64_t>(clickPeak) * 1000000ULL /
        static_cast<std::uint64_t>(
            (std::numeric_limits<std::int32_t>::max)()));
    update_peak(control->metronome_peak_ppm, peakPpm);
    update_peak(control->gui_metronome_peak_ppm, peakPpm);
}

} // namespace jam2::audio::device_processing

#include "audio_device_processing.hpp"
#include "engine.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <thread>

namespace {

namespace processing = jam2::audio::device_processing;

int failures = 0;

void expect(bool condition, const char* message)
{
    if (condition) {
        return;
    }
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

template <std::size_t Size>
bool anyNonzero(const std::array<std::int32_t, Size>& samples)
{
    return std::any_of(samples.begin(), samples.end(), [](std::int32_t sample) {
        return sample != 0;
    });
}

void configureMetronome(jam2::audio::StreamControl& control)
{
    control.metronome_enabled.store(true);
    control.metronome_epoch_valid.store(true);
    control.metronome_epoch_sample_time.store(0);
    control.metronome_level_ppm.store(500000);
    control.metronome_bpm.store(120);
    control.metronome_beats_per_bar.store(4);
    control.metronome_division.store(1);
    control.metronome_step_count.store(4);
    control.metronome_play_mask_low.store(0x0fULL);
    control.metronome_accent_mask_low.store(0x01ULL);
}

void testCallbackTiming()
{
    processing::CallbackIntervalState state;
    processing::observe_callback_interval(state, 1000, 480, 48000.0);
    processing::observe_callback_interval(state, 11000, 480, 48000.0);
    processing::observe_callback_interval(state, 23000, 480, 48000.0);
    processing::observe_callback_interval(state, 39000, 480, 48000.0);
    processing::observe_callback_interval(state, 61000, 480, 48000.0);
    expect(state.minimumUs.load() == 10000 &&
            state.sumUs.load() == 60000 &&
            state.maximumUs.load() == 22000 &&
            state.samples.load() == 4,
        "callback timing records exact interval aggregates");
    expect(state.gapsOver1_1x.load() == 3 &&
            state.gapsOver1_5x.load() == 2 &&
            state.gapsOver2x.load() == 1,
        "callback timing classifies all strict gap thresholds");

    processing::CallbackIntervalState invalid;
    processing::observe_callback_interval(invalid, 1000, 480, 48000.0);
    processing::observe_callback_interval(
        invalid,
        2000,
        480,
        (std::numeric_limits<double>::quiet_NaN)());
    processing::observe_callback_interval(invalid, 1500, 480, 48000.0);
    processing::observe_callback_interval(invalid, 2500, 0, 48000.0);
    expect(invalid.samples.load() == 0 && invalid.sumUs.load() == 0,
        "callback timing rejects non-finite, backwards, and empty observations");
    processing::CallbackIntervalState extreme;
    processing::observe_callback_interval(extreme, 1, 480, 48000.0);
    processing::observe_callback_interval(
        extreme,
        2,
        (std::numeric_limits<std::size_t>::max)(),
        (std::numeric_limits<double>::denorm_min)());
    expect(extreme.samples.load() == 1 &&
            extreme.gapsOver1_1x.load() == 0 &&
            extreme.gapsOver1_5x.load() == 0 &&
            extreme.gapsOver2x.load() == 0,
        "unrepresentable callback thresholds remain defined and non-gaps");
    expect(processing::callback_now_us() > 0,
        "callback clock publishes a monotonic-clock timestamp");
}

void testNetworkPlaybackTimelineCoherence()
{
    jam2::audio::StreamControl control;
    jam2::audio::MonoRingBuffer playback(32);
    const std::array<std::int32_t, 7> frames{};
    (void)playback.push(frames);
    control.engine_frame_counter.store(4096, std::memory_order_relaxed);
    std::uint64_t engineFrame = 0;
    std::size_t queuedFrames = 0;
    expect(processing::read_network_playback_timeline(
               control, playback, engineFrame, queuedFrames) &&
            engineFrame == 4096 && queuedFrames == frames.size(),
        "playback timeline returns one coherent frame/depth pair");

    control.audio_callback_generation.store(1, std::memory_order_release);
    engineFrame = 99;
    queuedFrames = 99;
    expect(!processing::read_network_playback_timeline(
               control, playback, engineFrame, queuedFrames) &&
            engineFrame == 99 && queuedFrames == 99,
        "playback timeline rejects values while an audio callback is active");
}

void testPeakGainAndMixing()
{
    constexpr auto minimum = (std::numeric_limits<std::int32_t>::min)();
    constexpr auto maximum = (std::numeric_limits<std::int32_t>::max)();
    const std::array<std::int32_t, 0> empty{};
    const std::array<std::int32_t, 3> extremes{minimum, 0, maximum};
    expect(processing::i32_peak_ppm(empty) == 0 &&
            processing::i32_peak_ppm(extremes) == 1000000,
        "integer peak conversion handles empty and signed-minimum inputs");

    std::atomic<int> peak{100};
    processing::update_peak(peak, 50);
    processing::update_peak(peak, 200);
    expect(peak.load() == 200, "peak publication retains the interval maximum");
    expect(processing::scale_i32_sample(1000, 0.5) == 500 &&
            processing::scale_i32_sample(
                1000, (std::numeric_limits<double>::quiet_NaN)()) == 0 &&
            processing::scale_i32_sample(maximum, 4.0) == maximum &&
            processing::scale_i32_sample(minimum, 4.0) == minimum,
        "sample scaling handles finite gain, non-finite gain, and saturation");
    expect(processing::mix_i32_samples(maximum, 1) == maximum &&
            processing::mix_i32_samples(minimum, -1) == minimum &&
            processing::mix_i32_samples(20, -7) == 13,
        "integer mixing saturates both rails");

    jam2::audio::StreamControl control;
    control.send_level_ppm.store(2000000);
    const std::array<std::int32_t, 2> input{maximum / 2, -(maximum / 4)};
    const int inputPeak = processing::i32_peak_ppm(input);
    processing::observe_input_peaks(&control, input);
    expect(control.input_peak_ppm.load() == inputPeak &&
            control.gui_input_peak_ppm.load() == inputPeak &&
            control.send_peak_ppm.load() == inputPeak * 2 &&
            control.gui_send_peak_ppm.load() == inputPeak * 2,
        "input observation publishes input and gain-adjusted send peaks");
    processing::observe_input_peaks(nullptr, input);

    std::array<std::int32_t, 3> remote{1000, -1000, maximum};
    control.remote_level_ppm.store(500000);
    processing::apply_remote_level(&control, remote);
    expect(remote[0] == 500 && remote[1] == -500 && remote[2] == maximum / 2,
        "remote gain applies its clamped numeric control");
    control.output_level_ppm.store(4000000);
    processing::apply_output_level(&control, remote);
    expect(remote[0] == 2000 && remote[1] == -2000 && remote[2] == maximum,
        "output gain saturates after remote mixing");
    const auto unchanged = remote;
    processing::apply_remote_level(nullptr, remote);
    processing::apply_output_level(nullptr, remote);
    expect(remote == unchanged, "null gain controls leave audio untouched");
    jam2::audio::StreamControl unityControl;
    std::array<std::int32_t, 2> unityGain{123, -456};
    processing::apply_remote_level(&unityControl, unityGain);
    processing::apply_output_level(&unityControl, unityGain);
    expect(unityGain == std::array<std::int32_t, 2>{123, -456},
        "unity remote and output levels preserve exact samples");

    std::array<std::int32_t, 3> monitoredOutput{1, 2, 3};
    control.monitor_peak_ppm.store(99);
    control.gui_monitor_peak_ppm.store(99);
    processing::mix_local_monitor(&control, monitoredOutput, input);
    expect(monitoredOutput == std::array<std::int32_t, 3>{1, 2, 3} &&
            control.monitor_peak_ppm.load() == 0 &&
            control.gui_monitor_peak_ppm.load() == 0,
        "disabled monitoring clears its meters without changing output");
    control.local_monitor_enabled.store(true);
    control.local_monitor_level_ppm.store(500000);
    processing::mix_local_monitor(&control, monitoredOutput, input);
    expect(monitoredOutput[0] == 1 + maximum / 4 &&
            monitoredOutput[1] == 2 - maximum / 8 &&
            monitoredOutput[2] == 3 &&
            control.monitor_peak_ppm.load() > 0 &&
            control.gui_monitor_peak_ppm.load() == control.monitor_peak_ppm.load(),
        "monitoring mixes the shared frame range and publishes its peak");
    control.local_monitor_level_ppm.store(0);
    processing::mix_local_monitor(&control, monitoredOutput, input);
    expect(control.monitor_peak_ppm.load() == 0,
        "zero monitor level clears the current monitor meter");
    const auto monitorUnchanged = monitoredOutput;
    processing::mix_local_monitor(nullptr, monitoredOutput, input);
    expect(monitoredOutput == monitorUnchanged,
        "missing monitor control leaves output untouched");

    control.output_peak_ppm.store(0);
    control.gui_output_peak_ppm.store(0);
    control.output_clipped_samples.store(0);
    processing::observe_output_peak(&control, extremes);
    expect(control.output_peak_ppm.load() == 1000000 &&
            control.gui_output_peak_ppm.load() == 1000000 &&
            control.output_clipped_samples.load() == 2,
        "output observation reports rail peaks and exact clipped-sample count");
    processing::observe_output_peak(nullptr, extremes);
}

void testPlaybackResampler()
{
    jam2::audio::StreamControl control;
    processing::PlaybackResamplerState state;
    std::array<std::int32_t, 4> output{9, 9, 9, 9};
    processing::pop_resampled_playback(nullptr, &control, state, output);
    expect(output == std::array<std::int32_t, 4>{0, 0, 0, 0},
        "missing playback source produces deterministic silence");
    output.fill(9);
    jam2::audio::MonoRingBuffer missingControlRing(8);
    processing::pop_resampled_playback(
        &missingControlRing, nullptr, state, output);
    expect(output == std::array<std::int32_t, 4>{0, 0, 0, 0},
        "missing playback control also produces deterministic silence");

    jam2::audio::MonoRingBuffer unityRing(8);
    const std::array<std::int32_t, 3> unityInput{10, 20, 30};
    unityRing.push(unityInput);
    output.fill(-1);
    processing::pop_resampled_playback(&unityRing, &control, state, output);
    expect(output == std::array<std::int32_t, 4>{10, 20, 30, 0} &&
            control.playback_ratio_applied_ppm.load() == 1000000 &&
            !control.playback_ratio_ramping.load(),
        "steady-unity playback uses the direct bounded ring path");

    constexpr auto minimum = (std::numeric_limits<std::int32_t>::min)();
    constexpr auto maximum = (std::numeric_limits<std::int32_t>::max)();
    jam2::audio::MonoRingBuffer halfRing(8);
    const std::array<std::int32_t, 3> halfInput{minimum, maximum, 0};
    halfRing.push(halfInput);
    control.playback_ratio_ppm.store(500000);
    state.reset();
    std::array<std::int32_t, 3> halfOutput{};
    processing::pop_resampled_playback(&halfRing, &control, state, halfOutput);
    expect(halfOutput == std::array<std::int32_t, 3>{minimum, 0, maximum},
        "half-rate interpolation crosses both integer rails without overflow");

    jam2::audio::MonoRingBuffer doubleRing(8);
    const std::array<std::int32_t, 4> doubleInput{10, 20, 30, 40};
    doubleRing.push(doubleInput);
    control.playback_ratio_ppm.store(2000000);
    state.reset();
    std::array<std::int32_t, 2> doubleOutput{};
    processing::pop_resampled_playback(&doubleRing, &control, state, doubleOutput);
    expect(doubleOutput == std::array<std::int32_t, 2>{10, 30},
        "double-rate playback consumes two source frames per output frame");

    jam2::audio::MonoRingBuffer rampRing(8);
    const std::array<std::int32_t, 4> rampInput{1, 2, 3, 4};
    rampRing.push(rampInput);
    control.playback_ratio_ppm.store(2000000);
    control.playback_ratio_ramp_frames.store(2);
    state.reset();
    std::array<std::int32_t, 2> rampOutput{};
    processing::pop_resampled_playback(&rampRing, &control, state, rampOutput);
    expect(control.playback_ratio_applied_ppm.load() == 2000000 &&
            !control.playback_ratio_ramping.load(),
        "playback-ratio ramp reaches and publishes its exact target");

    jam2::audio::MonoRingBuffer isolatedUnderrunRing(8);
    isolatedUnderrunRing.set_diagnostics_enabled(true);
    const std::array<std::int32_t, 2> steadyInput{1000000, 1000000};
    isolatedUnderrunRing.push(steadyInput);
    control.playback_ratio_ppm.store(1005000);
    control.playback_ratio_ramp_frames.store(0);
    state.reset();
    std::array<std::int32_t, 1> beforeRefill{};
    processing::pop_resampled_playback(
        &isolatedUnderrunRing, &control, state, beforeRefill);
    expect(isolatedUnderrunRing.stats().underruns == 1,
        "playback resampler fixture produces one isolated source underrun");
    const std::array<std::int32_t, 4> refill{1000000, 1000000, 1000000, 1000000};
    isolatedUnderrunRing.push(refill);
    std::array<std::int32_t, 4> afterRefill{};
    processing::pop_resampled_playback(
        &isolatedUnderrunRing, &control, state, afterRefill);
    expect(std::all_of(afterRefill.begin(), afterRefill.end(), [](std::int32_t sample) {
               return sample == 1000000;
           }),
        "isolated resampler underrun holds continuity instead of injecting a zero click");

    jam2::audio::MonoRingBuffer sustainedUnderrunRing(8);
    sustainedUnderrunRing.push(steadyInput);
    state.reset();
    std::array<std::int32_t, 40> sustainedUnderrunOutput{};
    processing::pop_resampled_playback(
        &sustainedUnderrunRing, &control, state, sustainedUnderrunOutput);
    expect(sustainedUnderrunOutput.front() == 1000000 &&
            sustainedUnderrunOutput.back() == 0,
        "sustained resampler underrun fades to silence instead of holding DC");

    state.reset();
    expect(!state.hasCurrent && !state.hasNext && state.phase == 0.0 &&
            state.current == 0 && state.next == 0 &&
            state.underrunConcealmentOrigin == 0 &&
            state.underrunConcealmentFrames == 0,
        "resampler reset discards all interpolation history");
}

void testPreparedSourceMixing()
{
    jam2::audio::StreamControl control;
    std::array<std::int32_t, 4> output{7, 7, 7, 7};
    std::array<std::int32_t, 4> stem{9, 9, 9, 9};
    control.prepared_track_peak_ppm.store(123);
    processing::mix_prepared_source(&control, output, 10, stem);
    expect(output == std::array<std::int32_t, 4>{7, 7, 7, 7} &&
            control.prepared_track_peak_ppm.load() == 0,
        "missing prepared source leaves output untouched and clears its live peak");

    jam2::audio::PreparedTrackSource source(16);
    const int slot = source.claimLoadingSlot();
    auto* data = source.loadingData(slot);
    expect(slot >= 0 && data != nullptr, "prepared-source fixture claims storage");
    if (data == nullptr) {
        return;
    }
    data[0] = 1000;
    data[1] = -2000;
    data[2] = 3000;
    expect(source.publishReady(slot, 3, 48000),
        "prepared-source fixture publishes a ready slot");
    jam2::audio::PreparedTrackSource::Command swap;
    swap.type = jam2::audio::PreparedTrackSource::CommandType::Swap;
    swap.slot = static_cast<std::uint32_t>(slot);
    swap.targetFrame = 10;
    jam2::audio::PreparedTrackSource::Command play;
    play.type = jam2::audio::PreparedTrackSource::CommandType::Play;
    play.targetFrame = 10;
    const std::array commands{swap, play};
    expect(source.enqueueBatch(commands),
        "prepared-source fixture queues one atomic swap/play transition");
    control.prepared_source = &source;
    output.fill(0);
    stem.fill(1);
    processing::mix_prepared_source(&control, output, 10, stem);
    expect(output == std::array<std::int32_t, 4>{
                65536000, -131072000, 196608000, 0} &&
            stem == output,
        "prepared source mixes PCM16 into output and the exact recorder stem");
    expect(control.prepared_track_peak_ppm.load() > 0 &&
            control.gui_prepared_track_peak_ppm.load() ==
                control.prepared_track_peak_ppm.load() &&
            control.prepared_source_frame.load() == 3 &&
            control.prepared_source_scheduled_start_frame.load() == 10 &&
            control.prepared_source_actual_start_frame.load() == 10 &&
            control.prepared_source_underruns.load() == source.underruns(),
        "prepared source publishes complete callback diagnostics");
    processing::mix_prepared_source(nullptr, output, 10, stem);
}

void testCancelledPreparedSwapsReleaseEveryReadySlot()
{
    jam2::audio::PreparedTrackSource source(16);
    for (std::size_t index = 0;
         index < jam2::audio::PreparedTrackSource::kSlots;
         ++index) {
        const int slot = source.claimLoadingSlot();
        expect(slot >= 0 && source.loadingData(slot) != nullptr &&
                source.publishReady(slot, 4, 48000),
            "cancelled-swap fixture fills each fixed source slot");
        if (slot < 0) return;
        jam2::audio::PreparedTrackSource::Command swap;
        swap.type = jam2::audio::PreparedTrackSource::CommandType::Swap;
        swap.slot = static_cast<std::uint32_t>(slot);
        swap.targetFrame = 48000;
        expect(source.enqueue(swap),
            "cancelled-swap fixture queues each future replacement");
    }
    expect(source.claimLoadingSlot() < 0,
        "all fixed source slots are occupied before cancellation");

    source.cancelScheduled();
    std::array<std::int32_t, 1> output{};
    (void)source.mix(output.data(), output.size(), 0);

    std::array<int, jam2::audio::PreparedTrackSource::kSlots> reclaimed{};
    for (int& slot : reclaimed) {
        slot = source.claimLoadingSlot();
    }
    expect(std::all_of(reclaimed.cbegin(), reclaimed.cend(),
               [](int slot) { return slot >= 0; }),
        "consuming cancelled swaps releases every orphaned Ready slot");
    for (int slot : reclaimed) {
        source.abandonLoadingSlot(slot);
    }
}

void testInjectedInput()
{
    const double nan = (std::numeric_limits<double>::quiet_NaN)();
    expect(processing::render_test_input_sample(1, 0, 48000.0, 1.0) == 0 &&
            processing::render_test_input_sample(99, 0, 48000.0, 1.0) == 0 &&
            processing::render_test_input_sample(2, 10, 0.0, 1.0) == 0 &&
            processing::render_test_input_sample(2, 10, nan, 1.0) == 0 &&
            processing::render_test_input_sample(2, 10, 48000.0, nan) == 0 &&
            processing::render_test_input_sample(
                2, 1, (std::numeric_limits<double>::denorm_min)(), 1.0) == 0 &&
            processing::render_test_input_sample(
                3, 0, (std::numeric_limits<double>::max)(), 1.0) == 0,
        "injected input rejects silent, unknown, and non-finite configurations");
    expect(processing::render_test_input_sample(2, 300, 48000.0, 0.5) != 0 &&
            processing::render_test_input_sample(5, 300, 48000.0, 0.5) != 0,
        "injected tone and bass modes render finite nonzero samples");
    expect(processing::render_test_input_sample(3, 0, 48000.0, 2.0) ==
                (std::numeric_limits<std::int32_t>::max)() &&
            processing::render_test_input_sample(3, 0, 48000.0, -2.0) ==
                (std::numeric_limits<std::int32_t>::min)() &&
            processing::render_test_input_sample(3, 1000, 48000.0, 1.0) == 0,
        "injected pulse mode clamps both rails and observes its pulse width");

    std::uint64_t counter = 7;
    std::array<std::int32_t, 3> output{1, 2, 3};
    processing::fill_test_input(nullptr, 48000.0, counter, output);
    expect(output == std::array<std::int32_t, 3>{0, 0, 0} && counter == 7,
        "missing injection control produces silence without consuming time");
    jam2::audio::StreamControl control;
    control.test_input_mode.store(3);
    control.test_input_level_ppm.store(500000);
    counter = 0;
    processing::fill_test_input(&control, 48000.0, counter, output);
    expect(output == std::array<std::int32_t, 3>{
                1073741823, 1073741823, 1073741823} && counter == 3,
        "buffer injection renders every sample and advances the shared counter");

    configureMetronome(control);
    control.metronome_epoch_sample_time.store(100);
    expect(processing::render_metronome_test_input_sample(
                control, 99, 48000.0, 0.5) == 0,
        "metronome injection remains silent before its epoch");
    std::array<std::int32_t, 64> click{};
    counter = 100;
    control.test_input_mode.store(4);
    processing::fill_test_input(&control, 48000.0, counter, click);
    expect(anyNonzero(click) && counter == 164,
        "metronome injection renders an epoch-aligned click block");
    control.metronome_enabled.store(false);
    expect(processing::render_metronome_test_input_sample(
                control, 101, 48000.0, 0.5) == 0 &&
            processing::render_metronome_test_input_sample(
                control,
                101,
                (std::numeric_limits<double>::max)(),
                0.5) == 0,
        "disabled or unrepresentable metronome injection is silent");
}

void testMetronomeOutput()
{
    jam2::audio::StreamControl control;
    std::array<std::int32_t, 64> output{};
    std::array<std::int32_t, 64> stem;
    stem.fill(9);
    std::uint64_t beatIndex = 0;
    processing::mix_metronome_click(
        &control, 48000.0, 0, beatIndex, output, stem);
    expect(!anyNonzero(output) && !anyNonzero(stem) && beatIndex == 0,
        "disabled metronome clears its stem without changing the mix");

    configureMetronome(control);
    processing::mix_metronome_click(
        &control, 48000.0, 0, beatIndex, output, stem);
    expect(anyNonzero(output) && output == stem && beatIndex == 1,
        "enabled metronome renders the shared-grid click and recorder stem");

    output.fill(0);
    stem.fill(7);
    control.metronome_mode.store(1);
    control.leader_audio_local_click.store(false);
    processing::mix_metronome_click(
        &control, 48000.0, 0, beatIndex, output, stem);
    expect(!anyNonzero(output) && !anyNonzero(stem),
        "leader-audio mode suppresses an unowned local click");

    control.leader_audio_local_click.store(true);
    processing::mix_metronome_click(
        &control, 48000.0, 0, beatIndex, output, stem);
    expect(anyNonzero(output) && output == stem,
        "leader-audio mode renders when this peer owns the local click");

    control.metronome_mode.store(0);
    control.leader_audio_local_click.store(false);
    control.metronome_transport_gated.store(true);
    control.transport_playback_active.store(false);
    output.fill(0);
    processing::mix_metronome_click(
        &control, 48000.0, 0, beatIndex, output, stem);
    expect(!anyNonzero(output),
        "transport gating suppresses a stopped normal click");

    control.transport_playback_active.store(true);
    output.fill(0);
    processing::mix_metronome_click(
        &control, 48000.0, 0, beatIndex, output, stem);
    expect(anyNonzero(output) && output == stem,
        "transport gating renders while playback is active");

    control.transport_playback_active.store(false);
    control.recording_count_in_active.store(true);
    control.recording_count_in_start_frame.store(100);
    control.recording_count_in_target_frame.store(1000);
    output.fill(0);
    processing::mix_metronome_click(
        &control, 48000.0, 100, beatIndex, output, stem);
    expect(anyNonzero(output) && output == stem && beatIndex == 1,
        "recording count-in overrides transport gating and uses its own origin");

    control.recording_count_in_active.store(false);
    control.metronome_enabled.store(false);
    control.playback_count_in_active.store(true);
    control.metronome_transport_gated.store(false);
    output.fill(0);
    processing::mix_metronome_click(
        &control, 48000.0, 0, beatIndex, output, stem);
    expect(anyNonzero(output) && output == stem,
        "playback count-in remains audible while the normal metronome is off");

    control.metronome_enabled.store(true);
    control.playback_count_in_active.store(false);
    control.metronome_transport_gated.store(false);
    control.metronome_render_offset_frames.store(
        (std::numeric_limits<std::int64_t>::min)());
    output.fill(0);
    stem.fill(5);
    processing::mix_metronome_click(
        &control,
        48000.0,
        (std::numeric_limits<std::uint64_t>::max)(),
        beatIndex,
        output,
        stem);
    expect(output == stem,
        "extreme frame and render-offset values remain in the defined domain");

    configureMetronome(control);
    control.metronome_bpm.store(400);
    control.metronome_division.store(8);
    control.metronome_tempo_pulse_units.store(3);
    control.metronome_play_mask_low.store(
        (std::numeric_limits<std::uint64_t>::max)());
    control.metronome_accent_mask_low.store(0);
    control.metronome_render_offset_frames.store(0);
    std::array<std::int32_t, 1> lastFrameOutput{};
    std::array<std::int32_t, 1> lastFrameStem{};
    beatIndex = 1;
    processing::mix_metronome_click(
        &control,
        1.0,
        (std::numeric_limits<std::uint64_t>::max)(),
        beatIndex,
        lastFrameOutput,
        lastFrameStem);
    expect(beatIndex == (std::numeric_limits<std::uint64_t>::max)(),
        "metronome beat diagnostics saturate at the final representable frame");

    stem.fill(11);
    processing::mix_metronome_click(
        nullptr, 48000.0, 0, beatIndex, output, stem);
    expect(std::all_of(stem.begin(), stem.end(), [](std::int32_t sample) {
               return sample == 11;
           }),
        "missing metronome control returns before touching caller buffers");
    processing::mix_metronome_click(
        &control,
        (std::numeric_limits<double>::quiet_NaN)(),
        0,
        beatIndex,
        output,
        stem);
}

void testHeadlessUsesSharedPipeline()
{
    jam2::Engine engine;
    jam2::EngineConfig config;
    config.backend = jam2::EngineAudioBackend::Headless;
    config.sample_rate = 48000;
    config.audio_buffer_frames = 64;
    config.diagnostics_enabled = true;
    config.test_input = jam2::EngineTestInput::Tone440;
    config.test_input_level_ppm = 250000;
    config.local_monitor_enabled = true;
    config.local_monitor_level_ppm = 500000;
    config.remote_level_ppm = 500000;
    engine.start(config);

    jam2::EngineSnapshot snapshot;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        snapshot = engine.snapshot();
        if (snapshot.engine_frame >= 2048 &&
            snapshot.input_peak_ppm > 0 &&
            snapshot.monitor_peak_ppm > 0 &&
            snapshot.output_peak_ppm > 0 &&
            snapshot.callback_timing.interval_samples > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const jam2::NetworkCaptureAttachment attachment = engine.attachNetworkCapture();
    const auto attachDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(30);
    while (!engine.networkCaptureReady(attachment) &&
           std::chrono::steady_clock::now() < attachDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::array<std::int32_t, 2048> remote;
    remote.fill((std::numeric_limits<std::int32_t>::max)());
    const std::size_t pushed = engine.pushNetworkPlayback(remote);
    const auto playbackDeadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < playbackDeadline) {
        snapshot = engine.snapshot();
        if (snapshot.remote_peak_ppm > 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    engine.detachNetworkCapture(attachment);
    engine.requestStop();
    engine.join();
    expect(snapshot.backend == jam2::EngineAudioBackend::Headless &&
            snapshot.callbacks > 0 && snapshot.engine_frame >= 2048,
        "headless engine advances the real shared callback pipeline");
    expect(snapshot.input_peak_ppm > 200000 &&
            snapshot.monitor_peak_ppm > 100000 &&
            snapshot.output_peak_ppm > 100000,
        "headless injection, monitoring, and output publish shared diagnostics");
    expect(attachment.generation != 0 && pushed > 0 &&
            snapshot.remote_peak_ppm >= 499000 &&
            snapshot.remote_peak_ppm <= 501000,
        "headless remote playback applies gain before shared meters and mixing");
    expect(snapshot.callback_timing.interval_samples > 0 &&
            snapshot.callback_timing.interval_min_us > 0 &&
            snapshot.callback_timing.interval_max_us >=
                snapshot.callback_timing.interval_min_us,
        "headless callbacks publish shared interval timing diagnostics");
}

} // namespace

int main()
{
    testCallbackTiming();
    testNetworkPlaybackTimelineCoherence();
    testPeakGainAndMixing();
    testPlaybackResampler();
    testPreparedSourceMixing();
    testCancelledPreparedSwapsReleaseEveryReadySlot();
    testInjectedInput();
    testMetronomeOutput();
    testHeadlessUsesSharedPipeline();
    if (failures != 0) {
        std::cerr << failures << " audio-device processing test(s) failed\n";
        return 1;
    }
    std::cout << "audio-device processing tests passed\n";
    return 0;
}

#include "audio_device_processing.hpp"
#include "engine.hpp"
#if defined(JAM2_PLATFORM_MACOS)
#include "coreaudio_buffer_processing.hpp"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace {

namespace processing = jam2::audio::device_processing;
#if defined(JAM2_PLATFORM_MACOS)
namespace coreaudio = jam2::audio::coreaudio_processing;
#endif

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

void testDeviceClock()
{
    jam2::audio::StreamControl control;
    std::uint64_t generation = 0;
    expect(processing::read_device_clock(control).time_us == 0, "unstarted device has no clock observation");
    processing::publish_callback_begin(&control, generation);
    processing::publish_device_clock(&control, {48000, 2000000});
    expect(processing::read_device_clock(control).time_us == 0, "clock reader skips an in-progress callback");
    processing::publish_callback_end(&control, generation);
    auto observed = processing::read_device_clock(control);
    expect(observed.frame == 48000 && observed.time_us == 2000000, "completed callback publishes a coherent clock pair");
    processing::publish_device_clock(nullptr, {});
    std::atomic<bool> done{false};
    std::thread publisher([&] {
        for (std::uint64_t frame = 1; frame <= 100000; ++frame) {
            processing::publish_callback_begin(&control, generation);
            processing::publish_device_clock(&control, {frame, frame * 1000 + 2000000});
            processing::publish_callback_end(&control, generation);
        }
        done.store(true, std::memory_order_release);
    });
    bool coherent = true;
    while (!done.load(std::memory_order_acquire)) {
        observed = processing::read_device_clock(control);
        if (observed.time_us != 0 && observed.time_us != 2000000)
            coherent = coherent && observed.time_us == observed.frame * 1000 + 2000000;
    }
    publisher.join();
    expect(coherent, "concurrent clock reader never combines frames and timestamps from different callbacks");

    for (int ppm : {-500, 0, 500}) {
        jam2::audio::DeviceClockEstimator estimator(48000);
        std::uint64_t lastTime = 0;
        for (std::uint64_t tick = 0; tick < 45000; ++tick) {
            const auto frame = tick * 32;
            const auto ideal = 1000000 + static_cast<std::uint64_t>(
                frame * 1000000.0L / (48000.0L * (1.0L + ppm / 1000000.0L)));
            // Periodic 4ms callback stalls followed by fast catch-up dispatch.
            const auto now = std::max<std::uint64_t>(lastTime + 1, ideal + (tick % 150 == 80 ? 4000ULL : 0ULL));
            estimator.observe({frame, now});
            estimator.observe({frame, now}); // network reads may repeat a callback
            lastTime = now;
        }
        expect(estimator.stats().valid && estimator.stats().observations > 50 &&
            std::abs(estimator.stats().ppm - ppm) < 3 && estimator.stats().resets == 0,
            "clock estimate rejects recurring dispatch delay and duplicate reads without biasing oscillator rate");
        estimator.observe({45000 * 32, lastTime + 20000000});
        expect(!estimator.stats().valid && estimator.stats().resets == 1 && estimator.rateRatio() == 1.0,
            "stopped device recalibrates instead of poisoning the clock estimate with its pause");
        estimator.observe({0, lastTime + 20001000});
        expect(estimator.stats().resets == 2, "restarted frame counter establishes a fresh device baseline");
        for (std::uint64_t tick = 1; tick <= 3000; ++tick)
            estimator.observe({tick * 48, lastTime + 20001000 + tick * 1000});
        expect(estimator.stats().valid && std::abs(estimator.stats().ppm) < 0.01,
            "restarted device clock can calibrate again");
    }
    jam2::audio::DeviceClockEstimator invalid(0);
    invalid.observe({48000, 1000000});
    expect(!invalid.stats().valid && invalid.rateRatio() == 1.0, "invalid nominal rate cannot affect playback ratio");
    jam2::audio::DeviceClockEstimator excessive(48000);
    for (std::uint64_t tick = 1; tick <= 4000; ++tick) excessive.observe({tick * 49, tick * 1000});
    expect(!excessive.stats().valid && excessive.stats().rejected_observations > 0 && excessive.rateRatio() == 1.0,
        "implausible device rates are counted and cannot drive resampling");
}

void testCaptureTimelineRing()
{
    jam2::audio::MonoRingBuffer ring(10, true);
    const std::array<std::int32_t, 8> first{100,101,102,103,104,105,106,107};
    const std::array<std::int32_t, 4> second{108,109,110,111};
    std::array<std::int32_t, 4> output{};
    std::uint64_t frame = 999;
    expect(ring.push_timed(first, 100) == 8 && ring.push_timed(second, 108) == 2,
        "capture ring can retain a partial callback before overflow");
    expect(ring.pop_exact_timed(output, frame) == 4 && frame == 100 && output.front() == 100,
        "capture ring preserves a complete queued packet");
    expect(ring.pop_exact_timed(output, frame) == 4 && frame == 104 && output.back() == 107,
        "capture ring preserves the next contiguous packet");
    const std::array<std::int32_t, 4> resumed{200,201,202,203};
    expect(ring.push_timed(resumed, 200) == 4 && ring.pop_exact_timed(output, frame) == 4 &&
        frame == 200 && output == resumed && ring.stats().timeline_discarded_frames == 2,
        "timed packet extraction retires an incomplete fragment instead of stitching across a gap and wrap");
    expect(ring.push_timed(std::span(resumed).first(2), 204) == 2, "partial contiguous data is queued");
    output.fill(-1);
    frame = 999;
    expect(ring.pop_exact_timed(output, frame) == 0 && frame == 999 && output.front() == -1 && ring.available_read() == 2,
        "incomplete contiguous packets stay queued and leave output and timestamp untouched");
    expect(ring.push_timed(std::span(resumed).last(2), 206) == 2 && ring.pop_exact_timed(output, frame) == 4 && frame == 204,
        "packet extraction joins contiguous callbacks across the ring wrap");
    ring.push_timed(resumed, 300);
    expect(ring.discard_all() == 4, "capture detach discards queued data");
    ring.push_timed(resumed, 1000);
    expect(ring.pop_exact_timed(output, frame) == 4 && frame == 1000,
        "reattached capture uses device time despite retained absolute ring indices");
    ring.reset();
    expect(ring.available_read() == 0 && ring.stats().timeline_discarded_frames == 0, "ring reset clears timeline diagnostics");
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
    expect(state.lastCallbackUs == 61000 &&
            state.minimumUs.load() == 10000 &&
            state.sumUs.load() == 60000 &&
            state.maximumUs.load() == 22000 &&
            state.samples.load() == 4,
        "single-writer callback timing records exact interval aggregates");
    expect(state.gapsOver1_1x.load() == 3 &&
            state.gapsOver1_5x.load() == 2 &&
            state.gapsOver2x.load() == 1,
        "callback timing classifies all strict gap thresholds");
    processing::observe_callback_work(state, 100, 107);
    processing::observe_callback_work(state, 200, 211);
    processing::observe_callback_work(state, 300, 305);
    processing::observe_callback_work(state, 400, 399);
    expect(state.workMinimumUs.load() == 5 &&
            state.workSumUs.load() == 23 &&
            state.workMaximumUs.load() == 11 &&
            state.workSamples.load() == 3,
        "callback timing records execution work and rejects backwards clocks");

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

void testOptionalDriverOutputReadyState()
{
    processing::DriverOutputReadyState supported;
    expect(!supported.shouldNotify() &&
            supported.status ==
                jam2::audio::DriverOutputReadyStatus::NotApplicable,
        "optional output-ready notifications begin disabled");
    supported.observe(processing::DriverOutputReadyObservation::Accepted);
    expect(supported.shouldNotify() &&
            supported.status == jam2::audio::DriverOutputReadyStatus::Active &&
            supported.error == 0 &&
            processing::driver_output_ready_latency_reduction(
                supported, 181, 149) == 32 &&
            processing::driver_output_ready_latency_reduction(
                supported, 149, 181) == 0,
        "an accepted output-ready probe enables callback notifications");
    supported.observe(processing::DriverOutputReadyObservation::Error, -1001);
    expect(!supported.shouldNotify() &&
            supported.status == jam2::audio::DriverOutputReadyStatus::Error &&
            supported.error == -1001,
        "an output-ready callback failure disables the optional driver path");

    processing::DriverOutputReadyState unsupported;
    unsupported.observe(processing::DriverOutputReadyObservation::Unsupported, -1000);
    expect(!unsupported.shouldNotify() &&
            unsupported.status ==
                jam2::audio::DriverOutputReadyStatus::Unsupported &&
            unsupported.error == 0 &&
            processing::driver_output_ready_latency_reduction(
                unsupported, 181, 149) == 0 &&
            std::string_view(jam2::audio::driver_output_ready_status_text(
                unsupported.status)) == "unsupported",
        "an unsupported output-ready probe remains a non-error no-op");
}

#if defined(JAM2_PLATFORM_MACOS)
template <std::size_t BufferCount>
struct AudioBufferListStorage {
    alignas(AudioBufferList) std::array<std::byte,
        offsetof(AudioBufferList, mBuffers) + BufferCount * sizeof(AudioBuffer)> bytes{};

    AudioBufferList* get()
    {
        auto* list = reinterpret_cast<AudioBufferList*>(bytes.data());
        list->mNumberBuffers = static_cast<UInt32>(BufferCount);
        return list;
    }
};

void testCoreAudioBufferProcessing()
{
    std::array<float, 6> interleaved{
        -1.0F, 0.25F,
        0.0F, 0.5F,
        1.0F, -0.25F,
    };
    AudioBufferListStorage<1> interleavedStorage;
    AudioBufferList* interleavedList = interleavedStorage.get();
    interleavedList->mBuffers[0] = AudioBuffer{
        2,
        static_cast<UInt32>(interleaved.size() * sizeof(float)),
        interleaved.data(),
    };
    const coreaudio::ConstFloatChannelView right =
        coreaudio::input_channel(interleavedList, 1);
    std::array<std::int32_t, 3> converted{};
    coreaudio::copy_input_channel(right, converted);
    expect(coreaudio::buffer_frames(interleavedList) == 3 &&
            right.available() && right.stride == 2 && right.frames == 3 &&
            converted[0] == 536870911 &&
            converted[1] == 1073741823 &&
            converted[2] == -536870911,
        "CoreAudio interleaved input resolves once and converts the selected strided channel");

    std::array<float, 3> leftOutput{9.0F, 9.0F, 9.0F};
    std::array<float, 3> rightOutput{8.0F, 8.0F, 8.0F};
    AudioBufferListStorage<2> planarStorage;
    AudioBufferList* planarList = planarStorage.get();
    planarList->mBuffers[0] = AudioBuffer{
        1,
        static_cast<UInt32>(leftOutput.size() * sizeof(float)),
        leftOutput.data(),
    };
    planarList->mBuffers[1] = AudioBuffer{
        1,
        static_cast<UInt32>(rightOutput.size() * sizeof(float)),
        rightOutput.data(),
    };
    const std::array<coreaudio::FloatChannelView, 2> outputs{
        coreaudio::output_channel(planarList, 0),
        coreaudio::output_channel(planarList, 1),
    };
    const std::array<std::int32_t, 3> mono{
        (std::numeric_limits<std::int32_t>::min)(),
        0,
        (std::numeric_limits<std::int32_t>::max)(),
    };
    coreaudio::write_mono_output(mono, outputs);
    expect(outputs[0].available() && outputs[1].available() &&
            outputs[0].stride == 1 && outputs[1].stride == 1 &&
            leftOutput[0] == -1.0F && rightOutput[0] == -1.0F &&
            leftOutput[1] == 0.0F && rightOutput[1] == 0.0F &&
            leftOutput[2] > 0.999999F && rightOutput[2] == leftOutput[2],
        "CoreAudio planar output converts mono once per frame and fans out to selected channels");

    std::array<float, 4> enabledStream{0.1F, 0.2F, 0.3F, 0.4F};
    AudioBufferListStorage<2> disabledStorage;
    AudioBufferList* disabledList = disabledStorage.get();
    disabledList->mBuffers[0] = AudioBuffer{
        2,
        static_cast<UInt32>(4 * 2 * sizeof(float)),
        nullptr,
    };
    disabledList->mBuffers[1] = AudioBuffer{
        1,
        static_cast<UInt32>(enabledStream.size() * sizeof(float)),
        enabledStream.data(),
    };
    const coreaudio::ConstFloatChannelView disabled =
        coreaudio::input_channel(disabledList, 0);
    const coreaudio::ConstFloatChannelView enabled =
        coreaudio::input_channel(disabledList, 2);
    std::array<std::int32_t, 4> disabledSamples{1, 2, 3, 4};
    coreaudio::copy_input_channel(disabled, disabledSamples);
    expect(coreaudio::buffer_frames(disabledList) == 4 &&
            !disabled.available() && enabled.available() &&
            enabled.samples[0] == 0.1F &&
            std::all_of(disabledSamples.begin(), disabledSamples.end(),
                [](std::int32_t sample) { return sample == 0; }),
        "CoreAudio disabled streams retain global channel numbering and render selected null input as silence");

    expect(coreaudio::float_to_i32(-2.0F) == -2147483647 &&
            coreaudio::float_to_i32(-1.0F) == -2147483647 &&
            coreaudio::float_to_i32(0.0F) == 0 &&
            coreaudio::float_to_i32(1.0F) == 2147483647 &&
            coreaudio::float_to_i32(2.0F) == 2147483647 &&
            coreaudio::i32_to_float((std::numeric_limits<std::int32_t>::min)()) == -1.0F,
        "CoreAudio Float32 and Q31 conversion preserves silence, clipping, and rail behavior");
}
#endif

void testNetworkPlaybackTimelineCoherence()
{
    jam2::audio::StreamControl control;
    std::uint64_t callbackGeneration = 0;
    processing::publish_callback_begin(&control, callbackGeneration);
    expect(callbackGeneration == 1 &&
            control.audio_callback_generation.load(std::memory_order_acquire) == 1,
        "single callback writer publishes an odd in-progress generation");
    processing::publish_callback_end(&control, callbackGeneration);
    expect(callbackGeneration == 2 &&
            control.audio_callback_generation.load(std::memory_order_acquire) == 2,
        "single callback writer publishes the following even completed generation");
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
    std::atomic<int> sharedCurrent{0};
    std::atomic<int> sharedInterval{0};
    processing::observe_shared_peak(sharedCurrent, sharedInterval, extremes);
    expect(sharedCurrent.load() == 1000000 &&
            sharedInterval.load() == 1000000,
        "shared peak observation publishes one scan to both consumers");
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
    std::array<std::int32_t, 2> unityMonitoredOutput{17, -29};
    const std::array<std::int32_t, 2> unityMonitorInput{
        123456789, -987654321};
    control.local_monitor_level_ppm.store(1000000);
    processing::mix_local_monitor(
        &control, unityMonitoredOutput, unityMonitorInput);
    expect(unityMonitoredOutput == std::array<std::int32_t, 2>{
                123456806, -987654350} &&
            control.monitor_peak_ppm.load() ==
                processing::i32_peak_ppm(unityMonitorInput),
        "unity monitoring preserves exact integer samples and peak metering");
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
    std::array<std::int32_t, 128> sourceScratch{};
    std::array<std::int32_t, 4> output{9, 9, 9, 9};
    processing::pop_resampled_playback(
        nullptr, &control, state, sourceScratch, output);
    expect(output == std::array<std::int32_t, 4>{0, 0, 0, 0},
        "missing playback source produces deterministic silence");
    output.fill(9);
    jam2::audio::MonoRingBuffer missingControlRing(8);
    processing::pop_resampled_playback(
        &missingControlRing, nullptr, state, sourceScratch, output);
    expect(output == std::array<std::int32_t, 4>{0, 0, 0, 0},
        "missing playback control also produces deterministic silence");

    jam2::audio::MonoRingBuffer unityRing(8);
    const std::array<std::int32_t, 3> unityInput{10, 20, 30};
    unityRing.push(unityInput);
    output.fill(-1);
    processing::pop_resampled_playback(
        &unityRing, &control, state, sourceScratch, output);
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
    processing::pop_resampled_playback(
        &halfRing, &control, state, sourceScratch, halfOutput);
    expect(halfOutput == std::array<std::int32_t, 3>{minimum, 0, maximum},
        "half-rate interpolation crosses both integer rails without overflow");

    jam2::audio::MonoRingBuffer doubleRing(8);
    const std::array<std::int32_t, 4> doubleInput{10, 20, 30, 40};
    doubleRing.push(doubleInput);
    control.playback_ratio_ppm.store(2000000);
    state.reset();
    std::array<std::int32_t, 2> doubleOutput{};
    processing::pop_resampled_playback(
        &doubleRing, &control, state, sourceScratch, doubleOutput);
    expect(doubleOutput == std::array<std::int32_t, 2>{10, 30},
        "double-rate playback consumes two source frames per output frame");

    jam2::audio::MonoRingBuffer rampRing(8);
    const std::array<std::int32_t, 4> rampInput{1, 2, 3, 4};
    rampRing.push(rampInput);
    control.playback_ratio_ppm.store(2000000);
    control.playback_ratio_ramp_frames.store(2);
    state.reset();
    std::array<std::int32_t, 2> rampOutput{};
    processing::pop_resampled_playback(
        &rampRing, &control, state, sourceScratch, rampOutput);
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
        &isolatedUnderrunRing, &control, state, sourceScratch, beforeRefill);
    expect(isolatedUnderrunRing.stats().underruns == 1,
        "playback resampler fixture produces one isolated source underrun");
    const std::array<std::int32_t, 4> refill{1000000, 1000000, 1000000, 1000000};
    isolatedUnderrunRing.push(refill);
    std::array<std::int32_t, 4> afterRefill{};
    processing::pop_resampled_playback(
        &isolatedUnderrunRing, &control, state, sourceScratch, afterRefill);
    expect(std::all_of(afterRefill.begin(), afterRefill.end(), [](std::int32_t sample) {
               return sample == 1000000;
           }),
        "isolated resampler underrun holds continuity instead of injecting a zero click");

    jam2::audio::MonoRingBuffer sustainedUnderrunRing(8);
    sustainedUnderrunRing.push(steadyInput);
    state.reset();
    std::array<std::int32_t, 40> sustainedUnderrunOutput{};
    processing::pop_resampled_playback(
        &sustainedUnderrunRing, &control, state, sourceScratch, sustainedUnderrunOutput);
    expect(sustainedUnderrunOutput.front() == 1000000 &&
            sustainedUnderrunOutput.back() == 0 &&
            sustainedUnderrunRing.stats().underrun_events == 1,
        "batched resampler underrun fades to silence with one callback-level ring event");

    state.reset();
    expect(!state.hasCurrent && !state.hasNext && state.phase == 0.0 &&
            state.current == 0 && state.next == 0 &&
            state.underrunConcealmentOrigin == 0 &&
            state.underrunConcealmentFrames == 0,
        "resampler reset discards all interpolation history");
}

void testPlaybackReleaseReturnRamp()
{
    for (const std::uint64_t rate : {44100U, 48000U}) {
        for (const std::size_t callback : {32U, 64U}) {
            for (const std::uint64_t normalMs : {0U, 10U, 250U}) {
                jam2::audio::MonoRingBuffer ring(512);
                jam2::audio::StreamControl control;
                processing::PlaybackResamplerState state;
                std::array<std::int32_t, 128> input, scratch{};
                std::array<std::int32_t, 64> output{};
                input.fill(123456789);
                const auto normalFrames = rate * normalMs / 1000;
                const auto returnFrames = std::min(normalFrames,
                    rate * jam2::audio::kPlaybackReleaseReturnRampMs / 1000);
                control.playback_ratio_ramp_frames.store(normalFrames);
                control.playback_release_return_ramp_frames.store(
                    rate * jam2::audio::kPlaybackReleaseReturnRampMs / 1000);
                bool continuous = true;
                auto pump = [&](std::uint64_t frames) {
                    while (frames > 0) {
                        const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(frames, callback));
                        if (ring.available_read() < count + 4) ring.push(input);
                        processing::pop_resampled_playback(&ring, &control, state, scratch,
                            std::span(output).first(count));
                        continuous &= std::all_of(output.begin(), output.begin() + count,
                            [](auto sample) { return sample == 123456789; });
                        frames -= count;
                    }
                };
                control.playback_ratio_ppm.store(1005000);
                if (normalFrames > 1) {
                    pump(normalFrames - 1);
                    expect(state.ratioSmoother.ramping(), "release acceleration keeps the full configured ramp");
                }
                pump(1);
                expect(state.ratioSmoother.appliedPpm() == 1005000 && !state.ratioSmoother.ramping(),
                    "release acceleration reaches the exact requested rate");
                control.playback_ratio_ppm.store(1000000);
                if (returnFrames > 1) {
                    pump(returnFrames - 1);
                    expect(state.ratioSmoother.ramping(), "release cancellation remains smooth until its last frame");
                }
                pump(1);
                expect(state.ratioSmoother.steadyUnity() && control.playback_ratio_applied_ppm.load() == 1000000,
                    "release cancellation completes within 50 ms and preserves faster custom ramps");
                expect(state.hasCurrent && state.hasNext && continuous && ring.underrun_frames() == 0,
                    "asymmetric release preserves interpolation history, audio continuity and source availability");
                if (normalMs == 250) {
                    control.playback_ratio_ppm.store(995000);
                    pump(normalFrames);
                    control.playback_ratio_ppm.store(1000000);
                    pump(returnFrames);
                    expect(state.ratioSmoother.ramping(),
                        "return from below-unity is not mistaken for cancelling accelerated playback");
                    pump(normalFrames - returnFrames);
                    expect(state.ratioSmoother.steadyUnity(), "below-unity return keeps its normal ramp");
                    control.playback_ratio_ppm.store(1005000);
                    pump(normalFrames / 2);
                    control.playback_ratio_ppm.store(1000000);
                    pump(returnFrames);
                    expect(state.ratioSmoother.steadyUnity(), "cancellation also stops a partially completed acceleration");
                }
            }
        }
    }
}

void testResampledPlaybackDepthAccounting()
{
    std::uint64_t referenceAudioHash = 0;
    for (const bool batched : {false, true}) {
        for (const bool diagnostics : {false, true}) {
            jam2::audio::MonoRingBuffer ring(2048);
            ring.set_diagnostics_enabled(diagnostics);
            ring.set_depth_bucket_thresholds(48000);
            jam2::audio::StreamControl control;
            processing::PlaybackResamplerState state;
            control.playback_ratio_ramp_frames.store(12000); // real 250 ms ramp
            std::array<std::int32_t, 520> scratch{};
            std::array<std::int32_t, 256> output{};
            std::array<std::int32_t, 1024> input{};
            input.fill(1000000);
            std::uint64_t requested = 0;
            std::uint64_t audioHash = 14695981039346656037ULL;
            bool accounting = true;
            // Unity, acceleration, deceleration and return to unity with live
            // interpolation history. Alternate plentiful audio and starvation.
            for (const int ratio : {1000000, 1005000, 995000, 1000000}) {
                control.playback_ratio_ppm.store(ratio);
                for (std::size_t callback = 0; callback < 100; ++callback) {
                    if (callback % 8 == 0) ring.push(input);
                    const auto before = ring.stats();
                    const auto depth = ring.available_read();
                    processing::pop_resampled_playback(&ring, &control, state,
                        batched ? std::span(scratch) : std::span<std::int32_t>{}, output);
                    for (const auto sample : output) {
                        audioHash = (audioHash ^ static_cast<std::uint32_t>(sample)) * 1099511628211ULL;
                    }
                    const auto after = ring.stats();
                    requested += depth - ring.available_read() + after.underruns - before.underruns;
                    accounting &= after.depth_observed_frames == (diagnostics ? requested : 0);
                    accounting &= after.depth_under_2ms_frames + after.depth_under_5ms_frames +
                        after.depth_under_10ms_frames + after.depth_10ms_plus_frames == after.depth_observed_frames;
                }
            }
            expect(accounting, "resampling observes every requested ring source frame exactly once, including return to unity");
            expect(!diagnostics || (ring.stats().underruns > 0 &&
                    ring.stats().underruns <= ring.stats().depth_observed_frames),
                "missing-source percentage has a matching denominator and cannot exceed 100 percent");
            expect(control.playback_ratio_applied_ppm.load() == 1000000,
                "depth accounting covers the completed return-to-unity ramp");
            if (referenceAudioHash == 0) referenceAudioHash = audioHash;
            expect(audioHash == referenceAudioHash,
                "diagnostics and scratch fallback preserve bit-identical rendered audio");
        }
    }
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
    expect(!source.needsProcessing(),
        "idle prepared source advertises no callback work");
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
    expect(source.needsProcessing(),
        "queued prepared commands keep future callback processing active");
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
    expect(!source.needsProcessing(),
        "completed non-looping prepared source returns to the idle fast path");
    const auto completedOutput = output;
    stem.fill(7);
    processing::mix_prepared_source(&control, output, 14, stem);
    expect(output == completedOutput && !anyNonzero(stem) &&
            control.prepared_track_peak_ppm.load() == 0,
        "idle prepared source clears a requested recording stem without mixing");
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

void testListeningTempoAndSpecificCancellation()
{
    jam2::audio::StreamControl control;
    control.metronome_enabled.store(true);
    control.metronome_epoch_valid.store(true);
    control.pending_musical_rate_ppm.store(500000);
    control.pending_musical_rate_anchor_frame.store(48000);
    control.pending_musical_rate_anchor_position.store(48000.0);
    control.musical_rate_commit_raw_frame.store(48000);
    std::vector<std::int32_t> output(192000), stem(192000);
    std::uint64_t beatIndex = 0;
    processing::mix_metronome_click(&control, 48000.0, 0, beatIndex, output, stem);
    const auto energyAt = [&stem](std::size_t frame) {
        return std::any_of(stem.cbegin() + frame, stem.cbegin() + frame + 1024,
            [](std::int32_t sample) { return sample != 0; });
    };
    expect(energyAt(0) && energyAt(24000) && energyAt(48000) && energyAt(96000) && energyAt(144000) &&
        !energyAt(72000) && !energyAt(120000),
        "scheduled listening tempo changes audible click spacing at the exact boundary without resetting phase");
    expect(jam2::audio::playback_tempo_at(control, 47999).rate_ppm == 1000000 &&
        jam2::audio::playback_tempo_at(control, 48000).rate_ppm == 500000,
        "tempo snapshot selects the same boundary as the real callback");
    control.musical_rate_revision.store(1);
    control.musical_rate_commit_raw_frame.store(0);
    std::array<std::int32_t, 1024> interruptedOutput{}, interruptedStem{};
    processing::mix_metronome_click(&control, 48000.0, 168000, beatIndex, interruptedOutput, interruptedStem);
    expect(std::all_of(interruptedStem.begin(), interruptedStem.end(), [](std::int32_t value) { return value == 0; }),
        "callback retains its coherent half-speed clock during a concurrent tempo publication without waiting");
    control.musical_rate_revision.store(2);

    jam2::audio::PreparedTrackSource source(8);
    const int cancelled = source.claimLoadingSlot();
    source.loadingData(cancelled)[0] = 3000;
    expect(source.publishReady(cancelled, 1, 48000), "listening cancellation fixture stages a source");
    jam2::audio::PreparedTrackSource::Command replacement;
    replacement.type = jam2::audio::PreparedTrackSource::CommandType::Swap;
    replacement.slot = static_cast<std::uint32_t>(cancelled);
    replacement.targetFrame = 10;
    replacement.listeningGeneration = 1;
    expect(source.enqueue(replacement), "listening replacement is tagged for its own cancellation");
    const int retained = source.claimLoadingSlot();
    source.loadingData(retained)[0] = 1000;
    expect(source.publishReady(retained, 1, 48000), "ordinary source fixture stages its own slot");
    const std::array normal{
        jam2::audio::PreparedTrackSource::Command{jam2::audio::PreparedTrackSource::CommandType::Swap, static_cast<std::uint32_t>(retained)},
        jam2::audio::PreparedTrackSource::Command{jam2::audio::PreparedTrackSource::CommandType::Play}};
    expect(source.enqueueBatch(normal), "ordinary commands can follow a listening transaction");
    source.cancelListeningScheduled();
    std::array<std::int32_t, 1> mixed{};
    source.mix(mixed.data(), mixed.size(), 0);
    expect(mixed[0] == 65536000 && source.actualStartFrame() == 0,
        "cancelling listening changes preserves unrelated source/play commands");
    const int reclaimed = source.claimLoadingSlot();
    expect(reclaimed == cancelled, "cancelled listening swaps release the owned Ready slot");
    source.abandonLoadingSlot(reclaimed);
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
    expect(anyNonzero(output) && output == stem && beatIndex == 1 &&
            control.metronome_peak_ppm.load() > 0 &&
            control.gui_metronome_peak_ppm.load() ==
                control.metronome_peak_ppm.load(),
        "enabled metronome renders and meters the shared-grid click and recorder stem");

    const auto uncachedOutput = output;
    const auto uncachedStem = stem;
    processing::MetronomeWaveBank waveBank(48000.0);
    output.fill(0);
    stem.fill(0);
    std::uint64_t cachedBeatIndex = 0;
    processing::mix_metronome_click(
        &control,
        48000.0,
        0,
        cachedBeatIndex,
        output,
        stem,
        &waveBank);
    expect(waveBank.preparedFor(48000.0) &&
            output == uncachedOutput && stem == uncachedStem &&
            cachedBeatIndex == beatIndex,
        "prepared block metronome rendering is sample-exact with direct synthesis");

    output.fill(0);
    stem.fill(0);
    control.metronome_pattern_origin_valid.store(false);
    control.metronome_pattern_scheduled_origin_raw_frame.store(32);
    processing::mix_metronome_click(
        &control, 48000.0, 0, beatIndex, output, stem);
    const bool clickBeforeReset = std::any_of(
        output.begin(), output.begin() + 32,
        [](std::int32_t sample) { return sample != 0; });
    const bool clickAfterReset = std::any_of(
        output.begin() + 32, output.end(),
        [](std::int32_t sample) { return sample != 0; });
    expect(clickBeforeReset && clickAfterReset && output == stem &&
            control.metronome_pattern_origin_valid.load() &&
            control.metronome_pattern_origin_frame.load() == 32 &&
            control.metronome_pattern_scheduled_origin_raw_frame.load() == 0,
        "scheduled metronome origin resets within a rendered callback block");
    jam2::audio::reset_metronome_pattern_origin(control);

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

void testEngineLocalCancellationPreservesClockReset()
{
    try {
        struct Fixture {
            std::filesystem::path path = std::filesystem::temp_directory_path() /
                ("jam2-listening-cancel-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".wav");
            ~Fixture() { std::error_code error; std::filesystem::remove(path, error); }
        } fixture;
        // Two mono PCM16 samples at 48 kHz, played in a tight deterministic loop.
        const std::array<unsigned char, 48> wav{{
            'R','I','F','F',40,0,0,0,'W','A','V','E','f','m','t',' ',
            16,0,0,0,1,0,1,0,128,187,0,0,0,119,1,0,2,0,16,0,
            'd','a','t','a',4,0,0,0,232,3,24,252}};
        std::ofstream file(fixture.path, std::ios::binary);
        file.write(reinterpret_cast<const char*>(wav.data()), wav.size());
        file.close();
        if (!file) { expect(false, "listening cancellation WAV fixture writes successfully"); return; }
        jam2::Engine engine;
        jam2::EngineConfig config;
        config.backend = jam2::EngineAudioBackend::Headless;
        config.sample_rate = 48000; config.audio_buffer_frames = 64;
        config.prepared_track_max_frames = 64;
        engine.start(config);
        const auto wait = [&](auto predicate) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (std::chrono::steady_clock::now() < deadline) {
                if (predicate()) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return false;
        };
        jam2::EngineCommand command;
        command.type = jam2::EngineCommandType::LoadPreparedTrack;
        if (!jam2::engine_command_set_text(command, fixture.path.string()) || !engine.submit(command)) {
            expect(false, "initial prepared source load submits"); return;
        }
        command = {}; command.type = jam2::EngineCommandType::PreparedSetLoop; command.signed_value = 2;
        expect(engine.submit(command), "initial source loop submits");
        command = {}; command.type = jam2::EngineCommandType::PreparedPlay;
        expect(engine.submit(command), "initial source play submits");
        expect(wait([&] { return engine.snapshot().prepared_source_playing; }), "initial source starts");
        const auto originalStart = engine.snapshot().prepared_source_actual_start_frame;
        command = {}; command.type = jam2::EngineCommandType::CommitListeningTools;
        command.cookie = 90; command.frame = engine.snapshot().engine_frame + 4800;
        command.playback_tempo = {500000, command.frame, static_cast<double>(command.frame)};
        expect(engine.submit(command), "initial slowed clock submits");
        expect(wait([&] { return engine.snapshot().playback_tempo.rate_ppm == 500000; }), "initial slowed clock commits");
        command = {}; command.type = jam2::EngineCommandType::StageListeningTrack; command.cookie = 100;
        expect(jam2::engine_command_set_text(command, fixture.path.string()) && engine.submit(command), "local source stages");
        expect(wait([&] {
            jam2::EngineEvent event;
            while (engine.pollEvent(event)) if (event.type == jam2::EngineEventType::ListeningTrackStaged && event.cookie == 100) return true;
            return false;
        }), "local staged source publishes readiness");
        const auto slowed = engine.snapshot();
        const auto resetFrame = slowed.engine_frame + 24000;
        const jam2::PlaybackTempo unity{1000000, resetFrame, slowed.playback_tempo.position(resetFrame)};
        command = {}; command.type = jam2::EngineCommandType::CommitListeningTools;
        command.frame = resetFrame; command.playback_tempo = unity;
        expect(engine.submit(command), "authority unity reset submits");
        command = {}; command.type = jam2::EngineCommandType::CommitListeningTools;
        command.cookie = 100; command.signed_value = 100; command.frame = resetFrame + 24000;
        command.enabled = true; command.update_playback_tempo = false;
        command.prepared_loop_enabled = true; command.prepared_loop_end_frame = 2;
        expect(engine.submit(command), "local-only future source swap submits");
        const auto audioTarget = command.frame;
        command = {}; command.type = jam2::EngineCommandType::CancelListeningTools; command.cookie = 100;
        expect(engine.submit(command), "local source cancellation submits");
        expect(wait([&] { return engine.snapshot().engine_frame > audioTarget + 64; }), "engine crosses both future boundaries");
        const auto final = engine.snapshot();
        engine.requestStop(); engine.join();
        expect(final.playback_tempo.rate_ppm == 1000000 && final.playback_tempo.anchor_frame == unity.anchor_frame,
            "local cancellation preserves the authority's pending clock reset");
        expect(final.prepared_source_playing && final.prepared_source_actual_start_frame == originalStart,
            "local-only cancellation removes the scheduled swap/play while retaining current playback");
    } catch (const std::exception& error) {
        ++failures; std::cerr << "FAIL: engine listening cancellation: " << error.what() << '\n';
    }
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
    testDeviceClock();
    testCaptureTimelineRing();
    testCallbackTiming();
    testOptionalDriverOutputReadyState();
#if defined(JAM2_PLATFORM_MACOS)
    testCoreAudioBufferProcessing();
#endif
    testNetworkPlaybackTimelineCoherence();
    testPeakGainAndMixing();
    testPlaybackResampler();
    testPlaybackReleaseReturnRamp();
    testResampledPlaybackDepthAccounting();
    testPreparedSourceMixing();
    testCancelledPreparedSwapsReleaseEveryReadySlot();
    testListeningTempoAndSpecificCancellation();
    testEngineLocalCancellationPreservesClockReset();
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

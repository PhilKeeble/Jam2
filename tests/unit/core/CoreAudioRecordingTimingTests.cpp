#include "coreaudio_recording_timing.hpp"
#include "common.hpp"
#include "pcm16_wav.hpp"

#include <CoreAudio/HostTime.h>

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace timing = jam2::audio::coreaudio_processing;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

AudioTimeStamp sampleTime(double frame)
{
    AudioTimeStamp timestamp{};
    timestamp.mFlags = kAudioTimeStampSampleTimeValid;
    timestamp.mSampleTime = frame;
    return timestamp;
}

void testQueriedLatencyAndTimestamps()
{
    const timing::RecordingLatencyProperties input{14, 2399, 43};
    const timing::RecordingLatencyProperties output{1, 556, 107};
    auto acquired = sampleTime(9894);
    auto cycle = sampleTime(10000);
    auto presented = sampleTime(10172);
    require(timing::recording_latency_frames(input, 64, &acquired, &cycle, 48000) == 2519 &&
            timing::recording_latency_frames(output, 64, &cycle, &presented, 48000) == 729,
        "logged aggregate-device timestamps include buffering once and retain both stream latencies");
    presented = sampleTime(10300);
    require(timing::recording_latency_frames(output, 192, &cycle, &presented, 48000) == 857,
        "a changed callback timestamp changes correction without a hardcoded buffer count");
    const timing::RecordingLatencyProperties otherDevice{7, 41, 12};
    require(timing::recording_latency_frames(otherDevice, 128, &cycle, &presented, 96000) == 348,
        "another device uses its own reported hardware latency");
    require(timing::recording_latency_frames({7, 41, 12, false}, 128,
                &cycle, &presented, 96000) == 0,
        "disabled output contributes no latency even with a populated timestamp");

    require(timing::recording_latency_frames(input, 64, nullptr, nullptr, 48000) == 2520 &&
            timing::recording_latency_frames(output, 64, nullptr, nullptr, 48000) == 728 &&
            timing::recording_latency_frames(output, 128, nullptr, nullptr, 48000) == 792,
        "missing timestamps fall back to queried safety and the actual callback size");
    acquired = sampleTime(10001);
    require(timing::recording_latency_frames(input, 64, &acquired, &cycle, 48000) == 2520,
        "backwards input timestamps fall back instead of producing a negative correction");
    acquired = sampleTime(std::numeric_limits<double>::quiet_NaN());
    require(timing::recording_latency_frames(input, 64, &acquired, &cycle, 48000) == 2520,
        "nonfinite sample timestamps fall back safely");
    acquired = sampleTime(-1e20);
    require(timing::recording_latency_frames(input, 64, &acquired, &cycle, 48000) == 2520,
        "unrepresentable timestamp intervals fall back without integer overflow");

    acquired = {};
    cycle = {};
    acquired.mFlags = cycle.mFlags = kAudioTimeStampHostTimeValid;
    acquired.mHostTime = AudioConvertNanosToHostTime(1000000000ULL);
    cycle.mHostTime = AudioConvertNanosToHostTime(1002000000ULL);
    require(timing::recording_latency_frames(input, 64, &acquired, &cycle, 48000) == 2509 &&
            timing::recording_latency_frames(input, 64, &acquired, &cycle, 96000) == 2605,
        "host-only timestamps are converted using the active device sample rate");
    require(timing::recording_latency_frames(input, 64, &acquired, &cycle, 0) == 2520 &&
            timing::recording_latency_frames(input, 64, &cycle, &acquired, 48000) == 2520,
        "invalid host-time conversion and reversed host clocks use the fallback");
    constexpr auto maximum = (std::numeric_limits<std::uint32_t>::max)();
    require(timing::recording_latency_frames({maximum, maximum, maximum},
                maximum, nullptr, nullptr, 48000) == maximum,
        "extreme driver properties cannot wrap the published latency");

    jam2::audio::StreamControl control;
    control.recording_latency_adjustment_frames.store(32);
    timing::publish_recording_latency(control, 2519, 729);
    require(control.input_latency_frames.load() == 2519 &&
            control.output_latency_frames.load() == 729 &&
            control.recording_latency_compensation_frames.load() == 3280,
        "live input/output totals and manual correction are published together");
    control.recording_latency_adjustment_frames.store(-10);
    timing::publish_recording_latency(control, 2519, 857);
    require(control.recording_latency_compensation_frames.load() == 3366,
        "timestamp updates retain the current negative manual correction");
    control.recording_latency_adjustment_frames.store(-4000);
    timing::publish_recording_latency(control, 2519, 729);
    require(control.recording_latency_compensation_frames.load() == 0,
        "manual correction cannot underflow the capture timestamp");
}

struct TemporaryWav {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("jam2-coreaudio-recording-" + std::to_string(jam2::monotonic_us()) + ".wav");
    ~TemporaryWav()
    {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
};

class DelayedInputSource final : public jam2::audio::InputSourceRenderer {
public:
    bool render_mono(const jam2::audio::InputSourceRenderRequest& request,
        std::span<std::int32_t> output) noexcept override
    {
        std::copy_n(request.inputs[0], output.size(), output.begin());
        return true;
    }
    std::size_t latency_frames(std::size_t) const noexcept override { return 23; }
};

void testRecordedPulseAlignment()
{
    // Four independent device configurations, including different callback
    // sizes/rates and manual offsets. Feed the same delayed two-hit take through
    // the exact CoreAudio input-recording handoff used by the application.
    for (int peer = 0; peer < 4; ++peer) {
        const int sampleRate = peer == 3 ? 44100 : 48000;
        const std::size_t bufferFrames = peer % 2 == 0 ? 64 : 128;
        const std::uint32_t inputDelay = peer == 0 ? 2519 : 100 + peer * 31;
        const std::uint32_t outputDelay = peer == 0 ? 729 : 80 + peer * 17;
        const std::int64_t manual = peer == 2 ? -11 : peer * 7;
        jam2::audio::StreamControl control;
        DelayedInputSource renderer;
        jam2::audio::InputSourceRouter router(bufferFrames, 1);
        std::vector<std::int32_t> block(bufferFrames);
        const std::int64_t calibration = peer == 0 ? 19 : peer == 2 ? -31 : 0;
        {
            require(router.configure(0, {jam2::audio::InputSourceKind::Audio,
                    0, jam2::audio::kNoInputChannel, 1000000, true, peer == 1 ? &renderer : nullptr}),
                "processed recording source can be configured");
            const std::array<const std::int32_t*, 1> inputs{block.data()};
            require(router.process(inputs, block.size(), 0, sampleRate, block),
                "processed source publishes its current recording latency");
            control.input_source_router = &router;
        }
        control.recording_latency_adjustment_frames.store(manual);
        jam2::audio::set_recording_calibration(control, 0, calibration);
        timing::publish_recording_latency(control, inputDelay, outputDelay);
        const auto delay = static_cast<std::uint64_t>(inputDelay + outputDelay + manual + calibration) +
            (peer == 1 ? router.recording_latency_frames() : 0);
        TemporaryWav wav;
        jam2::audio::TrackTakeRecorder recorder;
        std::string error;
        constexpr std::uint64_t start = 100000;
        constexpr std::uint64_t length = 512;
        require(recorder.arm("delayed-hits", wav.path, sampleRate, error) &&
                recorder.start_at(start, error) && recorder.stop_at(start + length, error),
            "compensated take can arm and schedule exact capture boundaries");
        for (std::uint64_t frame = start + delay - 32;
             frame < start + delay + length; frame += bufferFrames) {
            std::fill(block.begin(), block.end(), 0);
            for (std::size_t sample = 0; sample < block.size(); ++sample) {
                if (frame + sample == start + delay + 17) block[sample] = 16384 * 65536;
                if (frame + sample == start + delay + 417) block[sample] = -8192 * 65536;
            }
            timing::record_input_take(recorder, &control, frame, block);
        }
        const auto completion = recorder.consume_completion();
        // Completion is published by the writer, so wait on its observable
        // state rather than assuming its thread has drained the queue already.
        auto finished = completion;
        const auto deadline = jam2::monotonic_us() + 5000000;
        while (!finished.available && jam2::monotonic_us() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            finished = recorder.consume_completion();
        }
        require(finished.available && finished.ok && finished.frames_written == length &&
                finished.dropped_frames == 0 && !recorder.armed(),
            "compensated recording writes the full requested take and releases ownership");
        const auto inspected = jam2::wav::inspect_pcm16_file(wav.path);
        require(static_cast<bool>(inspected) && inspected.info.frames == length,
            "saved compensated WAV preserves exact start/stop duration");
        std::ifstream file(wav.path, std::ios::binary);
        file.seekg(static_cast<std::streamoff>(inspected.info.data_offset));
        for (std::uint64_t frame = 0; frame < length; ++frame) {
            std::array<unsigned char, 2> bytes{};
            file.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
            const auto sample = static_cast<std::int16_t>(
                static_cast<std::uint16_t>(bytes[0]) | (static_cast<std::uint16_t>(bytes[1]) << 8));
            require(file.good() && sample == (frame == 17 ? 16384 : frame == 417 ? -8192 : 0),
                "both recorded hits land at the intended metronome-relative sample positions");
        }

        require(recorder.arm("cancelled-hits", wav.path, sampleRate, error) &&
                recorder.start_at(0, error), "finished recorder can be rearmed");
        timing::record_input_take(recorder, &control, 1, block);
        recorder.cancel();
        timing::record_input_take(recorder, &control, 100000, block);
        const auto cancelled = recorder.consume_completion();
        require(cancelled.available && !cancelled.ok && !recorder.armed(),
            "startup timestamp saturation and cancellation leave no active take");
    }
}

} // namespace

int main()
{
    try {
        testQueriedLatencyAndTimestamps();
        testRecordedPulseAlignment();
        std::cout << "CoreAudio recording timing tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

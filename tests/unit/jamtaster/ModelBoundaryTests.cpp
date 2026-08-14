#include "Adtof.hpp"
#include "BasicPitch.hpp"
#include "BeatThis.hpp"
#include "ChordMini.hpp"
#include "DemucsAdapter.hpp"
#include "OnnxModel.hpp"
#include "Wav.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

template <typename Operation>
void requireThrows(Operation operation, const std::string& message)
{
    try {
        operation();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

jamtaster::native::AudioBuffer syntheticMusic(
    int sampleRate, double seconds, bool stereo = false)
{
    jamtaster::native::AudioBuffer audio;
    audio.sampleRate = sampleRate;
    audio.channels = stereo ? 2 : 1;
    const std::size_t frames = static_cast<std::size_t>(
        std::llround(sampleRate * seconds));
    audio.samples.assign(frames * static_cast<std::size_t>(audio.channels), 0.0F);
    constexpr double pi = 3.1415926535897932384626433832795;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const double time = static_cast<double>(frame) / sampleRate;
        float value = static_cast<float>(
            0.22 * std::sin(2.0 * pi * 110.0 * time) +
            0.16 * std::sin(2.0 * pi * 164.813778 * time) +
            0.12 * std::sin(2.0 * pi * 220.0 * time));
        const double beatPhase = std::fmod(time, 0.5);
        if (beatPhase < 0.012) {
            const double envelope = 1.0 - beatPhase / 0.012;
            value += static_cast<float>(0.65 * envelope *
                std::sin(2.0 * pi * 1400.0 * time));
        }
        value = std::clamp(value, -1.0F, 1.0F);
        for (int channel = 0; channel < audio.channels; ++channel) {
            audio.samples[frame * static_cast<std::size_t>(audio.channels) +
                static_cast<std::size_t>(channel)] =
                channel == 0 ? value : value * 0.8F;
        }
    }
    return audio;
}

void testRawOnnxBoundary(const std::filesystem::path& models)
{
    using namespace jamtaster::native;
    require(!OnnxModel::runtimeVersion().empty(),
        "ONNX Runtime publishes a version string");
    requireThrows([&] {
        OnnxModel missing(models / "missing.onnx", 1);
        (void)missing;
    }, "ONNX wrapper rejects a missing graph");

    OnnxModel model(models / "basic_pitch.onnx", 1);
    require(model.inputs().size() == 1 && !model.inputs().front().name.empty() &&
            !model.outputs().empty() &&
            std::all_of(model.outputs().begin(), model.outputs().end(),
                [](const TensorDescription& output) {
                    return !output.name.empty() && !output.shape.empty();
                }),
        "ONNX wrapper describes the staged graph's input and outputs");
    requireThrows([&] {
        (void)model.run({0.0F, 1.0F}, {1});
    }, "ONNX wrapper rejects a value count that disagrees with its shape");
    requireThrows([&] {
        (void)model.run({}, {-1});
    }, "ONNX wrapper rejects a non-concrete runtime shape");
    requireThrows([&] {
        (void)model.run({}, {
            (std::numeric_limits<std::int64_t>::max)(),
            (std::numeric_limits<std::int64_t>::max)()});
    }, "ONNX wrapper detects tensor element-count overflow");
}

void testBeatThis(const std::filesystem::path& models)
{
    using namespace jamtaster::native;
    BeatThis model(models / "beat_this.onnx", 1);
    const BeatAnalysis result = model.analyze(syntheticMusic(22050, 3.0));
    require(result.beatLogits.size() == 151 &&
            result.downbeatLogits.size() == result.beatLogits.size() &&
            result.preprocessingSeconds >= 0.0 && result.inferenceSeconds >= 0.0 &&
            result.postprocessingSeconds >= 0.0 &&
            result.beats.size() >= 4 &&
            std::all_of(result.beatLogits.begin(), result.beatLogits.end(),
                [](float value) { return std::isfinite(value); }) &&
            std::all_of(result.downbeatLogits.begin(), result.downbeatLogits.end(),
                [](float value) { return std::isfinite(value); }) &&
            std::is_sorted(result.beats.begin(), result.beats.end()) &&
            std::is_sorted(result.downbeats.begin(), result.downbeats.end()) &&
            std::all_of(result.beats.begin(), result.beats.end(), [](float beat) {
                return std::isfinite(beat) && beat >= 0.0F && beat <= 3.1F;
            }),
        "Beat This returns fixed-rate logits and bounded sorted beat events");
}

void testBasicPitch(const std::filesystem::path& models)
{
    using namespace jamtaster::native;
    BasicPitch model(models / "basic_pitch.onnx", 1);
    requireThrows([&] {
        (void)model.analyze(syntheticMusic(22050, 0.1), 20, 60);
    }, "Basic Pitch rejects a MIDI range outside 21..108");
    const PitchAnalysis result = model.analyze(syntheticMusic(22050, 2.0), 40, 80);
    require(result.activationFrames == 172 && !result.notes.empty() &&
            result.preprocessingSeconds >= 0.0 && result.inferenceSeconds >= 0.0 &&
            result.postprocessingSeconds >= 0.0 &&
            std::is_sorted(result.notes.begin(), result.notes.end(),
                [](const PitchNote& left, const PitchNote& right) {
                    return left.start != right.start
                        ? left.start < right.start : left.midi < right.midi;
                }) &&
            std::all_of(result.notes.begin(), result.notes.end(), [](const PitchNote& note) {
                return note.start >= 0.0 && note.end > note.start && note.end <= 2.0 &&
                    note.midi >= 40 && note.midi <= 80 &&
                    note.velocity >= 1 && note.velocity <= 127 &&
                    std::isfinite(note.confidence);
            }),
        "Basic Pitch emits bounded, ordered notes within the requested MIDI range");
}

void testAdtof(const std::filesystem::path& models)
{
    using namespace jamtaster::native;
    Adtof model(models / "adtof.onnx", 1);
    requireThrows([&] {
        (void)model.analyze(syntheticMusic(44100, 0.1),
            {0.1F, 0.2F, std::numeric_limits<float>::quiet_NaN(), 0.2F, 0.3F});
    }, "ADTOF rejects a non-finite activation threshold");
    const DrumAnalysis result = model.analyze(
        syntheticMusic(44100, 2.0), {0.0F, 0.0F, 0.0F, 0.0F, 0.0F});
    static const std::array<std::string, 5> lanes{
        "Kick", "Snare", "Mid Tom", "Closed HH", "Crash"};
    require(result.activationFrames == 201 &&
            result.preprocessingSeconds >= 0.0 && result.inferenceSeconds >= 0.0 &&
            result.postprocessingSeconds >= 0.0 && !result.hits.empty() &&
            std::is_sorted(result.hits.begin(), result.hits.end(),
                [](const DrumEvent& left, const DrumEvent& right) {
                    return left.time != right.time
                        ? left.time < right.time : left.midi < right.midi;
                }) &&
            std::all_of(result.hits.begin(), result.hits.end(), [&](const DrumEvent& hit) {
                return hit.time >= 0.0 && hit.time <= 2.1 &&
                    std::find(lanes.begin(), lanes.end(), hit.lane) != lanes.end() &&
                    std::isfinite(hit.confidence);
            }),
        "ADTOF emits sorted, classified, bounded drum activations");
}

void testChordMini(const std::filesystem::path& models)
{
    using namespace jamtaster::native;
    ChordMini model(models / "chordmini_btc.onnx", 1);
    const ChordAnalysis result = model.analyze(syntheticMusic(22050, 3.0));
    require(result.featureFrames == 33 && !result.chords.empty() &&
            result.preprocessingSeconds >= 0.0 && result.inferenceSeconds >= 0.0 &&
            result.postprocessingSeconds >= 0.0 &&
            std::all_of(result.chords.begin(), result.chords.end(),
                [](const ChordSegment& chord) {
                    return chord.start >= 0.0 && chord.end > chord.start &&
                        chord.end <= 3.1 && !chord.label.empty() &&
                        std::isfinite(chord.confidenceMargin);
                }),
        "ChordMini emits bounded labelled segments from the staged graph");
}

void testDemucsLoadBoundary(const std::filesystem::path& models)
{
    using namespace jamtaster::native;
    const auto output = std::filesystem::temp_directory_path() /
        "jam2-demucs-model-load-boundary";
    std::error_code ignored;
    std::filesystem::remove_all(output, ignored);
    requireThrows([&] {
        (void)runDemucsEnsemble(
            syntheticMusic(22050, 0.1), {}, output, 1, 17, {});
    }, "Demucs requires at least one model");
    requireThrows([&] {
        (void)runDemucsEnsemble(
            syntheticMusic(22050, 0.1), {models / "missing.onnx"},
            output, 1, 17, {});
    }, "Demucs converts input then rejects a missing staged graph");
    require(!std::filesystem::exists(output),
        "Demucs model-load failure does not publish partial stem output");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc != 2) {
            throw std::runtime_error("usage: jam2_jamtaster_model_tests <model-directory>");
        }
        const std::filesystem::path models = argv[1];
        testRawOnnxBoundary(models);
        testBeatThis(models);
        testBasicPitch(models);
        testAdtof(models);
        testChordMini(models);
        testDemucsLoadBoundary(models);
        std::cout << "JamTaster model boundary tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "JamTaster model boundary test failed: " << error.what() << '\n';
        return 1;
    }
}

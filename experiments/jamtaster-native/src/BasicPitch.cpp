#include "BasicPitch.hpp"

#include "Dsp.hpp"
#include "OnnxModel.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace jamtaster::native {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kWindowSamples = 43844;
constexpr std::size_t kOutputFrames = 172;
constexpr std::size_t kOverlapFrames = 30;
constexpr std::size_t kOverlapSamples = kOverlapFrames * 256;
constexpr std::size_t kHopSamples = kWindowSamples - kOverlapSamples;
constexpr std::size_t kCropFrames = kOverlapFrames / 2;
constexpr int kSampleRate = 22050;
constexpr int kFramesPerSecond = 86;
constexpr int kMidiOffset = 21;

double elapsed(Clock::time_point start)
{
    return std::chrono::duration<double>(Clock::now() - start).count();
}

const TensorResult& named(const std::vector<TensorResult>& outputs, std::string_view name)
{
    const auto found = std::find_if(outputs.begin(), outputs.end(),
        [name](const TensorResult& output) { return output.name == name; });
    if (found == outputs.end()) throw std::runtime_error("Basic Pitch output is missing");
    return *found;
}

float& cell(std::vector<float>& matrix, std::size_t frame, std::size_t pitch)
{
    return matrix[frame * 88 + pitch];
}

float cell(const std::vector<float>& matrix, std::size_t frame, std::size_t pitch)
{
    return matrix[frame * 88 + pitch];
}

double frameTime(std::size_t frame)
{
    constexpr double windowOffset = (256.0 / kSampleRate) *
        (kOutputFrames - (static_cast<double>(kWindowSamples) / 256.0)) + 0.0018;
    return static_cast<double>(frame * 256) / kSampleRate -
        windowOffset * std::floor(static_cast<double>(frame) / kOutputFrames);
}

} // namespace

class BasicPitch::Impl {
public:
    Impl(const std::filesystem::path& path, int threads) : model(path, threads) {}
    OnnxModel model;
};

BasicPitch::BasicPitch(const std::filesystem::path& modelPath, int threads)
    : impl_(std::make_unique<Impl>(modelPath, threads))
{
}

BasicPitch::~BasicPitch() = default;

PitchAnalysis BasicPitch::analyze(
    const AudioBuffer& audio, int minimumMidi, int maximumMidi)
{
    if (minimumMidi < 21 || maximumMidi > 108 || minimumMidi > maximumMidi) {
        throw std::invalid_argument("Basic Pitch MIDI range must be within 21..108");
    }
    PitchAnalysis result;
    auto stage = Clock::now();
    auto mono = audio.mono();
    mono = resampleSinc(mono, audio.sampleRate, kSampleRate);
    const std::size_t originalSamples = mono.size();
    std::vector<float> padded(kOverlapSamples / 2, 0.0F);
    padded.insert(padded.end(), mono.begin(), mono.end());
    result.preprocessingSeconds = elapsed(stage);

    std::vector<float> notes;
    std::vector<float> onsets;
    stage = Clock::now();
    for (std::size_t start = 0; start < padded.size(); start += kHopSamples) {
        std::vector<float> window(kWindowSamples, 0.0F);
        const auto available = std::min(kWindowSamples, padded.size() - start);
        std::copy_n(padded.begin() + static_cast<std::ptrdiff_t>(start), available, window.begin());
        const auto outputs = impl_->model.run(window, {1, static_cast<std::int64_t>(kWindowSamples), 1});
        const auto& noteOutput = named(outputs, "StatefulPartitionedCall:1");
        const auto& onsetOutput = named(outputs, "StatefulPartitionedCall:2");
        if (noteOutput.values.size() != kOutputFrames * 88 ||
            onsetOutput.values.size() != kOutputFrames * 88) {
            throw std::runtime_error("Basic Pitch output shape changed");
        }
        for (std::size_t frame = kCropFrames; frame < kOutputFrames - kCropFrames; ++frame) {
            const auto offset = frame * 88;
            notes.insert(notes.end(), noteOutput.values.begin() + static_cast<std::ptrdiff_t>(offset),
                noteOutput.values.begin() + static_cast<std::ptrdiff_t>(offset + 88));
            onsets.insert(onsets.end(), onsetOutput.values.begin() + static_cast<std::ptrdiff_t>(offset),
                onsetOutput.values.begin() + static_cast<std::ptrdiff_t>(offset + 88));
        }
    }
    result.inferenceSeconds = elapsed(stage);
    const std::size_t wantedFrames = originalSamples * kFramesPerSecond / kSampleRate;
    const std::size_t frames = std::min(wantedFrames, notes.size() / 88);
    notes.resize(frames * 88);
    onsets.resize(frames * 88);
    result.activationFrames = frames;

    stage = Clock::now();
    const std::size_t firstPitch = static_cast<std::size_t>(minimumMidi - kMidiOffset);
    const std::size_t lastPitch = static_cast<std::size_t>(maximumMidi - kMidiOffset);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::size_t pitch = 0; pitch < 88; ++pitch) {
            if (pitch < firstPitch || pitch > lastPitch) {
                cell(notes, frame, pitch) = 0.0F;
                cell(onsets, frame, pitch) = 0.0F;
            }
        }
    }

    // Basic Pitch supplements its onset head with positive changes common to
    // both the one-frame and two-frame note differences.
    float maximumOnset = 0.0F;
    float maximumDifference = 0.0F;
    std::vector<float> inferred(frames * 88, 0.0F);
    for (float value : onsets) maximumOnset = std::max(maximumOnset, value);
    for (std::size_t frame = 2; frame < frames; ++frame) {
        for (std::size_t pitch = firstPitch; pitch <= lastPitch; ++pitch) {
            const float difference = std::max(0.0F, std::min(
                cell(notes, frame, pitch) - cell(notes, frame - 1, pitch),
                cell(notes, frame, pitch) - cell(notes, frame - 2, pitch)));
            cell(inferred, frame, pitch) = difference;
            maximumDifference = std::max(maximumDifference, difference);
        }
    }
    if (maximumDifference > 0.0F) {
        const float scale = maximumOnset / maximumDifference;
        for (std::size_t index = 0; index < onsets.size(); ++index) {
            onsets[index] = std::max(onsets[index], inferred[index] * scale);
        }
    }

    struct Onset { std::size_t frame; std::size_t pitch; };
    std::vector<Onset> detected;
    for (std::size_t frame = 1; frame + 1 < frames; ++frame) {
        for (std::size_t pitch = firstPitch; pitch <= lastPitch; ++pitch) {
            const float value = cell(onsets, frame, pitch);
            if (value >= 0.5F && value > cell(onsets, frame - 1, pitch) &&
                value > cell(onsets, frame + 1, pitch)) {
                detected.push_back({frame, pitch});
            }
        }
    }
    std::reverse(detected.begin(), detected.end());
    std::vector<float> remaining = notes;
    struct FrameNote { std::size_t start; std::size_t end; std::size_t pitch; float confidence; };
    std::vector<FrameNote> decoded;
    auto consume = [&remaining, frames](std::size_t start, std::size_t pitch) {
        std::size_t cursor = start + 1;
        int quiet = 0;
        while (cursor + 1 < frames && quiet < 11) {
            quiet = cell(remaining, cursor, pitch) < 0.3F ? quiet + 1 : 0;
            ++cursor;
        }
        cursor -= static_cast<std::size_t>(quiet);
        return cursor;
    };
    for (const auto onset : detected) {
        if (onset.frame + 1 >= frames) continue;
        const auto end = consume(onset.frame, onset.pitch);
        if (end - onset.frame <= 11) continue;
        double confidence = 0.0;
        for (std::size_t frame = onset.frame; frame < end; ++frame) {
            confidence += cell(notes, frame, onset.pitch);
            cell(remaining, frame, onset.pitch) = 0.0F;
            if (onset.pitch > 0) cell(remaining, frame, onset.pitch - 1) = 0.0F;
            if (onset.pitch + 1 < 88) cell(remaining, frame, onset.pitch + 1) = 0.0F;
        }
        decoded.push_back({onset.frame, end, onset.pitch,
            static_cast<float>(confidence / static_cast<double>(end - onset.frame))});
    }

    // Melodia recovery finds sustained notes whose onset head was weak.
    for (;;) {
        const auto maximum = std::max_element(remaining.begin(), remaining.end());
        if (maximum == remaining.end() || *maximum <= 0.3F) break;
        const auto flat = static_cast<std::size_t>(maximum - remaining.begin());
        const std::size_t middle = flat / 88;
        const std::size_t pitch = flat % 88;
        if (pitch < firstPitch || pitch > lastPitch) { *maximum = 0.0F; continue; }
        *maximum = 0.0F;
        std::size_t forward = middle + 1;
        int quiet = 0;
        while (forward + 1 < frames && quiet < 11) {
            quiet = cell(remaining, forward, pitch) < 0.3F ? quiet + 1 : 0;
            cell(remaining, forward, pitch) = 0.0F;
            if (pitch > 0) cell(remaining, forward, pitch - 1) = 0.0F;
            if (pitch + 1 < 88) cell(remaining, forward, pitch + 1) = 0.0F;
            ++forward;
        }
        const std::size_t end = forward - 1 - static_cast<std::size_t>(quiet);
        std::int64_t backward = static_cast<std::int64_t>(middle) - 1;
        quiet = 0;
        while (backward > 0 && quiet < 11) {
            const auto frame = static_cast<std::size_t>(backward);
            quiet = cell(remaining, frame, pitch) < 0.3F ? quiet + 1 : 0;
            cell(remaining, frame, pitch) = 0.0F;
            if (pitch > 0) cell(remaining, frame, pitch - 1) = 0.0F;
            if (pitch + 1 < 88) cell(remaining, frame, pitch + 1) = 0.0F;
            --backward;
        }
        const auto rawStart = backward + 1 + quiet;
        const std::size_t start = static_cast<std::size_t>(std::clamp<std::int64_t>(
            rawStart, 0, static_cast<std::int64_t>(frames) - 1));
        if (end <= start || end - start <= 11) continue;
        double confidence = 0.0;
        for (std::size_t frame = start; frame < end; ++frame) {
            confidence += cell(notes, frame, pitch);
        }
        decoded.push_back({start, end, pitch,
            static_cast<float>(confidence / static_cast<double>(end - start))});
    }

    const double duration = static_cast<double>(originalSamples) / kSampleRate;
    result.notes.reserve(decoded.size());
    for (const auto& note : decoded) {
        const double start = std::clamp(frameTime(note.start), 0.0, duration);
        const double end = std::clamp(frameTime(note.end), start, duration);
        if (end <= start) continue;
        result.notes.push_back(PitchNote{start, end,
            static_cast<int>(note.pitch) + kMidiOffset,
            std::clamp(static_cast<int>(std::lround(127.0F * note.confidence)), 1, 127),
            note.confidence});
    }
    std::sort(result.notes.begin(), result.notes.end(), [](const PitchNote& left, const PitchNote& right) {
        return left.start != right.start ? left.start < right.start : left.midi < right.midi;
    });
    result.postprocessingSeconds = elapsed(stage);
    return result;
}

} // namespace jamtaster::native

#include "ChordMini.hpp"

#include "Dsp.hpp"
#include "OnnxModel.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <stdexcept>

namespace jamtaster::native {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kSequence = 108;
constexpr std::size_t kStride = 54;
constexpr std::size_t kFeatures = 144;
constexpr std::size_t kClasses = 170;
constexpr float kMean = -2.369753837585449F;
constexpr float kStd = 1.9627078771591187F;
constexpr double kFrameDuration = 2048.0 / 22050.0;

double elapsed(Clock::time_point start)
{
    return std::chrono::duration<double>(Clock::now() - start).count();
}

std::string chordLabel(std::size_t index)
{
    if (index == 168) return "X";
    if (index == 169 || index >= kClasses) return "N";
    static constexpr std::array<const char*, 12> roots{
        "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    static constexpr std::array<const char*, 14> qualities{
        "min", "maj", "dim", "aug", "min6", "maj6", "min7", "minmaj7",
        "maj7", "7", "dim7", "hdim7", "sus2", "sus4"};
    const std::size_t root = index / qualities.size();
    const std::size_t quality = index % qualities.size();
    if (quality == 1) return roots[root];
    return std::string(roots[root]) + ":" + qualities[quality];
}

void gaussianSmooth(std::vector<float>& logits)
{
    static const std::array<float, 9> weights = [] {
        std::array<float, 9> values{};
        double sum = 0.0;
        for (int index = 0; index < 9; ++index) {
            const double x = index - 4;
            values[static_cast<std::size_t>(index)] =
                static_cast<float>(std::exp(-0.5 * std::pow(x / 1.5, 2.0)));
            sum += values[static_cast<std::size_t>(index)];
        }
        for (float& value : values) value = static_cast<float>(value / sum);
        return values;
    }();
    std::vector<float> smoothed(logits.size(), 0.0F);
    for (std::size_t frame = 0; frame < kSequence; ++frame) {
        for (std::size_t chord = 0; chord < kClasses; ++chord) {
            double value = 0.0;
            for (int offset = -4; offset <= 4; ++offset) {
                const auto source = static_cast<std::size_t>(std::clamp<std::int64_t>(
                    static_cast<std::int64_t>(frame) + offset, 0,
                    static_cast<std::int64_t>(kSequence) - 1));
                value += logits[source * kClasses + chord] *
                    weights[static_cast<std::size_t>(offset + 4)];
            }
            smoothed[frame * kClasses + chord] = static_cast<float>(value);
        }
    }
    logits = std::move(smoothed);
}

} // namespace

class ChordMini::Impl {
public:
    Impl(const std::filesystem::path& path, int threads) : model(path, threads) {}
    OnnxModel model;
};

ChordMini::ChordMini(const std::filesystem::path& modelPath, int threads)
    : impl_(std::make_unique<Impl>(modelPath, threads))
{
}

ChordMini::~ChordMini() = default;

ChordAnalysis ChordMini::analyze(const AudioBuffer& audio)
{
    ChordAnalysis result;
    auto stage = Clock::now();
    auto mono = audio.mono();
    mono = resampleSinc(mono, audio.sampleRate, 22050);
    const auto cqt = chordMiniLogCqt(mono);
    result.featureFrames = cqt.rows;
    result.preprocessingSeconds = elapsed(stage);

    const std::size_t paddedFrames = ((cqt.rows + kSequence - 1) / kSequence) * kSequence;
    std::vector<float> accumulated(cqt.rows * kClasses, 0.0F);
    std::vector<unsigned int> counts(cqt.rows, 0);
    stage = Clock::now();
    for (std::size_t start = 0; start + kSequence <= paddedFrames; start += kStride) {
        std::vector<float> input(kSequence * kFeatures, 0.0F);
        const std::size_t valid = std::min(kSequence, cqt.rows > start ? cqt.rows - start : 0);
        if (valid == 0) continue;
        for (std::size_t frame = 0; frame < valid; ++frame) {
            for (std::size_t feature = 0; feature < kFeatures; ++feature) {
                input[frame * kFeatures + feature] =
                    (cqt.at(start + frame, feature) - kMean) / (kStd + 1.0e-8F);
            }
        }
        // Padding is applied before normalization in ChordMini, so its zeros
        // become this non-zero normalized value at the model boundary.
        for (std::size_t frame = valid; frame < kSequence; ++frame) {
            for (std::size_t feature = 0; feature < kFeatures; ++feature) {
                input[frame * kFeatures + feature] = -kMean / (kStd + 1.0e-8F);
            }
        }
        auto outputs = impl_->model.run(input, {1, 108, 144});
        if (outputs.size() != 1 || outputs.front().values.size() != kSequence * kClasses) {
            throw std::runtime_error("ChordMini output shape changed");
        }
        gaussianSmooth(outputs.front().values);
        for (std::size_t frame = 0; frame < valid; ++frame) {
            const std::size_t destination = start + frame;
            for (std::size_t chord = 0; chord < kClasses; ++chord) {
                accumulated[destination * kClasses + chord] +=
                    outputs.front().values[frame * kClasses + chord];
            }
            ++counts[destination];
        }
    }
    result.inferenceSeconds = elapsed(stage);

    stage = Clock::now();
    std::vector<std::size_t> predictions(cqt.rows, 169);
    std::vector<float> margins(cqt.rows, 0.0F);
    for (std::size_t frame = 0; frame < cqt.rows; ++frame) {
        if (counts[frame] == 0) continue;
        std::size_t best = 0;
        std::size_t second = 1;
        if (accumulated[second + frame * kClasses] > accumulated[best + frame * kClasses]) {
            std::swap(best, second);
        }
        for (std::size_t chord = 2; chord < kClasses; ++chord) {
            const float score = accumulated[frame * kClasses + chord];
            if (score > accumulated[frame * kClasses + best]) {
                second = best;
                best = chord;
            } else if (score > accumulated[frame * kClasses + second]) {
                second = chord;
            }
        }
        predictions[frame] = best;
        margins[frame] = (accumulated[frame * kClasses + best] -
            accumulated[frame * kClasses + second]) / static_cast<float>(counts[frame]);
    }
    const std::size_t maximumFrames = mono.size() / 2048;
    if (maximumFrames > 0 && maximumFrames < predictions.size()) {
        predictions.resize(maximumFrames);
        margins.resize(maximumFrames);
    }

    // Nine-frame categorical majority filter with edge replication and the
    // current class winning ties, matching the Python analysis path.
    std::vector<std::size_t> filtered = predictions;
    if (predictions.size() >= 9) {
        for (std::size_t frame = 0; frame < predictions.size(); ++frame) {
            std::map<std::size_t, int> votes;
            for (int offset = -4; offset <= 4; ++offset) {
                const auto source = static_cast<std::size_t>(std::clamp<std::int64_t>(
                    static_cast<std::int64_t>(frame) + offset, 0,
                    static_cast<std::int64_t>(predictions.size()) - 1));
                ++votes[predictions[source]];
            }
            int maximum = 0;
            for (const auto& entry : votes) maximum = std::max(maximum, entry.second);
            if (votes[predictions[frame]] == maximum) {
                filtered[frame] = predictions[frame];
            } else {
                filtered[frame] = std::find_if(votes.begin(), votes.end(),
                    [maximum](const auto& entry) { return entry.second == maximum; })->first;
            }
        }
    }

    if (!filtered.empty()) {
        std::size_t start = 0;
        for (std::size_t frame = 1; frame <= filtered.size(); ++frame) {
            if (frame < filtered.size() && filtered[frame] == filtered[start]) continue;
            const double segmentStart = start * kFrameDuration;
            const double segmentEnd = frame * kFrameDuration;
            if (segmentEnd - segmentStart >= 0.25) {
                double margin = 0.0;
                for (std::size_t index = start; index < frame; ++index) margin += margins[index];
                result.chords.push_back(ChordSegment{segmentStart, segmentEnd,
                    chordLabel(filtered[start]), static_cast<float>(margin / (frame - start))});
            }
            start = frame;
        }
    }
    result.postprocessingSeconds = elapsed(stage);
    return result;
}

} // namespace jamtaster::native

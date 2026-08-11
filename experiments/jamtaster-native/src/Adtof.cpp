#include "Adtof.hpp"

#include "Dsp.hpp"
#include "OnnxModel.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <stdexcept>

namespace jamtaster::native {
namespace {

using Clock = std::chrono::steady_clock;

double elapsed(Clock::time_point start)
{
    return std::chrono::duration<double>(Clock::now() - start).count();
}

} // namespace

class Adtof::Impl {
public:
    Impl(const std::filesystem::path& path, int threads) : model(path, threads) {}
    OnnxModel model;
};

Adtof::Adtof(const std::filesystem::path& modelPath, int threads)
    : impl_(std::make_unique<Impl>(modelPath, threads))
{
}

Adtof::~Adtof() = default;

DrumAnalysis Adtof::analyze(
    const AudioBuffer& audio, const std::array<float, 5>& thresholds)
{
    for (const float threshold : thresholds) {
        if (!std::isfinite(threshold) || threshold < 0.0F || threshold > 1.0F) {
            throw std::invalid_argument("ADTOF thresholds must be within 0..1");
        }
    }
    DrumAnalysis result;
    auto stage = Clock::now();
    auto mono = audio.mono();
    mono = resampleSinc(mono, audio.sampleRate, 44100);
    const auto features = adtofLogFilterbank(mono);
    result.preprocessingSeconds = elapsed(stage);

    stage = Clock::now();
    const auto outputs = impl_->model.run(features.values,
        {1, static_cast<std::int64_t>(features.rows), 84, 1});
    if (outputs.size() != 1 || outputs.front().shape.size() != 3 ||
        outputs.front().values.size() != features.rows * 5) {
        throw std::runtime_error("ADTOF output shape changed");
    }
    result.inferenceSeconds = elapsed(stage);
    result.activationFrames = features.rows;

    stage = Clock::now();
    static constexpr std::array<int, 5> midi{35, 38, 47, 42, 49};
    static constexpr std::array<const char*, 5> lanes{
        "Kick", "Snare", "Mid Tom", "Closed HH", "Crash"};
    const auto& activations = outputs.front().values;
    for (std::size_t drum = 0; drum < 5; ++drum) {
        std::vector<float> processed(features.rows, 0.0F);
        // Exact ADTOF-pytorch peak preparation: subtract the edge-padded
        // 12-frame moving average (10 before, current, one after).
        for (std::size_t frame = 0; frame < features.rows; ++frame) {
            double average = 0.0;
            for (int offset = -10; offset <= 1; ++offset) {
                const auto source = static_cast<std::size_t>(std::clamp<std::int64_t>(
                    static_cast<std::int64_t>(frame) + offset, 0,
                    static_cast<std::int64_t>(features.rows) - 1));
                average += activations[source * 5 + drum];
            }
            const float value = activations[frame * 5 + drum];
            processed[frame] = std::max(0.0F, value - static_cast<float>(average / 12.0));
        }
        std::vector<std::size_t> peaks;
        for (std::size_t frame = 0; frame < features.rows; ++frame) {
            float localMaximum = 0.0F;
            for (int offset = -2; offset <= 1; ++offset) {
                const auto source = static_cast<std::size_t>(std::clamp<std::int64_t>(
                    static_cast<std::int64_t>(frame) + offset, 0,
                    static_cast<std::int64_t>(features.rows) - 1));
                localMaximum = std::max(localMaximum, processed[source]);
            }
            if (processed[frame] >= localMaximum && processed[frame] >= thresholds[drum]) {
                peaks.push_back(frame);
            }
        }
        // Combine candidates at most two frames apart, retaining the strongest.
        for (std::size_t begin = 0; begin < peaks.size();) {
            std::size_t end = begin + 1;
            while (end < peaks.size() && peaks[end] - peaks[end - 1] <= 2) ++end;
            auto best = peaks[begin];
            for (std::size_t index = begin + 1; index < end; ++index) {
                if (processed[peaks[index]] > processed[best]) best = peaks[index];
            }
            result.hits.push_back(DrumEvent{
                static_cast<double>(best) / 100.0, midi[drum], lanes[drum],
                activations[best * 5 + drum]});
            begin = end;
        }
    }
    std::sort(result.hits.begin(), result.hits.end(),
        [](const DrumEvent& left, const DrumEvent& right) {
            return left.time != right.time ? left.time < right.time : left.midi < right.midi;
        });
    result.postprocessingSeconds = elapsed(stage);
    return result;
}

} // namespace jamtaster::native

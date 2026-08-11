#include "BeatThis.hpp"

#include "Dsp.hpp"
#include "Wav.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace jamtaster::native {
namespace {

using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point start)
{
    return std::chrono::duration<double>(Clock::now() - start).count();
}

std::vector<int> chunkStarts(int frames)
{
    constexpr int chunk = 1500;
    constexpr int border = 6;
    std::vector<int> result;
    for (int start = -border; start < frames - border;
         start += chunk - 2 * border) {
        result.push_back(start);
    }
    if (result.empty()) result.push_back(-border);
    if (frames > chunk - 2 * border) result.back() = frames - (chunk - border);
    return result;
}

const TensorResult& outputNamed(
    const std::vector<TensorResult>& outputs,
    std::string_view wanted)
{
    auto found = std::find_if(outputs.begin(), outputs.end(), [&](const auto& output) {
        return output.name == wanted;
    });
    if (found == outputs.end()) {
        found = std::find_if(outputs.begin(), outputs.end(), [&](const auto& output) {
            return output.name.find(wanted) != std::string::npos;
        });
    }
    if (found == outputs.end()) {
        throw std::runtime_error("Beat This ONNX output is missing: " + std::string(wanted));
    }
    return *found;
}

std::vector<double> deduplicate(const std::vector<int>& peaks)
{
    std::vector<double> result;
    if (peaks.empty()) return result;
    double running = peaks.front();
    int count = 1;
    for (std::size_t index = 1; index < peaks.size(); ++index) {
        if (peaks[index] - running <= 1.0) {
            ++count;
            running += (peaks[index] - running) / count;
        } else {
            result.push_back(running);
            running = peaks[index];
            count = 1;
        }
    }
    result.push_back(running);
    return result;
}

} // namespace

BeatThis::BeatThis(const std::filesystem::path& modelPath, int threads)
    : model_(modelPath, threads)
{
    if (model_.outputs().size() != 2) {
        throw std::runtime_error("Beat This graph must expose beat and downbeat outputs");
    }
}

BeatAnalysis BeatThis::analyze(const AudioBuffer& audio)
{
    BeatAnalysis result;
    auto started = Clock::now();
    auto mono = audio.mono();
    mono = resampleSinc(mono, audio.sampleRate, 22050);
    const Matrix spectrogram = beatThisLogMel(mono);
    result.preprocessingSeconds = secondsSince(started);

    constexpr int chunkSize = 1500;
    constexpr int border = 6;
    const int frames = static_cast<int>(spectrogram.rows);
    const auto starts = chunkStarts(frames);
    result.beatLogits.assign(spectrogram.rows, -1000.0F);
    result.downbeatLogits.assign(spectrogram.rows, -1000.0F);
    started = Clock::now();
    for (auto iterator = starts.rbegin(); iterator != starts.rend(); ++iterator) {
        const int start = *iterator;
        const int actualStart = std::max(0, start);
        const int actualEnd = std::min(start + chunkSize, frames);
        const int leftPadding = std::max(0, -start);
        const int rightPadding = std::max(0,
            std::min(border, start + chunkSize - frames));
        const int chunkFrames = leftPadding + std::max(0, actualEnd - actualStart) + rightPadding;
        if (chunkFrames <= 2 * border) continue;
        std::vector<float> input(
            static_cast<std::size_t>(chunkFrames) * spectrogram.columns, 0.0F);
        for (int frame = actualStart; frame < actualEnd; ++frame) {
            const int target = leftPadding + frame - actualStart;
            std::copy_n(
                spectrogram.values.begin() +
                    static_cast<std::ptrdiff_t>(frame) * static_cast<std::ptrdiff_t>(spectrogram.columns),
                static_cast<std::ptrdiff_t>(spectrogram.columns),
                input.begin() +
                    static_cast<std::ptrdiff_t>(target) * static_cast<std::ptrdiff_t>(spectrogram.columns));
        }
        const auto outputs = model_.run(input,
            {1, chunkFrames, static_cast<std::int64_t>(spectrogram.columns)});
        const auto& beat = outputNamed(outputs, "beat");
        const auto& downbeat = outputNamed(outputs, "downbeat");
        if (beat.values.size() != downbeat.values.size() ||
            beat.values.size() < static_cast<std::size_t>(2 * border)) {
            throw std::runtime_error("Beat This returned invalid output shapes");
        }
        for (std::size_t frame = border; frame + border < beat.values.size(); ++frame) {
            const auto target = static_cast<std::int64_t>(start) +
                static_cast<std::int64_t>(frame);
            if (target < 0 || target >= frames) continue;
            result.beatLogits[static_cast<std::size_t>(target)] = beat.values[frame];
            result.downbeatLogits[static_cast<std::size_t>(target)] = downbeat.values[frame];
        }
    }
    result.inferenceSeconds = secondsSince(started);

    started = Clock::now();
    result.beats = peakTimes(result.beatLogits);
    result.downbeats = peakTimes(result.downbeatLogits);
    if (!result.beats.empty()) {
        for (float& downbeat : result.downbeats) {
            const auto nearest = std::min_element(result.beats.begin(), result.beats.end(),
                [&](float left, float right) {
                    return std::abs(left - downbeat) < std::abs(right - downbeat);
                });
            downbeat = *nearest;
        }
        std::sort(result.downbeats.begin(), result.downbeats.end());
        result.downbeats.erase(
            std::unique(result.downbeats.begin(), result.downbeats.end()),
            result.downbeats.end());
    }
    result.postprocessingSeconds = secondsSince(started);
    return result;
}

std::vector<float> BeatThis::peakTimes(const std::vector<float>& logits)
{
    std::vector<int> peaks;
    for (std::size_t index = 0; index < logits.size(); ++index) {
        const std::size_t first = index > 3 ? index - 3 : 0;
        const std::size_t last = std::min(logits.size(), index + 4);
        float maximum = -std::numeric_limits<float>::infinity();
        for (std::size_t neighbour = first; neighbour < last; ++neighbour) {
            maximum = std::max(maximum, logits[neighbour]);
        }
        if (logits[index] == maximum && logits[index] > 0.0F) {
            peaks.push_back(static_cast<int>(index));
        }
    }
    const auto deduplicated = deduplicate(peaks);
    std::vector<float> times;
    times.reserve(deduplicated.size());
    for (double peak : deduplicated) {
        times.push_back(static_cast<float>(peak / 50.0));
    }
    return times;
}

} // namespace jamtaster::native

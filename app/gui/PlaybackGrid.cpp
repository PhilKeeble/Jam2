#include "PlaybackGrid.hpp"

#include "metronome.hpp"
#include "runtime_limits.hpp"

#include <QtGlobal>

#include <cmath>
#include <limits>

namespace {

constexpr std::uint64_t kMaximumFrame =
    (std::numeric_limits<std::uint64_t>::max)();

std::uint64_t saturatingAdd(
    std::uint64_t left,
    std::uint64_t right) noexcept
{
    return left > kMaximumFrame - right ? kMaximumFrame : left + right;
}

std::uint64_t elapsedFrames(
    qint64 elapsedMilliseconds,
    int sampleRate) noexcept
{
    if (elapsedMilliseconds <= 0 ||
        !jam2::limits::valid_sample_rate(sampleRate)) {
        return 0;
    }
    const std::uint64_t milliseconds =
        static_cast<std::uint64_t>(elapsedMilliseconds);
    const std::uint64_t seconds = milliseconds / 1000ULL;
    const std::uint64_t remainder = milliseconds % 1000ULL;
    const std::uint64_t rate = static_cast<std::uint64_t>(sampleRate);
    if (seconds > kMaximumFrame / rate) {
        return kMaximumFrame;
    }
    return saturatingAdd(
        seconds * rate,
        remainder * rate / 1000ULL);
}

} // namespace

void PlaybackGrid::setPattern(
    double bpm,
    int beatsPerBar,
    int division,
    int tempoPulseUnits)
{
    bpm_ = std::isfinite(bpm) ? qBound(1.0, bpm, 400.0) : 120.0;
    beatsPerBar_ = jam2::metronome::clamp_beats_per_bar(beatsPerBar);
    division_ = jam2::metronome::clamp_division(division);
    tempoPulseUnits_ = jam2::metronome::clamp_tempo_pulse_units(tempoPulseUnits);
}

void PlaybackGrid::updateEngine(
    std::uint64_t rawFrame,
    std::uint64_t musicalFrame,
    std::uint64_t epochFrame,
    std::int64_t renderOffsetFrames,
    int sampleRate,
    bool running)
{
    engineFrame_ = rawFrame;
    engineMusicalFrame_ = musicalFrame;
    engineEpochFrame_ = epochFrame;
    engineRenderOffsetFrames_ = renderOffsetFrames;
    engineSampleRate_ = jam2::limits::valid_sample_rate(sampleRate)
        ? sampleRate
        : 0;
    engineRunning_ = running;
    engineValid_ = engineSampleRate_ > 0;
    engineReportTime_.start();
}

void PlaybackGrid::clearEngine()
{
    engineValid_ = false;
    engineRunning_ = false;
}

PlaybackGrid::Position PlaybackGrid::position() const
{
    Position result;
    double seconds = 0.0;
    if (engineValid_) {
        std::uint64_t rawFrame = engineFrame_;
        std::uint64_t musicalFrame = engineMusicalFrame_;
        if (engineRunning_ && engineReportTime_.isValid()) {
            const std::uint64_t advanced = elapsedFrames(
                engineReportTime_.elapsed(), engineSampleRate_);
            rawFrame = saturatingAdd(rawFrame, advanced);
            musicalFrame = saturatingAdd(musicalFrame, advanced);
        }
        const std::uint64_t epochFrame = engineEpochFrame_;
        result.engineAnchored = true;
        result.epochFrame = epochFrame;
        result.rawCurrentFrame = rawFrame;
        result.currentFrame = musicalFrame;
        result.renderOffsetFrames = engineRenderOffsetFrames_;
        result.sampleRate = engineSampleRate_;
        if (engineRunning_ && musicalFrame >= epochFrame) {
            seconds = static_cast<double>(musicalFrame - epochFrame) / engineSampleRate_;
        }
        result.running = engineRunning_;
    } else {
        return result;
    }

    result.secondsPerBeat =
        60.0 / bpm_ / static_cast<double>(tempoPulseUnits_);
    result.secondsPerStep = result.secondsPerBeat / static_cast<double>(division_);
    const double steps = seconds / result.secondsPerStep;
    result.absoluteStep = static_cast<std::uint64_t>(std::floor(qMax(0.0, steps)));
    result.absoluteBeat = result.absoluteStep / static_cast<std::uint64_t>(division_);
    result.beat = static_cast<int>((result.absoluteBeat) %
        static_cast<std::uint64_t>(beatsPerBar_));
    result.subdivision = static_cast<int>(result.absoluteStep % static_cast<std::uint64_t>(division_));
    result.secondsFromEpoch = seconds;
    return result;
}

double PlaybackGrid::bpm() const
{
    return bpm_;
}

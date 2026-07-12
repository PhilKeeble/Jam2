#include "PlaybackGrid.hpp"

#include <QtGlobal>

#include <cmath>

void PlaybackGrid::setPattern(double bpm, int beatsPerBar, int division)
{
    bpm_ = qBound(1.0, bpm, 400.0);
    beatsPerBar_ = qBound(1, beatsPerBar, 16);
    division_ = qMax(1, division);
}

void PlaybackGrid::scheduleEpoch(
    std::uint64_t targetRawFrame,
    std::uint64_t targetMusicalFrame,
    std::uint64_t revision)
{
    if (revision < pendingEpochRevision_) {
        return;
    }
    pendingEpochRawFrame_ = targetRawFrame;
    pendingEpochMusicalFrame_ = targetMusicalFrame;
    pendingEpochRevision_ = revision;
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
    engineSampleRate_ = qMax(0, sampleRate);
    engineRunning_ = running;
    engineValid_ = engineSampleRate_ > 0;
    engineReportTime_.start();
}

void PlaybackGrid::clearEngine()
{
    engineValid_ = false;
    engineRunning_ = false;
    pendingEpochRawFrame_ = 0;
    pendingEpochMusicalFrame_ = 0;
    pendingEpochRevision_ = 0;
}

PlaybackGrid::Position PlaybackGrid::position() const
{
    Position result;
    double seconds = 0.0;
    if (engineValid_) {
        std::uint64_t rawFrame = engineFrame_;
        std::uint64_t musicalFrame = engineMusicalFrame_;
        if (engineRunning_ && engineReportTime_.isValid()) {
            const std::uint64_t elapsedFrames = static_cast<std::uint64_t>(engineReportTime_.elapsed()) *
                static_cast<std::uint64_t>(engineSampleRate_) / 1000ULL;
            rawFrame += elapsedFrames;
            musicalFrame += elapsedFrames;
        }
        std::uint64_t epochFrame = engineEpochFrame_;
        if (pendingEpochRawFrame_ > 0 && rawFrame >= pendingEpochRawFrame_) {
            epochFrame = pendingEpochMusicalFrame_;
        }
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

    const double steps = seconds * bpm_ * division_ / 60.0;
    result.secondsPerBeat = 60.0 / bpm_;
    result.secondsPerStep = result.secondsPerBeat / static_cast<double>(division_);
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

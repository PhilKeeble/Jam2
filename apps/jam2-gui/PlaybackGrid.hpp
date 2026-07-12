#pragma once

#include <QElapsedTimer>

#include <cstdint>

// A UI view of the engine-owned musical clock. The elapsed timer only
// interpolates between engine reports for smooth drawing.
class PlaybackGrid {
public:
    struct Position {
        bool running = false;
        bool engineAnchored = false;
        std::uint64_t absoluteStep = 0;
        std::uint64_t absoluteBeat = 0;
        int beat = 0;
        int subdivision = 0;
        double secondsFromEpoch = 0.0;
        double secondsPerStep = 0.0;
        double secondsPerBeat = 0.0;
        std::uint64_t epochFrame = 0;
        std::uint64_t rawCurrentFrame = 0;
        std::uint64_t currentFrame = 0;
        std::int64_t renderOffsetFrames = 0;
        int sampleRate = 0;
    };

    void setPattern(double bpm, int beatsPerBar, int division);
    void updateEngine(
        std::uint64_t rawFrame,
        std::uint64_t musicalFrame,
        std::uint64_t epochFrame,
        std::int64_t renderOffsetFrames,
        int sampleRate,
        bool running);
    void scheduleEpoch(std::uint64_t targetRawFrame, std::uint64_t targetMusicalFrame, std::uint64_t revision);
    void clearEngine();
    Position position() const;
    double bpm() const;

private:
    double bpm_ = 120.0;
    int beatsPerBar_ = 4;
    int division_ = 1;
    bool engineRunning_ = false;
    bool engineValid_ = false;
    std::uint64_t engineFrame_ = 0;
    std::uint64_t engineMusicalFrame_ = 0;
    std::uint64_t engineEpochFrame_ = 0;
    std::int64_t engineRenderOffsetFrames_ = 0;
    int engineSampleRate_ = 0;
    QElapsedTimer engineReportTime_;
    std::uint64_t pendingEpochRawFrame_ = 0;
    std::uint64_t pendingEpochMusicalFrame_ = 0;
    std::uint64_t pendingEpochRevision_ = 0;
};

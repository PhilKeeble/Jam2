#pragma once

#include <QtGlobal>

#include <cmath>
#include <limits>

namespace jam2::gui {

inline constexpr int kSectionOverviewBarsPerPage = 32;

inline int sectionOverviewPageCount(int barCount) noexcept
{
    const int bounded = qMax(1, barCount);
    return 1 + (bounded - 1) / kSectionOverviewBarsPerPage;
}

inline int sectionOverviewPageForBar(int bar, int barCount) noexcept
{
    const int boundedBar = qBound(0, bar, qMax(1, barCount) - 1);
    return qBound(
        0,
        boundedBar / kSectionOverviewBarsPerPage,
        sectionOverviewPageCount(barCount) - 1);
}

inline int sectionBeatCountForTimelineEnd(
    qint64 endFrame,
    int sampleRate,
    double bpm,
    int tempoPulseUnits,
    int beatsPerBar) noexcept
{
    if (endFrame <= 0 || sampleRate <= 0 ||
        !std::isfinite(bpm) || bpm <= 0.0) {
        return qMax(1, beatsPerBar);
    }
    tempoPulseUnits = qMax(1, tempoPulseUnits);
    beatsPerBar = qMax(1, beatsPerBar);
    const long double framesPerBeat =
        static_cast<long double>(sampleRate) * 60.0L /
        (static_cast<long double>(bpm) *
         static_cast<long double>(tempoPulseUnits));
    if (!std::isfinite(framesPerBeat) || framesPerBeat <= 0.0L) {
        return beatsPerBar;
    }
    // A rendered musical endpoint is rounded up to a whole audio frame. Allow
    // that complete frame of padding so it cannot be mistaken for another beat
    // and then rounded up to an otherwise empty bar.
    const long double frameToleranceInBeats = 1.000001L / framesPerBeat;
    const long double occupied = std::ceil(
        static_cast<long double>(endFrame) / framesPerBeat -
        frameToleranceInBeats);
    const int maximum = (std::numeric_limits<int>::max)();
    if (!std::isfinite(occupied) || occupied >= static_cast<long double>(maximum)) {
        return maximum;
    }
    const int occupiedBeats = qMax(1, static_cast<int>(occupied));
    if (occupiedBeats > maximum - (beatsPerBar - 1)) return maximum;
    return ((occupiedBeats + beatsPerBar - 1) / beatsPerBar) * beatsPerBar;
}

inline qint64 sectionExtensionPreviewFrames(
    qint64 committedFrames,
    qint64 contentEndFrames) noexcept
{
    return qMax<qint64>(1, qMax(committedFrames, contentEndFrames));
}

struct SectionTimelineCrop {
    bool removePlacement = false;
    qint64 stopFrame = -1;
    qint64 sourceStartFrame = -1;
    qint64 sourceEndFrame = -1;
};

inline SectionTimelineCrop sectionTimelineCropForEnd(
    qint64 laneStartFrame,
    qint64 sourceStartFrame,
    int laneSampleRate,
    int timelineSampleRate,
    qint64 timelineEndFrame) noexcept
{
    laneStartFrame = qMax<qint64>(0, laneStartFrame);
    timelineEndFrame = qMax<qint64>(0, timelineEndFrame);
    if (laneStartFrame >= timelineEndFrame) {
        return {true, timelineEndFrame, -1, -1};
    }
    sourceStartFrame = qBound<qint64>(
        0,
        sourceStartFrame,
        (std::numeric_limits<qint64>::max)() - 1);
    qint64 retainedSourceFrames = timelineEndFrame - laneStartFrame;
    if (laneSampleRate > 0 && timelineSampleRate > 0 &&
        laneSampleRate != timelineSampleRate) {
        const long double converted = std::round(
            static_cast<long double>(retainedSourceFrames) *
            static_cast<long double>(laneSampleRate) /
            static_cast<long double>(timelineSampleRate));
        const qint64 available =
            (std::numeric_limits<qint64>::max)() - sourceStartFrame;
        retainedSourceFrames = !std::isfinite(converted) ||
                converted >= static_cast<long double>(available)
            ? available
            : qMax<qint64>(1, static_cast<qint64>(converted));
    }
    const qint64 available =
        (std::numeric_limits<qint64>::max)() - sourceStartFrame;
    retainedSourceFrames = qBound<qint64>(1, retainedSourceFrames, available);
    return {
        false,
        timelineEndFrame,
        sourceStartFrame,
        sourceStartFrame + retainedSourceFrames,
    };
}

} // namespace jam2::gui

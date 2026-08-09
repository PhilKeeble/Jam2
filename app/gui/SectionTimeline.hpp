#pragma once

#include <QtGlobal>

#include <cmath>

namespace jam2::gui {

inline constexpr int kSectionOverviewBarsPerPage = 32;

inline int sectionOverviewPageCount(int barCount) noexcept
{
    return qMax(1, (qMax(1, barCount) + kSectionOverviewBarsPerPage - 1) /
        kSectionOverviewBarsPerPage);
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
    if (endFrame <= 0 || sampleRate <= 0 || bpm <= 0.0) {
        return qMax(1, beatsPerBar);
    }
    tempoPulseUnits = qMax(1, tempoPulseUnits);
    beatsPerBar = qMax(1, beatsPerBar);
    const double framesPerBeat =
        static_cast<double>(sampleRate) * 60.0 /
        (bpm * static_cast<double>(tempoPulseUnits));
    // A rendered musical endpoint is rounded up to a whole audio frame. Allow
    // that complete frame of padding so it cannot be mistaken for another beat
    // and then rounded up to an otherwise empty bar.
    const double frameToleranceInBeats = 1.000001 / framesPerBeat;
    const int occupiedBeats = qMax(
        1,
        static_cast<int>(std::ceil(
            static_cast<double>(endFrame) / framesPerBeat -
            frameToleranceInBeats)));
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
    sourceStartFrame = qMax<qint64>(0, sourceStartFrame);
    qint64 retainedSourceFrames = timelineEndFrame - laneStartFrame;
    if (laneSampleRate > 0 && timelineSampleRate > 0 &&
        laneSampleRate != timelineSampleRate) {
        retainedSourceFrames = static_cast<qint64>(std::llround(
            static_cast<double>(retainedSourceFrames) *
            laneSampleRate / timelineSampleRate));
    }
    return {
        false,
        timelineEndFrame,
        sourceStartFrame,
        sourceStartFrame + qMax<qint64>(1, retainedSourceFrames),
    };
}

} // namespace jam2::gui

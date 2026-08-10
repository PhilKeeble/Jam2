#pragma once

#include "LooperProject.hpp"
#include "SharedTrackModel.hpp"

#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>

// Applied to every prepared backing-track mix before its final soft limiting.
// Preview paths that audition a rendered lane directly use the same gain so
// their level remains representative of normal prepared-track playback.
inline constexpr double kPreparedMixMasterPreGain = 0.72;

struct PreparedMixResult {
    int bankIndex = 0;
    QString path;
    qint64 frames = 0;
    qint64 renderMs = 0;
    qint64 fileBytes = 0;
    int sampleRate = 0;
    int durationMs = 0;
    double masterPreGain = kPreparedMixMasterPreGain;
    float preMasterPeak = 0.0f;
    float outputPeak = 0.0f;
    qint64 overUnitySamples = 0;
    QString sha256;
    QStringList warnings;
    QString error;
};

struct PreparedMixSequenceSegment {
    int bankIndex = 0;
    int repeats = 1;
    qint64 exactOutputFrames = 0;
};

class PreparedMixRenderer {
public:
    static bool hasRenderableSources(const LooperProject& project);
    static bool hasRenderableSources(const LooperProject& project, int bankIndex);

    static QString outputPath(
        const QString& workspaceFolder,
        int bankIndex,
        std::uint64_t generation,
        qint64 processId);

    // PCM16 WAV only. Rendering is caller-thread/offline work and never runs
    // in an audio callback.
    static PreparedMixResult render(
        const LooperProject& project,
        const QString& projectFolder,
        int sampleRate,
        const QString& outputPath,
        const SharedTrackModel& track,
        int bankIndex = -1,
        qint64 exactOutputFrames = 0);

    // Renders each referenced bank through the same path used by live prepared
    // playback, then crops or pads it to its musical section boundary before
    // concatenating the requested repeats into one mono PCM16 WAV.
    static PreparedMixResult renderSequence(
        const LooperProject& project,
        const QString& projectFolder,
        int sampleRate,
        const QString& outputPath,
        const SharedTrackModel& track,
        const QVector<PreparedMixSequenceSegment>& segments);
};

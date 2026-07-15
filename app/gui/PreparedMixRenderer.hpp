#pragma once

#include "LooperProject.hpp"
#include "SharedTrackModel.hpp"

#include <QString>

struct PreparedMixResult {
    QString path;
    qint64 frames = 0;
    qint64 renderMs = 0;
    qint64 fileBytes = 0;
    int sampleRate = 0;
    int durationMs = 0;
    QString sha256;
    QString error;
};

class PreparedMixRenderer {
public:
    // PCM16 WAV only. Rendering is caller-thread/offline work and never runs
    // in an audio callback.
    static PreparedMixResult render(
        const LooperProject& project,
        const QString& projectFolder,
        int sampleRate,
        const QString& outputPath,
        const SharedTrackModel& track);
};

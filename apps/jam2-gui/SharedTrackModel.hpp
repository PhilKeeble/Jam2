#pragma once

#include <QString>

struct SharedTrackModel {
    QString fileName = QStringLiteral("No track loaded");
    QString filePath;
    qint64 fileBytes = 0;
    int sampleRate = 0;
    int durationMs = 0;
    QString sha256;
    double guessedBpm = 0.0;
    double acceptedBpm = 120.0;
    QString key = QStringLiteral("Unknown");
    double speed = 1.0;
    int pitchCents = 0;
    bool loopEnabled = false;
    double loopStartSeconds = -1.0;
    double loopEndSeconds = -1.0;
    bool syncControls = true;
    double focusFrequencyHz = 120.0;
    double focusGainDb = 0.0;
    double focusQ = 1.4;
    double highpassHz = 40.0;
    double lowpassHz = 400.0;
};

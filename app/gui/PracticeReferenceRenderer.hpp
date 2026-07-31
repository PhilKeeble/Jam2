#pragma once

#include "BeatGridModel.hpp"
#include "PracticeIdeaDialogs.hpp"

#include <QJsonArray>
#include <QString>

namespace jam2::practice {

struct ReferenceWav {
    QString path;
    QString sha256;
    qint64 frames = 0;
    float peak = 0.0f;
    int eventCount = 0;
    float rms = 0.0f;
    float preMakeupPeak = 0.0f;
    double makeupGainDb = 0.0;
    qint64 limitedSamples = 0;
};

struct ReferenceRenderResult {
    ReferenceWav chords;
    ReferenceWav drums;
    ReferenceWav melody;
    ReferenceWav bass;
    ReferenceWav support;
    QString sourceSignature;
    QString diagnostics;
    QString error;
};

QString practiceIdeaSignature(
    const SongSection* chordSection,
    const SongSection* beatSection);
QString practiceReferenceSignature(
    const SongSection* chordSection,
    const SongSection* beatSection,
    const ReferenceRenderSettings& settings);

ReferenceRenderResult renderPracticeReferences(
    const SongSection* chordSection,
    const SongSection* beatSection,
    const ReferenceRenderSettings& settings,
    const QString& workspaceFolder);

// Hard-data diagnostic using the same resolved voicings as the renderer.
QJsonArray practiceChordVoicingDiagnostics(
    const SongSection& chordSection,
    ChordVoicing voicing,
    QString& error);

} // namespace jam2::practice

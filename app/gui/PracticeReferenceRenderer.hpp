#pragma once

#include "BeatGridModel.hpp"
#include "PracticeIdeaDialogs.hpp"

#include <QString>

namespace jam2::practice {

struct ReferenceWav {
    QString path;
    QString sha256;
    qint64 frames = 0;
};

struct ReferenceRenderResult {
    ReferenceWav chords;
    ReferenceWav drums;
    ReferenceWav melody;
    QString sourceSignature;
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

} // namespace jam2::practice

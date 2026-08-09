#pragma once

#include "PracticeIdeaGenerator.hpp"

#include <QString>
#include <QVector>

#include <cstdint>

namespace jam2::practice {

struct CuratedIdeaEntry {
    QString id;
    QString name;
    QString styleId;
    QString styleName;
    QString profileId;
    QString profileName;
    QString formId;
    QString formName;
    QString previewResource;
    QString previewSha256;
    QString chordFingerprint;
    QString beatFingerprint;
    QString tonic;
    QString mode;
    QString meterId;
    int generatorVersion = 0;
    std::uint32_t seed = 0;
    int complexity = 4;
    int bpm = 120;
    int meterNumerator = 4;
    int meterDenominator = 4;
    int bars = 4;
    int previewBars = 4;

    ChordIdeaRequest generationRequest(int targetSectionIndex = 0) const;
};

QVector<CuratedIdeaEntry> loadCuratedIdeaCatalog(QString& error);

} // namespace jam2::practice

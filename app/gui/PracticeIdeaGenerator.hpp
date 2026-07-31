#pragma once

#include "BeatGridModel.hpp"

#include <QStringList>

#include <cstdint>

namespace jam2::practice {

struct ChordIdeaRequest {
    int key = -1;
    QString styleId;
    QString profileId;
    QString formId;
    QString meterId;
    QString productionFamilyId;
    QString modeId;
    int bars = 0;
    int beatsPerBar = 0;
    int harmonicComplexity = 2;
    int rhythmicComplexity = 2;
};

struct BeatIdeaRequest {
    QString styleId;
    QString profileId;
    QString formId;
    QString meterId;
    QString productionFamilyId;
    int bars = 0;
    int beatsPerBar = 0;
    int rhythmicComplexity = 2;
};

struct GeneratedPracticeIdea {
    SongSection chordSection;
    SongSection beatSection;
    int bpm = 120;
    int clickDivision = 1;
    int meterNumerator = 4;
    int meterDenominator = 4;
    int tempoPulseUnits = 1;
    QVector<int> beatGrouping{4};
    QVector<bool> clickEnabled;
    QVector<bool> clickAccents;
    GenerationRecipe recipe;
};

QStringList chordStyleNames();
QStringList beatStyleNames();
QStringList styleIds();
QStringList profileIds(const QString& styleId);
QStringList profileNames(const QString& styleId);
QStringList nativeFormIds(const QString& profileId);
QStringList nativeFormNames(const QString& profileId);
QStringList meterIds(const QString& profileId);
QStringList meterNames(const QString& profileId);
QStringList productionFamilyIds(const QString& profileId);
QStringList productionFamilyNames(const QString& profileId);
QStringList modeIds(const QString& profileId);
QStringList modeNames(const QString& profileId);
QStringList grooveFamilyIds(const QString& styleId);
QStringList grooveFamilyNames(const QString& styleId);
QString styleNameForId(const QString& id);
QStringList keyNames();
QString generatedChordFingerprint(const SongSection& section);
QString generatedBeatFingerprint(const SongSection& section);

GeneratedPracticeIdea generateCoupledPracticeIdea(const ChordIdeaRequest& request);

// Explicit seeds are intentionally test-only inputs. They are never stored in
// the song or exposed by the practice UI.
SongSection generateChordIdeaForTest(const ChordIdeaRequest& request, std::uint32_t seed);
SongSection generateBeatIdeaForTest(const BeatIdeaRequest& request, std::uint32_t seed);
GeneratedPracticeIdea generateCoupledPracticeIdeaForTest(
    const ChordIdeaRequest& request,
    std::uint32_t seed);

} // namespace jam2::practice

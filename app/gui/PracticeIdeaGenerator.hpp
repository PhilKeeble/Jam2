#pragma once

#include "BeatGridModel.hpp"

#include <QStringList>

#include <cstdint>

namespace jam2::practice {

struct ChordIdeaRequest {
    int key = -1;
    QString styleId;
    QString characterId;
    int bars = 16;
    int beatsPerBar = 4;
    int harmonicComplexity = 2;
    int rhythmicComplexity = 2;
};

struct BeatIdeaRequest {
    QString styleId;
    QString characterId;
    int bars = 16;
    int beatsPerBar = 4;
    int rhythmicComplexity = 2;
};

struct GeneratedPracticeIdea {
    SongSection chordSection;
    SongSection beatSection;
    int bpm = 120;
    int clickDivision = 1;
    QVector<bool> clickEnabled;
    QVector<bool> clickAccents;
    GenerationRecipe recipe;
};

QStringList chordStyleNames();
QStringList beatStyleNames();
QStringList styleIds();
QStringList grooveFamilyIds(const QString& styleId);
QStringList grooveFamilyNames(const QString& styleId);
QStringList characterNames();
QStringList characterIds();
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

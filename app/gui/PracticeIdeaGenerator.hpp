#pragma once

#include "BeatGridModel.hpp"

#include <QStringList>

#include <cstdint>

namespace jam2::practice {

enum class PracticeIdeaParts {
    FullArrangement = 0,
    PitchedPartsOnly = 1,
    DrumsOnly = 2,
};

struct ChordIdeaRequest {
    int key = -1;
    QString styleId;
    QString profileId;
    QString formId;
    QString meterId;
    QString productionFamilyId;
    QString modeId;
    PracticeIdeaParts parts = PracticeIdeaParts::FullArrangement;
    int targetSectionIndex = -1;
    bool allowMeterOverride = false;
    bool allowModeOverride = false;
    int bpm = 0;
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
    int bpm = 0;
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

struct ContinueIdeaRequest {
    int sourceSectionIndex = 0;
    int targetSectionIndex = 1;
    int bpm = 120;
    QString meterId = QStringLiteral("4-4");
    int beatsPerBar = 4;
    int beatUnit = 4;
    int tempoPulseUnits = 1;
};

struct ContinuationAnalysis {
    QString inferredTonic;
    QString inferredMode;
    double inferredTonalConfidence = 0.0;
    QString inferredProfileId;
    double inferredProfileConfidence = 0.0;
    QStringList alternativeProfileIds;
    QString relationshipId;
    QString continuationRoleId;
    QString continuationRoleName;
    QStringList evidence;
    double chordVocabularySimilarity = 0.0;
    double chordQualityVocabularySimilarity = 0.0;
    double chordOrderContrast = 0.0;
    double openingChordPositionSimilarity = 0.0;
    double drumSimilarity = 0.0;
    double melodyRhythmSimilarity = 0.0;
    double melodyContourSimilarity = 0.0;
    double bassContourSimilarity = 0.0;
    double boundaryVoiceLeading = 0.0;
    double harmonicDensityRetention = 0.0;
    int sourceChordEvents = 0;
    int continuationChordEvents = 0;
    QString harmonicPacingId;
    int candidateCount = 0;
};

struct GeneratedContinuationIdea {
    GeneratedPracticeIdea idea;
    ContinuationAnalysis analysis;
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
QStringList compatibleMeterIds(const QString& styleId, const QString& profileId);
QStringList compatibleMeterNames(const QString& styleId, const QString& profileId);
QVector<int> compatibleBarCounts(
    const QString& styleId,
    const QString& profileId,
    const QString& meterId);
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
GeneratedContinuationIdea generateContinuationPracticeIdea(
    const SongSection& source,
    const ContinueIdeaRequest& request);

// Explicit seeds are intentionally test-only inputs. They are never stored in
// the song or exposed by the practice UI.
SongSection generateChordIdeaForTest(const ChordIdeaRequest& request, std::uint32_t seed);
SongSection generateBeatIdeaForTest(const BeatIdeaRequest& request, std::uint32_t seed);
GeneratedPracticeIdea generateCoupledPracticeIdeaForTest(
    const ChordIdeaRequest& request,
    std::uint32_t seed);
GeneratedContinuationIdea generateContinuationPracticeIdeaForTest(
    const SongSection& source,
    const ContinueIdeaRequest& request,
    std::uint32_t seed);

} // namespace jam2::practice

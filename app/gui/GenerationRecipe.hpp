#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>

namespace jam2::practice {

struct HarmonicRecipeEvent {
    int beat = 0;
    int durationBeats = 1;
    QString roman;
    QString chord;
};

struct TheoryDecision {
    int beat = 0;
    QString kind;
    QString beforeChord;
    QString afterChord;
    QString analysis;
    QString resolutionTarget;
    QString explanation;
};

struct MelodyRecipeEvent {
    int tick = 0;
    int durationTicks = 1;
    int midi = 60;
    int velocity = 88;
    QString note;
    QString chord;
    QString chordRole;
    QString melodicRole;
};

struct MelodyPhraseRecipe {
    int startBar = 1;
    int endBar = 1;
    QString label;
    QString summary;
};

struct GenerationRecipe {
    int generatorVersion = 4;
    std::uint32_t seed = 0;
    QString styleId;
    QString styleName;
    QString moodId;
    QString moodName;
    QString tonic;
    QString mode;
    QStringList moodDecisions;
    int bpm = 120;
    int beatsPerBar = 4;
    int bars = 16;
    int complexity = 2;

    QString progressionId;
    QString progressionName;
    QVector<HarmonicRecipeEvent> baseHarmony;
    QStringList finalChordPlan;
    QVector<TheoryDecision> theoryDecisions;

    QString motifCell;
    QString motifRhythm;
    QString motifForm;
    QStringList motifTransformations;
    QVector<MelodyRecipeEvent> melodyEvents;
    QVector<MelodyPhraseRecipe> melodyPhrases;
    QString melodyRange;

    QString grooveId;
    QString grooveName;
    QString grooveCore;
    QString grooveFeelName;
    int swingPercent = 50;
    int snareOffsetMs = 0;
    int timingVariationMs = 0;
    int velocityVariationPercent = 0;
    int kickVariationCount = 0;
    int ghostVariationCount = 0;
    int cymbalVariationCount = 0;
    int fillCount = 0;
    int advancedCellCount = 0;
    QStringList grooveDecisions;

    QString chordPatchId;
    QString chordPatchName;
    QString melodyPatchId;
    QString melodyPatchName;
    QString drumPatchId;
    QString drumPatchName;
    QStringList patchModifiers;

    QString chordFingerprint;
    QString beatFingerprint;

    bool isValid() const;
};

QJsonObject generationRecipeToJson(const GenerationRecipe& recipe);
bool generationRecipeFromJson(const QJsonObject& object, GenerationRecipe& recipe);
QString generationRecipeDetails(const GenerationRecipe& recipe, bool contentChanged);

} // namespace jam2::practice

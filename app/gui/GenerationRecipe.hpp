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

struct RoleRecipeEvent {
    int tick = 0;
    int durationTicks = 1;
    int midi = 48;
    int velocity = 84;
    QString note;
    QString role;
    QString relationship;
    QString articulation;
};

struct DrumPerformanceEvent {
    int tick = 0;
    QString laneId;
    int velocity = 96;
    int offsetMs = 0;
    QString articulation;
    QString role;
    int repeatGroup = 0;
    bool fill = false;
};

struct DrumPhraseRecipe {
    int startBar = 1;
    int endBar = 1;
    QString label;
    QString formRole;
    int energy = 0;
    QString development;
    QString transition;
    QString fillId;
    int fillStartBeat = -1;
    int fillBeatCount = 0;
};

struct LaneTimingRecipe {
    QString laneId;
    QString anchorGrid;
    QString subdivisionMapping;
    int offsetMs = 0;
    int varianceMs = 0;
    QString velocityShape;
};

struct ComplexityToolRecipe {
    int level = 1;
    QString toolId;
    QString name;
    bool selected = false;
    QString explanation;
};

struct SynthVoiceRecipe {
    QString roleId;
    QString engine;
    QString oscillator;
    double attackMs = 4.0;
    double releaseMs = 120.0;
    double cutoffHz = 6000.0;
    double resonance = 0.1;
    double drive = 0.0;
    double detuneCents = 0.0;
    double noiseMix = 0.0;
    QStringList effects;
};

struct AutomationRecipeEvent {
    int startTick = 0;
    int endTick = 1;
    QString target;
    double startValue = 0.0;
    double endValue = 0.0;
    QString curve = QStringLiteral("linear");
    QString explanation;
};

struct FormSectionRecipe {
    QString label;
    int startBar = 1;
    int bars = 1;
    QString role;
    QString relationship;
};

struct GenerationRecipe {
    int generatorVersion = 7;
    std::uint32_t seed = 0;
    QString styleId;
    QString styleName;
    QString profileId;
    QString profileName;
    QString productionFamilyId;
    QString productionFamilyName;
    QString variationId;
    QString variationSummary;
    int variationDensity = 0;
    int variationRegister = 0;
    int variationArticulation = 0;
    int variationBrightness = 0;
    int variationSpace = 0;
    int variationTiming = 0;
    QString tonic;
    QString mode;
    QStringList variationDecisions;
    int bpm = 120;
    int beatsPerBar = 4;
    int bars = 16;
    int complexity = 2;
    QString meterId = QStringLiteral("4-4");
    int meterNumerator = 4;
    int meterDenominator = 4;
    int beatUnit = 4;
    int tempoPulseUnits = 1;
    QString tempoPulseName = QStringLiteral("quarter note");
    QVector<int> beatGrouping{4};
    QString subdivisionFamily = QStringLiteral("straight-eighth");
    QString perceivedTime = QStringLiteral("normal");
    int clickDivision = 1;
    QString formId;
    QString formName;
    int phraseBars = 4;
    QString formDescription;
    QVector<FormSectionRecipe> formSections;

    QString progressionId;
    QString progressionName;
    QString progressionFamilyId;
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
    QVector<RoleRecipeEvent> bassEvents;
    QVector<RoleRecipeEvent> supportingEvents;
    QString bassGrammar;
    QStringList supportingRoles;
    QStringList continuationStrategies;
    QStringList variationAxes;
    QVector<ComplexityToolRecipe> complexityTools;

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
    QVector<DrumPhraseRecipe> drumPhrases;
    QVector<DrumPerformanceEvent> drumEvents;

    QString chordPatchId;
    QString chordPatchName;
    QString melodyPatchId;
    QString melodyPatchName;
    QString bassPatchId;
    QString bassPatchName;
    QString supportPatchId;
    QString supportPatchName;
    QString drumPatchId;
    QString drumPatchName;
    int drumPatchRevision = 1;
    double drumMixGainDb = 3.0;
    QStringList patchModifiers;
    QVector<LaneTimingRecipe> laneTiming;
    QVector<SynthVoiceRecipe> synthVoices;
    QVector<AutomationRecipeEvent> automationEvents;

    QString teachingSummary;
    QString jamGuidance;

    QString chordFingerprint;
    QString beatFingerprint;

    bool isValid() const;
};

QJsonObject generationRecipeToJson(const GenerationRecipe& recipe);
bool generationRecipeFromJson(const QJsonObject& object, GenerationRecipe& recipe);
QString generationRecipeTeaching(const GenerationRecipe& recipe, bool contentChanged);
QString generationRecipeDetails(const GenerationRecipe& recipe, bool contentChanged);

} // namespace jam2::practice

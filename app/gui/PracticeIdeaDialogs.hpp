#pragma once

#include "PracticeIdeaGenerator.hpp"

#include <optional>

class QWidget;

namespace jam2::practice {

enum class ChordVoicing {
    StyleDefault,
    Close,
    Spread,
    VoiceLed,
};

enum class ReferenceDrumKit {
    StyleDefault,
    Acoustic,
    Electronic,
};

struct ReferenceRenderSettings {
    bool renderChords = true;
    bool renderDrums = true;
    bool renderMelody = false;
    bool renderBass = false;
    bool renderSupport = false;
    ChordVoicing voicing = ChordVoicing::StyleDefault;
    ReferenceDrumKit drumKit = ReferenceDrumKit::StyleDefault;
    int sampleRate = 48000;
    double bpm = 120.0;
    int meterNumerator = 4;
    int meterDenominator = 4;
    int tempoPulseUnits = 1;
    double chordLevel = 0.32;
    double drumLevel = 0.65;
    double melodyLevel = 0.30;
    double bassLevel = 0.42;
    double supportLevel = 0.24;
    double attackMs = 8.0;
    double releaseMs = 100.0;
};

struct PracticeIdeaDialogDefaults {
    int bpm = 80;
    QString meterId;
    int bars = 8;
    PracticeIdeaParts parts = PracticeIdeaParts::FullArrangement;
    int key = -1;
    QString styleId;
    QString profileId;
    QString preferredMeterId;
    int preferredBars = 0;
    bool exactBpm = false;
    int complexity = 2;
    int targetSectionIndex = 0;
    QVector<int> bankBpms;
    QStringList bankMeterIds;
    QVector<int> bankBars;
};

struct ContinueIdeaDialogDefaults {
    int sourceSectionIndex = 0;
    int targetSectionIndex = 1;
    QVector<bool> bankHasContent;
    QStringList bankNames;
};

std::optional<ChordIdeaRequest> askForPracticeIdea(
    QWidget* parent,
    const PracticeIdeaDialogDefaults& defaults);
std::optional<ContinueIdeaRequest> askForIdeaContinuation(
    QWidget* parent,
    const ContinueIdeaDialogDefaults& defaults);
std::optional<ReferenceRenderSettings> askForReferenceRender(
    QWidget* parent,
    ReferenceRenderSettings defaults,
    int chordBeats,
    int beatBeats,
    int melodyBeats,
    int bassBeats,
    int supportBeats,
    int sectionCount = 1);
void showIdeaDetails(QWidget* parent, const GenerationRecipe& recipe, bool contentChanged);

} // namespace jam2::practice

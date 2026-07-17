#pragma once

#include "PracticeIdeaGenerator.hpp"

#include <optional>

class QWidget;

namespace jam2::practice {

enum class ChordVoicing {
    Close,
    Spread,
    VoiceLed,
};

struct ReferenceRenderSettings {
    bool renderChords = true;
    bool renderDrums = true;
    bool renderMelody = false;
    ChordVoicing voicing = ChordVoicing::VoiceLed;
    int sampleRate = 48000;
    double bpm = 120.0;
    double chordLevel = 0.32;
    double drumLevel = 0.65;
    double melodyLevel = 0.30;
    double attackMs = 8.0;
    double releaseMs = 100.0;
};

std::optional<ChordIdeaRequest> askForPracticeIdea(QWidget* parent, int beatsPerBar);
std::optional<ReferenceRenderSettings> askForReferenceRender(
    QWidget* parent,
    ReferenceRenderSettings defaults,
    int chordBeats,
    int beatBeats,
    int melodyBeats);

} // namespace jam2::practice

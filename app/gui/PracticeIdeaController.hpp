#pragma once

#include "BeatGridModel.hpp"
#include "GeneratedDrumMixPolicy.hpp"
#include "LooperProject.hpp"
#include "PracticeIdeaGenerator.hpp"
#include "PracticeReferenceRenderer.hpp"

#include <optional>

namespace jam2::practice {

struct ReferenceLayerAvailability {
    bool chords = false;
    bool drums = false;
    bool melody = false;
    bool bass = false;
    bool support = false;

    bool any() const noexcept { return chords || drums || melody || bass || support; }
};

class PracticeIdeaController final {
public:
    static std::optional<GeneratedPracticeIdea> generateCoupled(
        BeatGridModel& chordModel,
        BeatGridModel& beatModel,
        const ChordIdeaRequest& request);
    static std::optional<GeneratedPracticeIdea> applyCoupled(
        BeatGridModel& chordModel,
        BeatGridModel& beatModel,
        GeneratedPracticeIdea idea,
        PracticeIdeaParts parts,
        int targetSectionIndex,
        bool matchIdeaLength);
    static SongSection fitRepeatingDrums(SongSection source, int targetBeats);
    static std::optional<GeneratedContinuationIdea> generateContinuation(
        BeatGridModel& model,
        const ContinueIdeaRequest& request);
    static std::optional<SongSection> generatedSection(
        const BeatGridModel& model,
        const QString& kind);
    static ReferenceLayerAvailability referenceLayers(const SongSection& section);
    static void clearReferences(LooperProject& project);
    static void clearReferences(LooperProject& project, int bankIndex);
    static bool applyReferences(
        LooperProject& project,
        int bankIndex,
        const ReferenceRenderSettings& settings,
        const ReferenceRenderResult& result,
        QString& error,
        const QString& lanePrefix = {});
};

} // namespace jam2::practice

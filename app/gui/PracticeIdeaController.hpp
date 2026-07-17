#pragma once

#include "BeatGridModel.hpp"
#include "LooperProject.hpp"
#include "PracticeIdeaGenerator.hpp"
#include "PracticeReferenceRenderer.hpp"

#include <optional>

namespace jam2::practice {

class PracticeIdeaController final {
public:
    static std::optional<GeneratedPracticeIdea> generateCoupled(
        BeatGridModel& chordModel,
        BeatGridModel& beatModel,
        const ChordIdeaRequest& request);
    static std::optional<SongSection> generatedSection(
        const BeatGridModel& model,
        const QString& kind);
    static void clearReferences(LooperProject& project);
    static bool applyReferences(
        LooperProject& project,
        int bankIndex,
        const ReferenceRenderSettings& settings,
        const ReferenceRenderResult& result,
        QString& error);
};

} // namespace jam2::practice

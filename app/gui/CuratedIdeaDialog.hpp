#pragma once

#include "CuratedIdeaCatalog.hpp"

#include <QWidget>

#include <functional>
#include <optional>

namespace jam2::practice {

enum class CuratedIdeaTimingPolicy {
    KeepSectionTiming = 0,
    UseIdeaTiming = 1,
};

struct CuratedIdeaDialogDefaults {
    int targetSectionIndex = 0;
    CuratedIdeaTimingPolicy timing = CuratedIdeaTimingPolicy::UseIdeaTiming;
    int importBars = 0;
    QVector<int> bankBpms;
    QVector<int> bankMeterNumerators;
    QVector<int> bankMeterDenominators;
    QVector<int> bankBeats;
};

struct CuratedIdeaSelection {
    CuratedIdeaEntry idea;
    CuratedIdeaTimingPolicy timing = CuratedIdeaTimingPolicy::UseIdeaTiming;
    // Zero fits the repeating groove to the current section. Positive values
    // import that many bars from the start of the 32-bar performance.
    int importBars = 32;
    int targetSectionIndex = 0;
};

struct CuratedIdeaPreviewCallbacks {
    bool available = false;
    QString unavailableReason;
    std::function<bool(const CuratedIdeaEntry&, QString&)> play;
    std::function<void()> stop;
};

std::optional<CuratedIdeaSelection> askForCuratedIdea(
    QWidget* parent,
    const CuratedIdeaDialogDefaults& defaults,
    const CuratedIdeaPreviewCallbacks& preview);

} // namespace jam2::practice

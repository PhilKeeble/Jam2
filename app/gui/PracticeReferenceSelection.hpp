#pragma once

#include <QString>

namespace jam2::practice {

struct ReferenceRenderSettings;

struct ReferenceLayerAvailability {
    bool chords = false;
    bool drums = false;
    bool melody = false;
    bool bass = false;
    bool support = false;

    bool any() const noexcept { return chords || drums || melody || bass || support; }
    bool selected(const ReferenceRenderSettings& settings) const noexcept;
};

struct ReferenceRenderSection {
    int sectionIndex = 0;
    QString bankId;
    ReferenceLayerAvailability layers;
};

} // namespace jam2::practice

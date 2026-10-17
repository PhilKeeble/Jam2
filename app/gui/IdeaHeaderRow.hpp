#pragma once

#include "ActionTileRow.hpp"
#include <array>

class BeatGridModel;
struct SongSection;
class QPushButton;

namespace jam2::gui {
const SongSection* ideaDetailsSection(const BeatGridModel& chords, const BeatGridModel& beats, int section);

class IdeaHeaderRow final : public ActionTileRow {
public:
    enum class Action { Generate, Continue, Details, Wav, Browse, JamTaster };
    explicit IdeaHeaderRow(QWidget* parent = nullptr, bool includeDetails = true);
    QPushButton* button(Action action) const noexcept { return buttons_[static_cast<std::size_t>(action)]; }
    void updateSection(const BeatGridModel& chords, const BeatGridModel& beats, int section);
private:
    std::array<QPushButton*, 6> buttons_{};
};
} // namespace jam2::gui

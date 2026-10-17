#include "IdeaHeaderRow.hpp"
#include "BeatGridModel.hpp"

#include <QPushButton>

namespace jam2::gui {
namespace {
int styleIdeaHeaderAction(QPushButton* button, IdeaHeaderRow::Action action)
{
    if (!button) return 0;
    QString outline;
    QString fill;
    QString hover;
    int width = 116;
    switch (action) {
    case IdeaHeaderRow::Action::Generate:
        outline = QStringLiteral("#e2ac53");
        fill = QStringLiteral("rgba(35,27,20,232)");
        hover = QStringLiteral("#302317");
        width = 154;
        break;
    case IdeaHeaderRow::Action::JamTaster:
    case IdeaHeaderRow::Action::Browse:
        outline = QStringLiteral("#9fb56f");
        fill = QStringLiteral("rgba(28,36,21,232)");
        hover = QStringLiteral("#26331c");
        width = 132;
        break;
    case IdeaHeaderRow::Action::Continue:
        outline = QStringLiteral("#66d4cf");
        fill = QStringLiteral("rgba(13,42,43,232)");
        hover = QStringLiteral("#123638");
        width = 124;
        break;
    case IdeaHeaderRow::Action::Wav:
        outline = QStringLiteral("#b08be4");
        fill = QStringLiteral("rgba(27,20,37,232)");
        hover = QStringLiteral("#261b34");
        width = 116;
        break;
    case IdeaHeaderRow::Action::Details:
        outline = QStringLiteral("#56a4f4");
        fill = QStringLiteral("rgba(17,31,50,232)");
        hover = QStringLiteral("#172c46");
        width = 112;
        break;
    }
    button->setFont(jam2::gui::actionTileFont());
    button->setStyleSheet(QStringLiteral(
        "QPushButton { color:#f3e6cf;background:%1;border:1px solid %2;border-radius:4px;"
        "padding:0 9px; }"
        "QPushButton:hover { background:%3;border-color:%2; }"
        "QPushButton:pressed { background:#241c20;border-color:#ffd68e; }"
        "QPushButton:disabled { color:#737b83;background:#191c20;border-color:#454b52; }")
        .arg(fill, outline, hover));
    return width;
}

struct ActionDescription { IdeaHeaderRow::Action action; const char* label; const char* tooltip; };
constexpr ActionDescription descriptions[]{
    {IdeaHeaderRow::Action::Generate, "GENERATE NEW IDEA", "Generate a new procedural idea to populate the chord and beat views"},
    {IdeaHeaderRow::Action::Continue, "CONTINUE IDEA", "Pick a source section and generate a related idea in another section to spark new ideas"},
    {IdeaHeaderRow::Action::Details, "IDEA DETAILS", "Explore how this section's generated idea works, including its chords, groove and musical parts"},
    {IdeaHeaderRow::Action::Wav, "GENERATE WAV", "Render the selected sections and musical parts into WAV backing tracks"},
    {IdeaHeaderRow::Action::Browse, "GROOVE LIBRARY", "Browse and preview drum grooves, then import one into the active section"},
    {IdeaHeaderRow::Action::JamTaster, "JAMTASTER", "Analyse a WAV to detect tempo, separate stems and extract chords, beats, bass and song structure"},
};
}
const SongSection* ideaDetailsSection(const BeatGridModel& chords, const BeatGridModel& beats, int section) {
    if (section >= 0 && section < chords.sections().size() && chords.section(section).generatedRecipe.isValid())
        return &chords.section(section);
    if (section >= 0 && section < beats.sections().size() && beats.section(section).generatedRecipe.isValid())
        return &beats.section(section);
    return nullptr;
}
IdeaHeaderRow::IdeaHeaderRow(QWidget* parent, bool includeDetails) : ActionTileRow(parent) {
    for (const auto& description : descriptions) {
        if (description.action == Action::Details && !includeDetails) continue;
        auto* control = new QPushButton(QString::fromLatin1(description.label), this);
        buttons_[static_cast<std::size_t>(description.action)] = control;
        addButton(control, styleIdeaHeaderAction(control, description.action));
        control->setToolTip(QString::fromLatin1(description.tooltip));
    }
    if (auto* details = button(Action::Details)) {
        details->setEnabled(false);
        details->setToolTip(QStringLiteral("Generate an idea in this section to view its details"));
    }
}
void IdeaHeaderRow::updateSection(const BeatGridModel& chords, const BeatGridModel& beats, int section) {
    auto* details = button(Action::Details);
    if (!details) return;
    const bool available = ideaDetailsSection(chords, beats, section) != nullptr;
    details->setEnabled(available);
    details->setToolTip(available ? QString::fromLatin1(descriptions[static_cast<std::size_t>(Action::Details)].tooltip)
        : QStringLiteral("Generate an idea in this section to view its details"));
}
} // namespace jam2::gui

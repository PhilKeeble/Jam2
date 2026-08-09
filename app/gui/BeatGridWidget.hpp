#pragma once

#include "BeatGridModel.hpp"

#include <QWidget>
#include <QVector>

#include <functional>

class QResizeEvent;
class QPushButton;
class QLabel;
class QComboBox;
class QScrollArea;
class QToolButton;
class QVBoxLayout;

namespace jam2::gui {

inline double beatOverviewHitPhase(
    int beatWithinBar,
    int step,
    int division,
    int beatsPerBar) noexcept
{
    division = qMax(1, division);
    beatsPerBar = qMax(1, beatsPerBar);
    const double beat = qMax(0, beatWithinBar) +
        static_cast<double>(qBound(0, step, division - 1)) / division;
    return qBound(0.0, beat / beatsPerBar, 1.0);
}

struct ChordDetailGroup {
    int start = 0;
    int count = 1;
};

inline ChordDetailGroup chordDetailGroupForWidths(
    const QVector<int>& barWidths,
    int selectedBar,
    int availableWidth,
    int maximumBars = 4,
    int spacing = 8) noexcept
{
    if (barWidths.isEmpty()) return {};
    const int barCount = static_cast<int>(barWidths.size());
    selectedBar = qBound(0, selectedBar, barCount - 1);
    availableWidth = qMax(1, availableWidth);
    maximumBars = qMax(1, maximumBars);
    int start = 0;
    while (start < barCount) {
        int count = 1;
        int used = qMax(1, barWidths.at(start));
        while (count < maximumBars && start + count < barCount) {
            const int next = qMax(1, barWidths.at(start + count));
            if (used + spacing + next > availableWidth) break;
            used += spacing + next;
            ++count;
        }
        if (selectedBar < start + count) return {start, count};
        start += count;
    }
    return {barCount - 1, 1};
}

}

class BeatGridWidget : public QWidget {
public:
    BeatGridWidget(BeatGridModel* model, const QString& lane, QWidget* parent = nullptr);

    BeatGridModel& model();
    QWidget* createOverviewPagination(QWidget* parent);
    int selectedSectionIndex() const;
    void setSelectedSectionIndex(int section);
    void refresh();
    void focusGeneratedSection(const QString& kind);
    void setBeatsPerBar(int beatsPerBar);
    void setGridPosition(quint64 absoluteBeat, int subdivision, bool running, double beatPhase = 0.0);
    void setUpcomingSection(
        int section,
        quint64 beatsRemaining,
        int countdownBeatsPerBar,
        int targetBeatsPerBar,
        bool arrangementActive,
        bool arrangementArmed);
    void setEditingProtected(bool protectedState);
    void applyRemoteCell(int section, const QString& lane, int beat, const QString& text);

    std::function<void(int, const QString&, int, const QString&, int)> onCellEdited;
    std::function<void(int, int, int, const QString&, int)> onBeatHitEdited;
    std::function<void(int, int, int, int)> onBeatDivisionChanged;
    std::function<void(int, int, int, int)> onMusicalDivisionChanged;
    std::function<void(int, int, int, const QString&, const QString&, int)> onMusicalStepEdited;
    std::function<void(int, int, int)> onGridResized;
    std::function<void(int)> onShrinkRequested;
    std::function<void()> onStructureChanged;
    std::function<void(int)> onSelectedSectionChanged;

private:
    enum class Mode {
        Chord,
        Beat,
        Lyrics,
    };

    Mode mode() const;
    void rebuildChordReference();
    void rebuildAuthoringView();
    void rebuildChordCards();
    void rebuildBeatSequencer();
    void rebuildLyricRows();
    QWidget* buildAuthoringOverview(bool chords);
    void selectFocusedChordBar(int bar, int chordBeat = -1);
    void expandCurrent();
    void updateActionButtons();
    void updateUpcomingPreview();
    void emitStructureChanged();
    void resizeEvent(QResizeEvent* event) override;
    QVector<int> chordDetailBarWidths(const SongSection& section) const;
    int chordDetailAvailableWidth() const;
    jam2::gui::ChordDetailGroup currentChordDetailGroup(
        const SongSection& section) const;
    void scheduleResponsiveChordRebuild();
    void setOverviewPage(int page, bool pinToPage);
    void updateOverviewPagination();

    BeatGridModel ownedModel_;
    BeatGridModel* model_ = nullptr;
    QString fixedLane_;
    QScrollArea* authoringScroll_ = nullptr;
    QWidget* authoringContent_ = nullptr;
    QVBoxLayout* authoringLayout_ = nullptr;
    QWidget* chordReferenceCanvas_ = nullptr;
    QComboBox* guitarStringCountBox_ = nullptr;
    QComboBox* guitarTuningBox_ = nullptr;
    QLabel* upcomingPreview_ = nullptr;
    QPushButton* duplicateButton_ = nullptr;
    QPushButton* deleteButton_ = nullptr;
    QPushButton* expandButton_ = nullptr;
    QPushButton* shrinkButton_ = nullptr;
    QWidget* overviewPagination_ = nullptr;
    QLabel* overviewPageLabel_ = nullptr;
    QToolButton* overviewFirstButton_ = nullptr;
    QToolButton* overviewPreviousButton_ = nullptr;
    QToolButton* overviewNextButton_ = nullptr;
    QToolButton* overviewLastButton_ = nullptr;
    int selectedSection_ = -1;
    int selectedBar_ = 0;
    int selectedChordBeat_ = 0;
    int authoringLiveBar_ = -1;
    int authoringLiveBeat_ = -1;
    int guitarStringCount_ = 6;
    bool guitarDropTuning_ = false;
    bool chordReferenceVisible_ = true;
    bool musicalLinesVisible_ = true;
    quint64 gridBeat_ = 0;
    int gridSubdivision_ = 0;
    double gridBeatPhase_ = 0.0;
    bool gridRunning_ = false;
    int beatsPerBar_ = 4;
    int upcomingSection_ = -1;
    quint64 upcomingBeatsRemaining_ = 0;
    int upcomingCountdownBeatsPerBar_ = 4;
    int upcomingTargetBeatsPerBar_ = 4;
    bool upcomingArrangementActive_ = false;
    bool upcomingArrangementArmed_ = false;
    int visibleChordGroupStart_ = 0;
    int visibleChordGroupCount_ = 1;
    int overviewPage_ = 0;
    bool overviewPagePinned_ = false;
    bool responsiveChordRebuildPending_ = false;
    QWidget* editingBlocker_ = nullptr;
};

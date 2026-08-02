#pragma once

#include "BeatGridModel.hpp"

#include <QWidget>

#include <functional>

class QPushButton;
class QLabel;
class QComboBox;
class QScrollArea;
class QVBoxLayout;

class BeatGridWidget : public QWidget {
public:
    BeatGridWidget(BeatGridModel* model, const QString& lane, QWidget* parent = nullptr);

    BeatGridModel& model();
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
    void applyRemoteCell(int section, const QString& lane, int beat, const QString& text);

    std::function<void(int, const QString&, int, const QString&, int)> onCellEdited;
    std::function<void(int, int, int, const QString&, int)> onBeatHitEdited;
    std::function<void(int, int, int, int)> onBeatDivisionChanged;
    std::function<void(int, int, int, int)> onMusicalDivisionChanged;
    std::function<void(int, int, int, const QString&, const QString&, int)> onMusicalStepEdited;
    std::function<void(int, int, int)> onGridResized;
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
    void shrinkCurrent();
    void updateActionButtons();
    void updateUpcomingPreview();
    void emitStructureChanged();

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
};

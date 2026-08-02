#pragma once

#include "BeatGridModel.hpp"

#include <QComboBox>
#include <QLineEdit>
#include <QTableWidget>
#include <QWidget>

#include <functional>

class QPushButton;
class QHeaderView;
class QLabel;

class BeatGridWidget : public QWidget {
public:
    explicit BeatGridWidget(QWidget* parent = nullptr);
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

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    enum class Mode {
        Chord,
        Beat,
        Lyrics,
        Legacy,
    };

    Mode mode() const;
    QString currentLane() const;
    void rebuildSectionList();
    void rebuildTable();
    void rebuildChordTable();
    void rebuildBeatTable();
    void rebuildLyricsBox();
    void expandCurrent();
    void shrinkCurrent();
    int sectionForRow(int row) const;
    int laneForRow(int row) const;
    void selectSection(int section);
    void updateSectionSelectionMarkers();
    void updateActionButtons();
    void updateUpcomingPreview();
    void emitStructureChanged();

    BeatGridModel ownedModel_;
    BeatGridModel* model_ = nullptr;
    QString fixedLane_;
    QComboBox* sectionBox_ = nullptr;
    QComboBox* laneBox_ = nullptr;
    QLineEdit* labelEdit_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QTableWidget* table_ = nullptr;
    QLabel* upcomingPreview_ = nullptr;
    QHeaderView* beatHeader_ = nullptr;
    QPushButton* duplicateButton_ = nullptr;
    QPushButton* deleteButton_ = nullptr;
    QPushButton* expandButton_ = nullptr;
    QPushButton* shrinkButton_ = nullptr;
    QVector<int> rowToSection_;
    QVector<int> rowToLane_;
    int selectedSection_ = -1;
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
    bool updating_ = false;
};

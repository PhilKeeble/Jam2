#pragma once

#include "BeatGridModel.hpp"

#include <QComboBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QWidget>

#include <functional>

class QPushButton;

class BeatGridWidget : public QWidget {
public:
    explicit BeatGridWidget(QWidget* parent = nullptr);
    BeatGridWidget(BeatGridModel* model, const QString& lane, QWidget* parent = nullptr);

    BeatGridModel& model();
    void refresh();
    void applyRemoteCell(int section, const QString& lane, int beat, const QString& text);

    std::function<void(int, const QString&, int, const QString&, int)> onCellEdited;
    std::function<void(int, int, int, const QString&, int)> onBeatHitEdited;
    std::function<void(int, int, int, int)> onBeatDivisionChanged;
    std::function<void(const QString&, int)> onLyricsEdited;
    std::function<void(int, int, int)> onGridResized;
    std::function<void()> onStructureChanged;

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
    int selectedSectionIndex() const;
    int sectionForRow(int row) const;
    int laneForRow(int row) const;
    void selectSection(int section);
    void updateActionButtons();
    void emitStructureChanged();

    BeatGridModel ownedModel_;
    BeatGridModel* model_ = nullptr;
    QString fixedLane_;
    QComboBox* sectionBox_ = nullptr;
    QComboBox* laneBox_ = nullptr;
    QLineEdit* labelEdit_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QTableWidget* table_ = nullptr;
    QPlainTextEdit* lyricsEdit_ = nullptr;
    QPushButton* duplicateButton_ = nullptr;
    QPushButton* deleteButton_ = nullptr;
    QPushButton* expandButton_ = nullptr;
    QPushButton* shrinkButton_ = nullptr;
    QVector<int> rowToSection_;
    QVector<int> rowToLane_;
    int selectedSection_ = -1;
    bool updating_ = false;
};

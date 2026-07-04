#pragma once

#include "BeatGridModel.hpp"

#include <QComboBox>
#include <QLineEdit>
#include <QTableWidget>
#include <QWidget>

#include <functional>

class BeatGridWidget : public QWidget {
public:
    explicit BeatGridWidget(QWidget* parent = nullptr);
    BeatGridWidget(BeatGridModel* model, const QString& lane, QWidget* parent = nullptr);

    BeatGridModel& model();
    void refresh();
    void applyRemoteCell(int section, const QString& lane, int beat, const QString& text);

    std::function<void(int, const QString&, int, const QString&, int)> onCellEdited;
    std::function<void(int, int, int)> onGridResized;
    std::function<void()> onStructureChanged;

private:
    QString currentLane() const;
    void rebuildSectionList();
    void rebuildTable();
    void expandCurrent();
    void shrinkCurrent();

    BeatGridModel ownedModel_;
    BeatGridModel* model_ = nullptr;
    QString fixedLane_;
    QComboBox* sectionBox_ = nullptr;
    QComboBox* laneBox_ = nullptr;
    QLineEdit* labelEdit_ = nullptr;
    QLineEdit* nameEdit_ = nullptr;
    QTableWidget* table_ = nullptr;
    bool updating_ = false;
};

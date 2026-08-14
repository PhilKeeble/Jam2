#pragma once

#include "LooperProject.hpp"

#include <QDialog>

class QCheckBox;
class QComboBox;
class QEvent;
class QSpinBox;
class QTableWidget;
class QWidget;

class ArrangementEditorDialog final : public QDialog {
public:
    enum class Action {
        Save,
        Start,
        Stop,
    };

    struct Result {
        ArrangementDefinition definition;
        Action action = Action::Save;
    };

    ArrangementEditorDialog(
        const ArrangementDefinition& definition,
        int bankCount,
        bool arrangementActive,
        QWidget* parent = nullptr);

    Result result() const;

private:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void appendRow(ArrangementStep step);
    void removeSelectedRow();
    void moveSelectedRow(int direction);
    void rebuildRows(const QVector<ArrangementStep>& steps, int selectedRow);
    void refreshRowControlIds();
    void selectEditorRow(const QWidget* editor);
    ArrangementStep stepAt(int row) const;
    QComboBox* bankEditorAt(int row) const;
    QSpinBox* repeatsEditorAt(int row) const;

    QTableWidget* table_ = nullptr;
    QCheckBox* loop_ = nullptr;
    int bankCount_ = 1;
    bool arrangementActive_ = false;
    Action action_ = Action::Save;
};

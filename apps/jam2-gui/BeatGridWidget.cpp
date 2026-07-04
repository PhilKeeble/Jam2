#include "BeatGridWidget.hpp"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QVBoxLayout>

BeatGridWidget::BeatGridWidget(QWidget* parent)
    : BeatGridWidget(nullptr, QString(), parent)
{
}

BeatGridWidget::BeatGridWidget(BeatGridModel* model, const QString& lane, QWidget* parent)
    : QWidget(parent)
    , model_(model ? model : &ownedModel_)
    , fixedLane_(lane)
{
    sectionBox_ = new QComboBox(this);
    laneBox_ = new QComboBox(this);
    laneBox_->addItem(QStringLiteral("Chords"), QStringLiteral("chord"));
    laneBox_->addItem(QStringLiteral("Beat Notes"), QStringLiteral("beat"));
    laneBox_->addItem(QStringLiteral("Lyrics"), QStringLiteral("lyric"));
    if (!fixedLane_.isEmpty()) {
        const int index = laneBox_->findData(fixedLane_);
        if (index >= 0) {
            laneBox_->setCurrentIndex(index);
        }
        laneBox_->setVisible(false);
    }
    labelEdit_ = new QLineEdit(this);
    nameEdit_ = new QLineEdit(this);
    table_ = new QTableWidget(this);
    table_->setAlternatingRowColors(true);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table_->verticalHeader()->setVisible(false);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);

    auto* addButton = new QPushButton(QStringLiteral("Add"), this);
    auto* duplicateButton = new QPushButton(QStringLiteral("Duplicate"), this);
    auto* deleteButton = new QPushButton(QStringLiteral("Delete"), this);
    auto* expandButton = new QPushButton(QStringLiteral("+4 Beats"), this);
    auto* shrinkButton = new QPushButton(QStringLiteral("-4 Beats"), this);

    auto* top = new QHBoxLayout();
    top->addWidget(sectionBox_);
    top->addWidget(laneBox_);
    top->addWidget(labelEdit_);
    top->addWidget(nameEdit_, 1);
    top->addWidget(addButton);
    top->addWidget(duplicateButton);
    top->addWidget(deleteButton);
    top->addWidget(expandButton);
    top->addWidget(shrinkButton);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(table_, 1);

    QObject::connect(sectionBox_, &QComboBox::currentIndexChanged, this, [this] { rebuildTable(); });
    QObject::connect(laneBox_, &QComboBox::currentIndexChanged, this, [this] { rebuildTable(); });
    QObject::connect(addButton, &QPushButton::clicked, this, [this] {
        model_->addSection();
        refresh();
        if (onStructureChanged) {
            onStructureChanged();
        }
    });
    QObject::connect(duplicateButton, &QPushButton::clicked, this, [this] {
        model_->duplicateSection(sectionBox_->currentIndex());
        refresh();
        if (onStructureChanged) {
            onStructureChanged();
        }
    });
    QObject::connect(deleteButton, &QPushButton::clicked, this, [this] {
        model_->deleteSection(sectionBox_->currentIndex());
        refresh();
        if (onStructureChanged) {
            onStructureChanged();
        }
    });
    QObject::connect(expandButton, &QPushButton::clicked, this, [this] { expandCurrent(); });
    QObject::connect(shrinkButton, &QPushButton::clicked, this, [this] { shrinkCurrent(); });
    QObject::connect(labelEdit_, &QLineEdit::editingFinished, this, [this] {
        model_->renameSection(sectionBox_->currentIndex(), labelEdit_->text(), nameEdit_->text());
        rebuildSectionList();
        if (onStructureChanged) {
            onStructureChanged();
        }
    });
    QObject::connect(nameEdit_, &QLineEdit::editingFinished, this, [this] {
        model_->renameSection(sectionBox_->currentIndex(), labelEdit_->text(), nameEdit_->text());
        rebuildSectionList();
        if (onStructureChanged) {
            onStructureChanged();
        }
    });
    QObject::connect(table_, &QTableWidget::cellChanged, this, [this](int, int column) {
        if (updating_) {
            return;
        }
        const int section = sectionBox_->currentIndex();
        const QString lane = currentLane();
        const QString text = table_->item(0, column) ? table_->item(0, column)->text() : QString();
        model_->setCell(section, lane, column, text);
        if (onCellEdited) {
            onCellEdited(section, lane, column, text, model_->revision());
        }
    });

    refresh();
}

BeatGridModel& BeatGridWidget::model()
{
    return *model_;
}

void BeatGridWidget::refresh()
{
    rebuildSectionList();
    rebuildTable();
}

void BeatGridWidget::applyRemoteCell(int section, const QString& lane, int beat, const QString& text)
{
    model_->setCell(section, lane, beat, text);
    rebuildTable();
}

QString BeatGridWidget::currentLane() const
{
    return fixedLane_.isEmpty() ? laneBox_->currentData().toString() : fixedLane_;
}

void BeatGridWidget::rebuildSectionList()
{
    const int current = qMax(0, sectionBox_->currentIndex());
    updating_ = true;
    sectionBox_->clear();
    for (const SongSection& section : model_->sections()) {
        sectionBox_->addItem(section.name + QStringLiteral(" ") + section.label);
    }
    sectionBox_->setCurrentIndex(qMin(current, sectionBox_->count() - 1));
    updating_ = false;
}

void BeatGridWidget::rebuildTable()
{
    if (model_->sections().isEmpty()) {
        return;
    }
    const int sectionIndex = qMax(0, sectionBox_->currentIndex());
    const SongSection& section = model_->section(sectionIndex);
    const QString lane = currentLane();
    labelEdit_->setText(section.label);
    nameEdit_->setText(section.name);

    updating_ = true;
    table_->clear();
    table_->setRowCount(1);
    table_->setColumnCount(section.beats);
    QStringList headers;
    for (int i = 0; i < section.beats; ++i) {
        headers << QStringLiteral("Beat %1").arg(i + 1);
        QString text;
        if (lane == QStringLiteral("chord")) {
            text = section.chords[i];
        } else if (lane == QStringLiteral("beat")) {
            text = section.beatNotes[i];
        } else {
            text = section.lyrics[i];
        }
        table_->setItem(0, i, new QTableWidgetItem(text));
    }
    table_->setHorizontalHeaderLabels(headers);
    table_->setVerticalHeaderLabels(QStringList{laneBox_->currentText()});
    updating_ = false;
}

void BeatGridWidget::expandCurrent()
{
    const int section = sectionBox_->currentIndex();
    const int beats = model_->section(section).beats + 4;
    model_->resizeSection(section, beats);
    refresh();
    if (onGridResized) {
        onGridResized(section, beats, model_->revision());
    }
}

void BeatGridWidget::shrinkCurrent()
{
    const int section = sectionBox_->currentIndex();
    const int beats = qMax(4, model_->section(section).beats - 4);
    model_->resizeSection(section, beats);
    refresh();
    if (onGridResized) {
        onGridResized(section, beats, model_->revision());
    }
}

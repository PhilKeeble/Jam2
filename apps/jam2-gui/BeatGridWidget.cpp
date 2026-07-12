#include "BeatGridWidget.hpp"

#include <QAction>
#include <QAbstractItemView>
#include <QBrush>
#include <QColor>
#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <limits>

namespace {

constexpr int kBeatCellKindRole = Qt::UserRole + 1;
constexpr int kBeatDivisionRole = Qt::UserRole + 2;
constexpr int kBeatHitTextRole = Qt::UserRole + 3;

enum class BeatCellKind {
    None = 0,
    Division = 1,
    Hit = 2,
};

class GridHeaderView final : public QHeaderView {
public:
    explicit GridHeaderView(Qt::Orientation orientation, QWidget* parent = nullptr)
        : QHeaderView(orientation, parent)
    {
        setMinimumHeight(28);
    }

    void setCurrentBeatColumn(int column)
    {
        if (currentBeatColumn_ == column) {
            return;
        }
        currentBeatColumn_ = column;
        viewport()->update();
    }

protected:
    void paintSection(QPainter* painter, const QRect& rect, int logicalIndex) const override
    {
        QHeaderView::paintSection(painter, rect, logicalIndex);
        if (logicalIndex != currentBeatColumn_) {
            return;
        }
        painter->save();
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(102, 198, 166));
        painter->drawEllipse(QPoint(rect.center().x(), rect.top() + 6), 4, 4);
        painter->restore();
    }

private:
    int currentBeatColumn_ = -1;
};

QString sectionTitle(const SongSection& section)
{
    return section.name.trimmed().isEmpty() ? QStringLiteral("Section") : section.name.trimmed();
}

QString normalizedHitText(const QString& text, int division)
{
    QString out;
    out.reserve(division);
    const QString trimmed = text.trimmed();
    for (int i = 0; i < division; ++i) {
        const QChar value = i < trimmed.size() ? trimmed[i] : QChar('.');
        out.append(value == QChar('x') || value == QChar('X') || value == QChar('1') ? QChar('x') : QChar('.'));
    }
    return out;
}

QString withHitState(const QString& text, int division, int index, bool checked)
{
    QString out = normalizedHitText(text, division);
    if (index >= 0 && index < out.size()) {
        out[index] = checked ? QChar('x') : QChar('.');
    }
    return out;
}

bool hitChecked(const QString& text, int division, int index)
{
    const QString out = normalizedHitText(text, division);
    return index >= 0 && index < out.size() && out[index] == QChar('x');
}

int visualSlotCount(int division)
{
    return division == 3 || division == 6 ? 6 : 8;
}

QVector<int> activeVisualSlots(int division)
{
    switch (division) {
    case 1:
        return QVector<int>{0};
    case 2:
        return QVector<int>{0, 4};
    case 3:
        return QVector<int>{0, 2, 4};
    case 6:
        return QVector<int>{0, 1, 2, 3, 4, 5};
    case 8:
        return QVector<int>{0, 1, 2, 3, 4, 5, 6, 7};
    case 4:
    default:
        return QVector<int>{0, 2, 4, 6};
    }
}

class BeatGridDelegate : public QStyledItemDelegate {
public:
    explicit BeatGridDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent)
    {
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        const auto kind = static_cast<BeatCellKind>(index.data(kBeatCellKindRole).toInt());
        if (kind == BeatCellKind::Hit) {
            paintHitCell(painter, option, index);
            return;
        }
        if (kind == BeatCellKind::Division) {
            paintDivisionCell(painter, option, index);
            return;
        }
        QStyledItemDelegate::paint(painter, option, index);
    }

private:
    void paintHitCell(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
    {
        painter->save();
        const QRect rect = option.rect.adjusted(4, 4, -4, -4);
        painter->fillRect(option.rect, option.state & QStyle::State_Selected ? QColor(48, 82, 112) : QColor(27, 31, 34));
        const int division = index.data(kBeatDivisionRole).toInt();
        const QString text = index.data(kBeatHitTextRole).toString();
        const QVector<int> activeSlots = activeVisualSlots(division);
        const int slotCount = visualSlotCount(division);
        const int slotWidth = qMax(1, rect.width() / qMax(1, slotCount));
        const int boxSize = qMin(14, qMax(8, rect.height() - 10));
        for (int slot = 0; slot < slotCount; ++slot) {
            const int step = activeSlots.indexOf(slot);
            const int x = rect.left() + slot * slotWidth + (slotWidth - boxSize) / 2;
            const int y = rect.top() + (rect.height() - boxSize) / 2;
            const QRect box(x, y, boxSize, boxSize);
            if (step < 0) {
                painter->fillRect(box.adjusted(2, 2, -2, -2), QColor(42, 47, 51));
                continue;
            }
            const bool checked = hitChecked(text, division, step);
            painter->setPen(checked ? QColor(102, 198, 166) : QColor(82, 97, 108));
            painter->setBrush(checked ? QColor(102, 198, 166) : QColor(31, 36, 40));
            painter->drawRect(box);
        }
        painter->restore();
    }

    void paintDivisionCell(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
    {
        painter->save();
        painter->fillRect(option.rect, option.state & QStyle::State_Selected ? QColor(48, 82, 112) : QColor(27, 31, 34));
        const QRect box = option.rect.adjusted(8, 6, -8, -6);
        painter->setPen(QColor(82, 97, 108));
        painter->setBrush(QColor(31, 36, 40));
        painter->drawRect(box);
        painter->setPen(QColor(220, 224, 226));
        painter->drawText(box.adjusted(8, 0, -20, 0), Qt::AlignVCenter | Qt::AlignLeft, index.data(Qt::DisplayRole).toString());
        const int midY = box.center().y();
        const int right = box.right() - 10;
        QPoint points[3] = {QPoint(right - 4, midY - 2), QPoint(right + 4, midY - 2), QPoint(right, midY + 3)};
        painter->setBrush(QColor(150, 158, 166));
        painter->setPen(Qt::NoPen);
        painter->drawPolygon(points, 3);
        painter->restore();
    }
};

}

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
    lyricsEdit_ = new QPlainTextEdit(this);

    table_->setAlternatingRowColors(false);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    beatHeader_ = new GridHeaderView(Qt::Horizontal, table_);
    table_->setHorizontalHeader(beatHeader_);
    beatHeader_->setSectionResizeMode(QHeaderView::Stretch);
    table_->verticalHeader()->setVisible(true);
    table_->setStyleSheet(QStringLiteral(
        "QTableWidget::item { padding: 3px 6px; }"
        "QTableWidget QLineEdit { padding: 1px 4px; min-height: 24px; }"));
    table_->setItemDelegate(new BeatGridDelegate(table_));
    table_->viewport()->installEventFilter(this);

    auto* addButton = new QPushButton(QStringLiteral("Add Section"), this);
    duplicateButton_ = new QPushButton(QStringLiteral("Duplicate"), this);
    deleteButton_ = new QPushButton(QStringLiteral("Delete"), this);
    expandButton_ = new QPushButton(QStringLiteral("+4 Beats"), this);
    shrinkButton_ = new QPushButton(QStringLiteral("-4 Beats"), this);

    auto* top = new QHBoxLayout();
    top->addWidget(sectionBox_);
    top->addWidget(laneBox_);
    top->addWidget(labelEdit_);
    top->addWidget(nameEdit_, 1);
    top->addWidget(addButton);
    top->addWidget(duplicateButton_);
    top->addWidget(deleteButton_);
    top->addWidget(expandButton_);
    top->addWidget(shrinkButton_);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(top);
    layout->addWidget(table_, 1);
    layout->addWidget(lyricsEdit_, 1);

    const Mode currentMode = mode();
    const bool lyricsMode = currentMode == Mode::Lyrics;
    const bool chordMode = currentMode == Mode::Chord;
    sectionBox_->setVisible(currentMode == Mode::Legacy);
    labelEdit_->setVisible(currentMode == Mode::Legacy);
    nameEdit_->setVisible(currentMode == Mode::Legacy);
    table_->setVisible(!lyricsMode);
    lyricsEdit_->setVisible(lyricsMode);
    addButton->setVisible(!lyricsMode);
    duplicateButton_->setVisible(!lyricsMode);
    deleteButton_->setVisible(!lyricsMode);
    expandButton_->setVisible(!lyricsMode);
    shrinkButton_->setVisible(!lyricsMode);
    if (chordMode) {
        sectionBox_->setVisible(false);
        nameEdit_->setVisible(false);
    }

    QObject::connect(sectionBox_, &QComboBox::currentIndexChanged, this, [this] { rebuildTable(); });
    QObject::connect(laneBox_, &QComboBox::currentIndexChanged, this, [this] { rebuildTable(); });
    QObject::connect(addButton, &QPushButton::clicked, this, [this] {
        const int selected = selectedSectionIndex();
        const int beats = selected >= 0 && selected < model_->sections().size() ? model_->section(selected).beats : 8;
        model_->addSection(beats);
        selectedSection_ = model_->sections().size() - 1;
        refresh();
        emitStructureChanged();
    });
    QObject::connect(duplicateButton_, &QPushButton::clicked, this, [this] {
        const int selected = selectedSectionIndex();
        if (selected < 0) {
            return;
        }
        model_->duplicateSection(selected);
        selectedSection_ = selected + 1;
        refresh();
        emitStructureChanged();
    });
    QObject::connect(deleteButton_, &QPushButton::clicked, this, [this] {
        const int selected = selectedSectionIndex();
        if (selected < 0) {
            return;
        }
        model_->deleteSection(selected);
        selectedSection_ = -1;
        refresh();
        emitStructureChanged();
    });
    QObject::connect(expandButton_, &QPushButton::clicked, this, [this] { expandCurrent(); });
    QObject::connect(shrinkButton_, &QPushButton::clicked, this, [this] { shrinkCurrent(); });
    QObject::connect(labelEdit_, &QLineEdit::editingFinished, this, [this] {
        const int section = selectedSectionIndex();
        if (section >= 0 && section < model_->sections().size()) {
            model_->renameSection(section, labelEdit_->text(), nameEdit_->text());
            rebuildSectionList();
            emitStructureChanged();
        }
    });
    QObject::connect(nameEdit_, &QLineEdit::editingFinished, this, [this] {
        const int section = selectedSectionIndex();
        if (section >= 0 && section < model_->sections().size()) {
            model_->renameSection(section, model_->section(section).label, nameEdit_->text());
            rebuildSectionList();
            emitStructureChanged();
        }
    });
    QObject::connect(table_, &QTableWidget::cellChanged, this, [this](int row, int column) {
        if (updating_) {
            return;
        }
        const Mode currentMode = mode();
        const QString text = table_->item(row, column) ? table_->item(row, column)->text() : QString();
        if (currentMode == Mode::Chord) {
            if (row < 0 || row >= model_->sections().size()) {
                return;
            }
            selectSection(row);
            if (column == 0) {
                model_->renameSection(row, model_->section(row).label, text);
                emitStructureChanged();
                return;
            }
            model_->setCell(row, QStringLiteral("chord"), column - 1, text);
            if (onCellEdited) {
                onCellEdited(row, QStringLiteral("chord"), column - 1, text, model_->revision());
            }
            return;
        }
        if (currentMode == Mode::Beat) {
            const int section = sectionForRow(row);
            const int lane = laneForRow(row);
            if (section < 0) {
                return;
            }
            selectSection(section);
            if (lane < 0) {
                const auto kind = table_->item(row, column)
                    ? static_cast<BeatCellKind>(table_->item(row, column)->data(kBeatCellKindRole).toInt())
                    : BeatCellKind::None;
                if (kind == BeatCellKind::Division) {
                    const int division = table_->item(row, column)->data(kBeatDivisionRole).toInt();
                    model_->setBeatDivision(section, column, division);
                    if (onBeatDivisionChanged) {
                        onBeatDivisionChanged(section, column, division, model_->revision());
                    }
                    rebuildTable();
                    return;
                }
                if (column == 0) {
                    model_->renameSection(section, model_->section(section).label, text);
                    emitStructureChanged();
                }
                return;
            }
            model_->setBeatHit(section, column, lane, text);
            if (onBeatHitEdited) {
                onBeatHitEdited(section, column, lane, text, model_->revision());
            }
            return;
        }
        const int section = selectedSectionIndex();
        const QString lane = currentLane();
        model_->setCell(section, lane, column, text);
        if (onCellEdited) {
            onCellEdited(section, lane, column, text, model_->revision());
        }
    });
    QObject::connect(table_, &QTableWidget::cellClicked, this, [this](int row, int) {
        selectSection(sectionForRow(row));
    });
    QObject::connect(lyricsEdit_, &QPlainTextEdit::textChanged, this, [this] {
        if (updating_ || mode() != Mode::Lyrics) {
            return;
        }
        const QString text = lyricsEdit_->toPlainText();
        model_->setLyricsText(text);
        if (onLyricsEdited) {
            onLyricsEdited(text, model_->revision());
        }
    });

    refresh();
}

BeatGridModel& BeatGridWidget::model()
{
    return *model_;
}

bool BeatGridWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::Wheel) {
        return false;
    }
    if (watched == table_->viewport() && mode() == Mode::Beat && event->type() == QEvent::MouseButtonPress) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() != Qt::LeftButton) {
            return QWidget::eventFilter(watched, event);
        }
        const QModelIndex index = table_->indexAt(mouse->pos());
        if (!index.isValid()) {
            return QWidget::eventFilter(watched, event);
        }
        QTableWidgetItem* item = table_->item(index.row(), index.column());
        if (!item || !(item->flags() & Qt::ItemIsEnabled)) {
            return QWidget::eventFilter(watched, event);
        }
        const auto kind = static_cast<BeatCellKind>(item->data(kBeatCellKindRole).toInt());
        if (kind == BeatCellKind::Division) {
            const int section = sectionForRow(index.row());
            const int beat = index.column();
            if (section < 0 || section >= model_->sections().size() || beat < 0 || beat >= model_->section(section).beats) {
                return true;
            }
            selectSection(section);
            QMenu menu(this);
            for (int value : BeatGridModel::beatDivisionValues()) {
                QAction* action = menu.addAction(BeatGridModel::beatDivisionLabel(value));
                action->setData(value);
                action->setCheckable(true);
                action->setChecked(value == model_->section(section).beatPatterns[beat].division);
            }
            QAction* selected = menu.exec(table_->viewport()->mapToGlobal(mouse->pos()));
            if (!selected) {
                return true;
            }
            const int division = selected->data().toInt();
            model_->setBeatDivision(section, beat, division);
            if (onBeatDivisionChanged) {
                onBeatDivisionChanged(section, beat, division, model_->revision());
            }
            rebuildTable();
            return true;
        }
        if (kind != BeatCellKind::Hit) {
            return QWidget::eventFilter(watched, event);
        }

        const int section = sectionForRow(index.row());
        const int lane = laneForRow(index.row());
        const int beat = index.column();
        if (section < 0 || lane < 0 || section >= model_->sections().size() || beat < 0 || beat >= model_->section(section).beats) {
            return true;
        }

        const int division = model_->section(section).beatPatterns[beat].division;
        const QVector<int> activeSlots = activeVisualSlots(division);
        const int slotCount = visualSlotCount(division);
        const QRect rect = table_->visualRect(index).adjusted(4, 4, -4, -4);
        const int slotWidth = qMax(1, rect.width() / qMax(1, slotCount));
        const int visualSlot = qBound(0, (mouse->pos().x() - rect.left()) / slotWidth, slotCount - 1);
        const int step = activeSlots.indexOf(visualSlot);
        if (step < 0) {
            return true;
        }

        const QString current = model_->section(section).beatPatterns[beat].lanes[lane];
        const QString updated = withHitState(current, division, step, !hitChecked(current, division, step));
        model_->setBeatHit(section, beat, lane, updated);
        {
            QSignalBlocker tableBlocker(table_);
            item->setData(kBeatHitTextRole, updated);
        }
        selectSection(section);
        table_->viewport()->update(table_->visualRect(index));
        if (onBeatHitEdited) {
            onBeatHitEdited(section, beat, lane, updated, model_->revision());
        }
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void BeatGridWidget::refresh()
{
    if (selectedSection_ >= model_->sections().size()) {
        selectedSection_ = -1;
    }
    rebuildSectionList();
    rebuildTable();
    updateActionButtons();
    const quint64 beat = gridBeat_;
    const int subdivision = gridSubdivision_;
    const bool running = gridRunning_;
    gridBeat_ = (std::numeric_limits<quint64>::max)();
    setGridPosition(beat, subdivision, running);
}

void BeatGridWidget::setGridPosition(quint64 absoluteBeat, int subdivision, bool running)
{
    gridBeat_ = absoluteBeat;
    gridSubdivision_ = subdivision;
    gridRunning_ = running;
    if (table_ == nullptr) {
        return;
    }
    auto* header = static_cast<GridHeaderView*>(beatHeader_);
    if (header == nullptr || !running) {
        if (header != nullptr) {
            header->setCurrentBeatColumn(-1);
        }
        return;
    }
    const int activeSection = selectedSection_ >= 0 && selectedSection_ < model_->sections().size()
        ? selectedSection_ : 0;
    const int beatsInSection = activeSection < model_->sections().size()
        ? model_->section(activeSection).beats : 0;
    if (beatsInSection <= 0) {
        header->setCurrentBeatColumn(-1);
        return;
    }
    const int sectionBeat = static_cast<int>(absoluteBeat % static_cast<quint64>(beatsInSection));
    const int beatColumn = mode() == Mode::Chord ? sectionBeat + 1 : sectionBeat;
    header->setCurrentBeatColumn(beatColumn >= 0 && beatColumn < table_->columnCount() ? beatColumn : -1);
}

void BeatGridWidget::applyRemoteCell(int section, const QString& lane, int beat, const QString& text)
{
    model_->setCell(section, lane, beat, text);
    rebuildTable();
}

BeatGridWidget::Mode BeatGridWidget::mode() const
{
    if (fixedLane_ == QStringLiteral("chord")) {
        return Mode::Chord;
    }
    if (fixedLane_ == QStringLiteral("beat")) {
        return Mode::Beat;
    }
    if (fixedLane_ == QStringLiteral("lyric")) {
        return Mode::Lyrics;
    }
    return Mode::Legacy;
}

QString BeatGridWidget::currentLane() const
{
    return fixedLane_.isEmpty() ? laneBox_->currentData().toString() : fixedLane_;
}

void BeatGridWidget::rebuildSectionList()
{
    if (mode() == Mode::Chord || mode() == Mode::Beat || mode() == Mode::Lyrics) {
        return;
    }
    const int current = qMax(0, sectionBox_->currentIndex());
    updating_ = true;
    sectionBox_->clear();
    for (const SongSection& section : model_->sections()) {
        sectionBox_->addItem(sectionTitle(section));
    }
    sectionBox_->setCurrentIndex(qMin(current, sectionBox_->count() - 1));
    updating_ = false;
}

void BeatGridWidget::rebuildTable()
{
    rowToSection_.clear();
    rowToLane_.clear();
    switch (mode()) {
    case Mode::Chord:
        rebuildChordTable();
        return;
    case Mode::Beat:
        rebuildBeatTable();
        return;
    case Mode::Lyrics:
        rebuildLyricsBox();
        return;
    case Mode::Legacy:
        break;
    }

    if (model_->sections().isEmpty()) {
        return;
    }
    const int sectionIndex = selectedSectionIndex();
    const SongSection& section = model_->section(sectionIndex);
    const QString lane = currentLane();
    labelEdit_->setText(section.label);
    nameEdit_->setText(section.name);

    updating_ = true;
    QSignalBlocker tableBlocker(table_);
    table_->setUpdatesEnabled(false);
    table_->clear();
    table_->setRowCount(1);
    table_->setColumnCount(section.beats);
    QStringList headers;
    for (int i = 0; i < section.beats; ++i) {
        headers << QStringLiteral("%1").arg(i + 1);
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
    table_->setUpdatesEnabled(true);
    updating_ = false;
}

void BeatGridWidget::rebuildChordTable()
{
    updating_ = true;
    QSignalBlocker tableBlocker(table_);
    table_->setUpdatesEnabled(false);
    table_->setRowCount(0);
    table_->setColumnCount(0);
    table_->clear();
    table_->clearSpans();
    table_->verticalHeader()->setVisible(false);
    table_->setRowCount(model_->sections().size());
    rowToSection_.fill(-1, model_->sections().size());
    rowToLane_.fill(-1, model_->sections().size());
    int maxBeats = 4;
    for (const SongSection& section : model_->sections()) {
        maxBeats = qMax(maxBeats, section.beats);
    }
    table_->setColumnCount(maxBeats + 1);
    QStringList headers{QStringLiteral("Section")};
    for (int beat = 0; beat < maxBeats; ++beat) {
        headers << QStringLiteral("%1").arg(beat + 1);
    }
    table_->setHorizontalHeaderLabels(headers);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    for (int row = 0; row < model_->sections().size(); ++row) {
        const SongSection& section = model_->section(row);
        rowToSection_[row] = row;
        table_->setItem(row, 0, new QTableWidgetItem(sectionTitle(section)));
        table_->setRowHeight(row, 40);
        for (int beat = 0; beat < maxBeats; ++beat) {
            auto* item = new QTableWidgetItem(beat < section.beats ? section.chords[beat] : QString());
            if (beat >= section.beats) {
                Qt::ItemFlags flags = item->flags();
                flags &= ~Qt::ItemIsEditable;
                flags &= ~Qt::ItemIsEnabled;
                item->setFlags(flags);
                item->setBackground(QBrush(QColor(34, 36, 38)));
            }
            table_->setItem(row, beat + 1, item);
        }
    }
    if (selectedSection_ >= 0 && selectedSection_ < model_->sections().size()) {
        table_->selectRow(selectedSection_);
    } else {
        table_->clearSelection();
        table_->setCurrentItem(nullptr);
    }
    table_->setUpdatesEnabled(true);
    updating_ = false;
}

void BeatGridWidget::rebuildBeatTable()
{
    const QStringList lanes = BeatGridModel::beatLaneNames();
    QVector<int> visualLaneIndices;
    visualLaneIndices.reserve(lanes.size());
    for (int lane = lanes.size() - 1; lane >= 0; --lane) {
        visualLaneIndices.push_back(lane);
    }
    updating_ = true;
    QSignalBlocker tableBlocker(table_);
    table_->setUpdatesEnabled(false);
    table_->setRowCount(0);
    table_->setColumnCount(0);
    table_->clear();
    table_->clearSpans();
    table_->verticalHeader()->setVisible(true);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    table_->horizontalHeader()->setDefaultSectionSize(156);

    int maxBeats = 4;
    for (const SongSection& section : model_->sections()) {
        maxBeats = qMax(maxBeats, section.beats);
    }
    const int rowsPerSection = lanes.size() + 2;
    const int rowCount = model_->sections().size() * rowsPerSection;
    table_->setRowCount(rowCount);
    table_->setColumnCount(maxBeats);
    rowToSection_.fill(-1, rowCount);
    rowToLane_.fill(-1, rowCount);

    QStringList headers;
    for (int beat = 0; beat < maxBeats; ++beat) {
        headers << QStringLiteral("%1").arg(beat + 1);
    }
    table_->setHorizontalHeaderLabels(headers);

    QStringList rowLabels;
    rowLabels.reserve(rowCount);
    for (int sectionIndex = 0; sectionIndex < model_->sections().size(); ++sectionIndex) {
        rowLabels << QString();
        rowLabels << QStringLiteral("Division");
        for (int lane : visualLaneIndices) {
            rowLabels << lanes[lane];
        }
    }
    table_->setVerticalHeaderLabels(rowLabels);

    int row = 0;
    for (int sectionIndex = 0; sectionIndex < model_->sections().size(); ++sectionIndex) {
        const SongSection& section = model_->section(sectionIndex);
        rowToSection_[row] = sectionIndex;
        auto* header = new QTableWidgetItem(sectionTitle(section));
        Qt::ItemFlags headerFlags = header->flags();
        headerFlags |= Qt::ItemIsSelectable;
        headerFlags |= Qt::ItemIsEditable;
        header->setFlags(headerFlags);
        header->setBackground(QBrush(selectedSection_ == sectionIndex ? QColor(48, 82, 112) : QColor(34, 38, 42)));
        table_->setItem(row, 0, header);
        if (maxBeats > 1) {
            table_->setSpan(row, 0, 1, maxBeats);
        }
        table_->setRowHeight(row, 42);
        ++row;

        const int divisionRow = row;
        rowToSection_[divisionRow] = sectionIndex;
        rowToLane_[divisionRow] = -1;
        table_->setRowHeight(divisionRow, 42);
        for (int beat = 0; beat < maxBeats; ++beat) {
            auto* item = new QTableWidgetItem();
            if (beat >= section.beats) {
                Qt::ItemFlags flags = item->flags();
                flags &= ~Qt::ItemIsEditable;
                flags &= ~Qt::ItemIsEnabled;
                item->setFlags(flags);
                item->setBackground(QBrush(QColor(34, 36, 38)));
                table_->setItem(divisionRow, beat, item);
                continue;
            }
            const int division = section.beatPatterns[beat].division;
            item->setData(kBeatCellKindRole, static_cast<int>(BeatCellKind::Division));
            item->setData(kBeatDivisionRole, division);
            item->setText(BeatGridModel::beatDivisionLabel(division));
            Qt::ItemFlags flags = item->flags();
            flags &= ~Qt::ItemIsEditable;
            item->setFlags(flags);
            table_->setItem(divisionRow, beat, item);
        }
        ++row;

        for (int lane : visualLaneIndices) {
            const int laneRow = row;
            rowToSection_[laneRow] = sectionIndex;
            rowToLane_[laneRow] = lane;
            table_->setRowHeight(laneRow, 38);
            for (int beat = 0; beat < maxBeats; ++beat) {
                auto* item = new QTableWidgetItem();
                if (beat >= section.beats) {
                    Qt::ItemFlags flags = item->flags();
                    flags &= ~Qt::ItemIsEditable;
                    flags &= ~Qt::ItemIsEnabled;
                    item->setFlags(flags);
                    item->setBackground(QBrush(QColor(34, 36, 38)));
                    table_->setItem(laneRow, beat, item);
                    continue;
                }
                const int division = section.beatPatterns[beat].division;
                item->setData(kBeatCellKindRole, static_cast<int>(BeatCellKind::Hit));
                item->setData(kBeatDivisionRole, division);
                item->setData(kBeatHitTextRole, section.beatPatterns[beat].lanes[lane]);
                Qt::ItemFlags flags = item->flags();
                flags &= ~Qt::ItemIsEditable;
                item->setFlags(flags);
                table_->setItem(laneRow, beat, item);
            }
            ++row;
        }
    }
    if (selectedSection_ >= 0 && selectedSection_ < model_->sections().size()) {
        for (int candidate = 0; candidate < rowToSection_.size(); ++candidate) {
            if (rowToSection_[candidate] == selectedSection_) {
                table_->selectRow(candidate);
                break;
            }
        }
    } else {
        table_->clearSelection();
        table_->setCurrentItem(nullptr);
    }
    table_->setUpdatesEnabled(true);
    updating_ = false;
}

void BeatGridWidget::rebuildLyricsBox()
{
    updating_ = true;
    QSignalBlocker blocker(lyricsEdit_);
    lyricsEdit_->setPlainText(model_->lyricsText());
    updating_ = false;
}

void BeatGridWidget::expandCurrent()
{
    if (model_->sections().isEmpty()) {
        return;
    }
    if (mode() == Mode::Chord || mode() == Mode::Beat) {
        int beats = 4;
        for (const SongSection& section : model_->sections()) {
            beats = qMax(beats, section.beats);
        }
        beats += 4;
        model_->resizeAllSections(beats);
        refresh();
        emitStructureChanged();
        return;
    }
    const int section = selectedSectionIndex();
    if (section < 0 || section >= model_->sections().size()) {
        return;
    }
    const int beats = model_->section(section).beats + 4;
    model_->resizeSection(section, beats);
    refresh();
    if (onGridResized) {
        onGridResized(section, beats, model_->revision());
    }
}

void BeatGridWidget::shrinkCurrent()
{
    if (model_->sections().isEmpty()) {
        return;
    }
    if (mode() == Mode::Chord || mode() == Mode::Beat) {
        int beats = 4;
        for (const SongSection& section : model_->sections()) {
            beats = qMax(beats, section.beats);
        }
        beats = qMax(4, beats - 4);
        model_->resizeAllSections(beats);
        refresh();
        emitStructureChanged();
        return;
    }
    const int section = selectedSectionIndex();
    if (section < 0 || section >= model_->sections().size()) {
        return;
    }
    const int beats = qMax(4, model_->section(section).beats - 4);
    model_->resizeSection(section, beats);
    refresh();
    if (onGridResized) {
        onGridResized(section, beats, model_->revision());
    }
}

int BeatGridWidget::selectedSectionIndex() const
{
    if (model_->sections().isEmpty()) {
        return -1;
    }
    if (mode() == Mode::Chord || mode() == Mode::Beat) {
        return selectedSection_ >= 0 && selectedSection_ < model_->sections().size() ? selectedSection_ : -1;
    }
    return qBound(0, sectionBox_->currentIndex(), model_->sections().size() - 1);
}

int BeatGridWidget::sectionForRow(int row) const
{
    if (row >= 0 && row < rowToSection_.size()) {
        return rowToSection_[row];
    }
    if (mode() == Mode::Chord && row >= 0 && row < model_->sections().size()) {
        return row;
    }
    return -1;
}

int BeatGridWidget::laneForRow(int row) const
{
    return row >= 0 && row < rowToLane_.size() ? rowToLane_[row] : -1;
}

void BeatGridWidget::selectSection(int section)
{
    if (section < 0 || section >= model_->sections().size()) {
        selectedSection_ = -1;
        if (!updating_) {
            table_->clearSelection();
            table_->setCurrentItem(nullptr);
        }
    } else {
        selectedSection_ = section;
        if (!updating_) {
            for (int row = 0; row < rowToSection_.size(); ++row) {
                if (rowToSection_[row] == section) {
                    table_->selectRow(row);
                    break;
                }
            }
        }
    }
    updateActionButtons();
}

void BeatGridWidget::updateActionButtons()
{
    const bool selected = mode() == Mode::Legacy
        ? sectionBox_->currentIndex() >= 0 && sectionBox_->currentIndex() < model_->sections().size()
        : selectedSection_ >= 0 && selectedSection_ < model_->sections().size();
    const bool canUseSection = mode() != Mode::Lyrics && selected;
    if (duplicateButton_) {
        duplicateButton_->setEnabled(canUseSection);
    }
    if (deleteButton_) {
        deleteButton_->setEnabled(canUseSection);
    }
}

void BeatGridWidget::emitStructureChanged()
{
    if (onStructureChanged) {
        onStructureChanged();
    }
}

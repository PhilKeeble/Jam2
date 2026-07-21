#include "BeatGridWidget.hpp"
#include "GuiTheme.hpp"
#include "MusicTheory.hpp"

#include <QAction>
#include <QAbstractItemView>
#include <QBrush>
#include <QColor>
#include <QEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <cmath>
#include <limits>

namespace {

namespace theme = jam2::gui::theme;

constexpr int kBeatCellKindRole = Qt::UserRole + 1;
constexpr int kBeatDivisionRole = Qt::UserRole + 2;
constexpr int kBeatHitTextRole = Qt::UserRole + 3;
constexpr int kBeatActiveStepRole = Qt::UserRole + 4;
constexpr int kSectionHeaderRole = Qt::UserRole + 5;
constexpr int kSectionSelectedRole = Qt::UserRole + 6;
constexpr int kMusicalStepsRole = Qt::UserRole + 7;

enum class BeatCellKind {
    None = 0,
    Division = 1,
    Hit = 2,
    MusicalSteps = 3,
};

QString musicalStepText(const MusicalStep& step)
{
    if (step.state == MusicalStepState::Onset) return step.value;
    if (step.state == MusicalStepState::Hold) return QStringLiteral("~");
    return QStringLiteral("-");
}

class GridHeaderView final : public QHeaderView {
public:
    explicit GridHeaderView(Qt::Orientation orientation, QWidget* parent = nullptr)
        : QHeaderView(orientation, parent)
    {
        setMinimumHeight(28);
    }

    void setCurrentBeatColumn(int column, double phase = 0.0)
    {
        const double boundedPhase = qBound(0.0, phase, 0.999999);
        if (currentBeatColumn_ == column && qFuzzyCompare(currentBeatPhase_, boundedPhase)) {
            return;
        }
        currentBeatColumn_ = column;
        currentBeatPhase_ = boundedPhase;
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
        painter->setBrush(theme::playhead);
        const int x = rect.left() + qBound(
            0, static_cast<int>(std::lround(currentBeatPhase_ * rect.width())), rect.width() - 1);
        painter->drawEllipse(QPoint(x, rect.top() + 6), 4, 4);
        painter->restore();
    }

private:
    int currentBeatColumn_ = -1;
    double currentBeatPhase_ = 0.0;
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
        const QChar lower = value.toLower();
        out.append(lower == QLatin1Char('a') ? QLatin1Char('a')
            : lower == QLatin1Char('g') ? QLatin1Char('g')
            : lower == QLatin1Char('x') || value == QLatin1Char('1') ? QLatin1Char('x')
            : QLatin1Char('.'));
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
    return index >= 0 && index < out.size() && out[index] != QLatin1Char('.');
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
        QStyleOptionViewItem neutralOption(option);
        neutralOption.state &= ~QStyle::State_Selected;
        const auto kind = static_cast<BeatCellKind>(index.data(kBeatCellKindRole).toInt());
        if (kind == BeatCellKind::Hit) {
            paintHitCell(painter, neutralOption, index);
            return;
        }
        if (kind == BeatCellKind::Division) {
            paintDivisionCell(painter, neutralOption, index);
            return;
        }
        if (kind == BeatCellKind::MusicalSteps) {
            paintMusicalCell(painter, neutralOption, index);
            return;
        }
        QStyledItemDelegate::paint(painter, neutralOption, index);
        if (index.data(kSectionHeaderRole).toBool() &&
            index.data(kSectionSelectedRole).toBool()) {
            painter->save();
            painter->setPen(Qt::NoPen);
            painter->setBrush(theme::playhead);
            painter->drawEllipse(QPoint(option.rect.left() + 9, option.rect.top() + 9), 4, 4);
            painter->restore();
        }
    }

private:
    void paintMusicalCell(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
    {
        painter->save();
        painter->fillRect(option.rect, theme::panelRaised);
        const QStringList steps = index.data(kMusicalStepsRole).toStringList();
        const int count = qMax(1, steps.size());
        const int activeStep = index.data(kBeatActiveStepRole).toInt();
        for (int step = 0; step < count; ++step) {
            QRect box(
                option.rect.left() + step * option.rect.width() / count,
                option.rect.top(),
                qMax(1, (step + 1) * option.rect.width() / count - step * option.rect.width() / count),
                option.rect.height());
            if (step == activeStep) painter->fillRect(box.adjusted(1, 1, -1, -1), theme::selection);
            painter->setPen(theme::borderStrong);
            painter->drawRect(box.adjusted(0, 0, -1, -1));
            painter->setPen(steps.value(step) == QStringLiteral("-") ? theme::textMuted : theme::text);
            painter->drawText(
                box.adjusted(4, 2, -4, -2),
                Qt::AlignCenter,
                painter->fontMetrics().elidedText(steps.value(step), Qt::ElideRight, qMax(1, box.width() - 8)));
        }
        painter->restore();
    }

    void paintHitCell(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
    {
        painter->save();
        const QRect rect = option.rect.adjusted(4, 4, -4, -4);
        painter->fillRect(option.rect, option.state & QStyle::State_Selected ? theme::selection : theme::panelRaised);
        const int division = index.data(kBeatDivisionRole).toInt();
        const QString text = index.data(kBeatHitTextRole).toString();
        const QVariant activeStepValue = index.data(kBeatActiveStepRole);
        const int activeStep = activeStepValue.isValid() ? activeStepValue.toInt() : -1;
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
                painter->fillRect(box.adjusted(2, 2, -2, -2), theme::buttonHover);
                continue;
            }
            const QChar state = normalizedHitText(text, division).at(step);
            const bool checked = state != QLatin1Char('.');
            const QColor color = state == QLatin1Char('a') ? theme::nebulaBlue
                : state == QLatin1Char('g') ? theme::nebulaPurple
                : QColor(232, 92, 101);
            painter->setPen(QPen(checked ? color : theme::borderStrong, step == activeStep ? 3 : 1));
            painter->setBrush(checked ? QBrush(color) : QBrush(theme::buttonBg));
            if (state == QLatin1Char('g')) painter->drawEllipse(box);
            else painter->drawRect(box);
        }
        painter->restore();
    }

    void paintDivisionCell(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const
    {
        painter->save();
        painter->fillRect(option.rect, option.state & QStyle::State_Selected ? theme::selection : theme::panelRaised);
        const QRect box = option.rect.adjusted(8, 6, -8, -6);
        painter->setPen(theme::borderStrong);
        painter->setBrush(theme::buttonBg);
        painter->drawRect(box);
        painter->setPen(theme::text);
        painter->drawText(box.adjusted(8, 0, -20, 0), Qt::AlignVCenter | Qt::AlignLeft, index.data(Qt::DisplayRole).toString());
        const int midY = box.center().y();
        const int right = box.right() - 10;
        QPoint points[3] = {QPoint(right - 4, midY - 2), QPoint(right + 4, midY - 2), QPoint(right, midY + 3)};
        painter->setBrush(theme::textMuted);
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

    table_->setAlternatingRowColors(false);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
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

    const Mode currentMode = mode();
    const bool chordMode = currentMode == Mode::Chord;
    sectionBox_->setVisible(currentMode == Mode::Legacy);
    labelEdit_->setVisible(currentMode == Mode::Legacy);
    nameEdit_->setVisible(currentMode == Mode::Legacy);
    table_->setVisible(true);
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
            const int section = sectionForRow(row);
            const int chordLane = laneForRow(row);
            if (section < 0 || chordLane < 0) {
                return;
            }
            selectSection(section);
            if (column == 0) {
                if (chordLane != 0) {
                    return;
                }
                model_->renameSection(section, model_->section(section).label, text);
                emitStructureChanged();
                return;
            }
            // Musical beat cells are edited by the subdivision-aware mouse handler.
            return;
        }
        if (currentMode == Mode::Beat) {
            const int section = sectionForRow(row);
            const int lane = laneForRow(row);
            if (section < 0) {
                return;
            }
            selectSection(section);
            const auto kind = table_->item(row, column)
                ? static_cast<BeatCellKind>(table_->item(row, column)->data(kBeatCellKindRole).toInt())
                : BeatCellKind::None;
            if (lane < 0) {
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
            // Hit cells are intentionally non-editable. Mouse input is handled
            // by eventFilter, while playback highlighting updates custom item
            // roles. Treating those role changes as text edits would replace
            // the stored pattern with the item's empty display text.
            if (kind == BeatCellKind::Hit) {
                return;
            }
            model_->setBeatHit(section, column, lane, text);
            if (onBeatHitEdited) {
                onBeatHitEdited(section, column, lane, text, model_->revision());
            }
            return;
        }
        if (currentMode == Mode::Lyrics) {
            const int section = sectionForRow(row);
            if (section < 0 || section >= model_->sections().size()) {
                return;
            }
            selectSection(section);
            if (column == 0) {
                model_->renameSection(
                    section,
                    model_->section(section).label,
                    text);
                emitStructureChanged();
                return;
            }
            const int beat = column - 1;
            model_->setCell(section, QStringLiteral("lyric"), beat, text);
            if (onCellEdited) {
                onCellEdited(
                    section,
                    QStringLiteral("lyric"),
                    beat,
                    text,
                    model_->revision());
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
    refresh();
}

BeatGridModel& BeatGridWidget::model()
{
    return *model_;
}

void BeatGridWidget::focusGeneratedSection(const QString& kind)
{
    for (int index = 0; index < model_->sections().size(); ++index) {
        const QString generatedKind = model_->section(index).generatedKind;
        if (generatedKind == kind ||
            (generatedKind == QStringLiteral("practice") &&
             (kind == QStringLiteral("chord") || kind == QStringLiteral("beat")))) {
            selectedSection_ = index;
            refresh();
            const int row = mode() == Mode::Chord
                ? index * 4
                : mode() == Mode::Beat
                    ? index * (BeatGridModel::beatLaneNames().size() + 2)
                    : 0;
            if (QTableWidgetItem* item = table_->item(row, 0)) {
                table_->scrollToItem(item, QAbstractItemView::PositionAtTop);
            }
            return;
        }
    }
    refresh();
}

bool BeatGridWidget::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::Wheel) {
        return false;
    }
    if (watched == table_->viewport() &&
        mode() == Mode::Chord &&
        event->type() == QEvent::MouseButtonPress) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() != Qt::LeftButton && mouse->button() != Qt::RightButton) {
            return QWidget::eventFilter(watched, event);
        }
        const QModelIndex index = table_->indexAt(mouse->pos());
        if (!index.isValid() || index.column() <= 0) {
            return QWidget::eventFilter(watched, event);
        }
        const int section = sectionForRow(index.row());
        const int beat = index.column() - 1;
        const int rowLane = laneForRow(index.row());
        if (section < 0 || section >= model_->sections().size() ||
            beat < 0 || beat >= model_->section(section).beats) {
            return true;
        }
        selectSection(section);
        if (rowLane == 0) {
            QMenu menu(this);
            const int current = model_->section(section).musicalPatterns[beat].division;
            for (int value : BeatGridModel::musicalDivisionValues()) {
                QAction* action = menu.addAction(BeatGridModel::musicalDivisionLabel(value));
                action->setData(value);
                action->setCheckable(true);
                action->setChecked(value == current);
            }
            QAction* selected = menu.exec(table_->viewport()->mapToGlobal(mouse->pos()));
            if (!selected) return true;
            const int division = selected->data().toInt();
            model_->setMusicalDivision(section, beat, division);
            if (onMusicalDivisionChanged) {
                onMusicalDivisionChanged(section, beat, division, model_->revision());
            }
            rebuildChordTable();
            selectSection(section);
            return true;
        }
        if (rowLane != 1 && rowLane != 3) return true;

        const MusicalBeatPattern& pattern = model_->section(section).musicalPatterns[beat];
        const QRect cell = table_->visualRect(index);
        const int step = qBound(
            0,
            (mouse->pos().x() - cell.left()) * pattern.division / qMax(1, cell.width()),
            pattern.division - 1);
        const QVector<MusicalStep>& steps = rowLane == 1 ? pattern.chords : pattern.melody;
        QString updated;
        bool accepted = false;

        if (rowLane == 1 && mouse->button() == Qt::RightButton) {
            QMenu menu(this);
            QAction* sustain = menu.addAction(QStringLiteral("Hold previous (~)"));
            sustain->setData(QStringLiteral("~"));
            QAction* rest = menu.addAction(QStringLiteral("Rest (-)"));
            rest->setData(QStringLiteral("-"));
            menu.addSeparator();
            const QStringList roots{
                QStringLiteral("C"), QStringLiteral("C#"), QStringLiteral("Db"),
                QStringLiteral("D"), QStringLiteral("Eb"), QStringLiteral("E"),
                QStringLiteral("F"), QStringLiteral("F#"), QStringLiteral("Gb"),
                QStringLiteral("G"), QStringLiteral("Ab"), QStringLiteral("A"),
                QStringLiteral("Bb"), QStringLiteral("B")};
            const QStringList suffixes{
                QString(), QStringLiteral("m"), QStringLiteral("5"),
                QStringLiteral("sus2"), QStringLiteral("sus4"), QStringLiteral("dim"),
                QStringLiteral("aug"), QStringLiteral("6"), QStringLiteral("m6"),
                QStringLiteral("7"), QStringLiteral("maj7"), QStringLiteral("m7"),
                QStringLiteral("m7b5"), QStringLiteral("dim7"), QStringLiteral("add9"),
                QStringLiteral("madd9"), QStringLiteral("9"), QStringLiteral("maj9"),
                QStringLiteral("m9"), QStringLiteral("13"), QStringLiteral("7b9"),
                QStringLiteral("7#9"), QStringLiteral("alt"), QStringLiteral("#11"),
                QStringLiteral("maj7#11"), QStringLiteral("maj9#11")};
            for (const QString& root : roots) {
                QMenu* rootMenu = menu.addMenu(root);
                for (const QString& suffix : suffixes) {
                    const QString symbol = root + suffix;
                    QAction* action = rootMenu->addAction(symbol);
                    action->setData(symbol);
                }
            }
            QAction* selected = menu.exec(table_->viewport()->mapToGlobal(mouse->pos()));
            if (!selected) return true;
            updated = selected->data().toString();
            accepted = true;
        } else {
            updated = QInputDialog::getText(
                this,
                rowLane == 1 ? QStringLiteral("Edit chord step") : QStringLiteral("Edit melody step"),
                QStringLiteral("Value (~ holds, - rests):"),
                QLineEdit::Normal,
                musicalStepText(steps[step]),
                &accepted).trimmed();
        }
        if (!accepted) return true;
        const QString lane = rowLane == 1 ? QStringLiteral("chord") : QStringLiteral("melody");
        model_->setMusicalStep(section, beat, step, lane, updated);
        if (onMusicalStepEdited) {
            onMusicalStepEdited(section, beat, step, lane, updated, model_->revision());
        }
        rebuildChordTable();
        selectSection(section);
        return true;
    }
    if (watched == table_->viewport() && mode() == Mode::Beat && event->type() == QEvent::MouseButtonPress) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (mouse->button() != Qt::LeftButton && mouse->button() != Qt::RightButton) {
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
        QString updated;
        if (mouse->button() == Qt::RightButton) {
            QMenu menu(this);
            struct Choice {
                const char* label;
                QChar state;
            };
            for (const Choice& choice : {
                    Choice{"Empty", QLatin1Char('.')}, Choice{"Normal", QLatin1Char('x')},
                    Choice{"Accent", QLatin1Char('a')}, Choice{"Ghost", QLatin1Char('g')}}) {
                QAction* action = menu.addAction(QString::fromLatin1(choice.label));
                action->setData(QString(choice.state));
            }
            QAction* selected = menu.exec(table_->viewport()->mapToGlobal(mouse->pos()));
            if (!selected) {
                return true;
            }
            updated = normalizedHitText(current, division);
            updated[step] = selected->data().toString().at(0);
        } else {
            updated = withHitState(current, division, step, !hitChecked(current, division, step));
        }
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
    const double beatPhase = gridBeatPhase_;
    const bool running = gridRunning_;
    gridBeat_ = (std::numeric_limits<quint64>::max)();
    setGridPosition(beat, subdivision, running, beatPhase);
}

void BeatGridWidget::setBeatsPerBar(int beatsPerBar)
{
    const int bounded = qMax(1, beatsPerBar);
    if (beatsPerBar_ == bounded) {
        return;
    }
    beatsPerBar_ = bounded;
    refresh();
}

void BeatGridWidget::setGridPosition(quint64 absoluteBeat, int subdivision, bool running, double beatPhase)
{
    const bool wasRunning = gridRunning_;
    gridBeat_ = absoluteBeat;
    gridSubdivision_ = subdivision;
    gridBeatPhase_ = qBound(0.0, beatPhase, 0.999999);
    gridRunning_ = running;
    if (table_ == nullptr) {
        return;
    }
    auto* header = static_cast<GridHeaderView*>(beatHeader_);
    if (header == nullptr || !running) {
        if (header != nullptr) {
            header->setCurrentBeatColumn(-1);
        }
        if ((mode() == Mode::Beat || mode() == Mode::Chord) && wasRunning) {
            const QSignalBlocker tableBlocker(table_);
            for (int row = 0; row < table_->rowCount(); ++row) {
                for (int column = 0; column < table_->columnCount(); ++column) {
                    if (QTableWidgetItem* item = table_->item(row, column)) {
                        if (item->data(kBeatActiveStepRole).toInt() != -1) {
                            item->setData(kBeatActiveStepRole, -1);
                        }
                    }
                }
            }
            table_->viewport()->update();
        }
        return;
    }
    quint64 totalBeats = 0;
    for (const SongSection& section : model_->sections()) {
        totalBeats += static_cast<quint64>(qMax(0, section.beats));
    }
    if (totalBeats == 0) {
        header->setCurrentBeatColumn(-1);
        return;
    }
    quint64 songBeat = absoluteBeat % totalBeats;
    int activeSection = 0;
    int sectionBeat = 0;
    for (int index = 0; index < model_->sections().size(); ++index) {
        const int beats = qMax(0, model_->section(index).beats);
        if (songBeat < static_cast<quint64>(beats)) {
            activeSection = index;
            sectionBeat = static_cast<int>(songBeat);
            break;
        }
        songBeat -= static_cast<quint64>(beats);
    }
    const int beatColumn =
        mode() == Mode::Chord || mode() == Mode::Lyrics
        ? sectionBeat + 1
        : sectionBeat;
    header->setCurrentBeatColumn(
        beatColumn >= 0 && beatColumn < table_->columnCount() ? beatColumn : -1,
        gridBeatPhase_);
    const int markerRow = mode() == Mode::Chord
        ? activeSection * 4
        : mode() == Mode::Beat
            ? activeSection * (BeatGridModel::beatLaneNames().size() + 2) + 1
            : mode() == Mode::Lyrics ? activeSection : 0;
    if (table_->item(markerRow, beatColumn) != nullptr) {
        QScrollBar* horizontal = table_->horizontalScrollBar();
        if (horizontal != nullptr && horizontal->maximum() > 0) {
            const int viewportMidpoint = table_->viewport()->width() / 2;
            const int markerPosition = table_->columnViewportPosition(beatColumn) +
                static_cast<int>(std::lround(gridBeatPhase_ * table_->columnWidth(beatColumn)));
            if (markerPosition >= viewportMidpoint || markerPosition < 0) {
                horizontal->setValue(qBound(
                    horizontal->minimum(),
                    horizontal->value() + markerPosition - viewportMidpoint,
                    horizontal->maximum()));
            }
        }
    }
    if (mode() == Mode::Beat || mode() == Mode::Chord) {
        const QSignalBlocker tableBlocker(table_);
        for (int row = 0; row < table_->rowCount(); ++row) {
            for (int column = 0; column < table_->columnCount(); ++column) {
                QTableWidgetItem* item = table_->item(row, column);
                if (!item) {
                    continue;
                }
                const auto kind = static_cast<BeatCellKind>(item->data(kBeatCellKindRole).toInt());
                if (kind != BeatCellKind::Hit && kind != BeatCellKind::MusicalSteps) continue;
                const int storedDivision = item->data(kBeatDivisionRole).toInt();
                const int division = storedDivision > 0 ? storedDivision : 1;
                const int activeStep = column == beatColumn
                    ? qBound(0, static_cast<int>(gridBeatPhase_ * division), division - 1) : -1;
                if (item->data(kBeatActiveStepRole).toInt() != activeStep) {
                    item->setData(kBeatActiveStepRole, activeStep);
                }
            }
        }
        table_->viewport()->update();
    }
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
    table_->verticalHeader()->setVisible(true);
    const int rowsPerSection = 4;
    const int rowCount = model_->sections().size() * rowsPerSection;
    table_->setRowCount(rowCount);
    rowToSection_.fill(-1, rowCount);
    rowToLane_.fill(-1, rowCount);
    int maxBeats = 4;
    for (const SongSection& section : model_->sections()) {
        maxBeats = qMax(maxBeats, section.beats);
    }
    table_->setColumnCount(maxBeats + 1);
    QStringList headers{QStringLiteral("Section")};
    for (int beat = 0; beat < maxBeats; ++beat) {
        headers << QStringLiteral("%1.%2")
            .arg(beat / beatsPerBar_ + 1)
            .arg(beat % beatsPerBar_ + 1);
    }
    table_->setHorizontalHeaderLabels(headers);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table_->horizontalHeader()->setMinimumSectionSize(96);
    table_->horizontalHeader()->setDefaultSectionSize(164);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    for (int column = 1; column < table_->columnCount(); ++column) {
        table_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::Interactive);
        table_->horizontalHeader()->resizeSection(column, 164);
    }
    QVector<int> contentWidths(maxBeats, 164);
    QStringList rowLabels;
    for (int sectionIndex = 0; sectionIndex < model_->sections().size(); ++sectionIndex) {
        const SongSection& section = model_->section(sectionIndex);
        const int firstRow = sectionIndex * rowsPerSection;
        QVector<QStringList> chordSteps(section.beats);
        QVector<QStringList> derivedNotes(section.beats);
        QVector<QStringList> melodySteps(section.beats);
        QString activeChord;
        for (int beat = 0; beat < section.beats; ++beat) {
            const MusicalBeatPattern& pattern = section.musicalPatterns[beat];
            for (int step = 0; step < pattern.division; ++step) {
                const MusicalStep& chord = pattern.chords[step];
                chordSteps[beat].push_back(musicalStepText(chord));
                if (chord.state == MusicalStepState::Onset) activeChord = chord.value;
                else if (chord.state == MusicalStepState::Rest) activeChord.clear();
                derivedNotes[beat].push_back(
                    activeChord.isEmpty() ? QStringLiteral("-")
                                          : jam2::practice::chordToneNames(activeChord));
                melodySteps[beat].push_back(musicalStepText(pattern.melody[step]));
            }
        }
        rowLabels << QStringLiteral("Subdivision") << QStringLiteral("Chords")
                  << QStringLiteral("Chord Tones") << QStringLiteral("Melody");
        for (int lane = 0; lane < rowsPerSection; ++lane) {
            const int row = firstRow + lane;
            rowToSection_[row] = sectionIndex;
            rowToLane_[row] = lane;
            table_->setRowHeight(row, lane == 0 ? 42 : lane == 1 ? 58 : 46);
            if (lane == 0) {
                auto* header = new QTableWidgetItem(sectionTitle(section));
                QFont headerFont(QStringLiteral("Georgia"));
                headerFont.setPointSizeF(13.0);
                header->setFont(headerFont);
                header->setForeground(QBrush(theme::textStrong));
                header->setData(kSectionHeaderRole, true);
                header->setData(kSectionSelectedRole, selectedSection_ == sectionIndex);
                table_->setItem(row, 0, header);
                table_->setSpan(row, 0, rowsPerSection, 1);
            }
            for (int beat = 0; beat < maxBeats; ++beat) {
                auto* item = new QTableWidgetItem();
                if (beat < section.beats && lane == 0) {
                    const int division = section.musicalPatterns[beat].division;
                    item->setText(BeatGridModel::musicalDivisionLabel(division));
                    item->setData(kBeatCellKindRole, static_cast<int>(BeatCellKind::Division));
                    item->setData(kBeatDivisionRole, division);
                    item->setToolTip(QStringLiteral(
                        "Chord and melody share this division. Drum division remains independent."));
                } else if (beat < section.beats) {
                    const int division = section.musicalPatterns[beat].division;
                    const QStringList values = lane == 1 ? chordSteps[beat]
                        : lane == 2 ? derivedNotes[beat] : melodySteps[beat];
                    item->setData(kBeatCellKindRole, static_cast<int>(BeatCellKind::MusicalSteps));
                    item->setData(kBeatDivisionRole, division);
                    item->setData(kMusicalStepsRole, values);
                    item->setData(kBeatActiveStepRole, -1);
                }
                if (lane == 1) {
                    QFont chordFont(QStringLiteral("Georgia"));
                    chordFont.setPointSizeF(12.0);
                    item->setFont(chordFont);
                    item->setForeground(QBrush(QColor(142, 237, 228)));
                } else if (lane == 2) {
                    QFont noteFont(QStringLiteral("Bahnschrift"));
                    noteFont.setPointSizeF(9.0);
                    item->setFont(noteFont);
                    item->setForeground(QBrush(theme::textMuted));
                } else if (lane == 3) {
                    QFont melodyFont(QStringLiteral("Georgia"));
                    melodyFont.setPointSizeF(11.0);
                    item->setFont(melodyFont);
                    item->setForeground(QBrush(QColor(193, 167, 232)));
                }
                Qt::ItemFlags flags = item->flags();
                flags &= ~Qt::ItemIsEditable;
                if (beat >= section.beats || lane == 2) flags &= ~Qt::ItemIsEnabled;
                item->setFlags(flags);
                if (beat >= section.beats) item->setBackground(QBrush(theme::panelRaised));
                if (lane == 1) {
                    item->setToolTip(QStringLiteral(
                        "Click a subcell to type a chord; right-click for chord choices. ~ holds and - rests."));
                }
                if (lane == 3) {
                    QString tooltip = QStringLiteral(
                        "Click a subcell to type a note with octave. ~ holds and - rests.");
                    if (!section.generatedKind.isEmpty()) {
                        const int startTick = beat * 12;
                        const int endTick = startTick + 12;
                        for (const auto& event : section.generatedRecipe.melodyEvents) {
                            if (event.tick >= startTick && event.tick < endTick) {
                                tooltip += QStringLiteral("\n%1: %2; %3")
                                    .arg(event.note, event.chordRole, event.melodicRole);
                            }
                        }
                    }
                    item->setToolTip(tooltip);
                }
                if (beat < section.beats) {
                    int requiredWidth = 164;
                    if (lane == 0) {
                        requiredWidth = QFontMetrics(item->font()).horizontalAdvance(item->text()) + 44;
                    } else {
                        const QStringList values = item->data(kMusicalStepsRole).toStringList();
                        int widestStep = 0;
                        const QFontMetrics metrics(item->font());
                        for (const QString& value : values) {
                            widestStep = qMax(widestStep, metrics.horizontalAdvance(value));
                        }
                        requiredWidth = qMax(164, values.size() * (widestStep + 20));
                    }
                    contentWidths[beat] = qMax(
                        contentWidths[beat], qBound(164, requiredWidth, 960));
                }
                table_->setItem(row, beat + 1, item);
            }
        }
    }
    for (int beat = 0; beat < contentWidths.size(); ++beat) {
        table_->horizontalHeader()->resizeSection(beat + 1, contentWidths[beat]);
    }
    table_->setVerticalHeaderLabels(rowLabels);
    table_->clearSelection();
    table_->setUpdatesEnabled(true);
    updating_ = false;
}

void BeatGridWidget::rebuildBeatTable()
{
    const QStringList lanes = BeatGridModel::beatLaneNames();
    const QStringList visualLanes = BeatGridModel::beatVisualLaneNames();
    QVector<int> visualLaneIndices;
    visualLaneIndices.reserve(visualLanes.size());
    for (const QString& visualLane : visualLanes) {
        const int lane = lanes.indexOf(visualLane);
        if (lane >= 0) visualLaneIndices.push_back(lane);
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
        headers << QStringLiteral("%1.%2")
            .arg(beat / beatsPerBar_ + 1)
            .arg(beat % beatsPerBar_ + 1);
    }
    table_->setHorizontalHeaderLabels(headers);
    for (int column = 0; column < table_->columnCount(); ++column) {
        table_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::Fixed);
        table_->horizontalHeader()->resizeSection(column, 156);
    }

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
        header->setData(kSectionHeaderRole, true);
        header->setData(kSectionSelectedRole, selectedSection_ == sectionIndex);
        header->setBackground(QBrush(theme::panelRaised));
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
                item->setBackground(QBrush(theme::panelRaised));
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
                    item->setBackground(QBrush(theme::panelRaised));
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
    table_->clearSelection();
    table_->setUpdatesEnabled(true);
    updating_ = false;
}

void BeatGridWidget::rebuildLyricsBox()
{
    updating_ = true;
    QSignalBlocker tableBlocker(table_);
    table_->setUpdatesEnabled(false);
    table_->setRowCount(0);
    table_->setColumnCount(0);
    table_->clear();
    table_->clearSpans();
    table_->verticalHeader()->setVisible(false);
    int maxBeats = 4;
    for (const SongSection& section : model_->sections()) {
        maxBeats = qMax(maxBeats, section.beats);
    }
    table_->setRowCount(model_->sections().size());
    table_->setColumnCount(maxBeats + 1);
    rowToSection_.fill(-1, model_->sections().size());
    rowToLane_.fill(-1, model_->sections().size());
    QStringList headers{QStringLiteral("Section")};
    for (int beat = 0; beat < maxBeats; ++beat) {
        headers << QStringLiteral("%1.%2")
            .arg(beat / beatsPerBar_ + 1)
            .arg(beat % beatsPerBar_ + 1);
    }
    table_->setHorizontalHeaderLabels(headers);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    table_->horizontalHeader()->setDefaultSectionSize(190);
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    for (int column = 1; column < table_->columnCount(); ++column) {
        table_->horizontalHeader()->setSectionResizeMode(column, QHeaderView::Fixed);
        table_->horizontalHeader()->resizeSection(column, 190);
    }
    for (int sectionIndex = 0; sectionIndex < model_->sections().size(); ++sectionIndex) {
        const SongSection& section = model_->section(sectionIndex);
        rowToSection_[sectionIndex] = sectionIndex;
        auto* name = new QTableWidgetItem(sectionTitle(section));
        name->setData(kSectionHeaderRole, true);
        name->setData(kSectionSelectedRole, selectedSection_ == sectionIndex);
        table_->setItem(sectionIndex, 0, name);
        table_->setRowHeight(sectionIndex, 58);
        for (int beat = 0; beat < maxBeats; ++beat) {
            auto* item = new QTableWidgetItem(
                beat < section.beats ? section.lyrics[beat] : QString{});
            item->setToolTip(
                beat < section.beats
                ? QStringLiteral("Lyric cue beginning at %1.%2")
                    .arg(beat / beatsPerBar_ + 1)
                    .arg(beat % beatsPerBar_ + 1)
                : QString{});
            if (beat >= section.beats) {
                Qt::ItemFlags flags = item->flags();
                flags &= ~Qt::ItemIsEditable;
                flags &= ~Qt::ItemIsEnabled;
                item->setFlags(flags);
                item->setBackground(QBrush(theme::panelRaised));
            }
            table_->setItem(sectionIndex, beat + 1, item);
        }
    }
    table_->setUpdatesEnabled(true);
    updating_ = false;
}

void BeatGridWidget::expandCurrent()
{
    if (model_->sections().isEmpty()) {
        return;
    }
    if (mode() == Mode::Chord || mode() == Mode::Beat || mode() == Mode::Lyrics) {
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
    if (mode() == Mode::Chord || mode() == Mode::Beat || mode() == Mode::Lyrics) {
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
    if (mode() == Mode::Chord || mode() == Mode::Beat || mode() == Mode::Lyrics) {
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
    } else {
        selectedSection_ = section;
    }
    updateSectionSelectionMarkers();
    updateActionButtons();
}

void BeatGridWidget::updateSectionSelectionMarkers()
{
    if (updating_) {
        return;
    }
    const QSignalBlocker tableBlocker(table_);
    for (int row = 0; row < table_->rowCount(); ++row) {
        QTableWidgetItem* item = table_->item(row, 0);
        if (item && item->data(kSectionHeaderRole).toBool()) {
            item->setData(
                kSectionSelectedRole,
                sectionForRow(row) == selectedSection_);
        }
    }
    table_->clearSelection();
    table_->viewport()->update();
}

void BeatGridWidget::updateActionButtons()
{
    const bool selected = mode() == Mode::Legacy
        ? sectionBox_->currentIndex() >= 0 && sectionBox_->currentIndex() < model_->sections().size()
        : selectedSection_ >= 0 && selectedSection_ < model_->sections().size();
    const bool canUseSection = selected;
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

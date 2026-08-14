#include "ArrangementEditorDialog.hpp"

#include "GuiControlContract.hpp"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSizePolicy>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <utility>

namespace {

constexpr int kMaximumArrangementRows = 64;
constexpr int kMaximumRepeats = 64;
constexpr int kArrangementMinimumRowHeight = 46;

void registerModalControl(
    QObject& control,
    QString id,
    QString contract,
    QString family = {})
{
    jam2::gui::registerGuiControl(
        control,
        std::move(id),
        std::move(contract),
        jam2::gui::GuiControlAvailability::Modal,
        std::move(family));
}

} // namespace

ArrangementEditorDialog::ArrangementEditorDialog(
    const ArrangementDefinition& definition,
    int bankCount,
    bool arrangementActive,
    QWidget* parent)
    : QDialog(parent)
    , bankCount_(std::max(1, bankCount))
    , arrangementActive_(arrangementActive)
{
    setWindowTitle(QStringLiteral("Arrangement"));

    table_ = new QTableWidget(this);
    table_->setColumnCount(2);
    table_->setShowGrid(false);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setHorizontalHeaderLabels(
        {QStringLiteral("Section"), QStringLiteral("Repeats")});
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table_->horizontalHeader()->setHighlightSections(false);
    table_->verticalHeader()->setHighlightSections(false);
    table_->verticalHeader()->setMinimumSectionSize(kArrangementMinimumRowHeight);
    table_->verticalHeader()->setDefaultSectionSize(kArrangementMinimumRowHeight);
    registerModalControl(
        *table_,
        QStringLiteral("looper.arrangement-dialog.rows"),
        QStringLiteral("looper.arrangement-rows"));

    for (const ArrangementStep& step : definition.steps) appendRow(step);
    if (table_->rowCount() == 0) appendRow(ArrangementStep{});

    auto* add = new QPushButton(QStringLiteral("Add"), this);
    auto* remove = new QPushButton(QStringLiteral("Remove"), this);
    auto* up = new QPushButton(QStringLiteral("Up"), this);
    auto* down = new QPushButton(QStringLiteral("Down"), this);
    loop_ = new QCheckBox(QStringLiteral("Loop Arrangement"), this);
    loop_->setChecked(definition.loop);
    registerModalControl(
        *add,
        QStringLiteral("looper.arrangement-dialog.add"),
        QStringLiteral("looper.arrangement-row-structure"));
    registerModalControl(
        *remove,
        QStringLiteral("looper.arrangement-dialog.remove"),
        QStringLiteral("looper.arrangement-row-structure"));
    registerModalControl(
        *up,
        QStringLiteral("looper.arrangement-dialog.up"),
        QStringLiteral("looper.arrangement-row-order"));
    registerModalControl(
        *down,
        QStringLiteral("looper.arrangement-dialog.down"),
        QStringLiteral("looper.arrangement-row-order"));
    registerModalControl(
        *loop_,
        QStringLiteral("looper.arrangement-dialog.loop"),
        QStringLiteral("looper.arrangement-loop"));

    auto* rowButtons = new QHBoxLayout();
    rowButtons->addWidget(add);
    rowButtons->addWidget(remove);
    rowButtons->addWidget(up);
    rowButtons->addWidget(down);
    rowButtons->addStretch(1);
    rowButtons->addWidget(loop_);

    connect(add, &QPushButton::clicked, this, [this] { appendRow(ArrangementStep{}); });
    connect(remove, &QPushButton::clicked, this, [this] { removeSelectedRow(); });
    connect(up, &QPushButton::clicked, this, [this] { moveSelectedRow(-1); });
    connect(down, &QPushButton::clicked, this, [this] { moveSelectedRow(1); });

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    auto* toggle = buttons->addButton(
        arrangementActive_ ? QStringLiteral("Stop") : QStringLiteral("Save + Start"),
        QDialogButtonBox::ActionRole);
    if (QPushButton* save = buttons->button(QDialogButtonBox::Save)) {
        registerModalControl(
            *save,
            QStringLiteral("looper.arrangement-dialog.save"),
            QStringLiteral("looper.arrangement-save"));
    }
    if (QPushButton* cancel = buttons->button(QDialogButtonBox::Cancel)) {
        registerModalControl(
            *cancel,
            QStringLiteral("looper.arrangement-dialog.cancel"),
            QStringLiteral("looper.arrangement-cancel"));
    }
    registerModalControl(
        *toggle,
        QStringLiteral("looper.arrangement-dialog.toggle-active"),
        QStringLiteral("looper.arrangement-lifecycle"));
    connect(toggle, &QPushButton::clicked, this, [this] {
        action_ = arrangementActive_ ? Action::Stop : Action::Start;
        accept();
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(
        QStringLiteral("Each repeat plays the selected section for one complete loop."), this));
    layout->addWidget(table_, 1);
    layout->addLayout(rowButtons);
    layout->addWidget(buttons);
    resize(480, 420);
}

ArrangementEditorDialog::Result ArrangementEditorDialog::result() const
{
    ArrangementDefinition definition;
    definition.loop = loop_->isChecked();
    definition.enabled = arrangementActive_;
    definition.steps.reserve(table_->rowCount());
    for (int row = 0; row < table_->rowCount(); ++row) {
        definition.steps.push_back(stepAt(row));
    }
    return {std::move(definition), action_};
}

void ArrangementEditorDialog::appendRow(ArrangementStep step)
{
    if (table_->rowCount() >= kMaximumArrangementRows) return;
    const int row = table_->rowCount();
    table_->insertRow(row);

    auto* bank = new QComboBox(table_);
    for (int index = 0; index < bankCount_; ++index) {
        bank->addItem(
            QStringLiteral("Section %1")
                .arg(QChar(QLatin1Char('A').unicode() + index)),
            index);
    }
    bank->setCurrentIndex(qBound(0, step.bankIndex, bankCount_ - 1));
    auto* repeats = new QSpinBox(table_);
    repeats->setRange(1, kMaximumRepeats);
    repeats->setValue(qBound(1, step.repeats, kMaximumRepeats));

    bank->ensurePolished();
    repeats->ensurePolished();
    const int editorHeight = std::max({
        bank->sizeHint().height(),
        repeats->sizeHint().height(),
        bank->fontMetrics().height() + 20,
        repeats->fontMetrics().height() + 20});
    bank->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    repeats->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    table_->setRowHeight(row, std::max(kArrangementMinimumRowHeight, editorHeight));
    table_->setCellWidget(row, 0, bank);
    table_->setCellWidget(row, 1, repeats);
    bank->installEventFilter(this);
    repeats->installEventFilter(this);

    connect(bank, &QComboBox::currentIndexChanged, this,
        [this, bank](int) { selectEditorRow(bank); });
    connect(repeats, &QSpinBox::valueChanged, this,
        [this, repeats](int) { selectEditorRow(repeats); });
    refreshRowControlIds();
    table_->setCurrentCell(row, 0);
}

bool ArrangementEditorDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (event != nullptr &&
        (event->type() == QEvent::FocusIn || event->type() == QEvent::MouseButtonPress)) {
        selectEditorRow(qobject_cast<QWidget*>(watched));
    }
    return QDialog::eventFilter(watched, event);
}

void ArrangementEditorDialog::removeSelectedRow()
{
    const int row = table_->currentRow();
    if (row < 0) return;
    table_->removeRow(row);
    refreshRowControlIds();
    if (table_->rowCount() > 0) {
        table_->setCurrentCell(std::min(row, table_->rowCount() - 1), 0);
    }
}

void ArrangementEditorDialog::moveSelectedRow(int direction)
{
    const int row = table_->currentRow();
    const int target = row + direction;
    if (row < 0 || target < 0 || target >= table_->rowCount()) return;
    QVector<ArrangementStep> steps;
    steps.reserve(table_->rowCount());
    for (int index = 0; index < table_->rowCount(); ++index) {
        steps.push_back(stepAt(index));
    }
    steps.swapItemsAt(row, target);
    rebuildRows(steps, target);
}

void ArrangementEditorDialog::rebuildRows(
    const QVector<ArrangementStep>& steps,
    int selectedRow)
{
    table_->setRowCount(0);
    for (const ArrangementStep& step : steps) appendRow(step);
    if (selectedRow >= 0 && selectedRow < table_->rowCount()) {
        table_->setCurrentCell(selectedRow, 0);
    }
}

void ArrangementEditorDialog::refreshRowControlIds()
{
    for (int row = 0; row < table_->rowCount(); ++row) {
        if (QComboBox* bank = bankEditorAt(row)) {
            registerModalControl(
                *bank,
                QStringLiteral("looper.arrangement-dialog.row.%1.section").arg(row),
                QStringLiteral("looper.arrangement-step-section"),
                QStringLiteral("looper.arrangement-step"));
        }
        if (QSpinBox* repeats = repeatsEditorAt(row)) {
            registerModalControl(
                *repeats,
                QStringLiteral("looper.arrangement-dialog.row.%1.repeats").arg(row),
                QStringLiteral("looper.arrangement-step-repeats"),
                QStringLiteral("looper.arrangement-step"));
        }
    }
}

void ArrangementEditorDialog::selectEditorRow(const QWidget* editor)
{
    for (int row = 0; row < table_->rowCount(); ++row) {
        if (table_->cellWidget(row, 0) == editor || table_->cellWidget(row, 1) == editor) {
            table_->setCurrentCell(row, 0);
            return;
        }
    }
}

ArrangementStep ArrangementEditorDialog::stepAt(int row) const
{
    const QComboBox* bank = bankEditorAt(row);
    const QSpinBox* repeats = repeatsEditorAt(row);
    return {
        bank ? bank->currentData().toInt() : 0,
        repeats ? repeats->value() : 1,
    };
}

QComboBox* ArrangementEditorDialog::bankEditorAt(int row) const
{
    return qobject_cast<QComboBox*>(table_->cellWidget(row, 0));
}

QSpinBox* ArrangementEditorDialog::repeatsEditorAt(int row) const
{
    return qobject_cast<QSpinBox*>(table_->cellWidget(row, 1));
}

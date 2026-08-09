#include "CuratedIdeaDialog.hpp"

#include <QAbstractItemView>
#include <QButtonGroup>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QSize>
#include <QScrollBar>
#include <QSplitter>
#include <QVBoxLayout>

namespace jam2::practice {

std::optional<CuratedIdeaSelection> askForCuratedIdea(
    QWidget* parent,
    const CuratedIdeaDialogDefaults& defaults,
    const CuratedIdeaPreviewCallbacks& preview)
{
    QString catalogError;
    const QVector<CuratedIdeaEntry> catalog = loadCuratedIdeaCatalog(catalogError);
    if (!catalogError.isEmpty()) return std::nullopt;

    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Groove Library"));
    dialog.setMinimumSize(900, 610);

    auto* style = new QComboBox(&dialog);
    auto* profile = new QComboBox(&dialog);
    auto* list = new QListWidget(&dialog);
    list->setAlternatingRowColors(false);
    list->setSpacing(0);
    list->setUniformItemSizes(true);
    list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    list->verticalScrollBar()->setSingleStep(8);
    list->setMinimumWidth(330);
    auto* details = new QLabel(&dialog);
    details->setWordWrap(true);
    details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details->setAlignment(Qt::AlignTop | Qt::AlignLeft);

    style->addItem(QStringLiteral("All styles"), QString());
    QStringList styleIds;
    for (const CuratedIdeaEntry& entry : catalog) {
        if (!styleIds.contains(entry.styleId)) {
            styleIds.push_back(entry.styleId);
            style->addItem(entry.styleName, entry.styleId);
        }
    }
    profile->addItem(QStringLiteral("All profiles"), QString());

    auto* target = new QComboBox(&dialog);
    for (int bank = 0; bank < 4; ++bank) {
        target->addItem(
            QStringLiteral("Section %1").arg(QChar(QLatin1Char('A').unicode() + bank)),
            bank);
    }
    target->setCurrentIndex(qBound(0, defaults.targetSectionIndex, 3));

    auto* useGrooveTiming = new QRadioButton(&dialog);
    auto* keepTiming = new QRadioButton(&dialog);
    auto* timingGroup = new QButtonGroup(&dialog);
    timingGroup->addButton(useGrooveTiming);
    timingGroup->addButton(keepTiming);
    useGrooveTiming->setChecked(
        defaults.timing == CuratedIdeaTimingPolicy::UseIdeaTiming);
    keepTiming->setChecked(
        defaults.timing == CuratedIdeaTimingPolicy::KeepSectionTiming);
    auto* timingLayout = new QVBoxLayout();
    timingLayout->addWidget(useGrooveTiming);
    timingLayout->addWidget(keepTiming);

    auto* length = new QComboBox(&dialog);
    for (const int bars : {8, 12, 16, 24, 32}) {
        length->addItem(QStringLiteral("%1 bars").arg(bars), bars);
    }
    length->addItem(QStringLiteral("Current section length"), 0);
    length->setCurrentIndex(qMax(0, length->findData(defaults.importBars)));

    auto* previewButton = new QPushButton(QStringLiteral("PREVIEW 4 BARS"), &dialog);
    auto* previewStatus = new QLabel(&dialog);
    previewStatus->setWordWrap(true);
    previewStatus->setStyleSheet(QStringLiteral("color:#9eaaa9;"));
    previewButton->setEnabled(preview.available);
    if (!preview.available) previewStatus->setText(preview.unavailableReason);

    auto* filters = new QFormLayout();
    filters->addRow(QStringLiteral("Style"), style);
    filters->addRow(QStringLiteral("Profile"), profile);

    auto* left = new QWidget(&dialog);
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->addLayout(filters);
    leftLayout->addWidget(list, 1);

    auto* importBox = new QGroupBox(QStringLiteral("Import drums into your jam"), &dialog);
    auto* importForm = new QFormLayout(importBox);
    importForm->addRow(QStringLiteral("Target"), target);
    importForm->addRow(QStringLiteral("Timing"), timingLayout);
    importForm->addRow(QStringLiteral("Groove length"), length);

    auto* right = new QWidget(&dialog);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(8, 0, 0, 0);
    rightLayout->addWidget(details, 1);
    auto* previewRow = new QHBoxLayout();
    previewRow->addWidget(previewButton);
    previewRow->addWidget(previewStatus, 1);
    rightLayout->addLayout(previewRow);
    rightLayout->addWidget(importBox);

    auto* splitter = new QSplitter(Qt::Horizontal, &dialog);
    splitter->addWidget(left);
    splitter->addWidget(right);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Cancel | QDialogButtonBox::Ok, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Use Groove"));
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);

    bool previewPlaying = false;
    int selectedCatalogIndex = -1;

    const auto stopPreview = [&] {
        if (previewPlaying && preview.stop) preview.stop();
        previewPlaying = false;
        previewButton->setText(QStringLiteral("PREVIEW 4 BARS"));
        if (preview.available) previewStatus->clear();
    };

    const auto refreshTimingLabels = [&] {
        if (selectedCatalogIndex < 0 || selectedCatalogIndex >= catalog.size()) return;
        const CuratedIdeaEntry& entry = catalog.at(selectedCatalogIndex);
        const int bank = target->currentData().toInt();
        const int currentBpm = defaults.bankBpms.value(bank, 120);
        const int currentNumerator = defaults.bankMeterNumerators.value(bank, 4);
        const int currentDenominator = defaults.bankMeterDenominators.value(bank, 4);
        useGrooveTiming->setText(QStringLiteral("Use groove timing — %1 BPM, %2/%3")
            .arg(entry.bpm).arg(entry.meterNumerator).arg(entry.meterDenominator));
        keepTiming->setText(QStringLiteral("Keep Section %1 timing — %2 BPM, %3/%4")
            .arg(QChar(QLatin1Char('A').unicode() + bank))
            .arg(currentBpm).arg(currentNumerator).arg(currentDenominator));
    };

    const auto refreshDetails = [&] {
        const QListWidgetItem* selected = list->currentItem();
        selectedCatalogIndex = selected ? selected->data(Qt::UserRole).toInt() : -1;
        buttons->button(QDialogButtonBox::Ok)->setEnabled(selectedCatalogIndex >= 0);
        if (selectedCatalogIndex < 0 || selectedCatalogIndex >= catalog.size()) {
            details->setText(QStringLiteral("Choose a groove to see its exact musical data."));
            return;
        }
        const CuratedIdeaEntry& entry = catalog.at(selectedCatalogIndex);
        details->setText(QStringLiteral(
            "<h2>%1</h2><p><b>%2</b><br>%3</p>"
            "<p>%4 BPM · %5/%6 · 32-bar drum performance</p>"
            "<p>The preview loops a fixed four-bar drum excerpt. Importing loads 8, 12, 16, 24, or all 32 bars of editable drum events, including movement and fills. Keeping the current section length truncates the source or repeats it after bar 32.</p>")
            .arg(entry.name.toHtmlEscaped(), entry.profileName.toHtmlEscaped(),
                entry.formName.toHtmlEscaped())
            .arg(entry.bpm).arg(entry.meterNumerator).arg(entry.meterDenominator));
        refreshTimingLabels();
    };

    const auto rebuildProfiles = [&] {
        const QString previous = profile->currentData().toString();
        const QString selectedStyle = style->currentData().toString();
        QSignalBlocker blocker(profile);
        profile->clear();
        profile->addItem(QStringLiteral("All profiles"), QString());
        QStringList ids;
        for (const CuratedIdeaEntry& entry : catalog) {
            if ((!selectedStyle.isEmpty() && entry.styleId != selectedStyle) ||
                ids.contains(entry.profileId)) continue;
            ids.push_back(entry.profileId);
            profile->addItem(entry.profileName, entry.profileId);
        }
        const int previousIndex = profile->findData(previous);
        profile->setCurrentIndex(previousIndex >= 0 ? previousIndex : 0);
    };

    const auto rebuildList = [&] {
        stopPreview();
        const QString selectedStyle = style->currentData().toString();
        const QString selectedProfile = profile->currentData().toString();
        list->clear();
        for (int index = 0; index < catalog.size(); ++index) {
            const CuratedIdeaEntry& entry = catalog.at(index);
            if ((!selectedStyle.isEmpty() && entry.styleId != selectedStyle) ||
                (!selectedProfile.isEmpty() && entry.profileId != selectedProfile)) continue;
            auto* item = new QListWidgetItem(
                QStringLiteral("%1\n%2 · %3 BPM · %4/%5 · 32 bars")
                    .arg(entry.name, entry.profileName)
                    .arg(entry.bpm).arg(entry.meterNumerator).arg(entry.meterDenominator),
                list);
            item->setData(Qt::UserRole, index);
            item->setSizeHint(QSize(0, 58));
        }
        if (list->count() > 0) list->setCurrentRow(0);
        refreshDetails();
    };

    QObject::connect(style, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
        [&](int) { rebuildProfiles(); rebuildList(); });
    QObject::connect(profile, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
        [&](int) { rebuildList(); });
    QObject::connect(list, &QListWidget::currentRowChanged, &dialog,
        [&](int) { stopPreview(); refreshDetails(); });
    QObject::connect(target, qOverload<int>(&QComboBox::currentIndexChanged), &dialog,
        [&](int) { refreshTimingLabels(); });
    QObject::connect(previewButton, &QPushButton::clicked, &dialog, [&] {
        if (previewPlaying) {
            stopPreview();
            return;
        }
        if (selectedCatalogIndex < 0 || selectedCatalogIndex >= catalog.size() || !preview.play) return;
        QString reason;
        if (!preview.play(catalog.at(selectedCatalogIndex), reason)) {
            previewStatus->setText(reason);
            return;
        }
        previewPlaying = true;
        previewButton->setText(QStringLiteral("STOP PREVIEW"));
        previewStatus->setText(QStringLiteral("Looping four drum bars locally; this is not sent to peers."));
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    rebuildProfiles();
    rebuildList();

    auto* intro = new QLabel(QStringLiteral(
        "Browse fixed drum performances. Preview a four-bar excerpt, then import the useful portion of the full 32-bar groove into your jam."),
        &dialog);
    intro->setWordWrap(true);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(intro);
    layout->addWidget(splitter, 1);
    layout->addWidget(buttons);

    const int result = dialog.exec();
    stopPreview();
    if (result != QDialog::Accepted || selectedCatalogIndex < 0 ||
        selectedCatalogIndex >= catalog.size()) {
        return std::nullopt;
    }
    CuratedIdeaSelection selection;
    selection.idea = catalog.at(selectedCatalogIndex);
    selection.timing = useGrooveTiming->isChecked()
        ? CuratedIdeaTimingPolicy::UseIdeaTiming
        : CuratedIdeaTimingPolicy::KeepSectionTiming;
    selection.importBars = length->currentData().toInt();
    selection.targetSectionIndex = target->currentData().toInt();
    return selection;
}

} // namespace jam2::practice

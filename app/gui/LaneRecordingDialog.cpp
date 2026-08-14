#include "LaneRecordingDialog.hpp"

#include "GuiControlContract.hpp"
#include "GuiPresentation.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSpinBox>
#include <QVBoxLayout>

#include <utility>

namespace {

void registerRecordingControl(
    QObject& control,
    QString id,
    QString contract,
    jam2::gui::GuiControlAvailability availability =
        jam2::gui::GuiControlAvailability::Modal)
{
    jam2::gui::registerGuiControl(
        control,
        std::move(id),
        std::move(contract),
        availability,
        QStringLiteral("looper.recording-dialog"));
}

} // namespace

LaneRecordingDialog::LaneRecordingDialog(
    Input input,
    BrowseOutput browseOutput,
    RefreshLoopbackSources refreshLoopbackSources,
    QWidget* parent)
    : QDialog(parent)
    , input_(std::move(input))
    , browseOutput_(std::move(browseOutput))
    , refreshLoopbackSources_(std::move(refreshLoopbackSources))
    , activeMode_(input_.preferredMode)
    , inputOutputPath_(input_.inputOutputPath)
    , jamMixOutputPath_(input_.jamMixOutputPath)
    , loopbackOutputPath_(input_.loopbackOutputPath)
    , inputDraft_(input_.input)
    , loopbackDraft_(input_.loopback)
{
    setWindowTitle(QStringLiteral("Arm Lane Recording"));
    resize(720, 430);
    auto* content = new QWidget(this);
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    form_ = new QFormLayout(content);
    form_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form_->setRowWrapPolicy(QFormLayout::DontWrapRows);
    form_->setFormAlignment(Qt::AlignTop);
    form_->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
    form_->setVerticalSpacing(10);

    mode_ = new QComboBox(content);
    mode_->addItem(QStringLiteral("Local Input (instrument)"), QStringLiteral("input"));
    mode_->addItem(
        QStringLiteral("Jam Mix (Jam2 input + peers)"), QStringLiteral("current-jam"));
    mode_->addItem(
        QStringLiteral("System Loopback (desktop audio)"), QStringLiteral("loopback"));
    int preferredMode = mode_->findData(activeMode_);
    if (preferredMode < 0) preferredMode = 0;
    mode_->setCurrentIndex(preferredMode);
    activeMode_ = mode_->currentData().toString();
    applyMutedEditorStyle(mode_);

    inputSource_ = new QComboBox(content);
    for (const Choice& choice : input_.inputSources) {
        inputSource_->addItem(choice.label, choice.value);
    }
    if (inputSource_->count() == 0) {
        inputSource_->addItem(QStringLiteral("Combined My Send mix"), -1);
    }
    if (input_.selectedInputSource) {
        const int selected = inputSource_->findData(
            static_cast<qulonglong>(*input_.selectedInputSource));
        if (selected >= 0) inputSource_->setCurrentIndex(selected);
    }
    applyMutedEditorStyle(inputSource_);

    output_ = new QLineEdit(content);
    output_->setMinimumWidth(420);
    applyMutedEditorStyle(output_);
    auto* browse = new QPushButton(QStringLiteral("Browse"), content);
    outputRow_ = new QWidget(content);
    auto* outputLayout = new QHBoxLayout(outputRow_);
    outputLayout->setContentsMargins(0, 0, 0, 0);
    outputLayout->addWidget(output_, 1);
    outputLayout->addWidget(browse);

    loopbackSource_ = new QComboBox(content);
    loopbackSource_->setEditable(true);
    loopbackSource_->setMinimumWidth(360);
    applyMutedEditorStyle(loopbackSource_);
    populateLoopbackSources(input_.loopbackSources);
    auto* refreshSources = new QPushButton(QStringLiteral("Refresh Sources"), content);
    sourceRow_ = new QWidget(content);
    auto* sourceLayout = new QHBoxLayout(sourceRow_);
    sourceLayout->setContentsMargins(0, 0, 0, 0);
    sourceLayout->addWidget(loopbackSource_, 1);
    sourceLayout->addWidget(refreshSources);
    loopbackSource_->ensurePolished();
    const int sourceHeight = qMax(
        loopbackSource_->sizeHint().height(),
        loopbackSource_->fontMetrics().height() + 20);
    loopbackSource_->setMinimumHeight(sourceHeight);
    refreshSources->setMinimumHeight(sourceHeight);
    sourceRow_->setMinimumHeight(sourceHeight);

    includeBacking_ = new QCheckBox(QStringLiteral("Include backing track"), content);
    includeMetronome_ = new QCheckBox(
        QStringLiteral("Include metronome in WAV"), content);
    includeBacking_->setChecked(input_.jamMix.includeBackingTrack);
    includeMetronome_->setChecked(input_.jamMix.includeMetronome);
    leaderAudioWarning_ = new QLabel(QStringLiteral(
        "Leader-audio note: a click embedded in received peer audio cannot be removed from this recording."),
        content);
    leaderAudioWarning_->setWordWrap(true);

    latencyAdjustment_ = new QSpinBox(content);
    latencyAdjustment_->setRange(-8192, 8192);
    latencyAdjustment_->setValue(inputDraft_.latencyAdjustmentFrames);
    latencyAdjustment_->setSuffix(QStringLiteral(" frames"));
    latencyAdjustment_->setMinimumWidth(132);
    applyMutedEditorStyle(latencyAdjustment_);
    auto* latencyLabel = new QLabel(input_.latencySummary, content);
    latencyLabel->setWordWrap(true);
    latencyLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    latencyLabel->setMinimumHeight(qMax(48, content->fontMetrics().lineSpacing() * 3));
    latencyLabel->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    latencyRow_ = new QWidget(content);
    latencyRow_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    auto* latencyLayout = new QHBoxLayout(latencyRow_);
    latencyLayout->setContentsMargins(0, 0, 0, 0);
    latencyLayout->setAlignment(Qt::AlignTop);
    latencyLayout->addWidget(latencyLabel, 1);
    latencyLayout->addWidget(latencyAdjustment_, 0, Qt::AlignTop);
    latencyRow_->setMinimumHeight(qMax(
        latencyLabel->minimumHeight(), latencyAdjustment_->sizeHint().height()));

    silenceThreshold_ = new QDoubleSpinBox(content);
    silenceThreshold_->setRange(-120.0, 0.0);
    silenceThreshold_->setDecimals(1);
    silenceThreshold_->setValue(loopbackDraft_.silenceThresholdDb);
    silenceThreshold_->setSuffix(QStringLiteral(" dB"));
    silenceThreshold_->setMinimumWidth(120);
    applyMutedEditorStyle(silenceThreshold_);
    tailSilence_ = new QSpinBox(content);
    tailSilence_->setRange(0, 30000);
    tailSilence_->setValue(loopbackDraft_.tailSilenceMs);
    tailSilence_->setSuffix(QStringLiteral(" ms"));
    tailSilence_->setMinimumWidth(120);
    applyMutedEditorStyle(tailSilence_);
    trimLeading_ = new QCheckBox(QStringLiteral("Trim leading silence"), content);
    trimTrailing_ = new QCheckBox(QStringLiteral("Trim trailing silence"), content);
    trimLeading_->setChecked(loopbackDraft_.trimLeading);
    trimTrailing_->setChecked(loopbackDraft_.trimTrailing);

    engineStatus_ = new QLabel(content);
    engineStatus_->setWordWrap(true);
    engineStatus_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    engineStatus_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    inputLabel_ = new QLabel(QStringLiteral("Input"), content);
    inputLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    advancedToggle_ = new QPushButton(content);
    advancedToggle_->setText(QStringLiteral("\u25b8  ADVANCED"));
    advancedToggle_->setCheckable(true);
    advancedToggle_->setStyleSheet(QStringLiteral(
        "QPushButton { border:0;border-top:1px solid #2f3a3d;background:transparent;color:#ddd7e8;"
        "font:12px Bahnschrift;padding:9px 2px 4px;text-align:left; }"
        "QPushButton:hover { color:#ffd68a; }"));
    advancedContent_ = new QWidget(content);
    advancedForm_ = new QFormLayout(advancedContent_);
    advancedForm_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    advancedForm_->setRowWrapPolicy(QFormLayout::DontWrapRows);
    advancedForm_->setContentsMargins(14, 0, 0, 0);
    advancedForm_->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
    advancedForm_->addRow(QStringLiteral("Recording alignment"), latencyRow_);
    advancedForm_->addRow(QStringLiteral("Silence threshold"), silenceThreshold_);
    advancedForm_->addRow(QStringLiteral("Tail silence"), tailSilence_);
    advancedForm_->addRow(trimLeading_);
    advancedForm_->addRow(trimTrailing_);
    advancedContent_->hide();

    form_->addRow(QStringLiteral("Lane"), new QLabel(input_.laneName, content));
    form_->addRow(QStringLiteral("Source"), mode_);
    form_->addRow(QStringLiteral("Input source"), inputSource_);
    form_->addRow(QStringLiteral("Take WAV"), outputRow_);
    form_->addRow(inputLabel_, engineStatus_);
    form_->addRow(QStringLiteral("Loopback source"), sourceRow_);
    form_->addRow(QStringLiteral("Jam Mix options"), includeBacking_);
    form_->addRow(QString{}, includeMetronome_);
    form_->addRow(QString{}, leaderAudioWarning_);
    form_->addRow(advancedToggle_);
    form_->addRow(advancedContent_);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(content);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    arm_ = buttons->addButton(QStringLiteral("Arm"), QDialogButtonBox::AcceptRole);
    arm_->setDefault(true);

    registerRecordingControl(
        *mode_, QStringLiteral("looper.recording-dialog.mode"),
        QStringLiteral("looper.recording-source-mode"));
    registerRecordingControl(
        *inputSource_, QStringLiteral("looper.recording-dialog.input-source"),
        QStringLiteral("looper.recording-input-source"));
    registerRecordingControl(
        *output_, QStringLiteral("looper.recording.output-path"),
        QStringLiteral("looper.recording-settings"),
        jam2::gui::GuiControlAvailability::FileDialog);
    registerRecordingControl(
        *includeBacking_, QStringLiteral("looper.recording-dialog.include-backing"),
        QStringLiteral("looper.recording-jam-mix"));
    registerRecordingControl(
        *includeMetronome_, QStringLiteral("looper.recording-dialog.include-metronome"),
        QStringLiteral("looper.recording-jam-mix"));
    registerRecordingControl(
        *browse, QStringLiteral("looper.recording-dialog.browse-output"),
        QStringLiteral("looper.recording-output-file"),
        jam2::gui::GuiControlAvailability::FileDialog);
    registerRecordingControl(
        *loopbackSource_, QStringLiteral("looper.recording.loopback-source"),
        QStringLiteral("looper.recording-settings"),
        jam2::gui::GuiControlAvailability::HardwareProfile);
    registerRecordingControl(
        *refreshSources, QStringLiteral("looper.recording-dialog.refresh-loopback"),
        QStringLiteral("looper.recording-loopback-sources"),
        jam2::gui::GuiControlAvailability::HardwareProfile);
    registerRecordingControl(
        *advancedToggle_, QStringLiteral("looper.recording-dialog.advanced"),
        QStringLiteral("looper.recording-advanced"));
    registerRecordingControl(
        *latencyAdjustment_, QStringLiteral("looper.recording.latency-adjustment"),
        QStringLiteral("looper.recording-latency"));
    registerRecordingControl(
        *silenceThreshold_, QStringLiteral("looper.recording.silence-threshold"),
        QStringLiteral("looper.recording-finalization"));
    registerRecordingControl(
        *tailSilence_, QStringLiteral("looper.recording.tail-silence"),
        QStringLiteral("looper.recording-finalization"));
    registerRecordingControl(
        *trimLeading_, QStringLiteral("looper.recording.trim-leading"),
        QStringLiteral("looper.recording-finalization"));
    registerRecordingControl(
        *trimTrailing_, QStringLiteral("looper.recording.trim-trailing"),
        QStringLiteral("looper.recording-finalization"));
    registerRecordingControl(
        *arm_, QStringLiteral("looper.recording-dialog.arm"),
        QStringLiteral("looper.recording-arm"));
    if (QPushButton* cancel = buttons->button(QDialogButtonBox::Cancel)) {
        registerRecordingControl(
            *cancel, QStringLiteral("looper.recording-dialog.cancel"),
            QStringLiteral("looper.recording-cancel"));
    }

    connect(browse, &QPushButton::clicked, this, [this] {
        if (!browseOutput_) return;
        const QString selected = browseOutput_(output_->text().trimmed());
        if (!selected.isEmpty()) output_->setText(selected);
    });
    connect(refreshSources, &QPushButton::clicked, this, [this] {
        if (activeMode_ == QStringLiteral("loopback")) storeDraft();
        if (refreshLoopbackSources_) {
            populateLoopbackSources(refreshLoopbackSources_());
        }
        if (activeMode_ == QStringLiteral("loopback")) loadDraft();
    });
    connect(advancedToggle_, &QPushButton::toggled, this, [this](bool open) {
        advancedToggle_->setText(open
            ? QStringLiteral("\u25be  ADVANCED")
            : QStringLiteral("\u25b8  ADVANCED"));
        refreshMode();
    });
    connect(mode_, &QComboBox::currentIndexChanged, this, [this](int) {
        storeDraft();
        activeMode_ = mode_->currentData().toString();
        loadDraft();
        advancedToggle_->setChecked(false);
        refreshMode();
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(scroll, 1);
    layout->addWidget(buttons);
    loadDraft();
    refreshMode();
}

LaneRecordingDialog::Result LaneRecordingDialog::result()
{
    storeDraft();
    const qlonglong selectedSource = inputSource_->currentData().toLongLong();
    return {
        activeMode_,
        selectedSource >= 0
            ? std::optional<std::size_t>(static_cast<std::size_t>(selectedSource))
            : std::nullopt,
        output_->text().trimmed(),
        inputDraft_,
        loopbackDraft_,
        includeBacking_->isChecked(),
        includeMetronome_->isChecked(),
    };
}

void LaneRecordingDialog::storeDraft()
{
    if (activeMode_ == QStringLiteral("loopback")) {
        loopbackOutputPath_ = output_->text().trimmed();
        loopbackDraft_.sourceId = loopbackSource_->currentData().toString().isEmpty()
            ? loopbackSource_->currentText().trimmed()
            : loopbackSource_->currentData().toString();
        loopbackDraft_.sourceName = loopbackSource_->currentText().trimmed();
        loopbackDraft_.silenceThresholdDb = silenceThreshold_->value();
        loopbackDraft_.tailSilenceMs = tailSilence_->value();
        loopbackDraft_.trimLeading = trimLeading_->isChecked();
        loopbackDraft_.trimTrailing = trimTrailing_->isChecked();
        return;
    }
    if (activeMode_ == QStringLiteral("current-jam")) {
        jamMixOutputPath_ = output_->text().trimmed();
    } else {
        inputOutputPath_ = output_->text().trimmed();
    }
    inputDraft_.latencyAdjustmentFrames = latencyAdjustment_->value();
}

void LaneRecordingDialog::loadDraft()
{
    if (activeMode_ == QStringLiteral("loopback")) {
        output_->setText(loopbackOutputPath_);
        selectLoopbackSource(loopbackDraft_.sourceId, loopbackDraft_.sourceName);
        silenceThreshold_->setValue(loopbackDraft_.silenceThresholdDb);
        tailSilence_->setValue(loopbackDraft_.tailSilenceMs);
        trimLeading_->setChecked(loopbackDraft_.trimLeading);
        trimTrailing_->setChecked(loopbackDraft_.trimTrailing);
        return;
    }
    output_->setText(activeMode_ == QStringLiteral("current-jam")
        ? jamMixOutputPath_ : inputOutputPath_);
    latencyAdjustment_->setValue(inputDraft_.latencyAdjustmentFrames);
}

void LaneRecordingDialog::refreshMode()
{
    const bool inputMode = activeMode_ == QStringLiteral("input");
    const bool currentJamMode = activeMode_ == QStringLiteral("current-jam");
    const bool engineMode = inputMode || currentJamMode;
    const bool loopbackMode = !engineMode;
    const bool advancedAvailable = inputMode || loopbackMode;
    engineStatus_->setText(currentJamMode
        ? QStringLiteral(
            "Jam Mix records your local input plus received Jam2 peers. "
            "It does not capture other desktop applications.")
        : input_.engineRunning
            ? QStringLiteral("Records only your local Jam2 input.")
            : QStringLiteral("Start Perform or a jam before recording engine input."));
    setFormRowVisible(form_, engineStatus_, engineMode);
    setFormRowVisible(form_, inputSource_, inputMode);
    setFormRowVisible(form_, sourceRow_, loopbackMode);
    setFormRowVisible(form_, includeBacking_, currentJamMode);
    setFormRowVisible(form_, includeMetronome_, currentJamMode);
    setFormRowVisible(form_, leaderAudioWarning_, currentJamMode && input_.leaderAudio);
    setFormRowVisible(advancedForm_, latencyRow_, inputMode);
    setFormRowVisible(advancedForm_, silenceThreshold_, loopbackMode);
    setFormRowVisible(advancedForm_, tailSilence_, loopbackMode);
    setFormRowVisible(advancedForm_, trimLeading_, loopbackMode);
    setFormRowVisible(advancedForm_, trimTrailing_, loopbackMode);
    setFormRowVisible(form_, advancedToggle_, advancedAvailable);
    setFormRowVisible(
        form_, advancedContent_, advancedAvailable && advancedToggle_->isChecked());
    arm_->setEnabled(!engineMode || input_.engineRunning);
}

void LaneRecordingDialog::populateLoopbackSources(const QVector<Choice>& choices)
{
    loopbackSource_->clear();
    for (const Choice& choice : choices) {
        loopbackSource_->addItem(choice.label, choice.value);
    }
    if (loopbackSource_->count() == 0) {
        loopbackSource_->addItem(
            QStringLiteral("[default] System mix"), QStringLiteral("default"));
    }
}

void LaneRecordingDialog::selectLoopbackSource(const QString& id, const QString& name)
{
    int index = loopbackSource_->findData(id);
    if (index < 0) index = loopbackSource_->findText(name);
    if (index < 0) index = loopbackSource_->findData(QStringLiteral("default"));
    loopbackSource_->setCurrentIndex(qMax(0, index));
    if (index < 0 && loopbackSource_->isEditable()) loopbackSource_->setEditText(name);
}

void LaneRecordingDialog::setFormRowVisible(
    QFormLayout* form,
    QWidget* field,
    bool visible)
{
    if (!form || !field) return;
    field->setVisible(visible);
    if (QWidget* label = form->labelForField(field)) label->setVisible(visible);
}

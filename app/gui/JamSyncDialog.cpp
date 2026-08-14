#include "JamSyncDialog.hpp"

#include "GuiControlContract.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include <array>
#include <utility>

namespace {

void registerSyncControl(QObject& control, const char* id, const char* contract)
{
    jam2::gui::registerGuiControl(
        control,
        QStringLiteral("session.jam-sync-dialog.") + QString::fromLatin1(id),
        QString::fromLatin1(contract),
        jam2::gui::GuiControlAvailability::Modal,
        QStringLiteral("session.jam-sync-dialog"));
}

} // namespace

JamSyncDialog::JamSyncDialog(
    JamSyncPolicy policy,
    bool policyLocked,
    bool leaderAudio,
    QWidget* parent)
    : QDialog(parent)
    , initialPolicy_(std::move(policy))
    , policyLocked_(policyLocked)
    , leaderAudio_(leaderAudio)
{
    setWindowTitle(QStringLiteral("Jam Sync"));
    setMinimumWidth(500);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(14);

    auto* introduction = new QLabel(QStringLiteral(
        "Choose which changes Jam2 shares. Nothing changes until you apply the complete set to the jam."),
        this);
    introduction->setWordWrap(true);
    introduction->setStyleSheet(QStringLiteral("color:#9ca9ab;"));
    layout->addWidget(introduction);

    auto* contentGroup = new QGroupBox(QStringLiteral("Content Sharing"), this);
    auto* contentLayout = new QFormLayout(contentGroup);
    contentLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    contentLayout->setHorizontalSpacing(18);
    contentLayout->setVerticalSpacing(10);

    trackLanes_ = new QCheckBox(QStringLiteral("Sync Track Lanes"), contentGroup);
    trackLanes_->setChecked(initialPolicy_.trackLanes);
    trackLanes_->setToolTip(QStringLiteral(
        "Share lane creation, removal, names, positions and track metadata"));
    contentLayout->addRow(trackLanes_);

    automaticWavs_ = new QCheckBox(
        QStringLiteral("Sync WAVs Automatically"), contentGroup);
    automaticWavs_->setChecked(initialPolicy_.autoShareWavs);
    automaticWavs_->setToolTip(QStringLiteral(
        "Automatically transfer audio files belonging to shared lanes"));
    contentLayout->addRow(automaticWavs_);

    generatedIdeas_ = new QComboBox(contentGroup);
    generatedIdeas_->addItem(
        QStringLiteral("Whole Idea"), static_cast<int>(GeneratedIdeaSyncMode::Full));
    generatedIdeas_->addItem(
        QStringLiteral("Chords Only"), static_cast<int>(GeneratedIdeaSyncMode::Chords));
    generatedIdeas_->addItem(
        QStringLiteral("Beats Only"), static_cast<int>(GeneratedIdeaSyncMode::Beats));
    generatedIdeas_->addItem(
        QStringLiteral("Disabled"), static_cast<int>(GeneratedIdeaSyncMode::Off));
    generatedIdeas_->setCurrentIndex(qMax(0, generatedIdeas_->findData(
        static_cast<int>(initialPolicy_.generatedIdeas))));
    generatedIdeas_->setToolTip(QStringLiteral(
        "Choose which parts of newly generated and continued ideas are shared"));
    contentLayout->addRow(QStringLiteral("Sync Generated Ideas"), generatedIdeas_);
    layout->addWidget(contentGroup);

    auto* performanceGroup = new QGroupBox(QStringLiteral("Performance Sync"), this);
    auto* performanceLayout = new QFormLayout(performanceGroup);
    performanceLayout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    performanceLayout->setHorizontalSpacing(18);
    performanceLayout->setVerticalSpacing(10);

    globalPlayback_ = new QCheckBox(
        QStringLiteral("Sync Global Playback"), performanceGroup);
    globalPlayback_->setChecked(initialPolicy_.globalPlayback);
    globalPlayback_->setToolTip(QStringLiteral(
        "Share play, stop, restart and queued Section changes"));
    performanceLayout->addRow(globalPlayback_);

    metronomeState_ = new QCheckBox(
        QStringLiteral("Sync Metronome State"), performanceGroup);
    metronomeState_->setChecked(initialPolicy_.metronomeState);
    metronomeState_->setToolTip(QStringLiteral(
        "Share whether the metronome is turned on or off"));
    performanceLayout->addRow(metronomeState_);

    recordings_ = new QCheckBox(QStringLiteral("Sync Recordings"), performanceGroup);
    recordings_->setChecked(jam2JamSyncAllows(initialPolicy_, JamSyncRoute::Recording));
    performanceLayout->addRow(recordings_);

    dependencyNote_ = new QLabel(performanceGroup);
    dependencyNote_->setWordWrap(true);
    dependencyNote_->setStyleSheet(QStringLiteral("color:#9ca9ab;"));
    performanceLayout->addRow(dependencyNote_);
    layout->addWidget(performanceGroup);

    connect(trackLanes_, &QCheckBox::toggled, this,
        [this](bool) { updateDependencies(); });
    connect(globalPlayback_, &QCheckBox::toggled, this,
        [this](bool) { updateDependencies(); });

    const std::array<QWidget*, 6> policyControls{
        trackLanes_, automaticWavs_, generatedIdeas_,
        globalPlayback_, metronomeState_, recordings_,
    };
    for (QWidget* control : policyControls) {
        if (policyLocked_) control->setEnabled(false);
    }
    updateDependencies();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    auto* apply = buttons->addButton(
        QStringLiteral("Apply to Jam"), QDialogButtonBox::AcceptRole);
    apply->setEnabled(!policyLocked_);
    registerSyncControl(*trackLanes_, "track-lanes", "jam-sync.track-lanes");
    registerSyncControl(*automaticWavs_, "automatic-wavs", "jam-sync.automatic-wavs");
    registerSyncControl(*generatedIdeas_, "generated-ideas", "jam-sync.generated-ideas");
    registerSyncControl(*globalPlayback_, "global-playback", "jam-sync.global-playback");
    registerSyncControl(*metronomeState_, "metronome-state", "jam-sync.metronome-state");
    registerSyncControl(*recordings_, "recordings", "jam-sync.recordings");
    registerSyncControl(*apply, "apply", "jam-sync.apply-policy");
    if (QPushButton* cancel = buttons->button(QDialogButtonBox::Cancel)) {
        registerSyncControl(*cancel, "cancel", "jam-sync.cancel-policy");
    }
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

JamSyncPolicy JamSyncDialog::policy() const noexcept
{
    JamSyncPolicy result = initialPolicy_;
    result.trackLanes = trackLanes_->isChecked();
    result.autoShareWavs = automaticWavs_->isChecked();
    result.globalPlayback = globalPlayback_->isChecked();
    result.generatedIdeas = static_cast<GeneratedIdeaSyncMode>(
        generatedIdeas_->currentData().toInt());
    result.metronomeState = metronomeState_->isChecked();
    result.recordings = recordings_->isChecked();
    return result;
}

void JamSyncDialog::updateDependencies()
{
    const bool lanesEnabled = trackLanes_->isChecked();
    const bool recordingDependencies = lanesEnabled && globalPlayback_->isChecked();
    automaticWavs_->setEnabled(lanesEnabled && !policyLocked_);
    recordings_->setEnabled(recordingDependencies && !policyLocked_);
    if (!recordingDependencies) recordings_->setChecked(false);
    metronomeState_->setEnabled(!leaderAudio_ && !policyLocked_);
    automaticWavs_->setToolTip(lanesEnabled
        ? QStringLiteral("Automatically transfer audio files belonging to shared lanes")
        : QStringLiteral("Requires Sync Track Lanes"));
    recordings_->setToolTip(recordingDependencies
        ? QStringLiteral("Coordinate arm, count-in, recording and playback protection")
        : QStringLiteral("Requires Sync Track Lanes and Sync Global Playback"));
    dependencyNote_->setText(policyLocked_
        ? QStringLiteral("Jam Sync cannot be changed while a shared recording is active.")
        : leaderAudio_
            ? QStringLiteral("Recording sync requires Track Lanes and Global Playback. "
                "Metronome state is supplied by the leader in Leader Audio mode.")
            : QStringLiteral("Recording sync requires Track Lanes and Global Playback."));
}

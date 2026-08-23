#include "SessionStartupDialogs.hpp"

#include "GuiControlContract.hpp"
#include "GuiPresentation.hpp"

#include "audio_device.hpp"
#include "tuning_profile.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

#include <limits>
#include <utility>

namespace jam2::gui {
namespace {

QString view(std::string_view value)
{
    return QString::fromUtf8(
        value.data(), static_cast<qsizetype>(value.size()));
}

QSpinBox* spin(
    QWidget* parent,
    int minimum,
    int maximum,
    int value)
{
    auto* control = new QSpinBox(parent);
    control->setRange(minimum, maximum);
    control->setValue(value);
    return control;
}

QDoubleSpinBox* decimalSpin(
    QWidget* parent,
    double minimum,
    double maximum,
    double value,
    int decimals,
    double step)
{
    auto* control = new QDoubleSpinBox(parent);
    control->setRange(minimum, maximum);
    control->setDecimals(decimals);
    control->setSingleStep(step);
    control->setValue(value);
    return control;
}

QComboBox* fixedSampleRateCombo(QWidget* parent, int value)
{
    auto* combo = new QComboBox(parent);
    for (const int rate : jam2::audio::kTestSampleRates) {
        combo->addItem(QString::number(rate), rate);
    }
    const int index = combo->findData(value);
    combo->setCurrentIndex(index >= 0 ? index : combo->findData(48000));
    return combo;
}

QComboBox* fixedBufferSizeCombo(QWidget* parent, int value)
{
    auto* combo = new QComboBox(parent);
    for (const long size : jam2::audio::kTestBufferSizes) {
        combo->addItem(QString::number(size), static_cast<int>(size));
    }
    const int index = combo->findData(value);
    combo->setCurrentIndex(index >= 0 ? index : combo->findData(64));
    return combo;
}

void selectData(QComboBox* combo, const QVariant& value)
{
    const int index = combo->findData(value);
    if (index >= 0) combo->setCurrentIndex(index);
}

void registerDialogField(
    QObject& control,
    const char* id,
    const char* contract,
    GuiControlAvailability availability = GuiControlAvailability::Modal)
{
    registerGuiControl(
        control,
        QStringLiteral("session.dialog.") + QString::fromLatin1(id),
        QString::fromLatin1(contract),
        availability,
        QStringLiteral("session.dialog-field"));
}

void registerDialogAction(
    QObject& control,
    const char* dialog,
    const char* id,
    const char* contract,
    GuiControlAvailability availability = GuiControlAvailability::Modal)
{
    registerGuiControl(
        control,
        QStringLiteral("session.") + QString::fromLatin1(dialog) +
            QStringLiteral("-dialog.") + QString::fromLatin1(id),
        QString::fromLatin1(contract),
        availability,
        QStringLiteral("session.") + QString::fromLatin1(dialog) +
            QStringLiteral("-dialog"));
}

void styleEditors(std::initializer_list<QWidget*> editors)
{
    for (QWidget* editor : editors) applyMutedEditorStyle(editor);
}

void applyJoinTuning(
    const jam2::JoinProfile& profile,
    QComboBox* bufferSize,
    QSpinBox* prefill,
    QSpinBox* playbackMaximum,
    QSpinBox* captureRing,
    QSpinBox* playbackRing,
    QCheckBox* driftCorrection,
    QDoubleSpinBox* driftSmoothing,
    QSpinBox* driftDeadband,
    QSpinBox* driftMaximum,
    QCheckBox* sampleTimePlayout,
    QSpinBox* playoutDelay,
    QSpinBox* jitterBuffer,
    QSpinBox* jitterBufferMaximum,
    QCheckBox* adaptiveCushion,
    QSpinBox* adaptiveTarget,
    QSpinBox* adaptiveMinimum,
    QSpinBox* adaptiveMaximum,
    QSpinBox* adaptiveRelease,
    QSpinBox* adaptiveRatioRamp)
{
    selectData(bufferSize, static_cast<int>(profile.audio_buffer_size));
    prefill->setValue(static_cast<int>(profile.playback_prefill_frames));
    playbackMaximum->setValue(static_cast<int>(profile.playback_max_frames));
    captureRing->setValue(static_cast<int>(profile.capture_ring_frames));
    playbackRing->setValue(static_cast<int>(profile.playback_ring_frames));
    driftCorrection->setChecked(profile.drift_correction);
    driftSmoothing->setValue(profile.drift_smoothing);
    driftDeadband->setValue(profile.drift_deadband_ppm);
    driftMaximum->setValue(profile.drift_max_correction_ppm);
    sampleTimePlayout->setChecked(profile.sample_time_playout);
    playoutDelay->setValue(static_cast<int>(profile.playout_delay_frames));
    jitterBuffer->setValue(static_cast<int>(profile.jitter_buffer_frames));
    jitterBufferMaximum->setValue(
        static_cast<int>(profile.jitter_buffer_max_frames));
    adaptiveCushion->setChecked(profile.adaptive_playback_cushion);
    adaptiveTarget->setValue(
        static_cast<int>(profile.adaptive_playback_target_frames));
    adaptiveMinimum->setValue(
        static_cast<int>(profile.adaptive_playback_min_frames));
    adaptiveMaximum->setValue(
        static_cast<int>(profile.adaptive_playback_max_frames));
    adaptiveRelease->setValue(profile.adaptive_playback_release_ppm);
    adaptiveRatioRamp->setValue(profile.adaptive_playback_ratio_ramp_ms);
}

} // namespace

LocalEngineDialog::LocalEngineDialog(
    LocalEngineDialogState initial,
    SessionAudioDeviceList devices,
    LocalEngineDialogCallbacks callbacks,
    QWidget* parent)
    : QDialog(parent),
      callbacks_(std::move(callbacks))
{
    setWindowTitle(QStringLiteral("Start Local Engine"));
    setObjectName(QStringLiteral("LocalEngineDialog"));
    setModal(true);
    setWindowModality(Qt::WindowModal);
    setSizeGripEnabled(false);
    setWindowFlag(Qt::WindowMaximizeButtonHint, false);

    auto* form = new QFormLayout(this);
    form->setSizeConstraint(QLayout::SetFixedSize);
    form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    form->setFormAlignment(Qt::AlignHCenter | Qt::AlignTop);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setContentsMargins(22, 20, 22, 18);
    form->setHorizontalSpacing(18);
    form->setVerticalSpacing(12);

    device_ = new QComboBox(this);
    device_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    device_->setMinimumContentsLength(42);
    device_->setMinimumWidth(420);
    device_->setMaximumWidth(520);
    for (const SessionAudioDeviceChoice& device : devices.devices) {
        device_->addItem(device.label, device.id);
    }
    const int selectedDevice = device_->findData(initial.selectedDeviceId);
    device_->setCurrentIndex(selectedDevice >= 0 ? selectedDevice : 0);
    sampleRate_ = fixedSampleRateCombo(this, initial.sampleRate);
    bufferSize_ = fixedBufferSizeCombo(this, initial.bufferSize);
    inputChannels_ = new QLineEdit(initial.inputChannels, this);
    outputChannels_ = new QLineEdit(initial.outputChannels, this);
    inputChannels_->setMinimumWidth(320);
    outputChannels_->setMinimumWidth(320);
    auto* testDevice = new QPushButton(QStringLiteral("Test Device"), this);
    saveDefaults_ = new QCheckBox(QStringLiteral("Save as Local defaults"), this);
    saveDefaults_->setChecked(initial.saveDefaults);

    registerGuiControl(
        *device_, QStringLiteral("application.local-engine.device"),
        QStringLiteral("application.local-engine-settings"),
        GuiControlAvailability::HardwareProfile);
    registerGuiControl(
        *sampleRate_, QStringLiteral("application.local-engine.sample-rate"),
        QStringLiteral("application.local-engine-settings"),
        GuiControlAvailability::Modal);
    registerGuiControl(
        *bufferSize_, QStringLiteral("application.local-engine.buffer-size"),
        QStringLiteral("application.local-engine-settings"),
        GuiControlAvailability::Modal);
    registerGuiControl(
        *inputChannels_, QStringLiteral("application.local-engine.input-channels"),
        QStringLiteral("application.local-engine-settings"),
        GuiControlAvailability::Modal);
    registerGuiControl(
        *outputChannels_, QStringLiteral("application.local-engine.output-channels"),
        QStringLiteral("application.local-engine-settings"),
        GuiControlAvailability::Modal);
    registerGuiControl(
        *testDevice, QStringLiteral("application.local-engine.test-device"),
        QStringLiteral("application.audio-device-test"),
        GuiControlAvailability::HardwareProfile);
    registerGuiControl(
        *saveDefaults_, QStringLiteral("application.local-engine.save-defaults"),
        QStringLiteral("application.local-engine-settings"),
        GuiControlAvailability::Modal);

    form->addRow(QStringLiteral("Low-latency device"), device_);
    form->addRow(QStringLiteral("Sample rate"), sampleRate_);
    form->addRow(QStringLiteral("Buffer size"), bufferSize_);
    form->addRow(QStringLiteral("Input channels"), inputChannels_);
    form->addRow(QStringLiteral("Output channels"), outputChannels_);
    form->addRow(QString(), testDevice);
    form->addRow(QString(), saveDefaults_);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Cancel | QDialogButtonBox::Ok, this);
    auto* start = buttons->button(QDialogButtonBox::Ok);
    auto* cancel = buttons->button(QDialogButtonBox::Cancel);
    start->setText(QStringLiteral("Start Engine"));
    registerGuiControl(
        *start, QStringLiteral("application.local-engine.start"),
        QStringLiteral("application.local-engine-lifecycle"),
        GuiControlAvailability::HardwareProfile);
    registerGuiControl(
        *cancel, QStringLiteral("application.local-engine.cancel"),
        QStringLiteral("application.local-engine-lifecycle"),
        GuiControlAvailability::Modal);
    form->addRow(buttons);

    QObject::connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    QObject::connect(testDevice, &QPushButton::clicked, this,
        [this, testDevice] {
            if (callbacks_.testDevice) {
                callbacks_.testDevice(device_, testDevice, this);
            }
        });
    adjustSize();
}

LocalEngineDialogState LocalEngineDialog::state() const
{
    return {
        device_->currentData().toString(),
        sampleRate_->currentData().toInt(),
        bufferSize_->currentData().toInt(),
        inputChannels_->text().trimmed(),
        outputChannels_->text().trimmed(),
        saveDefaults_->isChecked(),
    };
}

StartJamDialog::StartJamDialog(
    StartJamDialogState initial,
    SessionAudioDeviceList devices,
    StartJamDialogCallbacks callbacks,
    QWidget* parent)
    : QDialog(parent),
      callbacks_(std::move(callbacks)),
      createTemplate_(initial.create),
      audioTemplate_(initial.audio)
{
    setWindowTitle(QStringLiteral("Start Jam"));
    resize(760, 620);

    auto* content = new QWidget(this);
    auto* contentLayout = new QVBoxLayout(content);

    bindHost_ = new QLineEdit(initial.create.bindHost, content);
    port_ = spin(content, 1, 65535, initial.create.port);
    publicHost_ = new QLineEdit(initial.create.publicHost, content);
    stunServer_ = new QLineEdit(initial.create.stunServer, content);
    stunTimeout_ = spin(content, 1, 60000, initial.create.stunTimeoutMs);
    stunRetries_ = spin(content, 1, 100, initial.create.stunRetries);
    noStun_ = new QCheckBox(QStringLiteral("No STUN"), content);
    noStun_->setChecked(initial.create.noStun);
    maximumPeers_ = spin(content, 0, 1000000, initial.create.maxPeers);

    auto* connectionForm = new QFormLayout();
    connectionForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    connectionForm->addRow(QStringLiteral("Bind"), bindHost_);
    connectionForm->addRow(QStringLiteral("Port"), port_);
    connectionForm->addRow(QStringLiteral("Public endpoint host"), publicHost_);
    connectionForm->addRow(QStringLiteral("STUN server"), stunServer_);
    connectionForm->addRow(QStringLiteral("STUN timeout ms"), stunTimeout_);
    connectionForm->addRow(QStringLiteral("STUN retries"), stunRetries_);
    connectionForm->addRow(QString(), noStun_);
    connectionForm->addRow(
        QStringLiteral("Maximum peers (0 = unlimited)"), maximumPeers_);
    auto* connectionBox = new QGroupBox(QStringLiteral("Connection"), content);
    connectionBox->setLayout(connectionForm);
    contentLayout->addWidget(connectionBox);

    profile_ = new QComboBox(content);
    for (const jam2::CreateProfile& profile : jam2::create_profiles()) {
        profile_->addItem(view(profile.label), view(profile.name));
    }
    selectData(profile_, initial.create.tuning.profile);
    device_ = new QComboBox(content);
    device_->setEditable(false);
    device_->setMinimumWidth(280);
    setDeviceList(std::move(devices));
    inputChannels_ = new QLineEdit(initial.audio.inputChannels, content);
    outputChannels_ = new QLineEdit(initial.audio.outputChannels, content);
    sampleRate_ = fixedSampleRateCombo(content, initial.create.sampleRate);
    bufferSize_ = fixedBufferSizeCombo(content, initial.create.tuning.bufferSize);
    frameSize_ = spin(content, 32, 256, initial.create.tuning.frameSize);
    audioFormat_ = new QComboBox(content);
    audioFormat_->addItem(QStringLiteral("16-bit PCM"), QStringLiteral("pcm16-mono"));
    audioFormat_->addItem(QStringLiteral("24-bit PCM"), QStringLiteral("pcm24-mono"));
    selectData(audioFormat_, initial.create.audioFormat);
    prefill_ = spin(content, 0, 65536, initial.create.tuning.prefillFrames);
    playbackMaximum_ = spin(
        content, 0, 65536, initial.create.tuning.playbackMaxFrames);
    captureRing_ = spin(
        content, 1, 1048576, initial.create.tuning.captureRingFrames);
    playbackRing_ = spin(
        content, 1, 1048576, initial.create.tuning.playbackRingFrames);
    auto* testDevice = new QPushButton(QStringLiteral("Test Device"), content);

    auto* audioForm = new QFormLayout();
    audioForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    audioForm->addRow(QStringLiteral("Profile"), profile_);
    audioForm->addRow(QStringLiteral("Audio device"), device_);
    audioForm->addRow(QStringLiteral("Input channels"), inputChannels_);
    audioForm->addRow(QStringLiteral("Output channels"), outputChannels_);
    audioForm->addRow(QStringLiteral("Sample rate"), sampleRate_);
    audioForm->addRow(QStringLiteral("Audio buffer size"), bufferSize_);
    audioForm->addRow(QString(), testDevice);
    audioForm->addRow(QStringLiteral("Frame size"), frameSize_);
    audioForm->addRow(QStringLiteral("Audio quality"), audioFormat_);
    audioForm->addRow(QStringLiteral("Playback prefill frames"), prefill_);
    audioForm->addRow(QStringLiteral("Playback max frames"), playbackMaximum_);
    audioForm->addRow(QStringLiteral("Capture ring frames"), captureRing_);
    audioForm->addRow(QStringLiteral("Playback ring frames"), playbackRing_);
    auto* audioBox = new QGroupBox(QStringLiteral("Local Audio"), content);
    audioBox->setLayout(audioForm);
    contentLayout->addWidget(audioBox);

    waitMs_ = spin(content, 0, 24 * 60 * 60 * 1000, initial.create.runtime.waitMs);
    streamMs_ = spin(
        content, 0, 24 * 60 * 60 * 1000, initial.create.runtime.streamMs);
    streamLingerMs_ = spin(
        content, 0, 60000, initial.create.runtime.streamLingerMs);
    diagnostics_ = new QCheckBox(QStringLiteral("Connection diagnostics"), content);
    diagnostics_->setChecked(initial.create.runtime.diagnostics);
    diagnosticsWarmup_ = spin(
        content, 0, 600000, initial.create.runtime.diagnosticsWarmupMs);
    diagnosticsFolder_ = new QLineEdit(initial.create.runtime.logStatsFolder, content);
    socketSendBuffer_ = spin(
        content, 0, std::numeric_limits<int>::max(), initial.create.socketSendBuffer);
    socketReceiveBuffer_ = spin(
        content, 0, std::numeric_limits<int>::max(), initial.create.socketRecvBuffer);
    osPriority_ = new QComboBox(content);
    osPriority_->addItem(QStringLiteral("High"), QStringLiteral("high"));
    osPriority_->addItem(QStringLiteral("Off"), QStringLiteral("off"));
    selectData(osPriority_, initial.create.runtime.osPriority);
    driftCorrection_ = new QCheckBox(QStringLiteral("Drift correction"), content);
    driftCorrection_->setChecked(initial.create.tuning.driftCorrection);
    driftSmoothing_ = decimalSpin(
        content, 0.0, 1.0, initial.create.tuning.driftSmoothing, 3, 0.005);
    driftDeadband_ = spin(
        content, 0, 50000, initial.create.tuning.driftDeadbandPpm);
    driftMaximum_ = spin(
        content, 0, 50000, initial.create.tuning.driftMaxCorrectionPpm);
    sampleTimePlayout_ = new QCheckBox(QStringLiteral("Sample-time playout"), content);
    sampleTimePlayout_->setChecked(initial.create.tuning.sampleTimePlayout);
    playoutDelay_ = spin(
        content, 0, 1048576, initial.create.tuning.playoutDelayFrames);
    jitterBuffer_ = spin(
        content, 0, 1048576, initial.create.tuning.jitterBufferFrames);
    jitterBufferMaximum_ = spin(
        content, 0, 1048576, initial.create.tuning.jitterBufferMaxFrames);
    adaptiveCushion_ = new QCheckBox(QStringLiteral("Adaptive cushion"), content);
    adaptiveCushion_->setChecked(initial.create.tuning.adaptiveCushion);
    adaptiveTarget_ = spin(
        content, 0, 1048576, initial.create.tuning.adaptiveTargetFrames);
    adaptiveMinimum_ = spin(
        content, 0, 1048576, initial.create.tuning.adaptiveMinFrames);
    adaptiveMaximum_ = spin(
        content, 0, 1048576, initial.create.tuning.adaptiveMaxFrames);
    adaptiveRelease_ = spin(
        content, 0, 1000000, initial.create.tuning.adaptiveReleasePpm);
    adaptiveRatioRamp_ = spin(
        content, 0, 60000, initial.create.tuning.adaptiveRatioRampMs);

    auto* advancedForm = new QFormLayout();
    advancedForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    advancedForm->addRow(QStringLiteral("Wait ms"), waitMs_);
    advancedForm->addRow(QStringLiteral("Stream ms"), streamMs_);
    advancedForm->addRow(QStringLiteral("Stream linger ms"), streamLingerMs_);
    advancedForm->addRow(QString(), diagnostics_);
    advancedForm->addRow(QStringLiteral("Stats warmup ms"), diagnosticsWarmup_);
    advancedForm->addRow(QStringLiteral("Log stats folder"), diagnosticsFolder_);
    advancedForm->addRow(QStringLiteral("Socket send buffer"), socketSendBuffer_);
    advancedForm->addRow(QStringLiteral("Socket recv buffer"), socketReceiveBuffer_);
    advancedForm->addRow(QStringLiteral("OS priority"), osPriority_);
    advancedForm->addRow(QString(), driftCorrection_);
    advancedForm->addRow(QStringLiteral("Drift smoothing"), driftSmoothing_);
    advancedForm->addRow(QStringLiteral("Drift deadband ppm"), driftDeadband_);
    advancedForm->addRow(QStringLiteral("Drift max correction ppm"), driftMaximum_);
    advancedForm->addRow(QString(), sampleTimePlayout_);
    advancedForm->addRow(QStringLiteral("Playout delay frames"), playoutDelay_);
    advancedForm->addRow(QStringLiteral("Jitter buffer frames"), jitterBuffer_);
    advancedForm->addRow(
        QStringLiteral("Jitter buffer max frames"), jitterBufferMaximum_);
    advancedForm->addRow(QString(), adaptiveCushion_);
    advancedForm->addRow(QStringLiteral("Adaptive target frames"), adaptiveTarget_);
    advancedForm->addRow(QStringLiteral("Adaptive min frames"), adaptiveMinimum_);
    advancedForm->addRow(QStringLiteral("Adaptive max frames"), adaptiveMaximum_);
    advancedForm->addRow(QStringLiteral("Adaptive release ppm"), adaptiveRelease_);
    advancedForm->addRow(
        QStringLiteral("Adaptive ratio ramp ms"), adaptiveRatioRamp_);
    auto* advancedBox = new QGroupBox(QStringLiteral("Engine Options"), content);
    advancedBox->setLayout(advancedForm);
    contentLayout->addWidget(advancedBox);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setWidget(content);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    auto* start = buttons->addButton(QStringLiteral("Start"), QDialogButtonBox::AcceptRole);
    auto* saveDefaults = buttons->addButton(
        QStringLiteral("Save Defaults"), QDialogButtonBox::ActionRole);
    auto* refresh = buttons->addButton(
        QStringLiteral("Refresh Devices"), QDialogButtonBox::ActionRole);
    auto* regenerate = buttons->addButton(
        QStringLiteral("New Session"), QDialogButtonBox::ActionRole);

    registerDialogField(*bindHost_, "bind-host", "session.connection-settings");
    registerDialogField(*port_, "port", "session.connection-settings");
    registerDialogField(*publicHost_, "public-host", "session.connection-settings");
    registerDialogField(*stunServer_, "stun-server", "session.connection-settings");
    registerDialogField(*stunTimeout_, "stun-timeout", "session.connection-settings");
    registerDialogField(*stunRetries_, "stun-retries", "session.connection-settings");
    registerDialogField(*noStun_, "no-stun", "session.connection-settings");
    registerDialogField(*maximumPeers_, "maximum-peers", "session.connection-settings");
    registerDialogField(*profile_, "create-profile", "session.audio-settings");
    registerDialogField(
        *device_, "audio-device", "session.audio-device",
        GuiControlAvailability::HardwareProfile);
    registerDialogField(*inputChannels_, "input-channels", "session.audio-settings");
    registerDialogField(*outputChannels_, "output-channels", "session.audio-settings");
    registerDialogField(*frameSize_, "frame-size", "session.audio-settings");
    registerDialogField(*audioFormat_, "network-audio-format", "session.audio-settings");
    registerDialogField(*prefill_, "playback-prefill", "session.audio-settings");
    registerDialogField(*playbackMaximum_, "playback-maximum", "session.audio-settings");
    registerDialogField(*captureRing_, "capture-ring", "session.audio-settings");
    registerDialogField(*playbackRing_, "playback-ring", "session.audio-settings");
    registerDialogField(*waitMs_, "wait-ms", "session.runtime-settings");
    registerDialogField(*streamMs_, "stream-ms", "session.runtime-settings");
    registerDialogField(*streamLingerMs_, "stream-linger-ms", "session.runtime-settings");
    registerDialogField(*diagnostics_, "diagnostics", "session.diagnostics-settings");
    registerDialogField(
        *diagnosticsWarmup_, "stats-warmup-ms", "session.diagnostics-settings");
    registerDialogField(
        *diagnosticsFolder_, "stats-folder", "session.diagnostics-settings");
    registerDialogField(
        *socketSendBuffer_, "socket-send-buffer", "session.connection-settings");
    registerDialogField(
        *socketReceiveBuffer_, "socket-recv-buffer", "session.connection-settings");
    registerDialogField(*osPriority_, "os-priority", "session.runtime-settings");
    registerDialogField(*driftCorrection_, "drift-correction", "session.drift-settings");
    registerDialogField(*driftSmoothing_, "drift-smoothing", "session.drift-settings");
    registerDialogField(*driftDeadband_, "drift-deadband", "session.drift-settings");
    registerDialogField(*driftMaximum_, "drift-maximum", "session.drift-settings");
    registerDialogField(
        *sampleTimePlayout_, "sample-time-playout", "session.playout-settings");
    registerDialogField(*playoutDelay_, "playout-delay", "session.playout-settings");
    registerDialogField(*jitterBuffer_, "jitter-buffer", "session.playout-settings");
    registerDialogField(
        *jitterBufferMaximum_, "jitter-buffer-maximum", "session.playout-settings");
    registerDialogField(*adaptiveCushion_, "adaptive-cushion", "session.playout-settings");
    registerDialogField(*adaptiveTarget_, "adaptive-target", "session.playout-settings");
    registerDialogField(*adaptiveMinimum_, "adaptive-minimum", "session.playout-settings");
    registerDialogField(*adaptiveMaximum_, "adaptive-maximum", "session.playout-settings");
    registerDialogField(*adaptiveRelease_, "adaptive-release", "session.playout-settings");
    registerDialogField(
        *adaptiveRatioRamp_, "adaptive-ratio-ramp", "session.playout-settings");
    registerDialogAction(*sampleRate_, "start", "sample-rate", "session.audio-settings");
    registerDialogAction(*bufferSize_, "start", "buffer-size", "session.audio-settings");
    registerDialogAction(
        *testDevice, "start", "test-device", "session.audio-device-test",
        GuiControlAvailability::HardwareProfile);
    registerDialogAction(*start, "start", "accept", "session.create-lifecycle");
    registerDialogAction(
        *saveDefaults, "start", "save-defaults", "session.create-defaults");
    registerDialogAction(
        *refresh, "start", "refresh-devices", "session.audio-device-refresh");
    registerDialogAction(
        *regenerate, "start", "new-session", "session.create-credentials");
    registerDialogAction(
        *buttons->button(QDialogButtonBox::Cancel), "start", "cancel",
        "session.create-lifecycle");

    styleEditors({
        bindHost_, port_, publicHost_, stunServer_, stunTimeout_, stunRetries_,
        maximumPeers_, profile_, device_, inputChannels_, outputChannels_,
        sampleRate_, bufferSize_, frameSize_, audioFormat_, prefill_,
        playbackMaximum_, captureRing_, playbackRing_, waitMs_, streamMs_,
        streamLingerMs_, diagnosticsWarmup_, diagnosticsFolder_, socketSendBuffer_,
        socketReceiveBuffer_, osPriority_, driftSmoothing_, driftDeadband_,
        driftMaximum_, playoutDelay_, jitterBuffer_, jitterBufferMaximum_,
        adaptiveTarget_, adaptiveMinimum_, adaptiveMaximum_, adaptiveRelease_,
        adaptiveRatioRamp_});

    connect(profile_, qOverload<int>(&QComboBox::currentIndexChanged), this,
        [this] { applyCreateProfile(profile_->currentData().toString()); });
    connect(noStun_, &QCheckBox::toggled, this,
        [this](bool checked) { updateStunAvailability(checked); });
    connect(refresh, &QPushButton::clicked, this, [this] {
        if (callbacks_.refreshDevices) {
            setDeviceList(callbacks_.refreshDevices(
                device_->currentData().toString()));
        }
    });
    connect(regenerate, &QPushButton::clicked, this, [this] {
        if (callbacks_.generateSession) callbacks_.generateSession();
    });
    connect(testDevice, &QPushButton::clicked, this, [this, testDevice] {
        if (callbacks_.testDevice) callbacks_.testDevice(device_, testDevice, this);
    });
    connect(saveDefaults, &QPushButton::clicked, this, [this] {
        if (callbacks_.saveDefaults) callbacks_.saveDefaults(state());
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* outer = new QVBoxLayout(this);
    outer->addWidget(scroll, 1);
    outer->addWidget(buttons);
    start->setDefault(true);
    updateStunAvailability(noStun_->isChecked());
}

StartJamDialogState StartJamDialog::state() const
{
    StartJamDialogState result;
    result.create = createTemplate_;
    result.create.bindHost = bindHost_->text().trimmed();
    result.create.port = port_->value();
    result.create.publicHost = publicHost_->text().trimmed();
    result.create.stunServer = stunServer_->text().trimmed();
    result.create.stunTimeoutMs = stunTimeout_->value();
    result.create.stunRetries = stunRetries_->value();
    result.create.noStun = noStun_->isChecked();
    result.create.maxPeers = maximumPeers_->value();
    result.create.sampleRate = sampleRate_->currentData().toInt();
    result.create.audioFormat = audioFormat_->currentData().toString();
    result.create.socketSendBuffer = socketSendBuffer_->value();
    result.create.socketRecvBuffer = socketReceiveBuffer_->value();
    result.create.tuning.profile = profile_->currentData().toString();
    result.create.tuning.bufferSize = bufferSize_->currentData().toInt();
    result.create.tuning.frameSize = frameSize_->value();
    result.create.tuning.prefillFrames = prefill_->value();
    result.create.tuning.playbackMaxFrames = playbackMaximum_->value();
    result.create.tuning.captureRingFrames = captureRing_->value();
    result.create.tuning.playbackRingFrames = playbackRing_->value();
    result.create.tuning.driftCorrection = driftCorrection_->isChecked();
    result.create.tuning.driftSmoothing = driftSmoothing_->value();
    result.create.tuning.driftDeadbandPpm = driftDeadband_->value();
    result.create.tuning.driftMaxCorrectionPpm = driftMaximum_->value();
    result.create.tuning.sampleTimePlayout = sampleTimePlayout_->isChecked();
    result.create.tuning.playoutDelayFrames = playoutDelay_->value();
    result.create.tuning.jitterBufferFrames = jitterBuffer_->value();
    result.create.tuning.jitterBufferMaxFrames = jitterBufferMaximum_->value();
    result.create.tuning.adaptiveCushion = adaptiveCushion_->isChecked();
    result.create.tuning.adaptiveTargetFrames = adaptiveTarget_->value();
    result.create.tuning.adaptiveMinFrames = adaptiveMinimum_->value();
    result.create.tuning.adaptiveMaxFrames = adaptiveMaximum_->value();
    result.create.tuning.adaptiveReleasePpm = adaptiveRelease_->value();
    result.create.tuning.adaptiveRatioRampMs = adaptiveRatioRamp_->value();
    result.create.runtime.diagnostics = diagnostics_->isChecked();
    result.create.runtime.diagnosticsWarmupMs = diagnosticsWarmup_->value();
    result.create.runtime.logStatsFolder = diagnosticsFolder_->text().trimmed();
    result.create.runtime.osPriority = osPriority_->currentData().toString();
    result.create.runtime.waitMs = waitMs_->value();
    result.create.runtime.streamMs = streamMs_->value();
    result.create.runtime.streamLingerMs = streamLingerMs_->value();
    result.audio = audioTemplate_;
    result.audio.inputChannels = inputChannels_->text().trimmed();
    result.audio.outputChannels = outputChannels_->text().trimmed();
    result.audio.sampleRate = result.create.sampleRate;
    result.audio.bufferSize = result.create.tuning.bufferSize;
    result.selectedDeviceId = device_->currentData().toString();
    return result;
}

void StartJamDialog::applyCreateProfile(const QString& name)
{
    const QByteArray utf8 = name.toUtf8();
    const jam2::CreateProfile* profile = jam2::find_create_profile(
        std::string_view(utf8.constData(), static_cast<std::size_t>(utf8.size())));
    if (profile == nullptr) profile = &jam2::default_create_profile();
    selectData(sampleRate_, profile->sample_rate);
    frameSize_->setValue(profile->frame_size);
    applyJoinTuning(
        *profile->local, bufferSize_, prefill_, playbackMaximum_, captureRing_,
        playbackRing_, driftCorrection_, driftSmoothing_, driftDeadband_,
        driftMaximum_, sampleTimePlayout_, playoutDelay_, jitterBuffer_,
        jitterBufferMaximum_, adaptiveCushion_, adaptiveTarget_, adaptiveMinimum_,
        adaptiveMaximum_, adaptiveRelease_, adaptiveRatioRamp_);
}

void StartJamDialog::setDeviceList(SessionAudioDeviceList devices)
{
    const QSignalBlocker blocker(device_);
    device_->clear();
    for (const SessionAudioDeviceChoice& choice : devices.devices) {
        device_->addItem(choice.label, choice.id);
    }
    const int selected = device_->findData(devices.selectedId);
    if (device_->count() > 0) device_->setCurrentIndex(selected >= 0 ? selected : 0);
}

void StartJamDialog::updateStunAvailability(bool noStun)
{
    publicHost_->setEnabled(noStun);
    stunServer_->setEnabled(!noStun);
    stunTimeout_->setEnabled(!noStun);
    stunRetries_->setEnabled(!noStun);
}

JoinJamDialog::JoinJamDialog(
    JoinJamDialogState initial,
    SessionAudioDeviceList devices,
    JoinJamDialogCallbacks callbacks,
    QWidget* parent)
    : QDialog(parent),
      callbacks_(std::move(callbacks)),
      joinTemplate_(initial.join),
      audioTemplate_(initial.audio)
{
    setWindowTitle(QStringLiteral("Join Jam"));
    resize(680, 520);

    auto* content = new QWidget(this);
    auto* contentLayout = new QVBoxLayout(content);
    inviteUrl_ = new QLineEdit(initial.inviteUrl, content);
    inviteUrl_->setMinimumWidth(420);
    bindHost_ = new QLineEdit(initial.join.bindHost, content);
    port_ = spin(content, 1, 65535, initial.join.port);
    auto* connectionForm = new QFormLayout();
    connectionForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    connectionForm->addRow(QStringLiteral("jam2 URL"), inviteUrl_);
    connectionForm->addRow(QStringLiteral("Local UDP bind host"), bindHost_);
    connectionForm->addRow(QStringLiteral("Local UDP bind port"), port_);
    auto* connectionBox = new QGroupBox(QStringLiteral("Connection"), content);
    connectionBox->setLayout(connectionForm);
    contentLayout->addWidget(connectionBox);

    profile_ = new QComboBox(content);
    for (const jam2::JoinProfile& profile : jam2::join_profiles()) {
        profile_->addItem(view(profile.label), view(profile.name));
    }
    selectData(profile_, initial.join.tuning.profile);
    device_ = new QComboBox(content);
    device_->setEditable(false);
    device_->setMinimumWidth(280);
    setDeviceList(std::move(devices));
    inputChannels_ = new QLineEdit(initial.audio.inputChannels, content);
    outputChannels_ = new QLineEdit(initial.audio.outputChannels, content);
    bufferSize_ = fixedBufferSizeCombo(content, initial.join.tuning.bufferSize);
    auto* testDevice = new QPushButton(QStringLiteral("Test Device"), content);
    prefill_ = spin(content, 0, 65536, initial.join.tuning.prefillFrames);
    playbackMaximum_ = spin(
        content, 0, 65536, initial.join.tuning.playbackMaxFrames);
    captureRing_ = spin(
        content, 1, 1048576, initial.join.tuning.captureRingFrames);
    playbackRing_ = spin(
        content, 1, 1048576, initial.join.tuning.playbackRingFrames);
    auto* audioForm = new QFormLayout();
    audioForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    audioForm->addRow(QStringLiteral("Join profile"), profile_);
    audioForm->addRow(QStringLiteral("Audio device"), device_);
    audioForm->addRow(QStringLiteral("Input channels"), inputChannels_);
    audioForm->addRow(QStringLiteral("Output channels"), outputChannels_);
    audioForm->addRow(QStringLiteral("Audio buffer size"), bufferSize_);
    audioForm->addRow(QString(), new QLabel(
        QStringLiteral("The creator supplies the session sample rate and frame size."),
        content));
    audioForm->addRow(QString(), testDevice);
    audioForm->addRow(QStringLiteral("Playback prefill frames"), prefill_);
    audioForm->addRow(QStringLiteral("Playback max frames"), playbackMaximum_);
    audioForm->addRow(QStringLiteral("Capture ring frames"), captureRing_);
    audioForm->addRow(QStringLiteral("Playback ring frames"), playbackRing_);
    auto* audioBox = new QGroupBox(QStringLiteral("Local Audio"), content);
    audioBox->setLayout(audioForm);
    contentLayout->addWidget(audioBox);

    diagnostics_ = new QCheckBox(QStringLiteral("Connection diagnostics"), content);
    diagnostics_->setChecked(initial.join.runtime.diagnostics);
    diagnosticsWarmup_ = spin(
        content, 0, 600000, initial.join.runtime.diagnosticsWarmupMs);
    diagnosticsFolder_ = new QLineEdit(initial.join.runtime.logStatsFolder, content);
    osPriority_ = new QComboBox(content);
    osPriority_->addItem(QStringLiteral("High"), QStringLiteral("high"));
    osPriority_->addItem(QStringLiteral("Off"), QStringLiteral("off"));
    selectData(osPriority_, initial.join.runtime.osPriority);
    waitMs_ = spin(content, 0, 24 * 60 * 60 * 1000, initial.join.runtime.waitMs);
    streamMs_ = spin(content, 0, 24 * 60 * 60 * 1000, initial.join.runtime.streamMs);
    streamLingerMs_ = spin(
        content, 0, 60000, initial.join.runtime.streamLingerMs);
    auto* statsForm = new QFormLayout();
    statsForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    statsForm->addRow(QString(), diagnostics_);
    statsForm->addRow(QStringLiteral("Stats warmup ms"), diagnosticsWarmup_);
    statsForm->addRow(QStringLiteral("Log stats folder"), diagnosticsFolder_);
    statsForm->addRow(QStringLiteral("OS priority"), osPriority_);
    statsForm->addRow(QStringLiteral("Wait ms"), waitMs_);
    statsForm->addRow(QStringLiteral("Stream ms"), streamMs_);
    statsForm->addRow(QStringLiteral("Stream linger ms"), streamLingerMs_);
    auto* statsBox = new QGroupBox(QStringLiteral("Local Stats"), content);
    statsBox->setLayout(statsForm);
    contentLayout->addWidget(statsBox);

    driftCorrection_ = new QCheckBox(QStringLiteral("Drift correction"), content);
    driftCorrection_->setChecked(initial.join.tuning.driftCorrection);
    driftSmoothing_ = decimalSpin(
        content, 0.0, 1.0, initial.join.tuning.driftSmoothing, 3, 0.005);
    driftDeadband_ = spin(content, 0, 50000, initial.join.tuning.driftDeadbandPpm);
    driftMaximum_ = spin(
        content, 0, 50000, initial.join.tuning.driftMaxCorrectionPpm);
    sampleTimePlayout_ = new QCheckBox(QStringLiteral("Sample-time playout"), content);
    sampleTimePlayout_->setChecked(initial.join.tuning.sampleTimePlayout);
    playoutDelay_ = spin(content, 0, 1048576, initial.join.tuning.playoutDelayFrames);
    jitterBuffer_ = spin(content, 0, 1048576, initial.join.tuning.jitterBufferFrames);
    jitterBufferMaximum_ = spin(
        content, 0, 1048576, initial.join.tuning.jitterBufferMaxFrames);
    adaptiveCushion_ = new QCheckBox(QStringLiteral("Adaptive cushion"), content);
    adaptiveCushion_->setChecked(initial.join.tuning.adaptiveCushion);
    adaptiveTarget_ = spin(content, 0, 1048576, initial.join.tuning.adaptiveTargetFrames);
    adaptiveMinimum_ = spin(content, 0, 1048576, initial.join.tuning.adaptiveMinFrames);
    adaptiveMaximum_ = spin(content, 0, 1048576, initial.join.tuning.adaptiveMaxFrames);
    adaptiveRelease_ = spin(content, 0, 1000000, initial.join.tuning.adaptiveReleasePpm);
    adaptiveRatioRamp_ = spin(content, 0, 60000, initial.join.tuning.adaptiveRatioRampMs);
    auto* tuningForm = new QFormLayout();
    tuningForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    tuningForm->addRow(QString(), driftCorrection_);
    tuningForm->addRow(QStringLiteral("Drift smoothing"), driftSmoothing_);
    tuningForm->addRow(QStringLiteral("Drift deadband ppm"), driftDeadband_);
    tuningForm->addRow(QStringLiteral("Drift max correction ppm"), driftMaximum_);
    tuningForm->addRow(QString(), sampleTimePlayout_);
    tuningForm->addRow(QStringLiteral("Playout delay frames"), playoutDelay_);
    tuningForm->addRow(QStringLiteral("Jitter buffer frames"), jitterBuffer_);
    tuningForm->addRow(
        QStringLiteral("Jitter buffer max frames"), jitterBufferMaximum_);
    tuningForm->addRow(QString(), adaptiveCushion_);
    tuningForm->addRow(QStringLiteral("Adaptive target frames"), adaptiveTarget_);
    tuningForm->addRow(QStringLiteral("Adaptive min frames"), adaptiveMinimum_);
    tuningForm->addRow(QStringLiteral("Adaptive max frames"), adaptiveMaximum_);
    tuningForm->addRow(QStringLiteral("Adaptive release ppm"), adaptiveRelease_);
    tuningForm->addRow(
        QStringLiteral("Adaptive ratio ramp ms"), adaptiveRatioRamp_);
    auto* tuningBox = new QGroupBox(QStringLiteral("Local Engine Options"), content);
    tuningBox->setLayout(tuningForm);
    contentLayout->addWidget(tuningBox);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setWidget(content);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    auto* join = buttons->addButton(QStringLiteral("Join"), QDialogButtonBox::AcceptRole);
    auto* saveDefaults = buttons->addButton(
        QStringLiteral("Save Defaults"), QDialogButtonBox::ActionRole);
    auto* refresh = buttons->addButton(
        QStringLiteral("Refresh Devices"), QDialogButtonBox::ActionRole);

    registerDialogField(*inviteUrl_, "invite-url", "session.connection-settings");
    registerDialogField(*bindHost_, "bind-host", "session.connection-settings");
    registerDialogField(*port_, "port", "session.connection-settings");
    registerDialogField(
        *device_, "audio-device", "session.audio-device",
        GuiControlAvailability::HardwareProfile);
    registerDialogField(*inputChannels_, "input-channels", "session.audio-settings");
    registerDialogField(*outputChannels_, "output-channels", "session.audio-settings");
    registerDialogField(*prefill_, "playback-prefill", "session.audio-settings");
    registerDialogField(*playbackMaximum_, "playback-maximum", "session.audio-settings");
    registerDialogField(*captureRing_, "capture-ring", "session.audio-settings");
    registerDialogField(*playbackRing_, "playback-ring", "session.audio-settings");
    registerDialogField(*waitMs_, "wait-ms", "session.runtime-settings");
    registerDialogField(*streamMs_, "stream-ms", "session.runtime-settings");
    registerDialogField(*streamLingerMs_, "stream-linger-ms", "session.runtime-settings");
    registerDialogField(*diagnostics_, "diagnostics", "session.diagnostics-settings");
    registerDialogField(
        *diagnosticsWarmup_, "stats-warmup-ms", "session.diagnostics-settings");
    registerDialogField(
        *diagnosticsFolder_, "stats-folder", "session.diagnostics-settings");
    registerDialogField(*osPriority_, "os-priority", "session.runtime-settings");
    registerDialogField(*driftCorrection_, "drift-correction", "session.drift-settings");
    registerDialogField(*driftSmoothing_, "drift-smoothing", "session.drift-settings");
    registerDialogField(*driftDeadband_, "drift-deadband", "session.drift-settings");
    registerDialogField(*driftMaximum_, "drift-maximum", "session.drift-settings");
    registerDialogField(
        *sampleTimePlayout_, "sample-time-playout", "session.playout-settings");
    registerDialogField(*playoutDelay_, "playout-delay", "session.playout-settings");
    registerDialogField(*jitterBuffer_, "jitter-buffer", "session.playout-settings");
    registerDialogField(
        *jitterBufferMaximum_, "jitter-buffer-maximum", "session.playout-settings");
    registerDialogField(*adaptiveCushion_, "adaptive-cushion", "session.playout-settings");
    registerDialogField(*adaptiveTarget_, "adaptive-target", "session.playout-settings");
    registerDialogField(*adaptiveMinimum_, "adaptive-minimum", "session.playout-settings");
    registerDialogField(*adaptiveMaximum_, "adaptive-maximum", "session.playout-settings");
    registerDialogField(*adaptiveRelease_, "adaptive-release", "session.playout-settings");
    registerDialogField(
        *adaptiveRatioRamp_, "adaptive-ratio-ramp", "session.playout-settings");
    registerDialogAction(*profile_, "join", "profile", "session.join-profile");
    registerDialogAction(*bufferSize_, "join", "buffer-size", "session.audio-settings");
    registerDialogAction(
        *testDevice, "join", "test-device", "session.audio-device-test",
        GuiControlAvailability::HardwareProfile);
    registerDialogAction(*join, "join", "accept", "session.join-lifecycle");
    registerDialogAction(
        *saveDefaults, "join", "save-defaults", "session.join-defaults");
    registerDialogAction(
        *refresh, "join", "refresh-devices", "session.audio-device-refresh");
    registerDialogAction(
        *buttons->button(QDialogButtonBox::Cancel), "join", "cancel",
        "session.join-lifecycle");

    styleEditors({
        inviteUrl_, bindHost_, port_, profile_, device_, inputChannels_,
        outputChannels_, bufferSize_, prefill_, playbackMaximum_, captureRing_,
        playbackRing_, diagnosticsWarmup_, diagnosticsFolder_, osPriority_,
        waitMs_, streamMs_, streamLingerMs_, driftSmoothing_, driftDeadband_,
        driftMaximum_, playoutDelay_, jitterBuffer_, jitterBufferMaximum_,
        adaptiveTarget_, adaptiveMinimum_, adaptiveMaximum_, adaptiveRelease_,
        adaptiveRatioRamp_});

    connect(profile_, qOverload<int>(&QComboBox::currentIndexChanged), this,
        [this] { applyJoinProfile(profile_->currentData().toString()); });
    connect(refresh, &QPushButton::clicked, this, [this] {
        if (callbacks_.refreshDevices) {
            setDeviceList(callbacks_.refreshDevices(
                device_->currentData().toString()));
        }
    });
    connect(testDevice, &QPushButton::clicked, this, [this, testDevice] {
        if (callbacks_.testDevice) callbacks_.testDevice(device_, testDevice, this);
    });
    connect(saveDefaults, &QPushButton::clicked, this, [this] {
        if (callbacks_.saveDefaults) callbacks_.saveDefaults(state());
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* outer = new QVBoxLayout(this);
    outer->addWidget(scroll, 1);
    outer->addWidget(buttons);
    join->setDefault(true);
}

JoinJamDialogState JoinJamDialog::state() const
{
    JoinJamDialogState result;
    result.inviteUrl = inviteUrl_->text().trimmed();
    result.join = joinTemplate_;
    result.join.bindHost = bindHost_->text().trimmed();
    result.join.port = port_->value();
    result.join.tuning.profile = profile_->currentData().toString();
    result.join.tuning.bufferSize = bufferSize_->currentData().toInt();
    result.join.tuning.prefillFrames = prefill_->value();
    result.join.tuning.playbackMaxFrames = playbackMaximum_->value();
    result.join.tuning.captureRingFrames = captureRing_->value();
    result.join.tuning.playbackRingFrames = playbackRing_->value();
    result.join.tuning.driftCorrection = driftCorrection_->isChecked();
    result.join.tuning.driftSmoothing = driftSmoothing_->value();
    result.join.tuning.driftDeadbandPpm = driftDeadband_->value();
    result.join.tuning.driftMaxCorrectionPpm = driftMaximum_->value();
    result.join.tuning.sampleTimePlayout = sampleTimePlayout_->isChecked();
    result.join.tuning.playoutDelayFrames = playoutDelay_->value();
    result.join.tuning.jitterBufferFrames = jitterBuffer_->value();
    result.join.tuning.jitterBufferMaxFrames = jitterBufferMaximum_->value();
    result.join.tuning.adaptiveCushion = adaptiveCushion_->isChecked();
    result.join.tuning.adaptiveTargetFrames = adaptiveTarget_->value();
    result.join.tuning.adaptiveMinFrames = adaptiveMinimum_->value();
    result.join.tuning.adaptiveMaxFrames = adaptiveMaximum_->value();
    result.join.tuning.adaptiveReleasePpm = adaptiveRelease_->value();
    result.join.tuning.adaptiveRatioRampMs = adaptiveRatioRamp_->value();
    result.join.runtime.diagnostics = diagnostics_->isChecked();
    result.join.runtime.diagnosticsWarmupMs = diagnosticsWarmup_->value();
    result.join.runtime.logStatsFolder = diagnosticsFolder_->text().trimmed();
    result.join.runtime.osPriority = osPriority_->currentData().toString();
    result.join.runtime.waitMs = waitMs_->value();
    result.join.runtime.streamMs = streamMs_->value();
    result.join.runtime.streamLingerMs = streamLingerMs_->value();
    result.audio = audioTemplate_;
    result.audio.inputChannels = inputChannels_->text().trimmed();
    result.audio.outputChannels = outputChannels_->text().trimmed();
    result.audio.bufferSize = result.join.tuning.bufferSize;
    result.selectedDeviceId = device_->currentData().toString();
    return result;
}

void JoinJamDialog::applyJoinProfile(const QString& name)
{
    const QByteArray utf8 = name.toUtf8();
    const jam2::JoinProfile* profile = jam2::find_join_profile(
        std::string_view(utf8.constData(), static_cast<std::size_t>(utf8.size())));
    if (profile == nullptr) profile = &jam2::default_join_profile();
    applyJoinTuning(
        *profile, bufferSize_, prefill_, playbackMaximum_, captureRing_,
        playbackRing_, driftCorrection_, driftSmoothing_, driftDeadband_,
        driftMaximum_, sampleTimePlayout_, playoutDelay_, jitterBuffer_,
        jitterBufferMaximum_, adaptiveCushion_, adaptiveTarget_, adaptiveMinimum_,
        adaptiveMaximum_, adaptiveRelease_, adaptiveRatioRamp_);
}

void JoinJamDialog::setDeviceList(SessionAudioDeviceList devices)
{
    const QSignalBlocker blocker(device_);
    device_->clear();
    for (const SessionAudioDeviceChoice& choice : devices.devices) {
        device_->addItem(choice.label, choice.id);
    }
    const int selected = device_->findData(devices.selectedId);
    if (device_->count() > 0) device_->setCurrentIndex(selected >= 0 ? selected : 0);
}

} // namespace jam2::gui

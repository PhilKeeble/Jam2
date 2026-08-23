#include "SettingsDialog.hpp"

#include "AudioDeviceUiSupport.hpp"

#include "GuiControlContract.hpp"
#include "GuiPresentation.hpp"
#include "MusicTheory.hpp"
#include "PracticeIdeaGenerator.hpp"
#include "StyleProfileCatalog.hpp"

#include "tuning_profile.hpp"

#include <QAbstractSpinBox>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <initializer_list>
#include <utility>

namespace jam2::gui {
namespace {

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

int preferredDeviceIndex(
    const QComboBox* combo,
    const std::vector<jam2::audio::DeviceInfo>& devices,
    const AudioDevicePreference& preference)
{
    if (combo == nullptr) return -1;
    for (const auto& device : devices) {
        const QString stable = QString::fromStdString(
            device.clsid.empty() ? device.name : device.clsid);
        if (QString::fromStdString(device.backend) == preference.backend &&
            stable == preference.stableId) {
            return combo->findData(QString::number(device.id));
        }
    }
    return -1;
}

void selectPreferredDevice(
    QComboBox* combo,
    const std::vector<jam2::audio::DeviceInfo>& devices,
    const AudioDevicePreference& preference)
{
    const int index = preferredDeviceIndex(combo, devices, preference);
    if (index >= 0) combo->setCurrentIndex(index);
}

} // namespace

std::optional<SettingsDialogResult> SettingsDialog::run(
    SettingsDialogInput input,
    SettingsDialogCallbacks callbacks,
    QWidget* parent)
{
    UserPreferences& preferences_ = input.preferences;
    const std::vector<jam2::audio::DeviceInfo>& availableDevices_ = input.devices;
    QVector<SettingsLoopbackSourceChoice> loopbackSources =
        std::move(input.loopbackSources);
    const bool networkActive = input.networkActive;

    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Settings"));
    const QRect availableSettingsGeometry = dialog.screen()->availableGeometry();
    const int settingsWidth = qMax(820, qMin(1180, availableSettingsGeometry.width() - 80));
    const int settingsHeight = qMax(620, qMin(820, availableSettingsGeometry.height() - 80));
    dialog.setMinimumSize(qMin(1000, settingsWidth), qMin(680, settingsHeight));
    dialog.resize(settingsWidth, settingsHeight);

    auto makeSpin = [&dialog](int value, int minimum, int maximum) {
        auto* spin = new QSpinBox(&dialog);
        spin->setRange(minimum, maximum);
        spin->setValue(value);
        spin->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        spin->setAttribute(Qt::WA_MacShowFocusRect, false);
        return spin;
    };
    auto makeDoubleSpin = [&dialog](double value, double minimum, double maximum, int decimals) {
        auto* spin = new QDoubleSpinBox(&dialog);
        spin->setRange(minimum, maximum);
        spin->setDecimals(decimals);
        spin->setValue(value);
        spin->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        spin->setAttribute(Qt::WA_MacShowFocusRect, false);
        return spin;
    };
    auto makePriority = [&dialog](const QString& value) {
        auto* combo = new QComboBox(&dialog);
        combo->addItem(QStringLiteral("High"), QStringLiteral("high"));
        combo->addItem(QStringLiteral("Off"), QStringLiteral("off"));
        combo->setCurrentIndex(qMax(0, combo->findData(value)));
        return combo;
    };
    auto makeScrollTab = [&dialog](QWidget* content) {
        auto* scroll = new QScrollArea(&dialog);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        scroll->setWidget(content);
        return scroll;
    };

    auto makeDeviceCombo = [&availableDevices_, &dialog](const AudioDevicePreference& preference) {
        auto* combo = new QComboBox(&dialog);
        for (const jam2::audio::DeviceInfo& device : availableDevices_) {
            combo->addItem(
                QStringLiteral("[%1] %2 %3")
                    .arg(device.id)
                    .arg(QString::fromStdString(device.backend),
                         QString::fromStdString(device.name)),
                QString::number(device.id));
        }
        selectPreferredDevice(combo, availableDevices_, preference);
        if (combo->currentIndex() < 0 && combo->count() > 0) combo->setCurrentIndex(0);
        return combo;
    };

    AudioDevicePreference localInitial = input.localAudio;

    auto* localDevice = makeDeviceCombo(localInitial);
    auto* localSampleRate = fixedSampleRateCombo(&dialog, localInitial.sampleRate);
    auto* localBufferSize = fixedBufferSizeCombo(&dialog, localInitial.bufferSize);
    auto* localInput = new QLineEdit(localInitial.inputChannels, &dialog);
    auto* localOutput = new QLineEdit(localInitial.outputChannels, &dialog);
    auto* localTest = new QPushButton(QStringLiteral("Test Device"), &dialog);
    auto* localApply = new QPushButton(QStringLiteral("Apply Audio"), &dialog);
    auto* localActions = new QWidget(&dialog);
    auto* localActionsLayout = new QHBoxLayout(localActions);
    localActionsLayout->setContentsMargins(0, 0, 0, 0);
    localActionsLayout->setSpacing(8);
    localActionsLayout->addWidget(localTest, 1);
    localActionsLayout->addWidget(localApply, 1);
    auto* localForm = new QFormLayout();
    localForm->addRow(QStringLiteral("Device"), localDevice);
    localForm->addRow(QStringLiteral("Sample rate"), localSampleRate);
    localForm->addRow(QStringLiteral("Buffer size"), localBufferSize);
    localForm->addRow(QStringLiteral("Input channels"), localInput);
    localForm->addRow(QStringLiteral("Output channels"), localOutput);
    localForm->addRow(QString(), localActions);
    auto* localBox = new QGroupBox(QStringLiteral("Local Audio"), &dialog);
    localBox->setLayout(localForm);

    struct NetworkAudioEditors {
        QComboBox* device = nullptr;
        QLineEdit* input = nullptr;
        QLineEdit* output = nullptr;
        QPushButton* test = nullptr;
        QGroupBox* box = nullptr;
    };
    auto makeNetworkAudioEditors = [&](const QString& title, const AudioDevicePreference& preference) {
        NetworkAudioEditors editors;
        editors.device = makeDeviceCombo(preference);
        editors.input = new QLineEdit(preference.inputChannels, &dialog);
        editors.output = new QLineEdit(preference.outputChannels, &dialog);
        editors.test = new QPushButton(QStringLiteral("Test Device"), &dialog);
        auto* form = new QFormLayout();
        form->addRow(QStringLiteral("Device"), editors.device);
        form->addRow(QStringLiteral("Input channels"), editors.input);
        form->addRow(QStringLiteral("Output channels"), editors.output);
        form->addRow(QString(), editors.test);
        editors.box = new QGroupBox(title, &dialog);
        editors.box->setLayout(form);
        return editors;
    };
    const NetworkAudioEditors networkAudio = makeNetworkAudioEditors(
        QStringLiteral("Network Audio"), preferences_.networkAudio);
    const NetworkAudioEditors createJamAudio = makeNetworkAudioEditors(
        QStringLiteral("Create Jam Audio"), preferences_.createJamAudio);
    const NetworkAudioEditors joinJamAudio = makeNetworkAudioEditors(
        QStringLiteral("Join Jam Audio"), preferences_.joinJamAudio);
    auto* splitNetworkAudio = new QCheckBox(
        QStringLiteral("Use different audio devices and channels for Create and Join"), &dialog);
    splitNetworkAudio->setChecked(preferences_.splitNetworkAudioByRole);

    auto* audioContent = new QWidget(&dialog);
    auto* audioLayout = new QVBoxLayout(audioContent);
    audioLayout->addWidget(localBox);
    audioLayout->addWidget(splitNetworkAudio);
    audioLayout->addWidget(networkAudio.box);
    audioLayout->addWidget(createJamAudio.box);
    audioLayout->addWidget(joinJamAudio.box);
    audioLayout->addStretch(1);
    auto audioFromEditors = [&availableDevices_](
        const AudioDevicePreference& original,
        const NetworkAudioEditors& editors)
    {
        AudioDevicePreference value = original;
        value.inputChannels = editors.input->text().trimmed();
        value.outputChannels = editors.output->text().trimmed();
        jam2::gui::storeSelectedDevicePreference(
            value, editors.device, availableDevices_);
        return value;
    };
    auto applyAudioToEditors = [&availableDevices_](
        const AudioDevicePreference& value,
        const NetworkAudioEditors& editors)
    {
        selectPreferredDevice(editors.device, availableDevices_, value);
        editors.input->setText(value.inputChannels);
        editors.output->setText(value.outputChannels);
    };
    auto updateNetworkAudioVisibility = [=] {
        const bool split = splitNetworkAudio->isChecked();
        networkAudio.box->setVisible(!split);
        createJamAudio.box->setVisible(split);
        joinJamAudio.box->setVisible(split);
    };
    QObject::connect(splitNetworkAudio, &QCheckBox::toggled, &dialog,
        [=, &preferences_, splitInitialized = preferences_.splitNetworkAudioByRole](bool checked) mutable {
            if (checked && !splitInitialized) {
                const AudioDevicePreference shared = audioFromEditors(
                    preferences_.networkAudio, networkAudio);
                applyAudioToEditors(shared, createJamAudio);
                applyAudioToEditors(shared, joinJamAudio);
                splitInitialized = true;
            }
            updateNetworkAudioVisibility();
        });
    updateNetworkAudioVisibility();

    auto* createBind = new QLineEdit(preferences_.create.bindHost, &dialog);
    auto* createPort = makeSpin(preferences_.create.port, 1, 65535);
    auto* createManualEndpoint = new QCheckBox(QStringLiteral("Use manual public endpoint (disable STUN)"), &dialog);
    createManualEndpoint->setChecked(preferences_.create.noStun);
    auto* createPublicHost = new QLineEdit(preferences_.create.publicHost, &dialog);
    auto* createStun = new QLineEdit(preferences_.create.stunServer, &dialog);
    auto* createStunTimeout = makeSpin(preferences_.create.stunTimeoutMs, 1, 60000);
    auto* createStunRetries = makeSpin(preferences_.create.stunRetries, 0, 100);
    auto* createMaxPeers = makeSpin(preferences_.create.maxPeers, 0, 1024);
    auto* createSocketSend = makeSpin(preferences_.create.socketSendBuffer, 0, 16777216);
    auto* createSocketReceive = makeSpin(preferences_.create.socketRecvBuffer, 0, 16777216);
    auto* connectionContent = new QWidget(&dialog);
    auto* connectionForm = new QFormLayout(connectionContent);
    connectionForm->addRow(QStringLiteral("Bind host"), createBind);
    connectionForm->addRow(QStringLiteral("Port"), createPort);
    connectionForm->addRow(QString(), createManualEndpoint);
    connectionForm->addRow(QStringLiteral("Public endpoint host"), createPublicHost);
    connectionForm->addRow(QStringLiteral("STUN server"), createStun);
    connectionForm->addRow(QStringLiteral("STUN timeout ms"), createStunTimeout);
    connectionForm->addRow(QStringLiteral("STUN retries"), createStunRetries);
    connectionForm->addRow(QStringLiteral("Maximum peers (0 = unlimited)"), createMaxPeers);
    connectionForm->addRow(QStringLiteral("Socket send buffer (0 = system)"), createSocketSend);
    connectionForm->addRow(QStringLiteral("Socket receive buffer (0 = system)"), createSocketReceive);
    auto updateDiscoveryControls = [=] {
        const bool manual = createManualEndpoint->isChecked();
        createPublicHost->setEnabled(manual);
        createStun->setEnabled(!manual);
        createStunTimeout->setEnabled(!manual);
        createStunRetries->setEnabled(!manual);
    };
    QObject::connect(createManualEndpoint, &QCheckBox::toggled, &dialog, updateDiscoveryControls);
    updateDiscoveryControls();

    struct TuningEditors {
        QComboBox* profile = nullptr;
        QComboBox* buffer = nullptr;
        QSpinBox* frame = nullptr;
        QSpinBox* prefill = nullptr;
        QSpinBox* playbackMax = nullptr;
        QSpinBox* captureRing = nullptr;
        QSpinBox* playbackRing = nullptr;
        QCheckBox* drift = nullptr;
        QDoubleSpinBox* driftSmoothing = nullptr;
        QSpinBox* driftDeadband = nullptr;
        QSpinBox* driftMax = nullptr;
        QCheckBox* sampleTime = nullptr;
        QSpinBox* playout = nullptr;
        QSpinBox* jitter = nullptr;
        QSpinBox* jitterMax = nullptr;
        QCheckBox* adaptive = nullptr;
        QSpinBox* adaptiveTarget = nullptr;
        QSpinBox* adaptiveMin = nullptr;
        QSpinBox* adaptiveMax = nullptr;
        QSpinBox* adaptiveRelease = nullptr;
        QSpinBox* adaptiveRamp = nullptr;
    };
    struct RuntimeEditors {
        QCheckBox* diagnostics = nullptr;
        QSpinBox* warmup = nullptr;
        QComboBox* priority = nullptr;
        QSpinBox* wait = nullptr;
        QSpinBox* stream = nullptr;
        QSpinBox* linger = nullptr;
    };
    auto addTuning = [&](QFormLayout* form, const LocalTuningPreference& p, bool creator) {
        TuningEditors e;
        e.profile = new QComboBox(&dialog);
        if (creator) {
            for (const jam2::CreateProfile& profile : jam2::create_profiles()) {
                e.profile->addItem(
                    QString::fromUtf8(profile.label.data(), static_cast<qsizetype>(profile.label.size())),
                    QString::fromUtf8(profile.name.data(), static_cast<qsizetype>(profile.name.size())));
            }
        } else {
            for (const jam2::JoinProfile& profile : jam2::join_profiles()) {
                e.profile->addItem(
                    QString::fromUtf8(profile.label.data(), static_cast<qsizetype>(profile.label.size())),
                    QString::fromUtf8(profile.name.data(), static_cast<qsizetype>(profile.name.size())));
            }
        }
        e.profile->setCurrentIndex(qMax(0, e.profile->findData(p.profile)));
        e.buffer = fixedBufferSizeCombo(&dialog, p.bufferSize);
        if (creator) {
            e.frame = makeSpin(p.frameSize, 32, 256);
        }
        e.prefill = makeSpin(p.prefillFrames, 0, 1048576);
        e.playbackMax = makeSpin(p.playbackMaxFrames, 0, 1048576);
        e.captureRing = makeSpin(p.captureRingFrames, 1, 1048576);
        e.playbackRing = makeSpin(p.playbackRingFrames, 1, 1048576);
        e.drift = new QCheckBox(QStringLiteral("Drift correction"), &dialog); e.drift->setChecked(p.driftCorrection);
        e.driftSmoothing = makeDoubleSpin(p.driftSmoothing, 0.0, 1.0, 3); e.driftSmoothing->setSingleStep(0.005);
        e.driftDeadband = makeSpin(p.driftDeadbandPpm, 0, 50000);
        e.driftMax = makeSpin(p.driftMaxCorrectionPpm, 0, 50000);
        e.sampleTime = new QCheckBox(QStringLiteral("Sample-time playout"), &dialog); e.sampleTime->setChecked(p.sampleTimePlayout);
        e.playout = makeSpin(p.playoutDelayFrames, 0, 1048576);
        e.jitter = makeSpin(p.jitterBufferFrames, 0, 1048576);
        e.jitterMax = makeSpin(p.jitterBufferMaxFrames, 0, 1048576);
        e.adaptive = new QCheckBox(QStringLiteral("Adaptive playback cushion"), &dialog); e.adaptive->setChecked(p.adaptiveCushion);
        e.adaptiveTarget = makeSpin(p.adaptiveTargetFrames, 0, 1048576);
        e.adaptiveMin = makeSpin(p.adaptiveMinFrames, 0, 1048576);
        e.adaptiveMax = makeSpin(p.adaptiveMaxFrames, 0, 1048576);
        e.adaptiveRelease = makeSpin(p.adaptiveReleasePpm, 0, 1000000);
        e.adaptiveRamp = makeSpin(p.adaptiveRatioRampMs, 0, 60000);
        form->addRow(QStringLiteral("Profile"), e.profile);
        form->addRow(QStringLiteral("Local device buffer"), e.buffer);
        if (e.frame != nullptr) {
            form->addRow(QStringLiteral("Session frame size"), e.frame);
        }
        form->addRow(QStringLiteral("Playback prefill frames"), e.prefill);
        form->addRow(QStringLiteral("Playback max frames"), e.playbackMax);
        form->addRow(QStringLiteral("Capture ring frames"), e.captureRing);
        form->addRow(QStringLiteral("Playback ring frames"), e.playbackRing);
        form->addRow(QString(), e.drift);
        form->addRow(QStringLiteral("Drift smoothing"), e.driftSmoothing);
        form->addRow(QStringLiteral("Drift deadband ppm"), e.driftDeadband);
        form->addRow(QStringLiteral("Drift max correction ppm"), e.driftMax);
        form->addRow(QString(), e.sampleTime);
        form->addRow(QStringLiteral("Playout delay frames"), e.playout);
        form->addRow(QStringLiteral("Jitter target frames"), e.jitter);
        form->addRow(QStringLiteral("Jitter max frames"), e.jitterMax);
        form->addRow(QString(), e.adaptive);
        form->addRow(QStringLiteral("Adaptive target frames"), e.adaptiveTarget);
        form->addRow(QStringLiteral("Adaptive minimum frames"), e.adaptiveMin);
        form->addRow(QStringLiteral("Adaptive maximum frames"), e.adaptiveMax);
        form->addRow(QStringLiteral("Adaptive release ppm"), e.adaptiveRelease);
        form->addRow(QStringLiteral("Adaptive ratio ramp ms"), e.adaptiveRamp);
        return e;
    };
    auto addRuntime = [&](QFormLayout* form, const RuntimePreference& p) {
        RuntimeEditors e;
        e.diagnostics = new QCheckBox(QStringLiteral("Connection diagnostics and CSV logging"), &dialog);
        e.diagnostics->setChecked(p.diagnostics);
        e.warmup = makeSpin(p.diagnosticsWarmupMs, 0, 3600000);
        e.priority = makePriority(p.osPriority);
        e.wait = makeSpin(p.waitMs, 0, 86400000);
        e.stream = makeSpin(p.streamMs, 0, 86400000);
        e.linger = makeSpin(p.streamLingerMs, 0, 3600000);
        form->addRow(QString(), e.diagnostics);
        form->addRow(QStringLiteral("Stats warmup ms"), e.warmup);
        form->addRow(QStringLiteral("OS priority"), e.priority);
        form->addRow(QStringLiteral("Wait ms"), e.wait);
        form->addRow(QStringLiteral("Stream limit ms (0 = unlimited)"), e.stream);
        form->addRow(QStringLiteral("Stream linger ms"), e.linger);
        return e;
    };

    auto* createContent = new QWidget(&dialog);
    auto* createForm = new QFormLayout(createContent);
    auto* createRate = fixedSampleRateCombo(&dialog, preferences_.create.sampleRate);
    auto* createQuality = new QComboBox(&dialog);
    createQuality->addItem(QStringLiteral("16-bit PCM"), QStringLiteral("pcm16-mono"));
    createQuality->addItem(QStringLiteral("24-bit PCM"), QStringLiteral("pcm24-mono"));
    createQuality->setCurrentIndex(qMax(0, createQuality->findData(preferences_.create.audioFormat)));
    createForm->addRow(QStringLiteral("Session sample rate"), createRate);
    createForm->addRow(QStringLiteral("Audio quality"), createQuality);
    TuningEditors createTuning = addTuning(createForm, preferences_.create.tuning, true);
    RuntimeEditors createRuntime = addRuntime(createForm, preferences_.create.runtime);

    auto* joinContent = new QWidget(&dialog);
    auto* joinForm = new QFormLayout(joinContent);
    auto* joinBind = new QLineEdit(preferences_.join.bindHost, &dialog);
    auto* joinPort = makeSpin(preferences_.join.port, 1, 65535);
    joinForm->addRow(QStringLiteral("Local bind host"), joinBind);
    joinForm->addRow(QStringLiteral("Local bind port"), joinPort);
    auto* joinContractNotice = new QLabel(
        QStringLiteral("The creator supplies sample rate, frame size, and audio quality."), &dialog);
    joinContractNotice->setWordWrap(true);
    joinForm->addRow(QString(), joinContractNotice);
    TuningEditors joinTuning = addTuning(joinForm, preferences_.join.tuning, false);
    RuntimeEditors joinRuntime = addRuntime(joinForm, preferences_.join.runtime);

    auto applyJoinProfile = [](TuningEditors& e, const jam2::JoinProfile& p) {
        e.buffer->setCurrentIndex(qMax(0, e.buffer->findData(static_cast<int>(p.audio_buffer_size))));
        e.prefill->setValue(static_cast<int>(p.playback_prefill_frames));
        e.playbackMax->setValue(static_cast<int>(p.playback_max_frames));
        e.captureRing->setValue(static_cast<int>(p.capture_ring_frames));
        e.playbackRing->setValue(static_cast<int>(p.playback_ring_frames));
        e.drift->setChecked(p.drift_correction); e.driftSmoothing->setValue(p.drift_smoothing);
        e.driftDeadband->setValue(p.drift_deadband_ppm); e.driftMax->setValue(p.drift_max_correction_ppm);
        e.sampleTime->setChecked(p.sample_time_playout); e.playout->setValue(static_cast<int>(p.playout_delay_frames));
        e.jitter->setValue(static_cast<int>(p.jitter_buffer_frames)); e.jitterMax->setValue(static_cast<int>(p.jitter_buffer_max_frames));
        e.adaptive->setChecked(p.adaptive_playback_cushion);
        e.adaptiveTarget->setValue(static_cast<int>(p.adaptive_playback_target_frames));
        e.adaptiveMin->setValue(static_cast<int>(p.adaptive_playback_min_frames));
        e.adaptiveMax->setValue(static_cast<int>(p.adaptive_playback_max_frames));
        e.adaptiveRelease->setValue(p.adaptive_playback_release_ppm);
        e.adaptiveRamp->setValue(p.adaptive_playback_ratio_ramp_ms);
    };
    QObject::connect(createTuning.profile, qOverload<int>(&QComboBox::activated), &dialog,
        [=, &createTuning](int) {
            const auto* p = jam2::find_create_profile(createTuning.profile->currentData().toString().toStdString());
            if (!p || !p->local) return;
            createRate->setCurrentIndex(qMax(0, createRate->findData(p->sample_rate)));
            createTuning.frame->setValue(p->frame_size);
            applyJoinProfile(createTuning, *p->local);
        });
    QObject::connect(joinTuning.profile, qOverload<int>(&QComboBox::activated), &dialog,
        [=, &joinTuning](int) {
            const auto* p = jam2::find_join_profile(joinTuning.profile->currentData().toString().toStdString());
            if (p) applyJoinProfile(joinTuning, *p);
        });

    auto* logContent = new QWidget(&dialog);
    auto* logForm = new QFormLayout(logContent);
    auto* logFolder = new QLineEdit(preferences_.logging.folder, &dialog);
    auto* browseLogs = new QPushButton(QStringLiteral("Browse"), &dialog);
    auto* logRow = new QWidget(&dialog); auto* logRowLayout = new QHBoxLayout(logRow);
    logRowLayout->setContentsMargins(0, 0, 0, 0); logRowLayout->addWidget(logFolder, 1); logRowLayout->addWidget(browseLogs);
    logForm->addRow(QStringLiteral("GUI and CSV log folder"), logRow);
    auto* logNote = new QLabel(
        QStringLiteral("GUI jams write hidden 2-second CSV samples plus a final row. The GUI display remains compact and stdout remains quiet."),
        &dialog);
    logNote->setWordWrap(true); logForm->addRow(QString(), logNote);
    QObject::connect(browseLogs, &QPushButton::clicked, &dialog, [&dialog, logFolder] {
        const QString folder = QFileDialog::getExistingDirectory(
            &dialog,
            QStringLiteral("Log Folder"),
            logFolder->text(),
            QFileDialog::ShowDirsOnly);
        if (!folder.isEmpty()) logFolder->setText(QDir::toNativeSeparators(folder));
    });

    auto* recordingContent = new QWidget(&dialog);
    auto* recordingLayout = new QVBoxLayout(recordingContent);
    recordingLayout->setSpacing(12);
    auto* recordingTiles = new QWidget(recordingContent);
    auto* recordingTilesLayout = new QHBoxLayout(recordingTiles);
    recordingTilesLayout->setContentsMargins(0, 0, 0, 0);
    recordingTilesLayout->setSpacing(10);
    auto* jamRecordingTile = new QPushButton(QStringLiteral("JAM RECORDING\nGlobal multi-stem capture"), recordingTiles);
    auto* trackRecordingTile = new QPushButton(QStringLiteral("TRACK RECORDING\nInput, Jam Mix, or loopback"), recordingTiles);
    for (QPushButton* tile : {jamRecordingTile, trackRecordingTile}) {
        tile->setCheckable(true);
        tile->setMinimumHeight(62);
        tile->setStyleSheet(QStringLiteral(
            "QPushButton { text-align:left; color:#bdc8c6; background:#11191b; "
            "border:1px solid #344245; border-radius:5px; padding:9px 14px; }"
            "QPushButton:hover { color:#eef2ef; background:#182224; border-color:#506164; }"
            "QPushButton:checked { color:#f2c66d; background:#211b12; "
            "border-color:#8a6835; font-weight:600; }"));
        recordingTilesLayout->addWidget(tile, 1);
    }
    auto* recordingTileGroup = new QButtonGroup(recordingTiles);
    recordingTileGroup->setExclusive(true);
    recordingTileGroup->addButton(jamRecordingTile, 0);
    recordingTileGroup->addButton(trackRecordingTile, 1);
    auto* recordingPages = new QStackedWidget(recordingContent);
    auto* jamRecordingPage = new QWidget(recordingPages);
    auto* jamRecordingPageLayout = new QVBoxLayout(jamRecordingPage);
    jamRecordingPageLayout->setContentsMargins(0, 0, 0, 0);
    auto* trackRecordingPage = new QWidget(recordingPages);
    auto* trackRecordingPageLayout = new QVBoxLayout(trackRecordingPage);
    trackRecordingPageLayout->setContentsMargins(0, 0, 0, 0);
    recordingPages->addWidget(jamRecordingPage);
    recordingPages->addWidget(trackRecordingPage);
    QObject::connect(recordingTileGroup, &QButtonGroup::idClicked,
        recordingPages, &QStackedWidget::setCurrentIndex);
    jamRecordingTile->setChecked(true);
    recordingPages->setCurrentIndex(0);
    recordingLayout->addWidget(recordingTiles);
    recordingLayout->addWidget(recordingPages, 1);

    auto* preferredMode = new QComboBox(&dialog);
    preferredMode->addItem(QStringLiteral("Local Input (instrument)"), QStringLiteral("input"));
    preferredMode->addItem(QStringLiteral("Jam Mix (Jam2 input + peers)"), QStringLiteral("current-jam"));
    preferredMode->addItem(QStringLiteral("System Loopback (desktop audio)"), QStringLiteral("loopback"));
    preferredMode->setCurrentIndex(qMax(0, preferredMode->findData(preferences_.recording.preferredMode)));
    auto* preferredForm = new QFormLayout(); preferredForm->addRow(QStringLiteral("Preferred recording mode"), preferredMode);
    trackRecordingPageLayout->addLayout(preferredForm);

    auto* jamMixIncludeBacking = new QCheckBox(
        QStringLiteral("Include the Section backing track"), &dialog);
    auto* jamMixIncludeMetronome = new QCheckBox(
        QStringLiteral("Include the metronome in the WAV"), &dialog);
    jamMixIncludeBacking->setChecked(
        preferences_.recording.jamMixTrack.includeBackingTrack);
    jamMixIncludeMetronome->setChecked(
        preferences_.recording.jamMixTrack.includeMetronome);
    auto* jamMixNote = new QLabel(QStringLiteral(
        "Jam Mix records your local input together with audio received from Jam2 peers. It is internal Jam2 audio, not the operating-system loopback source."),
        &dialog);
    jamMixNote->setWordWrap(true);
    jamMixNote->setStyleSheet(QStringLiteral("color:#9eaaa9;"));
    auto* jamMixForm = new QFormLayout();
    jamMixForm->addRow(QString(), jamMixIncludeBacking);
    jamMixForm->addRow(QString(), jamMixIncludeMetronome);
    jamMixForm->addRow(QString(), jamMixNote);
    auto* jamMixBox = new QGroupBox(QStringLiteral("Jam Mix source"), &dialog);
    jamMixBox->setLayout(jamMixForm);
    trackRecordingPageLayout->addWidget(jamMixBox);

    auto* jamPromptName = new QCheckBox(
        QStringLiteral("Ask for a recording name when starting"), &dialog);
    jamPromptName->setChecked(preferences_.recording.jam.promptForName);
    auto* jamCompletion = new QComboBox(&dialog);
    jamCompletion->addItem(QStringLiteral("Ask whether to import"), QStringLiteral("ask"));
    jamCompletion->addItem(QStringLiteral("Open the import dialog"), QStringLiteral("import"));
    jamCompletion->addItem(QStringLiteral("Only show where files were saved"), QStringLiteral("notify"));
    jamCompletion->setCurrentIndex(qMax(0,
        jamCompletion->findData(preferences_.recording.jam.completionAction)));
    auto* jamImportMix = new QCheckBox(QStringLiteral("Mix (what you heard)"), &dialog);
    auto* jamImportMyInput = new QCheckBox(QStringLiteral("My Input"), &dialog);
    auto* jamImportTheirInput = new QCheckBox(QStringLiteral("Their Input"), &dialog);
    auto* jamImportInputsMix = new QCheckBox(QStringLiteral("Inputs Mix"), &dialog);
    auto* jamImportMetronome = new QCheckBox(QStringLiteral("Metronome"), &dialog);
    jamImportMix->setChecked(preferences_.recording.jam.importMix);
    jamImportMyInput->setChecked(preferences_.recording.jam.importMyInput);
    jamImportTheirInput->setChecked(preferences_.recording.jam.importTheirInput);
    jamImportInputsMix->setChecked(preferences_.recording.jam.importInputsMix);
    jamImportMetronome->setChecked(preferences_.recording.jam.importMetronome);
    auto* jamImportParts = new QWidget(&dialog);
    auto* jamImportPartsLayout = new QVBoxLayout(jamImportParts);
    jamImportPartsLayout->setContentsMargins(0, 0, 0, 0);
    jamImportPartsLayout->setSpacing(4);
    for (QCheckBox* check : {jamImportMix, jamImportMyInput, jamImportTheirInput,
             jamImportInputsMix, jamImportMetronome}) {
        jamImportPartsLayout->addWidget(check);
    }
    auto* jamRecordingNote = new QLabel(QStringLiteral(
        "Jam recordings are stored inside the current JamJar. The engine always captures the complete stem set; these choices control which stems are preselected when importing them into a Section."),
        &dialog);
    jamRecordingNote->setWordWrap(true);
    jamRecordingNote->setStyleSheet(QStringLiteral("color:#9eaaa9;"));
    auto* jamRecordingForm = new QFormLayout();
    jamRecordingForm->addRow(QString(), jamPromptName);
    jamRecordingForm->addRow(QStringLiteral("When recording finishes"), jamCompletion);
    jamRecordingForm->addRow(QStringLiteral("Default import tracks"), jamImportParts);
    jamRecordingForm->addRow(QString(), jamRecordingNote);
    auto* jamRecordingBox = new QGroupBox(QStringLiteral("Jam Recording"), &dialog);
    jamRecordingBox->setLayout(jamRecordingForm);
    jamRecordingPageLayout->addWidget(jamRecordingBox);
    jamRecordingPageLayout->addStretch(1);

    auto makeFolderRow = [&](const QString& value) {
        auto* edit = new QLineEdit(value, &dialog);
        auto* browse = new QPushButton(QStringLiteral("Browse"), &dialog);
        auto* row = new QWidget(&dialog); auto* layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0); layout->addWidget(edit, 1); layout->addWidget(browse);
        QObject::connect(browse, &QPushButton::clicked, &dialog, [&dialog, edit] {
            const QString folder = QFileDialog::getExistingDirectory(
                &dialog,
                QStringLiteral("Recording Folder"),
                edit->text(),
                QFileDialog::ShowDirsOnly);
            if (!folder.isEmpty()) edit->setText(QDir::toNativeSeparators(folder));
        });
        return std::pair<QLineEdit*, QWidget*>{edit, row};
    };
    const auto inputFolderRow = makeFolderRow(preferences_.recording.input.outputFolder);
    auto* inputUntilStopped = new QCheckBox(QStringLiteral("Record until stopped"), &dialog);
    inputUntilStopped->setChecked(preferences_.recording.input.recordUntilStopped);
    auto* inputDuration = makeSpin(preferences_.recording.input.durationBars, 1, 128);
    auto* inputCountIn = new QCheckBox(QStringLiteral("Count-in"), &dialog); inputCountIn->setChecked(preferences_.recording.input.countIn);
    auto* inputCountBars = makeSpin(preferences_.recording.input.countInBars, 1, 8);
    auto* inputCountMetro = new QCheckBox(QStringLiteral("Metronome during count-in"), &dialog); inputCountMetro->setChecked(preferences_.recording.input.countInMetronome);
    auto* inputKeepMetro = new QCheckBox(QStringLiteral("Keep metronome on while recording"), &dialog); inputKeepMetro->setChecked(preferences_.recording.input.keepMetronome);
    auto* inputLatency = makeSpin(preferences_.recording.input.latencyAdjustmentFrames, -8192, 8192);
    auto* inputForm = new QFormLayout();
    inputForm->addRow(QStringLiteral("Output folder"), inputFolderRow.second);
    inputForm->addRow(QString(), inputUntilStopped); inputForm->addRow(QStringLiteral("Duration bars"), inputDuration);
    inputForm->addRow(QString(), inputCountIn); inputForm->addRow(QStringLiteral("Count-in bars"), inputCountBars);
    inputForm->addRow(QString(), inputCountMetro); inputForm->addRow(QString(), inputKeepMetro);
    inputForm->addRow(QStringLiteral("Manual latency adjustment frames"), inputLatency);
    auto* inputBox = new QGroupBox(QStringLiteral("Local Input Recording"), &dialog); inputBox->setLayout(inputForm);
    trackRecordingPageLayout->addWidget(inputBox);
    inputDuration->setEnabled(!inputUntilStopped->isChecked());
    QObject::connect(inputUntilStopped, &QCheckBox::toggled, inputDuration, [=](bool checked) { inputDuration->setEnabled(!checked); });

    const auto loopFolderRow = makeFolderRow(preferences_.recording.loopback.outputFolder);
    auto* loopSource = new QComboBox(&dialog); loopSource->setEditable(true);
    auto populateLoopSources = [=, &preferences_, &loopbackSources] {
        const QString wanted = loopSource->currentData().toString().isEmpty()
            ? preferences_.recording.loopback.sourceId : loopSource->currentData().toString();
        loopSource->clear();
        for (const SettingsLoopbackSourceChoice& source : loopbackSources) {
            loopSource->addItem(source.label, source.id);
        }
        int index = loopSource->findData(wanted);
        if (index < 0) index = loopSource->findText(preferences_.recording.loopback.sourceName);
        if (index < 0) index = loopSource->findData(QStringLiteral("default"));
        loopSource->setCurrentIndex(qMax(0, index));
    };
    populateLoopSources();
    auto* refreshLoopSources = new QPushButton(QStringLiteral("Refresh Sources"), &dialog);
    auto* loopSourceRow = new QWidget(&dialog); auto* loopSourceLayout = new QHBoxLayout(loopSourceRow);
    loopSourceLayout->setContentsMargins(0, 0, 0, 0); loopSourceLayout->addWidget(loopSource, 1); loopSourceLayout->addWidget(refreshLoopSources);
    QObject::connect(refreshLoopSources, &QPushButton::clicked, &dialog,
        [=, &callbacks, &loopbackSources] {
            if (callbacks.refreshLoopbackSources) {
                loopbackSources = callbacks.refreshLoopbackSources();
            }
            populateLoopSources();
        });
    auto* loopUntilStopped = new QCheckBox(QStringLiteral("Record until stopped"), &dialog); loopUntilStopped->setChecked(preferences_.recording.loopback.recordUntilStopped);
    auto* loopDuration = makeSpin(preferences_.recording.loopback.durationBars, 1, 128);
    auto* loopSilenceThreshold = makeDoubleSpin(preferences_.recording.loopback.silenceThresholdDb, -120.0, 0.0, 1);
    auto* loopTailSilence = makeSpin(preferences_.recording.loopback.tailSilenceMs, 0, 30000);
    auto* loopTrimLeading = new QCheckBox(QStringLiteral("Trim leading silence"), &dialog); loopTrimLeading->setChecked(preferences_.recording.loopback.trimLeading);
    auto* loopTrimTrailing = new QCheckBox(QStringLiteral("Trim trailing silence"), &dialog); loopTrimTrailing->setChecked(preferences_.recording.loopback.trimTrailing);
    auto* loopForm = new QFormLayout();
    loopForm->addRow(QStringLiteral("Output folder"), loopFolderRow.second); loopForm->addRow(QStringLiteral("Loopback source"), loopSourceRow);
    loopForm->addRow(QString(), loopUntilStopped); loopForm->addRow(QStringLiteral("Duration bars"), loopDuration);
    loopForm->addRow(QStringLiteral("Silence threshold dB"), loopSilenceThreshold); loopForm->addRow(QStringLiteral("Tail silence ms"), loopTailSilence);
    loopForm->addRow(QString(), loopTrimLeading); loopForm->addRow(QString(), loopTrimTrailing);
    auto* loopBox = new QGroupBox(QStringLiteral("System Loopback Recording"), &dialog); loopBox->setLayout(loopForm);
    trackRecordingPageLayout->addWidget(loopBox); trackRecordingPageLayout->addStretch(1);
    loopDuration->setEnabled(!loopUntilStopped->isChecked());
    QObject::connect(loopUntilStopped, &QCheckBox::toggled, loopDuration, [=](bool checked) { loopDuration->setEnabled(!checked); });

    auto* generalContent = new QWidget(&dialog);
    auto* generalLayout = new QVBoxLayout(generalContent);
    auto* startupBox = new QGroupBox(QStringLiteral("When Jam2 opens"), generalContent);
    auto* startupForm = new QFormLayout(startupBox);
    auto* startupView = new QComboBox(startupBox);
    for (const auto& view : QList<QPair<QString, QString>>{
             {QStringLiteral("Performance"), QStringLiteral("performance")},
             {QStringLiteral("Chords"), QStringLiteral("chords")},
             {QStringLiteral("Drums"), QStringLiteral("drums")},
             {QStringLiteral("Lyrics"), QStringLiteral("lyrics")},
             {QStringLiteral("Metronome"), QStringLiteral("metronome")},
             {QStringLiteral("Track"), QStringLiteral("track")}}) {
        startupView->addItem(view.first, view.second);
    }
    startupView->setCurrentIndex(qMax(0, startupView->findData(preferences_.general.startupView)));
    auto* defaultBpm = makeSpin(preferences_.general.bpm, 20, 400);
    defaultBpm->setSuffix(QStringLiteral(" BPM"));
    auto* defaultMeter = new QComboBox(startupBox);
    for (const auto& meter : jam2::practice::meterCatalog()) {
        defaultMeter->addItem(meter.name, meter.id);
    }
    QString currentDefaultMeter;
    for (const auto& meter : jam2::practice::meterCatalog()) {
        if (meter.numerator == preferences_.general.meterNumerator &&
            meter.denominator == preferences_.general.meterDenominator &&
            meter.tempoPulseUnits == preferences_.general.tempoPulseUnits) {
            currentDefaultMeter = meter.id;
            break;
        }
    }
    defaultMeter->setCurrentIndex(qMax(0, defaultMeter->findData(currentDefaultMeter)));
    auto* defaultDivision = new QComboBox(startupBox);
    for (const auto& division : QList<QPair<QString, int>>{
             {QStringLiteral("Quarter"), 1}, {QStringLiteral("Eighth"), 2},
             {QStringLiteral("Triplet"), 3}, {QStringLiteral("16th"), 4},
             {QStringLiteral("6th"), 6}, {QStringLiteral("32nd"), 8}}) {
        defaultDivision->addItem(division.first, division.second);
    }
    defaultDivision->setCurrentIndex(qMax(0,
        defaultDivision->findData(preferences_.general.clickDivision)));
    auto* generateOnStartup = new QCheckBox(
        QStringLiteral("Generate an idea automatically"), startupBox);
    generateOnStartup->setChecked(preferences_.general.generateIdeaOnStartup);
    startupForm->addRow(QStringLiteral("Opening view"), startupView);
    startupForm->addRow(QStringLiteral("Default tempo"), defaultBpm);
    startupForm->addRow(QStringLiteral("Default meter"), defaultMeter);
    startupForm->addRow(QStringLiteral("Click division"), defaultDivision);
    startupForm->addRow(QString(), generateOnStartup);
    generalLayout->addWidget(startupBox);
    auto* generalNote = new QLabel(QStringLiteral(
        "These defaults initialise a new empty jam. Saved JamJars retain their own song timing and content."),
        generalContent);
    generalNote->setWordWrap(true);
    generalNote->setStyleSheet(QStringLiteral("color:#9eaaa9;"));
    generalLayout->addWidget(generalNote);
    generalLayout->addStretch(1);

    auto* ideaContent = new QWidget(&dialog);
    auto* ideaLayout = new QVBoxLayout(ideaContent);
    auto* generationBox = new QGroupBox(QStringLiteral("Generate Idea defaults"), ideaContent);
    auto* generationForm = new QFormLayout(generationBox);
    auto* ideaParts = new QComboBox(generationBox);
    ideaParts->addItem(QStringLiteral("Full arrangement"), 0);
    ideaParts->addItem(QStringLiteral("Chords, Bass & Melody Only"), 1);
    ideaParts->addItem(QStringLiteral("Drums Only"), 2);
    ideaParts->setCurrentIndex(qMax(0, ideaParts->findData(preferences_.ideas.parts)));
    auto* ideaKey = new QComboBox(generationBox);
    ideaKey->addItem(QStringLiteral("Random"), -1);
    const QStringList ideaKeyNames = jam2::practice::keyNames();
    for (int index = 0; index < ideaKeyNames.size(); ++index) {
        ideaKey->addItem(ideaKeyNames.at(index), index);
    }
    ideaKey->setCurrentIndex(qMax(0, ideaKey->findData(preferences_.ideas.key)));
    auto* ideaStyle = new QComboBox(generationBox);
    ideaStyle->addItem(QStringLiteral("Random"), QString());
    const QStringList ideaStyleNames = jam2::practice::chordStyleNames();
    const QStringList ideaStyleIds = jam2::practice::styleIds();
    for (int index = 0; index < ideaStyleNames.size(); ++index) {
        ideaStyle->addItem(ideaStyleNames.at(index), ideaStyleIds.value(index));
    }
    ideaStyle->addItem(QStringLiteral("Experimental - Modern Progressive Metalcore"),
        QStringLiteral("metal-experimental"));
    ideaStyle->setCurrentIndex(qMax(0, ideaStyle->findData(preferences_.ideas.styleId)));
    auto* ideaProfile = new QComboBox(generationBox);
    const auto refreshIdeaProfiles = [ideaStyle, ideaProfile](const QString& preferred = QString()) {
        ideaProfile->clear();
        ideaProfile->addItem(QStringLiteral("Random profile"), QString());
        const QString styleId = ideaStyle->currentData().toString();
        if (styleId == QStringLiteral("metal-experimental")) {
            ideaProfile->addItem(QStringLiteral("Modern Progressive Metalcore (sound test)"),
                QStringLiteral("metal_modern_progressive"));
        } else {
            const QStringList ids = jam2::practice::profileIds(styleId);
            const QStringList names = jam2::practice::profileNames(styleId);
            for (int index = 0; index < names.size(); ++index) {
                ideaProfile->addItem(names.at(index), ids.value(index));
            }
        }
        ideaProfile->setCurrentIndex(qMax(0, ideaProfile->findData(preferred)));
    };
    refreshIdeaProfiles(preferences_.ideas.profileId);
    QObject::connect(ideaStyle, &QComboBox::currentIndexChanged, &dialog,
        [refreshIdeaProfiles](int) { refreshIdeaProfiles(); });
    auto* ideaMeter = new QComboBox(generationBox);
    ideaMeter->addItem(QStringLiteral("Random compatible / use jam meter at startup"), QString());
    for (const auto& meter : jam2::practice::meterCatalog()) {
        ideaMeter->addItem(meter.name, meter.id);
    }
    ideaMeter->setCurrentIndex(qMax(0, ideaMeter->findData(preferences_.ideas.meterId)));
    auto* ideaLength = new QComboBox(generationBox);
    ideaLength->addItem(QStringLiteral("Current section / compatible default"), 0);
    for (int bars : {4, 8, 12, 16, 24, 32}) {
        ideaLength->addItem(QStringLiteral("%1 bars").arg(bars), bars);
    }
    ideaLength->setCurrentIndex(qMax(0, ideaLength->findData(preferences_.ideas.bars)));
    auto* ideaExactBpm = new QCheckBox(QStringLiteral("Use exact BPM"), generationBox);
    ideaExactBpm->setChecked(preferences_.ideas.exactBpm);
    auto* ideaBpm = makeSpin(preferences_.ideas.bpm, 20, 400);
    ideaBpm->setSuffix(QStringLiteral(" BPM"));
    ideaBpm->setEnabled(ideaExactBpm->isChecked());
    QObject::connect(ideaExactBpm, &QCheckBox::toggled, ideaBpm, &QSpinBox::setEnabled);
    auto* ideaTempoRow = new QWidget(generationBox);
    auto* ideaTempoLayout = new QHBoxLayout(ideaTempoRow);
    ideaTempoLayout->setContentsMargins(0, 0, 0, 0);
    ideaTempoLayout->addWidget(ideaExactBpm);
    ideaTempoLayout->addWidget(ideaBpm, 1);
    auto* ideaComplexity = makeSpin(preferences_.ideas.complexity, 1, 8);
    generationForm->addRow(QStringLiteral("Parts"), ideaParts);
    generationForm->addRow(QStringLiteral("Key"), ideaKey);
    generationForm->addRow(QStringLiteral("Style"), ideaStyle);
    generationForm->addRow(QStringLiteral("Profile"), ideaProfile);
    generationForm->addRow(QStringLiteral("Meter"), ideaMeter);
    generationForm->addRow(QStringLiteral("Length"), ideaLength);
    generationForm->addRow(QStringLiteral("Tempo"), ideaTempoRow);
    generationForm->addRow(QStringLiteral("Complexity"), ideaComplexity);
    ideaLayout->addWidget(generationBox);

    auto* wavBox = new QGroupBox(QStringLiteral("Reference WAV defaults"), ideaContent);
    auto* wavForm = new QFormLayout(wavBox);
    auto* startupWavs = new QCheckBox(
        QStringLiteral("Render reference WAVs for the startup idea"), wavBox);
    startupWavs->setChecked(preferences_.ideas.renderWavsOnStartup);
    startupWavs->setEnabled(generateOnStartup->isChecked());
    startupWavs->setToolTip(QStringLiteral(
        "Requires Generate an idea automatically in General"));
    QObject::connect(generateOnStartup, &QCheckBox::toggled,
        startupWavs, &QCheckBox::setEnabled);
    auto* renderChords = new QCheckBox(QStringLiteral("Chords"), wavBox);
    auto* renderDrums = new QCheckBox(QStringLiteral("Drums"), wavBox);
    auto* renderMelody = new QCheckBox(QStringLiteral("Melody"), wavBox);
    auto* renderBass = new QCheckBox(QStringLiteral("Bass"), wavBox);
    auto* renderSupport = new QCheckBox(QStringLiteral("Supporting line"), wavBox);
    renderChords->setChecked(preferences_.ideas.renderChords);
    renderDrums->setChecked(preferences_.ideas.renderDrums);
    renderMelody->setChecked(preferences_.ideas.renderMelody);
    renderBass->setChecked(preferences_.ideas.renderBass);
    renderSupport->setChecked(preferences_.ideas.renderSupport);
    auto* renderPartsRow = new QWidget(wavBox);
    auto* renderPartsLayout = new QHBoxLayout(renderPartsRow);
    renderPartsLayout->setContentsMargins(0, 0, 0, 0);
    for (QCheckBox* check : {renderChords, renderDrums, renderMelody, renderBass, renderSupport}) {
        renderPartsLayout->addWidget(check);
    }
    auto* chordVoicing = new QComboBox(wavBox);
    chordVoicing->addItems({QStringLiteral("Style default"), QStringLiteral("Close"),
        QStringLiteral("Spread"), QStringLiteral("Voice-led")});
    chordVoicing->setCurrentIndex(qBound(0, preferences_.ideas.chordVoicing, 3));
    auto* drumKit = new QComboBox(wavBox);
    drumKit->addItems({QStringLiteral("Style default"), QStringLiteral("Acoustic"),
        QStringLiteral("Electronic")});
    drumKit->setCurrentIndex(qBound(0, preferences_.ideas.drumKit, 2));
    wavForm->addRow(QString(), startupWavs);
    wavForm->addRow(QStringLiteral("Render parts"), renderPartsRow);
    wavForm->addRow(QStringLiteral("Chord voicing"), chordVoicing);
    wavForm->addRow(QStringLiteral("Drum kit"), drumKit);
    ideaLayout->addWidget(wavBox);

    auto* grooveBox = new QGroupBox(QStringLiteral("Groove Library defaults"), ideaContent);
    auto* grooveForm = new QFormLayout(grooveBox);
    auto* grooveTiming = new QComboBox(grooveBox);
    grooveTiming->addItem(QStringLiteral("Use groove timing"), true);
    grooveTiming->addItem(QStringLiteral("Keep section timing"), false);
    grooveTiming->setCurrentIndex(qMax(0,
        grooveTiming->findData(preferences_.ideas.grooveUseIdeaTiming)));
    auto* grooveLength = new QComboBox(grooveBox);
    grooveLength->addItem(QStringLiteral("Current section length"), 0);
    for (int bars : {8, 12, 16, 24, 32}) {
        grooveLength->addItem(QStringLiteral("%1 bars").arg(bars), bars);
    }
    grooveLength->setCurrentIndex(qMax(0,
        grooveLength->findData(preferences_.ideas.grooveBars)));
    grooveForm->addRow(QStringLiteral("Timing"), grooveTiming);
    grooveForm->addRow(QStringLiteral("Groove length"), grooveLength);
    ideaLayout->addWidget(grooveBox);
    ideaLayout->addStretch(1);

    auto* levelsContent = new QWidget(&dialog);
    auto* levelsForm = new QFormLayout(levelsContent);
    auto makeDbSpin = [&makeSpin](int value) {
        QSpinBox* spin = makeSpin(value, -60, 12);
        spin->setSuffix(QStringLiteral(" dB"));
        return spin;
    };
    auto* defaultSend = makeDbSpin(preferences_.levels.sendDb);
    auto* defaultMonitorEnabled = new QCheckBox(QStringLiteral("Monitor input by default"), levelsContent);
    defaultMonitorEnabled->setChecked(preferences_.levels.monitorInput);
    auto* defaultMonitor = makeDbSpin(preferences_.levels.monitorDb);
    auto* defaultMetronomeLevel = makeDbSpin(preferences_.levels.metronomeDb);
    auto* defaultMaster = makeDbSpin(preferences_.levels.masterDb);
    auto* defaultRemote = makeDbSpin(preferences_.levels.remotePeerDb);
    auto* defaultBacking = makeDbSpin(preferences_.levels.backingTrackDb);
    levelsForm->addRow(QStringLiteral("Local send"), defaultSend);
    levelsForm->addRow(QString(), defaultMonitorEnabled);
    levelsForm->addRow(QStringLiteral("Input monitor"), defaultMonitor);
    levelsForm->addRow(QStringLiteral("Metronome"), defaultMetronomeLevel);
    levelsForm->addRow(QStringLiteral("Master output"), defaultMaster);
    levelsForm->addRow(QStringLiteral("New remote peers"), defaultRemote);
    levelsForm->addRow(QStringLiteral("New backing tracks"), defaultBacking);

    auto* metronomeContent = new QWidget(&dialog);
    auto* metronomeLayout = new QVBoxLayout(metronomeContent);
    auto* metronomeGeneralBox = new QGroupBox(
        QStringLiteral("General metronome"), metronomeContent);
    auto* metronomeGeneralForm = new QFormLayout(metronomeGeneralBox);
    auto* defaultClickSound = new QComboBox(metronomeGeneralBox);
    defaultClickSound->addItems({QStringLiteral("Classic"), QStringLiteral("Woodblock"),
        QStringLiteral("Rim Click"), QStringLiteral("Digital Tick")});
    defaultClickSound->setCurrentIndex(qBound(0, preferences_.metronome.sound, 3));
    auto* defaultMetronomeMode = new QComboBox(metronomeGeneralBox);
    defaultMetronomeMode->addItem(
        QStringLiteral("Shared Grid"), QStringLiteral("shared-grid"));
    defaultMetronomeMode->addItem(
        QStringLiteral("Leader Audio"), QStringLiteral("leader-audio"));
    defaultMetronomeMode->addItem(
        QStringLiteral("Listener Compensated"), QStringLiteral("listener-compensated"));
    defaultMetronomeMode->setCurrentIndex(qMax(0,
        defaultMetronomeMode->findData(preferences_.metronome.mode)));
    metronomeGeneralForm->addRow(QStringLiteral("Click sound"), defaultClickSound);
    metronomeGeneralForm->addRow(QStringLiteral("Sync mode"), defaultMetronomeMode);
    metronomeLayout->addWidget(metronomeGeneralBox);

    auto* listenerTimingBox = new QGroupBox(
        QStringLiteral("Listener-compensated timing"), metronomeContent);
    auto* listenerTimingForm = new QFormLayout(listenerTimingBox);
    auto* compensationMax = makeDoubleSpin(preferences_.metronome.compensationMaxMs, 0, 1000, 1);
    auto* compensationSmoothing = makeDoubleSpin(preferences_.metronome.compensationSmoothingMs, 0, 10000, 1);
    auto* compensationDeadband = makeDoubleSpin(preferences_.metronome.compensationDeadbandMs, 0, 1000, 1);
    auto* compensationSlew = makeDoubleSpin(preferences_.metronome.compensationSlewMsPerSecond, 0, 10000, 1);
    compensationMax->setSuffix(QStringLiteral(" ms"));
    compensationSmoothing->setSuffix(QStringLiteral(" ms"));
    compensationDeadband->setSuffix(QStringLiteral(" ms"));
    compensationSlew->setSuffix(QStringLiteral(" ms/s"));
    auto* listenerTimingNote = new QLabel(
        QStringLiteral(
            "These saved defaults are used only when the sync mode is Listener Compensated."),
        listenerTimingBox);
    listenerTimingNote->setWordWrap(true);
    listenerTimingNote->setStyleSheet(QStringLiteral("color:#9eaaa8;"));
    listenerTimingForm->addRow(QString(), listenerTimingNote);
    listenerTimingForm->addRow(QStringLiteral("Maximum compensation"), compensationMax);
    listenerTimingForm->addRow(QStringLiteral("Compensation smoothing"), compensationSmoothing);
    listenerTimingForm->addRow(QStringLiteral("Compensation deadband"), compensationDeadband);
    listenerTimingForm->addRow(QStringLiteral("Compensation slew"), compensationSlew);
    metronomeLayout->addWidget(listenerTimingBox);
    metronomeLayout->addStretch(1);

    auto* viewsContent = new QWidget(&dialog);
    auto* viewsLayout = new QVBoxLayout(viewsContent);
    auto* performanceDefaultsBox = new QGroupBox(
        QStringLiteral("Performance view"), viewsContent);
    auto* performanceDefaultsForm = new QFormLayout(performanceDefaultsBox);
    auto* performanceChordPreview = new QCheckBox(
        QStringLiteral("Show chord preview"), performanceDefaultsBox);
    auto* performanceBeatPreview = new QCheckBox(
        QStringLiteral("Show beat preview"), performanceDefaultsBox);
    performanceChordPreview->setChecked(preferences_.views.performanceChordPreview);
    performanceBeatPreview->setChecked(preferences_.views.performanceBeatPreview);
    auto* performancePreviewNote = new QLabel(
        QStringLiteral(
            "Hide either preview to leave more open visual space when you only need the jam audio."),
        performanceDefaultsBox);
    performancePreviewNote->setWordWrap(true);
    performancePreviewNote->setStyleSheet(QStringLiteral("color:#9eaaa8;"));
    performanceDefaultsForm->addRow(QString(), performanceChordPreview);
    performanceDefaultsForm->addRow(QString(), performanceBeatPreview);
    performanceDefaultsForm->addRow(QString(), performancePreviewNote);
    viewsLayout->addWidget(performanceDefaultsBox);
    auto* gridDefaultsBox = new QGroupBox(QStringLiteral("Chord and Drum views"), viewsContent);
    auto* gridDefaultsForm = new QFormLayout(gridDefaultsBox);
    auto* chordFollow = new QCheckBox(QStringLiteral("Focus current bar in Chords"), gridDefaultsBox);
    auto* drumFollow = new QCheckBox(QStringLiteral("Focus current bar in Drums"), gridDefaultsBox);
    chordFollow->setChecked(preferences_.views.chordFocusCurrentBar);
    drumFollow->setChecked(preferences_.views.drumFocusCurrentBar);
    auto* guitarStrings = new QComboBox(gridDefaultsBox);
    for (int strings : {6, 7, 8}) guitarStrings->addItem(QString::number(strings), strings);
    guitarStrings->setCurrentIndex(qMax(0, guitarStrings->findData(preferences_.views.guitarStrings)));
    auto* guitarTuning = new QComboBox(gridDefaultsBox);
    guitarTuning->addItem(QStringLiteral("Standard"), false);
    guitarTuning->addItem(QStringLiteral("Dropped"), true);
    guitarTuning->setCurrentIndex(qMax(0,
        guitarTuning->findData(preferences_.views.guitarDropTuning)));
    gridDefaultsForm->addRow(QString(), chordFollow);
    gridDefaultsForm->addRow(QString(), drumFollow);
    gridDefaultsForm->addRow(QStringLiteral("Guitar strings"), guitarStrings);
    gridDefaultsForm->addRow(QStringLiteral("Guitar tuning"), guitarTuning);
    viewsLayout->addWidget(gridDefaultsBox);
    auto* trackDefaultsBox = new QGroupBox(QStringLiteral("Track view"), viewsContent);
    auto* trackDefaultsForm = new QFormLayout(trackDefaultsBox);
    auto* defaultGridLock = new QCheckBox(QStringLiteral("Lock transport actions to the grid"), trackDefaultsBox);
    auto* defaultTrackLoop = new QCheckBox(QStringLiteral("Loop whole track"), trackDefaultsBox);
    defaultGridLock->setChecked(preferences_.views.trackGridLock);
    defaultTrackLoop->setChecked(preferences_.views.trackLoop);
    auto* defaultTrackSpeed = makeDoubleSpin(preferences_.views.trackSpeed, 0.1, 2.0, 2);
    auto* defaultTrackPitch = makeSpin(preferences_.views.trackPitch, -12, 12);
    auto* defaultFocusEnabled = new QCheckBox(QStringLiteral("Enable focus frequency"), trackDefaultsBox);
    defaultFocusEnabled->setChecked(preferences_.views.focusFrequencyEnabled);
    auto* defaultFocusPreset = new QComboBox(trackDefaultsBox);
    for (const QString& preset : {QStringLiteral("custom"), QStringLiteral("bass"),
             QStringLiteral("guitar"), QStringLiteral("vocals"), QStringLiteral("drums")}) {
        defaultFocusPreset->addItem(preset.front().toUpper() + preset.mid(1), preset);
    }
    defaultFocusPreset->setCurrentIndex(qMax(0,
        defaultFocusPreset->findData(preferences_.views.focusPreset)));
    auto* defaultFocusFrequency = makeSpin(preferences_.views.focusFrequencyHz, 40, 8000);
    defaultFocusFrequency->setSuffix(QStringLiteral(" Hz"));
    trackDefaultsForm->addRow(QString(), defaultGridLock);
    trackDefaultsForm->addRow(QString(), defaultTrackLoop);
    trackDefaultsForm->addRow(QStringLiteral("Playback speed"), defaultTrackSpeed);
    trackDefaultsForm->addRow(QStringLiteral("Pitch semitones"), defaultTrackPitch);
    trackDefaultsForm->addRow(QString(), defaultFocusEnabled);
    trackDefaultsForm->addRow(QStringLiteral("Focus preset"), defaultFocusPreset);
    trackDefaultsForm->addRow(QStringLiteral("Focus frequency"), defaultFocusFrequency);
    viewsLayout->addWidget(trackDefaultsBox);
    viewsLayout->addStretch(1);

    auto* syncContent = new QWidget(&dialog);
    auto* syncForm = new QFormLayout(syncContent);
    auto* syncTrackLanes = new QCheckBox(QStringLiteral("Sync Track Lanes"), syncContent);
    auto* syncWavs = new QCheckBox(QStringLiteral("Sync WAVs Automatically"), syncContent);
    auto* syncPlayback = new QCheckBox(QStringLiteral("Sync Global Playback"), syncContent);
    auto* syncMetronome = new QCheckBox(QStringLiteral("Sync Metronome State"), syncContent);
    auto* syncRecordings = new QCheckBox(QStringLiteral("Sync Recordings"), syncContent);
    syncTrackLanes->setChecked(preferences_.sync.trackLanes);
    syncWavs->setChecked(preferences_.sync.autoShareWavs);
    syncPlayback->setChecked(preferences_.sync.globalPlayback);
    syncMetronome->setChecked(preferences_.sync.metronomeState);
    syncRecordings->setChecked(preferences_.sync.recordings);
    auto* syncIdeas = new QComboBox(syncContent);
    syncIdeas->addItem(QStringLiteral("Disabled"), 0);
    syncIdeas->addItem(QStringLiteral("Whole Idea"), 1);
    syncIdeas->addItem(QStringLiteral("Chords Only"), 2);
    syncIdeas->addItem(QStringLiteral("Beats Only"), 3);
    syncIdeas->setCurrentIndex(qMax(0, syncIdeas->findData(preferences_.sync.generatedIdeas)));
    const auto updateSyncDefaults = [=] {
        syncWavs->setEnabled(syncTrackLanes->isChecked());
        const bool recordingDependencies = syncTrackLanes->isChecked() &&
            syncPlayback->isChecked();
        syncRecordings->setEnabled(recordingDependencies);
        if (!recordingDependencies) syncRecordings->setChecked(false);
    };
    QObject::connect(syncTrackLanes, &QCheckBox::toggled, &dialog,
        [updateSyncDefaults](bool) { updateSyncDefaults(); });
    QObject::connect(syncPlayback, &QCheckBox::toggled, &dialog,
        [updateSyncDefaults](bool) { updateSyncDefaults(); });
    updateSyncDefaults();
    syncForm->addRow(syncTrackLanes);
    syncForm->addRow(syncWavs);
    syncForm->addRow(QStringLiteral("Generated ideas"), syncIdeas);
    syncForm->addRow(syncPlayback);
    syncForm->addRow(syncMetronome);
    syncForm->addRow(syncRecordings);
    auto* syncNote = new QLabel(QStringLiteral(
        "These defaults initialise new local, created, and joined workflows. The jam creator can still apply a different policy to an active jam."),
        syncContent);
    syncNote->setWordWrap(true);
    syncNote->setStyleSheet(QStringLiteral("color:#9eaaa9;"));
    syncForm->addRow(syncNote);

    auto* keybindContent = new QWidget(&dialog);
    auto* keybindLayout = new QVBoxLayout(keybindContent);
    keybindLayout->setSpacing(12);
    auto* keybindNote = new QLabel(
        QStringLiteral(
            "Jam2 shortcuts are fixed and work from the main workflow views. "
            "They pause while you are typing, editing a numeric value, choosing from a list, "
            "or using a popup dialog."),
        keybindContent);
    keybindNote->setWordWrap(true);
    keybindNote->setStyleSheet(QStringLiteral("color:#9eaaa9; padding:2px 0 6px 0;"));
    keybindLayout->addWidget(keybindNote);

    const auto addKeybindGroup = [keybindContent, keybindLayout](
        const QString& title,
        const QList<QPair<QString, QString>>& bindings) {
        auto* group = new QGroupBox(title, keybindContent);
        auto* form = new QFormLayout(group);
        form->setHorizontalSpacing(18);
        form->setVerticalSpacing(9);
        for (const auto& binding : bindings) {
            auto* shortcut = new QLabel(binding.first, group);
            shortcut->setMinimumWidth(112);
            shortcut->setTextInteractionFlags(Qt::TextSelectableByMouse);
            shortcut->setStyleSheet(QStringLiteral(
                "color:#d9ad58; font-weight:600; padding:2px 8px; "
                "background:#11191b; border:1px solid #344245; border-radius:4px;"));
            auto* action = new QLabel(binding.second, group);
            action->setWordWrap(true);
            action->setStyleSheet(QStringLiteral("color:#d8dfdd;"));
            form->addRow(shortcut, action);
        }
        keybindLayout->addWidget(group);
    };

    addKeybindGroup(QStringLiteral("Views"), {
        {QStringLiteral("1"), QStringLiteral("Performance")},
        {QStringLiteral("2"), QStringLiteral("Chords")},
        {QStringLiteral("3"), QStringLiteral("Drums")},
        {QStringLiteral("4"), QStringLiteral("Lyrics")},
        {QStringLiteral("5"), QStringLiteral("Metronome")},
        {QStringLiteral("6"), QStringLiteral("Track")},
        {QStringLiteral("Tab"), QStringLiteral("No action in the main views")},
        {QStringLiteral("Shift+Tab"), QStringLiteral("Cycle to the next view in the order above")},
        {QStringLiteral("Home"), QStringLiteral("Return to Performance")},
    });
    addKeybindGroup(QStringLiteral("Playback and sections"), {
        {QStringLiteral("Space"), QStringLiteral("Start or stop global playback")},
        {QStringLiteral("Shift+Space"), QStringLiteral("Queue the next section while playback is running")},
        {QStringLiteral("M"), QStringLiteral("Toggle the metronome")},
        {QStringLiteral("F"), QStringLiteral("Toggle Focus current bar in Chords or Drums")},
    });
    addKeybindGroup(QStringLiteral("Track recording"), {
        {QStringLiteral("Ctrl+A"), QStringLiteral("Arm the selected lane in Track view")},
        {QStringLiteral("Ctrl+R"), QStringLiteral("Start or stop recording the selected lane in Track view")},
    });
    addKeybindGroup(QStringLiteral("Dialogs and editing"), {
        {QStringLiteral("Enter"), QStringLiteral("Confirm the active dialog; in Lyrics, move to the next bar")},
        {QStringLiteral("Shift+Enter"), QStringLiteral("Insert a new line in Lyrics")},
        {QStringLiteral("Ctrl+V"), QStringLiteral("Paste multiple lyric lines into consecutive bars")},
        {QStringLiteral("Escape"), QStringLiteral("Cancel or close the active dialog, rename, or expanded tuner")},
    });
    keybindLayout->addStretch(1);

    if (networkActive) {
        localBox->setEnabled(false);
        splitNetworkAudio->setEnabled(false);
        networkAudio.box->setEnabled(false);
        createJamAudio.box->setEnabled(false);
        joinJamAudio.box->setEnabled(false);
    }

    auto* notice = new QLabel(
        networkActive
            ? QStringLiteral("Leave the active jam before changing audio hardware settings.")
            : QStringLiteral(
                "Test Device checks the selection without changing playback. "
                "Apply Audio starts or restarts local audio and saves that selection."),
        &dialog);
    notice->setWordWrap(true);

    audioLayout->addWidget(notice);
    auto* tabs = new QWidget(&dialog);
    auto* tabLayout = new QHBoxLayout(tabs);
    tabLayout->setContentsMargins(0, 0, 0, 0);
    tabLayout->setSpacing(12);
    auto* settingsNavigation = new QListWidget(tabs);
    settingsNavigation->setObjectName(QStringLiteral("SettingsNavigation"));
    settingsNavigation->setFixedWidth(172);
    settingsNavigation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    settingsNavigation->setSpacing(2);
    settingsNavigation->setStyleSheet(QStringLiteral(
        "QListWidget#SettingsNavigation { background:#101718; border:1px solid #344245; "
        "border-radius:5px; padding:6px; outline:none; }"
        "QListWidget#SettingsNavigation::item { color:#bdc8c6; min-height:22px; "
        "padding:7px 10px; border:1px solid transparent; border-radius:4px; }"
        "QListWidget#SettingsNavigation::item:hover { color:#eef2ef; background:#182224; }"
        "QListWidget#SettingsNavigation::item:selected { color:#f2c66d; background:#211b12; "
        "border-color:#8a6835; font-weight:600; }"));
    auto* settingsPages = new QStackedWidget(tabs);
    const auto addSettingsPage = [=](const QString& title, QWidget* page) {
        settingsNavigation->addItem(title);
        settingsPages->addWidget(page);
    };
    addSettingsPage(QStringLiteral("Audio"), makeScrollTab(audioContent));
    addSettingsPage(QStringLiteral("Create Connection"), makeScrollTab(connectionContent));
    addSettingsPage(QStringLiteral("Create Defaults"), makeScrollTab(createContent));
    addSettingsPage(QStringLiteral("Join Defaults"), makeScrollTab(joinContent));
    addSettingsPage(QStringLiteral("Jam Sync"), makeScrollTab(syncContent));
    addSettingsPage(QStringLiteral("Ideas & WAVs"), makeScrollTab(ideaContent));
    addSettingsPage(QStringLiteral("Levels"), makeScrollTab(levelsContent));
    addSettingsPage(QStringLiteral("Metronome"), makeScrollTab(metronomeContent));
    addSettingsPage(QStringLiteral("Startup"), makeScrollTab(generalContent));
    addSettingsPage(QStringLiteral("Views & Tracks"), makeScrollTab(viewsContent));
    addSettingsPage(QStringLiteral("Logs"), makeScrollTab(logContent));
    addSettingsPage(QStringLiteral("Recording"), makeScrollTab(recordingContent));
    addSettingsPage(QStringLiteral("Keybinds"), makeScrollTab(keybindContent));
    QObject::connect(settingsNavigation, &QListWidget::currentRowChanged,
        settingsPages, &QStackedWidget::setCurrentIndex);
    settingsNavigation->setCurrentRow(0);
    tabLayout->addWidget(settingsNavigation);
    tabLayout->addWidget(settingsPages, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    const auto registerSettingsControl = [](
        QObject* control,
        const QString& id,
        const char* contract,
        jam2::gui::GuiControlAvailability availability =
            jam2::gui::GuiControlAvailability::Modal) {
        if (control == nullptr) return;
        jam2::gui::registerGuiControl(
            *control,
            QStringLiteral("application.settings-dialog.") + id,
            QString::fromLatin1(contract),
            availability,
            QStringLiteral("application.settings-dialog"));
    };
    const auto hardware = jam2::gui::GuiControlAvailability::HardwareProfile;
    const auto registerNetworkAudio = [&](const QString& prefix,
                                          const NetworkAudioEditors& editors) {
        registerSettingsControl(editors.device, prefix + QStringLiteral(".device"),
            "application.settings-audio", hardware);
        registerSettingsControl(editors.input, prefix + QStringLiteral(".input-channels"),
            "application.settings-audio");
        registerSettingsControl(editors.output, prefix + QStringLiteral(".output-channels"),
            "application.settings-audio");
        registerSettingsControl(editors.test, prefix + QStringLiteral(".test-device"),
            "application.settings-audio-test", hardware);
    };
    const auto registerTuning = [&](const QString& prefix, const TuningEditors& editors) {
        const auto add = [&](QObject* control, const char* suffix) {
            registerSettingsControl(
                control, prefix + QLatin1Char('.') + QString::fromLatin1(suffix),
                "application.settings-network-tuning");
        };
        add(editors.profile, "profile");
        add(editors.buffer, "buffer-size");
        add(editors.frame, "frame-size");
        add(editors.prefill, "playback-prefill");
        add(editors.playbackMax, "playback-maximum");
        add(editors.captureRing, "capture-ring");
        add(editors.playbackRing, "playback-ring");
        add(editors.drift, "drift-correction");
        add(editors.driftSmoothing, "drift-smoothing");
        add(editors.driftDeadband, "drift-deadband");
        add(editors.driftMax, "drift-maximum");
        add(editors.sampleTime, "sample-time-playout");
        add(editors.playout, "playout-delay");
        add(editors.jitter, "jitter-target");
        add(editors.jitterMax, "jitter-maximum");
        add(editors.adaptive, "adaptive-cushion");
        add(editors.adaptiveTarget, "adaptive-target");
        add(editors.adaptiveMin, "adaptive-minimum");
        add(editors.adaptiveMax, "adaptive-maximum");
        add(editors.adaptiveRelease, "adaptive-release");
        add(editors.adaptiveRamp, "adaptive-ramp");
    };
    const auto registerRuntime = [&](const QString& prefix, const RuntimeEditors& editors) {
        const auto add = [&](QObject* control, const char* suffix) {
            registerSettingsControl(
                control, prefix + QLatin1Char('.') + QString::fromLatin1(suffix),
                "application.settings-network-runtime");
        };
        add(editors.diagnostics, "diagnostics");
        add(editors.warmup, "diagnostics-warmup");
        add(editors.priority, "os-priority");
        add(editors.wait, "wait-ms");
        add(editors.stream, "stream-ms");
        add(editors.linger, "stream-linger-ms");
    };
    const auto registerPageControls = [&registerSettingsControl](
        const QString& prefix,
        const char* contract,
        std::initializer_list<std::pair<QObject*, const char*>> controls) {
        for (const auto& [control, suffix] : controls) {
            registerSettingsControl(
                control, prefix + QLatin1Char('.') + QString::fromLatin1(suffix), contract);
        }
    };

    registerPageControls(QStringLiteral("audio.local"), "application.settings-audio", {
        {localSampleRate, "sample-rate"}, {localBufferSize, "buffer-size"},
        {localInput, "input-channels"}, {localOutput, "output-channels"},
        {splitNetworkAudio, "split-network-by-role"},
    });
    registerSettingsControl(
        localDevice, QStringLiteral("audio.local.device"),
        "application.settings-audio", hardware);
    registerSettingsControl(
        localTest, QStringLiteral("audio.local.test-device"),
        "application.settings-audio-test", hardware);
    registerSettingsControl(
        localApply, QStringLiteral("audio.local.apply"),
        "application.settings-audio-apply", hardware);
    registerNetworkAudio(QStringLiteral("audio.network"), networkAudio);
    registerNetworkAudio(QStringLiteral("audio.create"), createJamAudio);
    registerNetworkAudio(QStringLiteral("audio.join"), joinJamAudio);

    registerPageControls(QStringLiteral("create-connection"),
        "application.settings-create-connection", {
            {createBind, "bind-host"}, {createPort, "port"},
            {createManualEndpoint, "manual-endpoint"},
            {createPublicHost, "public-host"}, {createStun, "stun-server"},
            {createStunTimeout, "stun-timeout"},
            {createStunRetries, "stun-retries"},
            {createMaxPeers, "maximum-peers"},
            {createSocketSend, "socket-send-buffer"},
            {createSocketReceive, "socket-receive-buffer"},
        });
    registerPageControls(QStringLiteral("create"),
        "application.settings-create-defaults", {
            {createRate, "sample-rate"}, {createQuality, "audio-quality"},
        });
    registerTuning(QStringLiteral("create.tuning"), createTuning);
    registerRuntime(QStringLiteral("create.runtime"), createRuntime);
    registerPageControls(QStringLiteral("join"),
        "application.settings-join-defaults", {
            {joinBind, "bind-host"}, {joinPort, "port"},
        });
    registerTuning(QStringLiteral("join.tuning"), joinTuning);
    registerRuntime(QStringLiteral("join.runtime"), joinRuntime);

    registerPageControls(QStringLiteral("logs"), "application.settings-logs", {
        {logFolder, "folder"}, {browseLogs, "browse"},
    });
    registerPageControls(QStringLiteral("recording"),
        "application.settings-recording", {
            {jamRecordingTile, "jam-page"}, {trackRecordingTile, "track-page"},
            {preferredMode, "preferred-mode"},
            {jamMixIncludeBacking, "jam-mix-backing"},
            {jamMixIncludeMetronome, "jam-mix-metronome"},
            {jamPromptName, "jam-prompt-name"},
            {jamCompletion, "jam-completion"},
            {jamImportMix, "jam-import-mix"},
            {jamImportMyInput, "jam-import-my-input"},
            {jamImportTheirInput, "jam-import-their-input"},
            {jamImportInputsMix, "jam-import-inputs-mix"},
            {jamImportMetronome, "jam-import-metronome"},
            {inputFolderRow.first, "input-folder"},
            {inputFolderRow.second->findChild<QPushButton*>(), "input-folder-browse"},
            {inputUntilStopped, "input-until-stopped"},
            {inputDuration, "input-duration"},
            {inputCountIn, "input-count-in"},
            {inputCountBars, "input-count-in-bars"},
            {inputCountMetro, "input-count-in-metronome"},
            {inputKeepMetro, "input-keep-metronome"},
            {inputLatency, "input-latency"},
            {loopFolderRow.first, "loopback-folder"},
            {loopFolderRow.second->findChild<QPushButton*>(), "loopback-folder-browse"},
            {loopSource, "loopback-source"},
            {refreshLoopSources, "loopback-refresh-sources"},
            {loopUntilStopped, "loopback-until-stopped"},
            {loopDuration, "loopback-duration"},
            {loopSilenceThreshold, "loopback-silence-threshold"},
            {loopTailSilence, "loopback-tail-silence"},
            {loopTrimLeading, "loopback-trim-leading"},
            {loopTrimTrailing, "loopback-trim-trailing"},
        });

    registerPageControls(QStringLiteral("startup"), "application.settings-startup", {
        {startupView, "view"}, {defaultBpm, "bpm"},
        {defaultMeter, "meter"}, {defaultDivision, "click-division"},
        {generateOnStartup, "generate-idea"},
    });
    registerPageControls(QStringLiteral("ideas"), "application.settings-ideas", {
        {ideaParts, "parts"}, {ideaKey, "key"}, {ideaStyle, "style"},
        {ideaProfile, "profile"}, {ideaMeter, "meter"},
        {ideaLength, "length"}, {ideaExactBpm, "exact-bpm"},
        {ideaBpm, "bpm"}, {ideaComplexity, "complexity"},
        {startupWavs, "render-on-startup"}, {renderChords, "render-chords"},
        {renderDrums, "render-drums"}, {renderMelody, "render-melody"},
        {renderBass, "render-bass"}, {renderSupport, "render-support"},
        {chordVoicing, "chord-voicing"}, {drumKit, "drum-kit"},
        {grooveTiming, "groove-timing"}, {grooveLength, "groove-length"},
    });
    registerPageControls(QStringLiteral("levels"), "application.settings-levels", {
        {defaultSend, "send"}, {defaultMonitorEnabled, "monitor-enabled"},
        {defaultMonitor, "monitor"}, {defaultMetronomeLevel, "metronome"},
        {defaultMaster, "master"}, {defaultRemote, "remote-peer"},
        {defaultBacking, "backing-track"},
    });
    registerPageControls(QStringLiteral("metronome"),
        "application.settings-metronome", {
            {defaultClickSound, "sound"}, {defaultMetronomeMode, "mode"},
            {compensationMax, "compensation-maximum"},
            {compensationSmoothing, "compensation-smoothing"},
            {compensationDeadband, "compensation-deadband"},
            {compensationSlew, "compensation-slew"},
        });
    registerPageControls(QStringLiteral("views"), "application.settings-views", {
        {performanceChordPreview, "performance-chord-preview"},
        {performanceBeatPreview, "performance-beat-preview"},
        {chordFollow, "chord-follow"}, {drumFollow, "drum-follow"},
        {guitarStrings, "guitar-strings"}, {guitarTuning, "guitar-tuning"},
        {defaultGridLock, "track-grid-lock"},
        {defaultTrackLoop, "track-loop"}, {defaultTrackSpeed, "track-speed"},
        {defaultTrackPitch, "track-pitch"},
        {defaultFocusEnabled, "focus-enabled"},
        {defaultFocusPreset, "focus-preset"},
        {defaultFocusFrequency, "focus-frequency"},
    });
    registerPageControls(QStringLiteral("sync"), "application.settings-sync", {
        {syncTrackLanes, "track-lanes"}, {syncWavs, "automatic-wavs"},
        {syncPlayback, "global-playback"}, {syncMetronome, "metronome-state"},
        {syncRecordings, "recordings"}, {syncIdeas, "generated-ideas"},
    });
    registerSettingsControl(settingsNavigation, QStringLiteral("navigation"),
        "application.settings-navigation");
    registerSettingsControl(buttons->button(QDialogButtonBox::Save),
        QStringLiteral("save"), "application.settings-save");
    registerSettingsControl(buttons->button(QDialogButtonBox::Cancel),
        QStringLiteral("cancel"), "application.settings-cancel");
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (callbacks.testDevice) {
        QObject::connect(localTest, &QPushButton::clicked, &dialog,
            [=, &callbacks, &dialog] {
                callbacks.testDevice(localDevice, localTest, &dialog);
            });
    }
    QObject::connect(localApply, &QPushButton::clicked, &dialog,
        [=, &callbacks, &dialog, &preferences_, &localInitial, &availableDevices_] {
            if (networkActive || !callbacks.applyLocalAudio) return;
            if (localDevice->currentData().toString().isEmpty()) {
                QMessageBox::warning(
                    &dialog,
                    QStringLiteral("Local Audio"),
                    QStringLiteral("Select a local audio device first."));
                return;
            }
            AudioDevicePreference desired = preferences_.localAudio;
            desired.sampleRate = localSampleRate->currentData().toInt();
            desired.bufferSize = localBufferSize->currentData().toInt();
            desired.inputChannels = localInput->text().trimmed();
            desired.outputChannels = localOutput->text().trimmed();
            jam2::gui::storeSelectedDevicePreference(
                desired, localDevice, availableDevices_);
            if (!callbacks.applyLocalAudio(
                    desired, localDevice->currentData().toString(), &dialog)) {
                return;
            }
            preferences_.localAudio = desired;
            localInitial = desired;
            localApply->setText(QStringLiteral("Applied"));
            QTimer::singleShot(1400, localApply, [localApply] {
                localApply->setText(QStringLiteral("Apply Audio"));
            });
        });
    if (callbacks.testDevice) {
        const auto connectDeviceTest = [&callbacks, &dialog](
            const NetworkAudioEditors& editors) {
            QObject::connect(editors.test, &QPushButton::clicked, &dialog,
                [editors, &callbacks, &dialog] {
                    callbacks.testDevice(editors.device, editors.test, &dialog);
                });
        };
        connectDeviceTest(networkAudio);
        connectDeviceTest(createJamAudio);
        connectDeviceTest(joinJamAudio);
    }
    auto* outer = new QVBoxLayout(&dialog);
    outer->addWidget(tabs, 1);
    outer->addWidget(buttons);
    for (QFormLayout* form : dialog.findChildren<QFormLayout*>()) {
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
        form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    }
    for (QWidget* editor : dialog.findChildren<QWidget*>()) {
        if (qobject_cast<QAbstractSpinBox*>(editor) ||
            qobject_cast<QLineEdit*>(editor) ||
            qobject_cast<QComboBox*>(editor)) {
            editor->setAttribute(Qt::WA_MacShowFocusRect, false);
        }
    }

    if (dialog.exec() != QDialog::Accepted) return std::nullopt;

    UserPreferences updated = preferences_;
    updated.localAudio.sampleRate = localSampleRate->currentData().toInt();
    updated.localAudio.bufferSize = localBufferSize->currentData().toInt();
    updated.localAudio.inputChannels = localInput->text().trimmed();
    updated.localAudio.outputChannels = localOutput->text().trimmed();
    jam2::gui::storeSelectedDevicePreference(
        updated.localAudio, localDevice, availableDevices_);
    updated.splitNetworkAudioByRole = splitNetworkAudio->isChecked();
    updated.networkAudio = audioFromEditors(updated.networkAudio, networkAudio);
    if (updated.splitNetworkAudioByRole) {
        updated.createJamAudio = audioFromEditors(updated.createJamAudio, createJamAudio);
        updated.joinJamAudio = audioFromEditors(updated.joinJamAudio, joinJamAudio);
    } else {
        updated.createJamAudio = updated.networkAudio;
        updated.joinJamAudio = updated.networkAudio;
    }
    updated.create.bindHost = createBind->text().trimmed(); updated.create.port = createPort->value();
    updated.create.noStun = createManualEndpoint->isChecked(); updated.create.publicHost = createPublicHost->text().trimmed();
    updated.create.stunServer = createStun->text().trimmed(); updated.create.stunTimeoutMs = createStunTimeout->value();
    updated.create.stunRetries = createStunRetries->value(); updated.create.maxPeers = createMaxPeers->value();
    updated.create.socketSendBuffer = createSocketSend->value(); updated.create.socketRecvBuffer = createSocketReceive->value();
    updated.create.tuning.profile = createTuning.profile->currentData().toString();
    updated.create.sampleRate = createRate->currentData().toInt();
    updated.create.audioFormat = createQuality->currentData().toString();
    updated.join.bindHost = joinBind->text().trimmed(); updated.join.port = joinPort->value();
    auto storeTuning = [](LocalTuningPreference& p, const TuningEditors& e, bool creator) {
        p.profile = e.profile->currentData().toString(); p.bufferSize = e.buffer->currentData().toInt();
        if (creator) p.frameSize = e.frame->value();
        p.prefillFrames = e.prefill->value(); p.playbackMaxFrames = e.playbackMax->value();
        p.captureRingFrames = e.captureRing->value(); p.playbackRingFrames = e.playbackRing->value();
        p.driftCorrection = e.drift->isChecked(); p.driftSmoothing = e.driftSmoothing->value();
        p.driftDeadbandPpm = e.driftDeadband->value(); p.driftMaxCorrectionPpm = e.driftMax->value();
        p.sampleTimePlayout = e.sampleTime->isChecked(); p.playoutDelayFrames = e.playout->value();
        p.jitterBufferFrames = e.jitter->value(); p.jitterBufferMaxFrames = e.jitterMax->value();
        p.adaptiveCushion = e.adaptive->isChecked(); p.adaptiveTargetFrames = e.adaptiveTarget->value();
        p.adaptiveMinFrames = e.adaptiveMin->value(); p.adaptiveMaxFrames = e.adaptiveMax->value();
        p.adaptiveReleasePpm = e.adaptiveRelease->value(); p.adaptiveRatioRampMs = e.adaptiveRamp->value();
    };
    auto storeRuntime = [](RuntimePreference& p, const RuntimeEditors& e) {
        p.diagnostics = e.diagnostics->isChecked(); p.diagnosticsWarmupMs = e.warmup->value();
        p.osPriority = e.priority->currentData().toString(); p.waitMs = e.wait->value();
        p.streamMs = e.stream->value(); p.streamLingerMs = e.linger->value();
    };
    storeTuning(updated.create.tuning, createTuning, true); storeRuntime(updated.create.runtime, createRuntime);
    storeTuning(updated.join.tuning, joinTuning, false); storeRuntime(updated.join.runtime, joinRuntime);
    updated.logging.folder = logFolder->text().trimmed();
    if (updated.logging.folder.isEmpty()) {
        updated.logging.folder = appReleaseFolderPath(QStringLiteral("logs"));
    }
    updated.create.runtime.logStatsFolder = updated.logging.folder;
    updated.join.runtime.logStatsFolder = updated.logging.folder;
    updated.recording.preferredMode = preferredMode->currentData().toString();
    updated.recording.jam.promptForName = jamPromptName->isChecked();
    updated.recording.jam.completionAction = jamCompletion->currentData().toString();
    updated.recording.jam.importMix = jamImportMix->isChecked();
    updated.recording.jam.importMyInput = jamImportMyInput->isChecked();
    updated.recording.jam.importTheirInput = jamImportTheirInput->isChecked();
    updated.recording.jam.importInputsMix = jamImportInputsMix->isChecked();
    updated.recording.jam.importMetronome = jamImportMetronome->isChecked();
    updated.recording.jamMixTrack.includeBackingTrack =
        jamMixIncludeBacking->isChecked();
    updated.recording.jamMixTrack.includeMetronome =
        jamMixIncludeMetronome->isChecked();
    updated.recording.input.outputFolder = inputFolderRow.first->text().trimmed();
    if (updated.recording.input.outputFolder.isEmpty()) {
        updated.recording.input.outputFolder = appReleaseFolderPath(QStringLiteral("captures"));
    }
    updated.recording.input.recordUntilStopped = inputUntilStopped->isChecked();
    updated.recording.input.durationBars = inputDuration->value();
    updated.recording.input.countIn = inputCountIn->isChecked(); updated.recording.input.countInBars = inputCountBars->value();
    updated.recording.input.countInMetronome = inputCountMetro->isChecked(); updated.recording.input.keepMetronome = inputKeepMetro->isChecked();
    updated.recording.input.latencyAdjustmentFrames = inputLatency->value();
    updated.recording.loopback.outputFolder = loopFolderRow.first->text().trimmed();
    if (updated.recording.loopback.outputFolder.isEmpty()) {
        updated.recording.loopback.outputFolder = appReleaseFolderPath(QStringLiteral("captures"));
    }
    updated.recording.loopback.sourceId = loopSource->currentData().toString().isEmpty()
        ? loopSource->currentText().trimmed() : loopSource->currentData().toString();
    updated.recording.loopback.sourceName = loopSource->currentText().trimmed();
    updated.recording.loopback.recordUntilStopped = loopUntilStopped->isChecked();
    updated.recording.loopback.durationBars = loopDuration->value();
    updated.recording.loopback.silenceThresholdDb = loopSilenceThreshold->value();
    updated.recording.loopback.tailSilenceMs = loopTailSilence->value(); updated.recording.loopback.trimLeading = loopTrimLeading->isChecked();
    updated.recording.loopback.trimTrailing = loopTrimTrailing->isChecked();

    updated.general.startupView = startupView->currentData().toString();
    updated.general.bpm = defaultBpm->value();
    if (const auto* meter = jam2::practice::findMeter(
            defaultMeter->currentData().toString())) {
        updated.general.meterNumerator = meter->numerator;
        updated.general.meterDenominator = meter->denominator;
        updated.general.tempoPulseUnits = meter->tempoPulseUnits;
    }
    updated.general.clickDivision = defaultDivision->currentData().toInt();
    updated.general.generateIdeaOnStartup = generateOnStartup->isChecked();

    updated.ideas.parts = ideaParts->currentData().toInt();
    updated.ideas.key = ideaKey->currentData().toInt();
    updated.ideas.styleId = ideaStyle->currentData().toString();
    updated.ideas.profileId = ideaProfile->currentData().toString();
    updated.ideas.meterId = ideaMeter->currentData().toString();
    updated.ideas.bars = ideaLength->currentData().toInt();
    updated.ideas.exactBpm = ideaExactBpm->isChecked();
    updated.ideas.bpm = ideaBpm->value();
    updated.ideas.complexity = ideaComplexity->value();
    updated.ideas.renderWavsOnStartup = startupWavs->isChecked();
    updated.ideas.renderChords = renderChords->isChecked();
    updated.ideas.renderDrums = renderDrums->isChecked();
    updated.ideas.renderMelody = renderMelody->isChecked();
    updated.ideas.renderBass = renderBass->isChecked();
    updated.ideas.renderSupport = renderSupport->isChecked();
    updated.ideas.chordVoicing = chordVoicing->currentIndex();
    updated.ideas.drumKit = drumKit->currentIndex();
    updated.ideas.grooveUseIdeaTiming = grooveTiming->currentData().toBool();
    updated.ideas.grooveBars = grooveLength->currentData().toInt();

    updated.levels.sendDb = defaultSend->value();
    updated.levels.monitorInput = defaultMonitorEnabled->isChecked();
    updated.levels.monitorDb = defaultMonitor->value();
    updated.levels.metronomeDb = defaultMetronomeLevel->value();
    updated.levels.masterDb = defaultMaster->value();
    updated.levels.remotePeerDb = defaultRemote->value();
    updated.levels.backingTrackDb = defaultBacking->value();

    updated.metronome.sound = defaultClickSound->currentIndex();
    updated.metronome.mode = defaultMetronomeMode->currentData().toString();
    updated.metronome.compensationMaxMs = compensationMax->value();
    updated.metronome.compensationSmoothingMs = compensationSmoothing->value();
    updated.metronome.compensationDeadbandMs = compensationDeadband->value();
    updated.metronome.compensationSlewMsPerSecond = compensationSlew->value();

    updated.views.performanceChordPreview = performanceChordPreview->isChecked();
    updated.views.performanceBeatPreview = performanceBeatPreview->isChecked();
    updated.views.chordFocusCurrentBar = chordFollow->isChecked();
    updated.views.drumFocusCurrentBar = drumFollow->isChecked();
    updated.views.guitarStrings = guitarStrings->currentData().toInt();
    updated.views.guitarDropTuning = guitarTuning->currentData().toBool();
    updated.views.trackGridLock = defaultGridLock->isChecked();
    updated.views.trackLoop = defaultTrackLoop->isChecked();
    updated.views.trackSpeed = defaultTrackSpeed->value();
    updated.views.trackPitch = defaultTrackPitch->value();
    updated.views.focusFrequencyEnabled = defaultFocusEnabled->isChecked();
    updated.views.focusPreset = defaultFocusPreset->currentData().toString();
    updated.views.focusFrequencyHz = defaultFocusFrequency->value();

    updated.sync.trackLanes = syncTrackLanes->isChecked();
    updated.sync.autoShareWavs = syncWavs->isChecked();
    updated.sync.globalPlayback = syncPlayback->isChecked();
    updated.sync.generatedIdeas = syncIdeas->currentData().toInt();
    updated.sync.metronomeState = syncMetronome->isChecked();
    updated.sync.recordings = syncRecordings->isChecked();

    return SettingsDialogResult{
        std::move(updated),
        localDevice->currentData().toString(),
    };
}

} // namespace jam2::gui

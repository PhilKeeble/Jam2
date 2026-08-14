#include "InputSourceDialogs.hpp"

#include "GuiControlContract.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFrame>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <memory>
#include <utility>

namespace jam2::gui {

void AudioInputSourcesDialog::run(
    AudioInputDialogCallbacks callbacks,
    QWidget* parent)
{
    if (!callbacks.snapshot) return;

    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Audio inputs"));
    dialog.setMinimumSize(900, 610);
    dialog.setProperty("jam2MaximumDialogHeight", 760);

    auto* outer = new QVBoxLayout(&dialog);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(14);
    auto* intro = new QLabel(QStringLiteral(
        "These are the input channels selected when the audio engine opened. "
        "Choose which sources enter My Send, set their mix, or combine any two "
        "available mono inputs by assigning an explicit left and right channel."), content);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* sourcePanels = new QWidget(content);
    auto* sourcePanelsLayout = new QVBoxLayout(sourcePanels);
    sourcePanelsLayout->setContentsMargins(0, 0, 0, 0);
    sourcePanelsLayout->setSpacing(14);
    layout->addWidget(sourcePanels);

    std::function<void()> rebuildSourcePanels;
    rebuildSourcePanels = [&dialog, sourcePanels, sourcePanelsLayout,
        &callbacks, &rebuildSourcePanels] {
        while (QLayoutItem* item = sourcePanelsLayout->takeAt(0)) {
            if (QWidget* widget = item->widget()) {
                widget->hide();
                widget->setParent(nullptr);
                widget->deleteLater();
            }
            delete item;
        }

        const AudioInputDialogState state = callbacks.snapshot();
        auto* inputs = new QGroupBox(
            QStringLiteral("Inputs selected for this engine"), sourcePanels);
        auto* inputsLayout = new QVBoxLayout(inputs);
        inputsLayout->setSpacing(10);
        for (const AudioInputDialogSource& source : state.sources) {
            const QString title = source.stereo
                ? QStringLiteral("Stereo · %1 left + %2 right")
                    .arg(source.firstName, source.secondName)
                : QStringLiteral("Mono · %1").arg(source.firstName);
            auto* tile = new QGroupBox(title, inputs);
            auto* tileLayout = new QVBoxLayout(tile);
            auto* description = new QLabel(source.stereo
                ? QStringLiteral(
                    "This pair is treated as one source. Its left/right layout is "
                    "preserved for a stereo-capable plugin, then mixed to mono for My Send.")
                : QStringLiteral(
                    "This engine-selected channel is treated as one mono source."), tile);
            description->setWordWrap(true);
            tileLayout->addWidget(description);
            auto* controls = new QHBoxLayout();
            auto* include = new QCheckBox(QStringLiteral("Send to Jam"), tile);
            include->setChecked(source.included);
            registerGuiControl(
                *include,
                QStringLiteral("performance.audio-input-dialog.source.%1.include")
                    .arg(source.slot),
                QStringLiteral("performance.audio-input-source-routing"),
                GuiControlAvailability::Modal,
                QStringLiteral("performance.audio-input-dialog.source"));
            controls->addWidget(include);
            QObject::connect(include, &QCheckBox::toggled, &dialog,
                [&callbacks, slot = source.slot](bool value) {
                    if (callbacks.setIncluded) callbacks.setIncluded(slot, value);
                });
            controls->addSpacing(16);
            controls->addWidget(new QLabel(QStringLiteral("Send Mix"), tile));
            auto* level = new QSpinBox(tile);
            level->setRange(0, 200);
            level->setSuffix(QStringLiteral("%"));
            level->setValue(source.levelPpm / 10000);
            level->setToolTip(QStringLiteral("Source level before the mono My Send mix"));
            registerGuiControl(
                *level,
                QStringLiteral("performance.audio-input-dialog.source.%1.level")
                    .arg(source.slot),
                QStringLiteral("performance.audio-input-source-routing"),
                GuiControlAvailability::Modal,
                QStringLiteral("performance.audio-input-dialog.source"));
            controls->addWidget(level);
            QObject::connect(level, qOverload<int>(&QSpinBox::valueChanged), &dialog,
                [&callbacks, slot = source.slot](int percent) {
                    if (callbacks.setLevel) callbacks.setLevel(slot, percent * 10000);
                });
            controls->addStretch(1);
            if (source.stereo) {
                auto* ungroup = new QPushButton(
                    QStringLiteral("Ungroup to Mono Inputs"), tile);
                ungroup->setEnabled(!state.topologyLocked);
                registerGuiControl(
                    *ungroup,
                    QStringLiteral("performance.audio-input-dialog.source.%1.ungroup")
                        .arg(source.slot),
                    QStringLiteral("performance.audio-input-topology"),
                    GuiControlAvailability::Modal,
                    QStringLiteral("performance.audio-input-dialog.source"));
                controls->addWidget(ungroup);
                QObject::connect(ungroup, &QPushButton::clicked, &dialog,
                    [&dialog, &callbacks, source, &rebuildSourcePanels] {
                        if (source.pluginLoaded) {
                            const auto answer = QMessageBox::question(
                                &dialog,
                                QStringLiteral("Ungroup stereo input"),
                                QStringLiteral(
                                    "Ungrouping changes the plugin input layout and removes "
                                    "the plugin currently loaded on this pair. Continue?"),
                                QMessageBox::Yes | QMessageBox::Cancel,
                                QMessageBox::Cancel);
                            if (answer != QMessageBox::Yes) return;
                        }
                        if (callbacks.ungroup) callbacks.ungroup(source.slot);
                        rebuildSourcePanels();
                    });
            }
            tileLayout->addLayout(controls);
            inputsLayout->addWidget(tile);
        }
        if (state.sources.isEmpty()) {
            inputsLayout->addWidget(new QLabel(QStringLiteral(
                "No input channels were selected when the engine opened."), inputs));
        }
        sourcePanelsLayout->addWidget(inputs);

        auto* grouping = new QGroupBox(
            QStringLiteral("Create a stereo pair"), sourcePanels);
        auto* groupingLayout = new QVBoxLayout(grouping);
        auto* groupingHelp = new QLabel(QStringLiteral(
            "Assign the physical channel that carries the left side and the channel "
            "that carries the right side. The pair becomes one source and is mixed "
            "to Jam2's mono send after plugin processing."), grouping);
        groupingHelp->setWordWrap(true);
        groupingLayout->addWidget(groupingHelp);
        auto* channelRow = new QHBoxLayout();
        auto* leftChannel = new QComboBox(grouping);
        auto* rightChannel = new QComboBox(grouping);
        QVector<AudioInputDialogSource> available;
        for (const AudioInputDialogSource& source : state.sources) {
            if (source.stereo || source.firstChannel != source.slot) continue;
            available.push_back(source);
            leftChannel->addItem(
                source.firstName, static_cast<qulonglong>(source.slot));
            rightChannel->addItem(
                source.firstName, static_cast<qulonglong>(source.slot));
        }
        if (rightChannel->count() > 1) rightChannel->setCurrentIndex(1);
        channelRow->addWidget(new QLabel(QStringLiteral("Left"), grouping));
        channelRow->addWidget(leftChannel, 1);
        channelRow->addSpacing(12);
        channelRow->addWidget(new QLabel(QStringLiteral("Right"), grouping));
        channelRow->addWidget(rightChannel, 1);
        auto* createPair = new QPushButton(
            QStringLiteral("Create Stereo Pair"), grouping);
        registerGuiControl(
            *leftChannel,
            QStringLiteral("performance.audio-input-dialog.pair.left"),
            QStringLiteral("performance.audio-input-topology"),
            GuiControlAvailability::Modal,
            QStringLiteral("performance.audio-input-dialog.pair"));
        registerGuiControl(
            *rightChannel,
            QStringLiteral("performance.audio-input-dialog.pair.right"),
            QStringLiteral("performance.audio-input-topology"),
            GuiControlAvailability::Modal,
            QStringLiteral("performance.audio-input-dialog.pair"));
        registerGuiControl(
            *createPair,
            QStringLiteral("performance.audio-input-dialog.pair.create"),
            QStringLiteral("performance.audio-input-topology"),
            GuiControlAvailability::Modal,
            QStringLiteral("performance.audio-input-dialog.pair"));
        channelRow->addWidget(createPair);
        groupingLayout->addLayout(channelRow);
        const auto updateCreatePair = [=] {
            createPair->setEnabled(!state.topologyLocked && available.size() >= 2 &&
                leftChannel->currentData() != rightChannel->currentData());
        };
        QObject::connect(leftChannel, qOverload<int>(&QComboBox::currentIndexChanged),
            &dialog, [=](int) { updateCreatePair(); });
        QObject::connect(rightChannel, qOverload<int>(&QComboBox::currentIndexChanged),
            &dialog, [=](int) { updateCreatePair(); });
        updateCreatePair();
        if (state.topologyLocked) {
            groupingHelp->setText(groupingHelp->text() + QStringLiteral(
                " Input grouping is locked while a recording is armed or active."));
        } else if (available.size() < 2) {
            groupingHelp->setText(groupingHelp->text() + QStringLiteral(
                " Ungroup an existing pair to make two mono inputs available."));
        }
        QObject::connect(createPair, &QPushButton::clicked, &dialog,
            [&dialog, &callbacks, available, leftChannel, rightChannel,
             &rebuildSourcePanels] {
                const std::size_t left = static_cast<std::size_t>(
                    leftChannel->currentData().toULongLong());
                const std::size_t right = static_cast<std::size_t>(
                    rightChannel->currentData().toULongLong());
                if (left == right) return;
                const auto leftSource = std::find_if(
                    available.cbegin(), available.cend(),
                    [left](const auto& source) { return source.slot == left; });
                const auto rightSource = std::find_if(
                    available.cbegin(), available.cend(),
                    [right](const auto& source) { return source.slot == right; });
                if (leftSource == available.cend() ||
                    rightSource == available.cend()) return;
                if (leftSource->pluginLoaded || rightSource->pluginLoaded) {
                    const auto answer = QMessageBox::question(
                        &dialog,
                        QStringLiteral("Create stereo pair"),
                        QStringLiteral(
                            "Creating this pair changes both input sources and removes "
                            "their currently loaded plugins. Continue?"),
                        QMessageBox::Yes | QMessageBox::Cancel,
                        QMessageBox::Cancel);
                    if (answer != QMessageBox::Yes) return;
                }
                if (callbacks.group) callbacks.group(left, right);
                rebuildSourcePanels();
            });
        sourcePanelsLayout->addWidget(grouping);
    };
    rebuildSourcePanels();

    layout->addStretch(1);
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);
    auto* footer = new QWidget(&dialog);
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(18, 8, 18, 14);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, footer);
    registerGuiControl(
        *buttons->button(QDialogButtonBox::Close),
        QStringLiteral("performance.audio-input-dialog.close"),
        QStringLiteral("performance.audio-input-dialog"),
        GuiControlAvailability::Modal);
    footerLayout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    outer->addWidget(footer);
    dialog.exec();
}

void MidiInputSourcesDialog::run(
    MidiInputDialogCallbacks callbacks,
    QWidget* parent)
{
    if (!callbacks.snapshot) return;
    auto callbackState = std::make_shared<MidiInputDialogCallbacks>(
        std::move(callbacks));

    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("MIDI inputs"));
    dialog.setMinimumSize(860, 580);
    dialog.setProperty("jam2MaximumDialogHeight", 740);
    QPointer<QDialog> dialogGuard(&dialog);

    auto* outer = new QVBoxLayout(&dialog);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(14);
    auto* intro = new QLabel(QStringLiteral(
        "Assign local MIDI controllers and choose how each one is interpreted. "
        "Standard MIDI and MPE messages stay local; the Plugins panel attaches "
        "an instrument and Jam2 sends only that instrument's mono audio output."), content);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* devicePanels = new QWidget(content);
    auto* devicePanelsLayout = new QVBoxLayout(devicePanels);
    devicePanelsLayout->setContentsMargins(0, 0, 0, 0);
    devicePanelsLayout->setSpacing(10);
    layout->addWidget(devicePanels);

    const auto rebuild = std::make_shared<std::function<void()>>();
    const std::weak_ptr<std::function<void()>> rebuildWeak = rebuild;
    *rebuild = [devicePanels, devicePanelsLayout, callbackState,
        dialogGuard, parent, rebuildWeak] {
        if (dialogGuard.isNull()) return;
        while (QLayoutItem* item = devicePanelsLayout->takeAt(0)) {
            if (QWidget* widget = item->widget()) {
                widget->hide();
                widget->setParent(nullptr);
                widget->deleteLater();
            }
            delete item;
        }

        const MidiInputDialogState state = callbackState->snapshot();
        auto* devices = new QGroupBox(QStringLiteral("Devices"), devicePanels);
        auto* devicesLayout = new QVBoxLayout(devices);
        devicesLayout->setSpacing(10);
        if (state.sources.isEmpty()) {
            auto* empty = new QLabel(QStringLiteral(
                "No MIDI devices are assigned. Add a controller, choose Standard MIDI "
                "or MPE, then attach its instrument in Plugins."), devices);
            empty->setWordWrap(true);
            devicesLayout->addWidget(empty);
        }

        for (const MidiInputDialogSource& source : state.sources) {
            auto* tile = new QGroupBox(source.deviceName, devices);
            auto* tileLayout = new QVBoxLayout(tile);
            auto* status = new QLabel(source.pluginLoaded && source.deviceOpen
                ? QStringLiteral("Device open · %1")
                    .arg(source.pluginName.isEmpty()
                        ? QStringLiteral("instrument active") : source.pluginName)
                : QStringLiteral(
                    "Assigned · choose an instrument from Plugins to open this device"),
                tile);
            status->setWordWrap(true);
            tileLayout->addWidget(status);
            auto* controls = new QHBoxLayout();
            controls->addWidget(new QLabel(QStringLiteral("Mode"), tile));
            auto* mode = new QComboBox(tile);
            mode->addItem(QStringLiteral("Standard MIDI"),
                static_cast<int>(jam2::midi::InputMode::Standard));
            mode->addItem(QStringLiteral("MPE"),
                static_cast<int>(jam2::midi::InputMode::Mpe));
            mode->setCurrentIndex(qMax(0,
                mode->findData(static_cast<int>(source.mode))));
            mode->setEnabled(!state.topologyLocked);
            const QString sourcePrefix = QStringLiteral(
                "performance.midi-input-dialog.source.%1")
                .arg(static_cast<qulonglong>(source.routerSlot));
            registerGuiControl(
                *mode,
                sourcePrefix + QStringLiteral(".mode"),
                QStringLiteral("performance.midi-input-routing"),
                GuiControlAvailability::HardwareProfile,
                QStringLiteral("performance.midi-input-dialog.source"));
            controls->addWidget(mode);
            QObject::connect(mode, qOverload<int>(&QComboBox::currentIndexChanged),
                dialogGuard.data(), [callbackState, source, mode](int) {
                    if (callbackState->setMode) {
                        callbackState->setMode(source.routerSlot,
                            static_cast<jam2::midi::InputMode>(
                                mode->currentData().toInt()));
                    }
                });
            controls->addSpacing(16);
            auto* include = new QCheckBox(
                QStringLiteral("Send instrument audio to Jam"), tile);
            include->setChecked(source.included);
            include->setToolTip(QStringLiteral(
                "Controls whether audio rendered by this device's instrument enters My Send."));
            registerGuiControl(
                *include,
                sourcePrefix + QStringLiteral(".include"),
                QStringLiteral("performance.midi-input-routing"),
                GuiControlAvailability::HardwareProfile,
                QStringLiteral("performance.midi-input-dialog.source"));
            controls->addWidget(include);
            QObject::connect(include, &QCheckBox::toggled, dialogGuard.data(),
                [callbackState, source](bool value) {
                    if (callbackState->setIncluded)
                        callbackState->setIncluded(source.routerSlot, value);
                });
            controls->addSpacing(16);
            auto* levelLabel = new QLabel(
                QStringLiteral("Instrument Audio Level"), tile);
            levelLabel->setToolTip(QStringLiteral(
                "Sets how much audio rendered by the instrument contributes to My Send. "
                "The MIDI device itself does not produce audio."));
            controls->addWidget(levelLabel);
            auto* level = new QSpinBox(tile);
            level->setRange(0, 200);
            level->setSuffix(QStringLiteral("%"));
            level->setValue(source.levelPpm / 10000);
            level->setToolTip(levelLabel->toolTip());
            registerGuiControl(
                *level,
                sourcePrefix + QStringLiteral(".level"),
                QStringLiteral("performance.midi-input-routing"),
                GuiControlAvailability::HardwareProfile,
                QStringLiteral("performance.midi-input-dialog.source"));
            controls->addWidget(level);
            QObject::connect(level, qOverload<int>(&QSpinBox::valueChanged),
                dialogGuard.data(), [callbackState, source](int percent) {
                    if (callbackState->setLevel)
                        callbackState->setLevel(source.routerSlot, percent * 10000);
                });
            controls->addStretch(1);
            auto* remove = new QPushButton(QStringLiteral("Remove Device"), tile);
            remove->setObjectName(QStringLiteral("PluginRemoveAction"));
            remove->setEnabled(!state.topologyLocked);
            registerGuiControl(
                *remove,
                sourcePrefix + QStringLiteral(".remove"),
                QStringLiteral("performance.midi-input-lifecycle"),
                GuiControlAvailability::HardwareProfile,
                QStringLiteral("performance.midi-input-dialog.source"));
            controls->addWidget(remove);
            QObject::connect(remove, &QPushButton::clicked, dialogGuard.data(),
                [callbackState, source, rebuildWeak] {
                    if (callbackState->remove)
                        callbackState->remove(source.routerSlot);
                    if (const auto currentRebuild = rebuildWeak.lock())
                        (*currentRebuild)();
                });
            tileLayout->addLayout(controls);
            devicesLayout->addWidget(tile);
        }

        auto* add = new QPushButton(QStringLiteral("Add MIDI Device…"), devices);
        add->setEnabled(!state.topologyLocked);
        registerGuiControl(
            *add,
            QStringLiteral("performance.midi-input-dialog.add"),
            QStringLiteral("performance.midi-input-lifecycle"),
            GuiControlAvailability::HardwareProfile,
            QStringLiteral("performance.midi-input-dialog"));
        devicesLayout->addWidget(add, 0, Qt::AlignLeft);
        devicePanelsLayout->addWidget(devices);

        QObject::connect(add, &QPushButton::clicked, dialogGuard.data(),
            [callbackState, dialogGuard, parent, rebuildWeak] {
                if (!callbackState->discover || dialogGuard.isNull()) return;
                auto* discovery = new QProgressDialog(
                    QStringLiteral("Finding MIDI input devices..."),
                    QStringLiteral("Cancel"), 0, 0, parent);
                discovery->setWindowTitle(QStringLiteral("MIDI input"));
                discovery->setWindowModality(Qt::WindowModal);
                discovery->setMinimumDuration(0);
                discovery->setAttribute(Qt::WA_DeleteOnClose);
                auto* discoveryCancel = new QPushButton(
                    QStringLiteral("Cancel"), discovery);
                discovery->setCancelButton(discoveryCancel);
                registerGuiControl(
                    *discoveryCancel,
                    QStringLiteral("performance.midi-discovery.cancel"),
                    QStringLiteral("performance.midi-input-discovery"),
                    GuiControlAvailability::HardwareProfile);
                QObject::connect(discovery, &QProgressDialog::canceled,
                    discovery, &QObject::deleteLater);
                discovery->show();

                QPointer<QProgressDialog> discoveryGuard(discovery);
                callbackState->discover([
                    callbackState, dialogGuard, discoveryGuard, parent, rebuildWeak
                ](MidiInputDiscoveryResult result) mutable {
                    if (discoveryGuard.isNull()) return;
                    discoveryGuard->close();
                    QWidget* messageParent = dialogGuard.isNull()
                        ? parent : dialogGuard.data();
                    if (!result.error.isEmpty()) {
                        QMessageBox::warning(messageParent,
                            QStringLiteral("MIDI input"),
                            QStringLiteral("Could not enumerate MIDI inputs: %1")
                                .arg(result.error));
                        return;
                    }
                    const MidiInputDialogState current = callbackState->snapshot();
                    QVector<MidiInputDeviceChoice> available;
                    for (const MidiInputDeviceChoice& device : result.devices) {
                        const auto assigned = std::find_if(
                            current.sources.cbegin(), current.sources.cend(),
                            [&device](const auto& source) {
                                return source.deviceId == device.id;
                            });
                        if (assigned == current.sources.cend())
                            available.push_back(device);
                    }
                    if (available.isEmpty()) {
                        QMessageBox::information(messageParent,
                            QStringLiteral("MIDI inputs"),
                            result.devices.isEmpty()
                                ? QStringLiteral(
                                    "No MIDI input devices are currently available.")
                                : QStringLiteral(
                                    "Every available MIDI input is already assigned."));
                        return;
                    }

                    QDialog configuration(messageParent);
                    configuration.setWindowTitle(QStringLiteral("Add MIDI device"));
                    configuration.setMinimumSize(700, 500);
                    auto* configurationLayout = new QVBoxLayout(&configuration);
                    auto* configureIntro = new QLabel(QStringLiteral(
                        "Choose one controller and how Jam2 should interpret its channel messages. "
                        "The device will open when an instrument is attached in PLUGINS."),
                        &configuration);
                    configureIntro->setWordWrap(true);
                    configurationLayout->addWidget(configureIntro);
                    auto* deviceBox = new QGroupBox(
                        QStringLiteral("Available devices"), &configuration);
                    auto* deviceLayout = new QVBoxLayout(deviceBox);
                    auto* deviceList = new QListWidget(deviceBox);
                    for (const MidiInputDeviceChoice& device : available)
                        deviceList->addItem(device.name);
                    deviceList->setCurrentRow(0);
                    registerGuiControl(
                        *deviceList,
                        QStringLiteral("performance.midi-add-dialog.device"),
                        QStringLiteral("performance.midi-input-assignment"),
                        GuiControlAvailability::HardwareProfile);
                    deviceLayout->addWidget(deviceList);
                    configurationLayout->addWidget(deviceBox, 1);
                    auto* modeBox = new QGroupBox(
                        QStringLiteral("Device configuration"), &configuration);
                    auto* modeLayout = new QFormLayout(modeBox);
                    auto* inputMode = new QComboBox(modeBox);
                    inputMode->addItem(QStringLiteral("Standard MIDI"),
                        static_cast<int>(jam2::midi::InputMode::Standard));
                    inputMode->addItem(QStringLiteral("MPE"),
                        static_cast<int>(jam2::midi::InputMode::Mpe));
                    registerGuiControl(
                        *inputMode,
                        QStringLiteral("performance.midi-add-dialog.mode"),
                        QStringLiteral("performance.midi-input-assignment"),
                        GuiControlAvailability::HardwareProfile);
                    modeLayout->addRow(QStringLiteral("Message mode"), inputMode);
                    auto* modeHelp = new QLabel(QStringLiteral(
                        "MPE preserves per-note channel expression. Standard MIDI keeps "
                        "ordinary channel-voice behaviour."), modeBox);
                    modeHelp->setWordWrap(true);
                    modeLayout->addRow(modeHelp);
                    configurationLayout->addWidget(modeBox);
                    auto* configurationButtons = new QDialogButtonBox(
                        QDialogButtonBox::Cancel, &configuration);
                    auto* assign = configurationButtons->addButton(
                        QStringLiteral("Add MIDI Device"),
                        QDialogButtonBox::AcceptRole);
                    registerGuiControl(
                        *assign,
                        QStringLiteral("performance.midi-add-dialog.accept"),
                        QStringLiteral("performance.midi-input-lifecycle"),
                        GuiControlAvailability::HardwareProfile);
                    registerGuiControl(
                        *configurationButtons->button(QDialogButtonBox::Cancel),
                        QStringLiteral("performance.midi-add-dialog.cancel"),
                        QStringLiteral("performance.midi-input-lifecycle"),
                        GuiControlAvailability::HardwareProfile);
                    QObject::connect(assign, &QPushButton::clicked,
                        &configuration, &QDialog::accept);
                    QObject::connect(configurationButtons,
                        &QDialogButtonBox::rejected,
                        &configuration, &QDialog::reject);
                    configurationLayout->addWidget(configurationButtons);
                    if (configuration.exec() != QDialog::Accepted ||
                        deviceList->currentRow() < 0) return;

                    const MidiInputDeviceChoice selected =
                        available.at(deviceList->currentRow());
                    const auto mode = static_cast<jam2::midi::InputMode>(
                        inputMode->currentData().toInt());
                    const MidiInputAssignmentError error = callbackState->assign
                        ? callbackState->assign(selected, mode)
                        : MidiInputAssignmentError::EngineStopped;
                    if (error == MidiInputAssignmentError::EngineStopped) {
                        QMessageBox::information(messageParent,
                            QStringLiteral("MIDI input"),
                            QStringLiteral(
                                "The audio engine stopped while MIDI inputs were being found."));
                        return;
                    }
                    if (error == MidiInputAssignmentError::SourceLimit) {
                        QMessageBox::warning(messageParent,
                            QStringLiteral("MIDI inputs"),
                            QStringLiteral(
                                "Jam2's 16 local input-source limit has been reached."));
                        return;
                    }
                    if (!dialogGuard.isNull()) {
                        if (const auto currentRebuild = rebuildWeak.lock())
                            (*currentRebuild)();
                    }
                });
            });
    };
    (*rebuild)();

    layout->addStretch(1);
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);
    auto* footer = new QWidget(&dialog);
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(18, 8, 18, 14);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, footer);
    registerGuiControl(
        *buttons->button(QDialogButtonBox::Close),
        QStringLiteral("performance.midi-input-dialog.close"),
        QStringLiteral("performance.midi-input-dialog"),
        GuiControlAvailability::Modal);
    footerLayout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected,
        &dialog, &QDialog::reject);
    outer->addWidget(footer);
    dialog.exec();
}

void InputPluginsDialog::run(
    InputPluginDialogCallbacks callbacks,
    QWidget* parent)
{
    if (!callbacks.snapshot) return;
    auto callbackState = std::make_shared<InputPluginDialogCallbacks>(
        std::move(callbacks));

    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Input plugins"));
    dialog.setMinimumSize(980, 680);
    dialog.setProperty("jam2MaximumDialogHeight", 800);
    QPointer<QDialog> dialogGuard(&dialog);

    auto* outer = new QVBoxLayout(&dialog);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(14);

    auto* intro = new QLabel(QStringLiteral(
        "Manage the plugin attached to each audio or MIDI source. Audio effects "
        "fall back to latency-aligned dry audio when bypassed; bypassed MIDI "
        "instruments are silent."), content);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* loading = new QGroupBox(QStringLiteral("Plugin loading"), content);
    auto* loadingLayout = new QVBoxLayout(loading);
    auto* loadingStatus = new QLabel(QStringLiteral("Ready"), loading);
    loadingStatus->setWordWrap(true);
    auto* loadingBar = new QProgressBar(loading);
    loadingBar->setObjectName(QStringLiteral("JamTasterProgress"));
    loadingBar->setRange(0, 100);
    loadingBar->setValue(0);
    loadingLayout->addWidget(loadingStatus);
    loadingLayout->addWidget(loadingBar);
    layout->addWidget(loading);

    const auto pluginLoadBusy = std::make_shared<bool>(false);
    QPointer<QLabel> loadingStatusGuard(loadingStatus);
    QPointer<QProgressBar> loadingBarGuard(loadingBar);
    const auto progress = [loadingStatusGuard, loadingBarGuard, pluginLoadBusy](
        int percent, const QString& text) {
        *pluginLoadBusy = percent < 0;
        if (loadingStatusGuard) loadingStatusGuard->setText(text);
        if (!loadingBarGuard) return;
        if (percent < 0) {
            loadingBarGuard->setRange(0, 0);
        } else {
            loadingBarGuard->setRange(0, 100);
            loadingBarGuard->setValue(qBound(0, percent, 100));
        }
    };

    auto* sourcePanels = new QWidget(content);
    auto* sourcePanelsLayout = new QVBoxLayout(sourcePanels);
    sourcePanelsLayout->setContentsMargins(0, 0, 0, 0);
    sourcePanelsLayout->setSpacing(14);
    layout->addWidget(sourcePanels);

    const auto rebuild = std::make_shared<std::function<void()>>();
    const std::weak_ptr<std::function<void()>> rebuildWeak = rebuild;
    *rebuild = [sourcePanels, sourcePanelsLayout, callbackState,
        dialogGuard, progress, pluginLoadBusy, rebuildWeak] {
        if (dialogGuard.isNull()) return;
        while (QLayoutItem* item = sourcePanelsLayout->takeAt(0)) {
            if (QWidget* widget = item->widget()) {
                widget->hide();
                widget->setParent(nullptr);
                widget->deleteLater();
            }
            delete item;
        }

        const InputPluginDialogState state = callbackState->snapshot();
        const auto addSourceTile = [callbackState, dialogGuard, progress,
            pluginLoadBusy, rebuildWeak, topologyLocked = state.topologyLocked](
                const InputPluginDialogSource& source,
                QVBoxLayout& targetLayout) {
            const bool isMidi =
                source.kind == InputPluginDialogKind::MidiInstrument;
            auto* tile = new QGroupBox(source.sourceName,
                targetLayout.parentWidget());
            auto* tileLayout = new QVBoxLayout(tile);
            auto* pluginStatus = new QLabel(tile);
            pluginStatus->setWordWrap(true);
            if (!source.loaded) {
                pluginStatus->setText(isMidi
                    ? QStringLiteral(
                        "No instrument loaded · MIDI source is silent")
                    : QStringLiteral(
                        "No plugin loaded · This source is dry"));
            } else if (!source.healthy) {
                pluginStatus->setText(isMidi
                    ? QStringLiteral("%1 · worker stopped · instrument silent")
                        .arg(source.pluginName)
                    : QStringLiteral(
                        "%1 · worker stopped · latency-aligned dry fallback")
                        .arg(source.pluginName));
            } else if (isMidi) {
                pluginStatus->setText(QStringLiteral(
                    "%1 · MIDI → %2 → mono · latency %3 + %4 transport frames · "
                    "process %5/%6 µs avg/max · misses %7 · concealed %8 · "
                    "queue %9/%10 · high %11 · drops %12")
                    .arg(source.pluginName)
                    .arg(source.stats.negotiatedOutputChannels)
                    .arg(source.stats.workerLatencyFrames)
                    .arg(source.stats.isolationLatencyFrames)
                    .arg(source.stats.workerProcessAverageUs)
                    .arg(source.stats.workerProcessMaxUs)
                    .arg(source.stats.deadlineMisses)
                    .arg(source.stats.deadlineConcealments)
                    .arg(source.stats.midiQueueDepth)
                    .arg(jam2::midi::kEventQueueCapacity)
                    .arg(source.stats.midiQueueHighWater)
                    .arg(source.stats.midiDropped));
            } else {
                pluginStatus->setText(QStringLiteral(
                    "%1 · I/O %2 → %3 → mono · latency %4 + %5 transport frames · "
                    "process %6/%7 µs avg/max · misses %8 · concealed %9")
                    .arg(source.pluginName)
                    .arg(source.stats.negotiatedInputChannels)
                    .arg(source.stats.negotiatedOutputChannels)
                    .arg(source.stats.workerLatencyFrames)
                    .arg(source.stats.isolationLatencyFrames)
                    .arg(source.stats.workerProcessAverageUs)
                    .arg(source.stats.workerProcessMaxUs)
                    .arg(source.stats.deadlineMisses)
                    .arg(source.stats.deadlineConcealments));
            }
            tileLayout->addWidget(pluginStatus);

            auto* actions = new QHBoxLayout();
            actions->addStretch(1);
            auto* open = new QPushButton(QStringLiteral("Open"), tile);
            auto* replace = new QPushButton(source.loaded
                ? QStringLiteral("Replace")
                : (isMidi ? QStringLiteral("Add Instrument")
                          : QStringLiteral("Add Plugin")), tile);
            auto* bypass = new QPushButton(QStringLiteral("Bypass"), tile);
            bypass->setObjectName(QStringLiteral("PluginBypassAction"));
            bypass->setCheckable(true);
            bypass->setChecked(source.bypassed);
            auto* remove = new QPushButton(QStringLiteral("Remove"), tile);
            remove->setObjectName(QStringLiteral("PluginRemoveAction"));
            open->setEnabled(source.loaded);
            replace->setEnabled(!topologyLocked);
            bypass->setEnabled(source.loaded);
            remove->setEnabled(source.loaded && !topologyLocked);

            const QString sourcePrefix = QStringLiteral(
                "performance.plugin-dialog.%1.%2")
                .arg(isMidi ? QStringLiteral("midi") : QStringLiteral("audio"))
                .arg(static_cast<qulonglong>(source.slot));
            const QString groupPrefix = QStringLiteral(
                "performance.plugin-dialog.%1")
                .arg(isMidi ? QStringLiteral("midi") : QStringLiteral("audio"));
            registerGuiControl(
                *open,
                sourcePrefix + QStringLiteral(".open"),
                QStringLiteral("performance.input-plugin-editor"),
                GuiControlAvailability::StateGated,
                groupPrefix);
            registerGuiControl(
                *replace,
                sourcePrefix + QStringLiteral(".load"),
                QStringLiteral("performance.input-plugin-lifecycle"),
                GuiControlAvailability::FileDialog,
                groupPrefix);
            registerGuiControl(
                *bypass,
                sourcePrefix + QStringLiteral(".bypass"),
                QStringLiteral("performance.input-plugin-routing"),
                GuiControlAvailability::StateGated,
                groupPrefix);
            registerGuiControl(
                *remove,
                sourcePrefix + QStringLiteral(".remove"),
                QStringLiteral("performance.input-plugin-lifecycle"),
                GuiControlAvailability::StateGated,
                groupPrefix);
            actions->addWidget(open);
            actions->addWidget(replace);
            actions->addWidget(bypass);
            actions->addWidget(remove);
            tileLayout->addLayout(actions);
            targetLayout.addWidget(tile);

            QObject::connect(open, &QPushButton::clicked, dialogGuard.data(),
                [callbackState, source] {
                    if (callbackState->open)
                        callbackState->open(source.kind, source.slot);
                });
            QObject::connect(bypass, &QPushButton::toggled, dialogGuard.data(),
                [callbackState, source](bool value) {
                    if (callbackState->setBypassed)
                        callbackState->setBypassed(source.kind, source.slot, value);
                });
            QObject::connect(remove, &QPushButton::clicked, dialogGuard.data(),
                [callbackState, source, rebuildWeak] {
                    if (callbackState->remove)
                        callbackState->remove(source.kind, source.slot);
                    if (const auto currentRebuild = rebuildWeak.lock())
                        (*currentRebuild)();
                });

            QPointer<QPushButton> openGuard(open);
            QPointer<QPushButton> replaceGuard(replace);
            QPointer<QPushButton> bypassGuard(bypass);
            QPointer<QPushButton> removeGuard(remove);
            QObject::connect(replace, &QPushButton::clicked, dialogGuard.data(),
                [callbackState, source, topologyLocked, progress,
                 pluginLoadBusy, openGuard, replaceGuard, bypassGuard,
                 removeGuard, rebuildWeak] {
                    if (*pluginLoadBusy) {
                        progress(-1, QStringLiteral(
                            "Finish the current plugin load before starting another."));
                        return;
                    }
                    const auto tileProgress = [source, topologyLocked, progress,
                        openGuard, replaceGuard, bypassGuard, removeGuard](
                            int percent, const QString& text) {
                        progress(percent, text);
                        const bool busy = percent < 0;
                        if (openGuard)
                            openGuard->setEnabled(!busy && source.loaded);
                        if (replaceGuard)
                            replaceGuard->setEnabled(!busy && !topologyLocked);
                        if (bypassGuard)
                            bypassGuard->setEnabled(!busy && source.loaded);
                        if (removeGuard) {
                            removeGuard->setEnabled(
                                !busy && source.loaded && !topologyLocked);
                        }
                    };
                    const auto finished = [rebuildWeak] {
                        if (const auto currentRebuild = rebuildWeak.lock())
                            (*currentRebuild)();
                    };
                    if (callbackState->load) {
                        (void)callbackState->load(
                            source.kind, source.slot, tileProgress, finished);
                    }
                });
        };

        auto* audioBox = new QGroupBox(
            QStringLiteral("Audio input effects"), sourcePanels);
        auto* audioLayout = new QVBoxLayout(audioBox);
        audioLayout->setSpacing(10);
        for (const InputPluginDialogSource& source : state.audioSources)
            addSourceTile(source, *audioLayout);
        if (state.audioSources.isEmpty()) {
            auto* empty = new QLabel(QStringLiteral(
                "No audio input channels were selected when the engine opened."),
                audioBox);
            empty->setWordWrap(true);
            audioLayout->addWidget(empty);
        }
        sourcePanelsLayout->addWidget(audioBox);

        auto* midiBox = new QGroupBox(
            QStringLiteral("MIDI instruments"), sourcePanels);
        auto* midiLayout = new QVBoxLayout(midiBox);
        midiLayout->setSpacing(10);
        for (const InputPluginDialogSource& source : state.midiSources)
            addSourceTile(source, *midiLayout);
        if (state.midiSources.isEmpty()) {
            auto* empty = new QLabel(QStringLiteral(
                "No MIDI devices are assigned. Add one from MIDI first."), midiBox);
            empty->setWordWrap(true);
            midiLayout->addWidget(empty);
        }
        sourcePanelsLayout->addWidget(midiBox);
    };
    (*rebuild)();

    layout->addStretch(1);
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);
    auto* footer = new QWidget(&dialog);
    auto* footerLayout = new QHBoxLayout(footer);
    footerLayout->setContentsMargins(18, 8, 18, 14);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, footer);
    registerGuiControl(
        *buttons->button(QDialogButtonBox::Close),
        QStringLiteral("performance.plugin-dialog.close"),
        QStringLiteral("performance.plugin-dialog"),
        GuiControlAvailability::Modal);
    footerLayout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected,
        &dialog, &QDialog::reject);
    outer->addWidget(footer);
    dialog.exec();
}

} // namespace jam2::gui

#include "AudioDeviceUiSupport.hpp"
#include "GuiControlContract.hpp"
#include "GuiPresentation.hpp"
#include "SessionStartupDialogs.hpp"
#include "SettingsDialog.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>

#include <iostream>
#include <stdexcept>

namespace {
using namespace jam2::gui;
using jam2::audio::AudioDeviceSetup;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T* control(QWidget* root, const QString& id) {
    for (auto* item : root->findChildren<T*>())
        if (guiControlId(*item) == id || item->objectName() == id) return item;
    throw std::runtime_error("missing actual control: " + id.toStdString());
}
void capture(QWidget& widget, const QString& fileName) {
    const QString root = qEnvironmentVariable("JAM2_TEST_ARTIFACT_ROOT");
    if (!root.isEmpty()) require(widget.grab().save(QDir(root).filePath(fileName)), "save actual dialog rendering");
}
void requireWarningLayout(QWidget& root, const QString& prefix) {
    auto* warning = control<QLabel>(&root, prefix + QStringLiteral(".no-duplex-device"));
    auto* output = control<QComboBox>(&root, prefix == QStringLiteral("session.dialog")
        ? prefix + QStringLiteral(".audio-device") : prefix + QStringLiteral(".device"));
    auto* rowWidget = control<QWidget>(&root, prefix + QStringLiteral(".no-duplex-warning"));
    auto* icon = control<QLabel>(&root, prefix + QStringLiteral(".no-duplex-icon"));
    bool fieldAligned = false;
    for (auto* form : root.findChildren<QFormLayout*>()) {
        int row = -1; QFormLayout::ItemRole role{};
        form->getWidgetPosition(rowWidget, &row, &role);
        if (row >= 0) fieldAligned = role == QFormLayout::FieldRole;
    }
    const bool fits = fieldAligned && warning->isVisible() && icon->isVisible() && !icon->pixmap().isNull() &&
        rowWidget->mapToGlobal(QPoint()).x() == output->mapToGlobal(QPoint()).x() &&
        icon->mapToGlobal(QPoint()).x() == rowWidget->mapToGlobal(QPoint()).x() &&
        warning->mapToGlobal(QPoint()).x() > icon->mapToGlobal(QPoint()).x() + icon->width() &&
        rowWidget->width() == output->width() &&
        warning->height() >= warning->heightForWidth(warning->width()) &&
        // A form can retain one line of padding after text stops wrapping at a wider size.
        rowWidget->height() <= qMax(icon->height(), warning->heightForWidth(warning->width())) + warning->fontMetrics().height() &&
        warning->alignment().testFlag(Qt::AlignTop) &&
        warning->mapToGlobal(QPoint()).y() == icon->mapToGlobal(QPoint()).y();
    if (!fits) {
        std::cerr << prefix.toStdString() << " field=" << fieldAligned << " row=" << rowWidget->width()
            << " output=" << output->width() << " text_height=" << warning->height()
            << " needed=" << warning->heightForWidth(warning->width())
            << " row_x=" << rowWidget->mapToGlobal(QPoint()).x() << " output_x=" << output->mapToGlobal(QPoint()).x()
            << " icon_x=" << icon->mapToGlobal(QPoint()).x() << " text_x=" << warning->mapToGlobal(QPoint()).x()
            << " visible=" << warning->isVisible() << '/' << icon->isVisible()
            << " icon_null=" << icon->pixmap().isNull() << '\n';
        capture(root, QStringLiteral("warning-layout-failure.png"));
    }
    require(fits, "warning icon and wrapped text align with the dropdown column without clipping");
}
std::vector<jam2::audio::DeviceInfo> devices() {
    std::vector<jam2::audio::DeviceInfo> result;
    for (int i = 0; i < 3; ++i) {
        jam2::audio::DeviceInfo device;
        device.id = i; device.backend = "CoreAudio";
        device.name = i == 0 ? "USB interface" : i == 1 ? "Headphones" : "USB microphone";
        device.clsid = "uid-" + std::to_string(i);
        device.input_channel_count = i == 1 ? 0 : 2;
        device.output_channel_count = i == 2 ? 0 : 2;
        result.push_back(device);
    }
    return result;
}
AudioDevicePreference preference(bool separate = false) {
    AudioDevicePreference value;
    value.backend = QStringLiteral("CoreAudio"); value.stableId = QStringLiteral("uid-0");
    value.coreAudioInputId = QStringLiteral("uid-2"); value.coreAudioInputName = QStringLiteral("USB microphone");
    value.deviceSetup = separate ? AudioDeviceSetup::SeparateInputOutput : AudioDeviceSetup::SingleDevice;
    return value;
}
void startup() {
    auto permission = preference();
    require(audioDeviceUsesInput(permission), "single-device capture requires microphone permission");
    permission.deviceSetup = AudioDeviceSetup::SeparateInputOutput;
    require(audioDeviceUsesInput(permission), "separate capture requires microphone permission");
    permission.coreAudioInputId.clear();
    require(!audioDeviceUsesInput(permission), "playback-only CoreAudio must not request microphone permission");
    permission.backend = QStringLiteral("WASAPI");
    require(!audioDeviceUsesInput(permission), "playback-only WASAPI must not request microphone permission");
    SessionAudioDeviceList list{audioDeviceChoices(devices()), QStringLiteral("0")};
    auto missingOutput = preference(); missingOutput.stableId = QStringLiteral("disconnected-output");
    require(preferredAudioDeviceId(list.devices, missingOutput).isEmpty(),
        "a disconnected saved output must not silently choose a different device for startup");
    require(preferredAudioDeviceId(list.devices, preference()) == QStringLiteral("0") &&
        preferredAudioDeviceId(list.devices, preference(), QStringLiteral("1")) == QStringLiteral("1"),
        "startup must resolve stable identities and respect an explicit device choice");
    LocalEngineDialogState initial; initial.audio = preference(); initial.selectedDeviceId = QStringLiteral("0");
    LocalEngineDialog dialog(initial, list, {}); dialog.show(); QApplication::processEvents();
    auto* setup = control<QComboBox>(&dialog, QStringLiteral("application.local-engine.device-setup"));
    require(setup->count() == 2 &&
        setup->itemText(setup->findData(static_cast<int>(AudioDeviceSetup::SingleDevice))) == QStringLiteral("Audio Interface") &&
        setup->itemText(setup->findData(static_cast<int>(AudioDeviceSetup::SeparateInputOutput))) == QStringLiteral("Separate Devices"),
        "CoreAudio setup labels describe the existing typed interface and separate-device modes");
    auto* input = control<QComboBox>(&dialog, QStringLiteral("application.local-engine.input-device"));
    auto* output = control<QComboBox>(&dialog, QStringLiteral("application.local-engine.device"));
    auto* channels = control<QLineEdit>(&dialog, QStringLiteral("application.local-engine.input-channels"));
    require(setup->isVisible() && input->isHidden() && channels->isVisible() && output->count() == 1,
        "single-device UI must offer duplex devices and native channel selection");
    require(!control<QLabel>(&dialog, QStringLiteral("application.local-engine.no-duplex-device"))->isVisible(),
        "available duplex devices must not show missing-device guidance");
    require(control<QComboBox>(&dialog, QStringLiteral("application.local-engine.driver"))->isHidden(),
        "CoreAudio must expose device setup rather than a redundant Driver row");
    setup->setCurrentIndex(1);
    require(input->isVisible() && output->count() == 2 && input->count() == 3,
        "separate mode must filter inputs/outputs by their reported channel counts");
    QApplication::processEvents();
    require(input->width() == output->width() && input->width() >= 420,
        "local input and output selectors must use the same wide boxes");
    output->setCurrentIndex(output->findData(QStringLiteral("1")));
    require(dialog.state().audio.deviceSetup == AudioDeviceSetup::SeparateInputOutput &&
        dialog.state().audio.stableId == QStringLiteral("uid-1") && dialog.state().audio.coreAudioInputId == QStringLiteral("uid-2"),
        "separate endpoint choices must reach typed startup state");
    input->setCurrentIndex(0);
    require(channels->isHidden() && dialog.state().audio.coreAudioInputId.isEmpty(),
        "None input must hide the channel editor and return playback-only state");
    QApplication::processEvents();
    require(input->width() == output->width(), "None input must retain the output selector width");
    setup->setCurrentIndex(0);
    require(output->currentData() == QStringLiteral("0") && channels->isVisible(), "single-device choice must be retained");
    setup->setCurrentIndex(1);
    require(output->currentData() == QStringLiteral("1") && input->currentData().toString().isEmpty(),
        "switching setup must retain separate output and optional input");
    QApplication::processEvents();
    require(input->width() == output->width(), "device mode changes must preserve matching selector widths");
    QTimer::singleShot(0, &dialog, [&] { dialog.reject(); });
    require(dialog.exec() == QDialog::Rejected, "local device setup supports normal modal cancellation");
    StartJamDialogState start; start.audio = preference(true);
    StartJamDialog creator(start, list, {});
    require(creator.state().audio.deviceSetup == AudioDeviceSetup::SeparateInputOutput &&
        creator.state().audio.coreAudioInputId == QStringLiteral("uid-2"), "Start Jam must restore separate CoreAudio input");
    JoinJamDialogState join; join.audio = preference(true);
    JoinJamDialog listener(join, list, {});
    require(listener.state().audio.deviceSetup == AudioDeviceSetup::SeparateInputOutput &&
        listener.state().audio.coreAudioInputId == QStringLiteral("uid-2"), "Join Jam must restore separate CoreAudio input");
    auto missing = initial; missing.audio = preference(true); missing.audio.coreAudioInputId = QStringLiteral("disconnected");
    LocalEngineDialog unavailable(missing, list, {});
    require(unavailable.state().audio.coreAudioInputId == QStringLiteral("disconnected") &&
        control<QComboBox>(&unavailable, QStringLiteral("application.local-engine.input-device"))->currentText().contains(QStringLiteral("Unavailable")),
        "missing saved inputs must remain explicit rather than selecting a different microphone");
}
void dualDeviceTestingAndGuidance() {
    auto metadata = devices(); metadata.erase(metadata.begin());
    SessionAudioDeviceList list{audioDeviceChoices(metadata), QStringLiteral("1")};
    LocalEngineDialogState initial; initial.audio = preference();
    initial.audio.stableId = QStringLiteral("uid-1"); initial.selectedDeviceId = QStringLiteral("1");
    int tests = 0;
    const SessionDeviceTest test = [&](const AudioDeviceTestRequest& request, QPushButton*, QWidget* parent) {
        require(request.outputDeviceId == 1 && request.inputDeviceId == 2 && request.separate,
            "Test Device must snapshot both physical selections");
        std::vector<int> probed;
        const auto report = testAudioDeviceSelection(request, [&](int id) {
            probed.push_back(id);
            jam2::audio::DeviceTestResult result;
            result.device = metadata.at(id - 1); return result;
        });
        require(probed == std::vector<int>{1, 2} && report.devices.size() == 2,
            "both physical devices must be tested exactly once");
        bool inspected = false;
        QTimer::singleShot(0, [&] {
            auto* result = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!result) return;
            auto* message = result->findChild<QLabel*>(QStringLiteral("AudioDeviceTestMessage"));
            inspected = message && message->text().contains(QStringLiteral("Output device\nDevice: CoreAudio Headphones")) &&
                message->text().contains(QStringLiteral("Input device\nDevice: CoreAudio USB microphone")) &&
                message->text().contains(QStringLiteral("Input channels: None\nOutput channels: 1,2\n")) &&
                message->text().contains(QStringLiteral("Input channels: 1,2\nOutput channels: None\n"));
            result->accept();
        });
        showAudioDeviceTestMessage(parent, report.text);
        require(inspected, "real modal results must identify both devices and their separate capabilities");
        ++tests;
    };
    LocalEngineDialog local(initial, list, {test}); local.show(); QApplication::processEvents();
    auto* warning = control<QLabel>(&local, QStringLiteral("application.local-engine.no-duplex-device"));
    auto* setup = control<QComboBox>(&local, QStringLiteral("application.local-engine.device-setup"));
    require(warning->isVisible() && warning->text().contains(QStringLiteral("Select Separate Devices instead")) &&
        control<QComboBox>(&local, QStringLiteral("application.local-engine.device"))->count() == 0,
        "one-device mode explains when no duplex device is available");
    for (int width : {560, 760}) {
        local.resize(width, local.height()); QApplication::processEvents();
        requireWarningLayout(local, QStringLiteral("application.local-engine"));
    }
    capture(local, QStringLiteral("no-interface-local.png"));
    setup->setCurrentIndex(1); QApplication::processEvents();
    require(!warning->isVisible() && !control<QLabel>(&local,
        QStringLiteral("application.local-engine.no-duplex-icon"))->isVisible(),
        "separate-device mode hides both the warning icon and guidance");
    control<QPushButton>(&local, QStringLiteral("application.local-engine.test-device"))->click();
    setup->setCurrentIndex(0); QApplication::processEvents();
    require(warning->isVisible(), "returning to one-device mode restores guidance");
    local.hide();
    StartJamDialogState start; start.audio = initial.audio; start.audio.deviceSetup = AudioDeviceSetup::SeparateInputOutput;
    StartJamDialog creator(start, list, {{}, test, {}, {}}); creator.show(); QApplication::processEvents();
    for (auto* button : creator.findChildren<QPushButton*>()) if (button->text() == QStringLiteral("Test Device")) button->click();
    control<QComboBox>(&creator, QStringLiteral("session.dialog.device-setup"))->setCurrentIndex(0);
    QApplication::processEvents(); requireWarningLayout(creator, QStringLiteral("session.dialog"));
    capture(creator, QStringLiteral("no-interface-create.png"));
    creator.hide();
    JoinJamDialogState join; join.audio = start.audio;
    JoinJamDialog listener(join, list, {{}, test, {}}); listener.show(); QApplication::processEvents();
    for (auto* button : listener.findChildren<QPushButton*>()) if (button->text() == QStringLiteral("Test Device")) button->click();
    control<QComboBox>(&listener, QStringLiteral("session.dialog.device-setup"))->setCurrentIndex(0);
    QApplication::processEvents(); requireWarningLayout(listener, QStringLiteral("session.dialog"));
    capture(listener, QStringLiteral("no-interface-join.png"));
    listener.hide(); require(tests == 3, "Local, Start Jam and Join Jam actions test both selected devices");
    SettingsDialogInput settingsInput; settingsInput.devices = metadata; settingsInput.localAudio = initial.audio;
    settingsInput.preferences.localAudio = initial.audio;
    bool settingsWarning = false;
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        try {
            requireWarningLayout(*dialog, QStringLiteral("application.settings-dialog.audio.local"));
            capture(*dialog, QStringLiteral("no-interface-settings.png")); settingsWarning = true;
        } catch (const std::exception& e) { std::cerr << e.what() << '\n'; }
        dialog->reject();
    });
    require(!SettingsDialog::run(settingsInput, {}) && settingsWarning,
        "Settings audio warning aligns with its dropdowns and supports cancellation");

    int calls = 0;
    const AudioDeviceProbe probe = [&](int id) { ++calls; jam2::audio::DeviceTestResult result; result.device = metadata.at(id - 1); return result; };
    require(testAudioDeviceSelection({1, 1, true}, probe).devices.size() == 1 && calls == 1,
        "one physical device selected twice is probed once");
    require(testAudioDeviceSelection({1, {}, true}, probe).text.contains(QStringLiteral("Input device: None")),
        "playback-only results explicitly report None input");
    const auto missing = testAudioDeviceSelection({1, -1, true}, probe);
    require(missing.devices.size() == 1 && missing.text.contains(QStringLiteral("Input device\nSelected audio device is unavailable")),
        "missing saved input does not hide successful output results");
    const auto failed = testAudioDeviceSelection({1, 2, true}, [&](int id) {
        if (id == 1) throw std::runtime_error("output disconnected"); return probe(id);
    });
    require(failed.devices.size() == 1 && failed.text.contains(QStringLiteral("output disconnected")) &&
        failed.text.contains(QStringLiteral("USB microphone")), "probe failures retain results for the other device");
}

void settings() {
    QTemporaryDir folder; QString error;
    require(folder.isValid() && UserPreferencesStore::setFilePathForTesting(folder.filePath(QStringLiteral("audio.ini")), error), "isolated settings required");
    SettingsDialogInput initial; initial.devices = devices(); initial.localAudio = preference();
    initial.preferences.localAudio = initial.localAudio; initial.preferences.networkAudio = preference(true);
    int applied = 0;
    SettingsDialogCallbacks callbacks;
    int tested = 0;
    callbacks.testDevice = [&](const AudioDeviceTestRequest& request, QPushButton*, QWidget*) {
        require(request.outputDeviceId == 1 && request.inputDeviceId == 2 && request.separate,
            "Settings Test Device must dispatch both selected devices"); ++tested;
    };
    callbacks.applyLocalAudio = [&](const AudioDevicePreference& value, const QString& id, QWidget*) {
        require(value.deviceSetup == AudioDeviceSetup::SeparateInputOutput && value.coreAudioInputId == QStringLiteral("uid-2") && id == QStringLiteral("1"),
            "Apply Audio must dispatch separate device choices");
        ++applied; return true;
    };
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        try {
            auto* setup = control<QComboBox>(dialog, QStringLiteral("application.settings-dialog.audio.local.device-setup"));
            require(setup->itemText(0) == QStringLiteral("Audio Interface") && setup->itemText(1) == QStringLiteral("Separate Devices"),
                "Settings exposes the same descriptive CoreAudio setup choices");
            setup->setCurrentIndex(1);
            auto* output = control<QComboBox>(dialog, QStringLiteral("application.settings-dialog.audio.local.device"));
            output->setCurrentIndex(output->findData(QStringLiteral("1")));
            control<QPushButton>(dialog, QStringLiteral("application.settings-dialog.audio.local.test-device"))->click();
            control<QPushButton>(dialog, QStringLiteral("application.settings-dialog.audio.local.apply"))->click();
            control<QPushButton>(dialog, QStringLiteral("application.settings-dialog.save"))->click();
        } catch (const std::exception& e) { std::cerr << e.what() << '\n'; dialog->reject(); }
    });
    const auto result = SettingsDialog::run(initial, callbacks);
    require(result && tested == 1 && applied == 1 && result->preferences.localAudio.deviceSetup == AudioDeviceSetup::SeparateInputOutput &&
        result->preferences.localAudio.coreAudioInputId == QStringLiteral("uid-2"), "Settings Save must return separate devices");
    require(UserPreferencesStore::saveChecked(result->preferences), "CoreAudio preferences save");
    const auto loaded = UserPreferencesStore::load();
    require(loaded.localAudio.deviceSetup == AudioDeviceSetup::SeparateInputOutput &&
        loaded.localAudio.stableId == QStringLiteral("uid-1") && loaded.localAudio.coreAudioInputId == QStringLiteral("uid-2") &&
        loaded.networkAudio.coreAudioInputId == QStringLiteral("uid-2"), "separate physical UIDs and setup mode survive persistence");
    QTimer::singleShot(0, [] {
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
            control<QComboBox>(dialog, QStringLiteral("application.settings-dialog.audio.local.device-setup"))->setCurrentIndex(1);
            dialog->reject();
        }
    });
    require(!SettingsDialog::run(initial, {}), "Settings Cancel discards setup edits");
    require(initial.localAudio.deviceSetup == AudioDeviceSetup::SingleDevice, "Cancel must preserve caller state");
    jam2::audio::StreamInfo stream; stream.device.backend = "CoreAudio"; stream.device.name = "<speakers>";
    stream.input_device_name = "<mic>"; stream.private_aggregate = true; stream.input_drift_compensation = true;
    const auto text = audioDeviceRuntimeText(stream, {});
    require(text.contains(QStringLiteral("&lt;mic&gt;")) && text.contains(QStringLiteral("Private aggregate: yes")) &&
        text.contains(QStringLiteral("Clock: output")), "diagnostics must expose the aggregate clock and escape device names");
}

void settingsPeerName() {
    QTemporaryDir folder; QString error;
    require(folder.isValid() && UserPreferencesStore::setFilePathForTesting(folder.filePath(QStringLiteral("name.ini")), error),
        "peer name test uses isolated saved preferences");
    SettingsDialogInput initial; initial.devices = devices(); initial.localAudio = preference();
    initial.preferences.localAudio = initial.localAudio;
    initial.preferences.create.name = QStringLiteral("Alex");
    initial.preferences.join.name = QStringLiteral("Blair");
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        try {
            auto* navigation = control<QListWidget>(dialog, QStringLiteral("application.settings-dialog.navigation"));
            const auto general = navigation->findItems(QStringLiteral("General"), Qt::MatchExactly);
            require(general.size() == 1, "Settings has a discoverable General page");
            navigation->setCurrentItem(general.front()); QApplication::processEvents();
            auto* name = control<QLineEdit>(dialog, QStringLiteral("application.settings-dialog.identity.peer-name"));
            auto* save = control<QPushButton>(dialog, QStringLiteral("application.settings-dialog.save"));
            require(name->isVisible() && name->text() == QStringLiteral("Alex"), "General displays the existing saved peer name");
            for (const QString& invalid : {QStringLiteral("Two Words"), QStringLiteral("Alex7"), QStringLiteral("TooLongName")}) {
                name->setText(invalid); require(!save->isEnabled(), "invalid peer name disables Settings Save");
                save->click(); require(dialog->isVisible(), "invalid peer name cannot accept the real Settings dialog");
            }
            name->setText(QStringLiteral("E\u0301lodie"));
            require(save->isEnabled(), "valid Unicode name enables Settings Save");
            capture(*dialog, QStringLiteral("settings-peer-name.png"));
            save->click();
        } catch (const std::exception& e) { std::cerr << e.what() << '\n'; dialog->reject(); }
    });
    const auto result = SettingsDialog::run(initial, {});
    require(result && result->preferences.create.name == QStringLiteral("\u00c9lodie") &&
        result->preferences.join.name == result->preferences.create.name, "Settings saves one canonical peer name for both roles");
    require(UserPreferencesStore::saveChecked(result->preferences), "save configured name to disk");
    const auto loaded = UserPreferencesStore::load();
    SessionAudioDeviceList list{audioDeviceChoices(devices()), QStringLiteral("0")};
    StartJamDialogState create; create.create = loaded.create; create.audio = preference();
    JoinJamDialogState join; join.join = loaded.join; join.audio = preference();
    StartJamDialog creator(create, list, {}); JoinJamDialog listener(join, list, {});
    require(control<QLineEdit>(&creator, QStringLiteral("session.dialog.name"))->text() == QStringLiteral("\u00c9lodie") &&
        control<QLineEdit>(&listener, QStringLiteral("session.dialog.name"))->text() == QStringLiteral("\u00c9lodie"),
        "reloaded preferences automatically fill the actual Create and Join name fields");
    auto saved = initial; saved.preferences = loaded;
    QTimer::singleShot(0, [] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        control<QLineEdit>(dialog, QStringLiteral("application.settings-dialog.identity.peer-name"))->setText(QStringLiteral("Casey"));
        control<QPushButton>(dialog, QStringLiteral("application.settings-dialog.cancel"))->click();
    });
    require(!SettingsDialog::run(saved, {}) && UserPreferencesStore::load().join.name == QStringLiteral("\u00c9lodie"),
        "Cancel preserves the persisted name for both roles");
    QTimer::singleShot(0, [] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        control<QLineEdit>(dialog, QStringLiteral("application.settings-dialog.identity.peer-name"))->clear();
        control<QPushButton>(dialog, QStringLiteral("application.settings-dialog.save"))->click();
    });
    const auto cleared = SettingsDialog::run(saved, {});
    require(cleared && cleared->preferences.create.name.isEmpty() && cleared->preferences.join.name.isEmpty(),
        "clearing the optional Settings name clears both dialog defaults");
    initial.preferences.create.name.clear(); initial.preferences.join.name = QStringLiteral("Casey");
    QTimer::singleShot(0, [] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        auto* name = control<QLineEdit>(dialog, QStringLiteral("application.settings-dialog.identity.peer-name"));
        if (name->text() != QStringLiteral("Casey")) { dialog->reject(); return; }
        control<QPushButton>(dialog, QStringLiteral("application.settings-dialog.save"))->click();
    });
    const auto legacy = SettingsDialog::run(initial, {});
    require(legacy && legacy->preferences.create.name == QStringLiteral("Casey"),
        "Settings retains a legacy Join-only default when no Create name was saved");
}
}
int main(int argc, char** argv) {
    QApplication app(argc, argv); installJam2Style();
    try { startup(); dualDeviceTestingAndGuidance(); settings(); settingsPeerName(); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    std::cout << "CoreAudio setup controls, modal actions and persistence passed\n";
}

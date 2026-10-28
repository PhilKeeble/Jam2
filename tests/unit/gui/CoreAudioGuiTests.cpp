#include "AudioDeviceUiSupport.hpp"
#include "GuiControlContract.hpp"
#include "GuiPresentation.hpp"
#include "SessionStartupDialogs.hpp"
#include "SettingsDialog.hpp"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
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
    auto* input = control<QComboBox>(&dialog, QStringLiteral("application.local-engine.input-device"));
    auto* output = control<QComboBox>(&dialog, QStringLiteral("application.local-engine.device"));
    auto* channels = control<QLineEdit>(&dialog, QStringLiteral("application.local-engine.input-channels"));
    require(setup->isVisible() && input->isHidden() && channels->isVisible() && output->count() == 1,
        "single-device UI must offer duplex devices and native channel selection");
    require(control<QComboBox>(&dialog, QStringLiteral("application.local-engine.driver"))->isHidden(),
        "CoreAudio must expose device setup rather than a redundant Driver row");
    setup->setCurrentIndex(1);
    require(input->isVisible() && output->count() == 2 && input->count() == 3,
        "separate mode must filter inputs/outputs by their reported channel counts");
    output->setCurrentIndex(output->findData(QStringLiteral("1")));
    require(dialog.state().audio.deviceSetup == AudioDeviceSetup::SeparateInputOutput &&
        dialog.state().audio.stableId == QStringLiteral("uid-1") && dialog.state().audio.coreAudioInputId == QStringLiteral("uid-2"),
        "separate endpoint choices must reach typed startup state");
    input->setCurrentIndex(0);
    require(channels->isHidden() && dialog.state().audio.coreAudioInputId.isEmpty(),
        "None input must hide the channel editor and return playback-only state");
    setup->setCurrentIndex(0);
    require(output->currentData() == QStringLiteral("0") && channels->isVisible(), "single-device choice must be retained");
    setup->setCurrentIndex(1);
    require(output->currentData() == QStringLiteral("1") && input->currentData().toString().isEmpty(),
        "switching setup must retain separate output and optional input");
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
void settings() {
    QTemporaryDir folder; QString error;
    require(folder.isValid() && UserPreferencesStore::setFilePathForTesting(folder.filePath(QStringLiteral("audio.ini")), error), "isolated settings required");
    SettingsDialogInput initial; initial.devices = devices(); initial.localAudio = preference();
    initial.preferences.localAudio = initial.localAudio; initial.preferences.networkAudio = preference(true);
    int applied = 0;
    SettingsDialogCallbacks callbacks;
    callbacks.applyLocalAudio = [&](const AudioDevicePreference& value, const QString& id, QWidget*) {
        require(value.deviceSetup == AudioDeviceSetup::SeparateInputOutput && value.coreAudioInputId == QStringLiteral("uid-2") && id == QStringLiteral("1"),
            "Apply Audio must dispatch separate device choices");
        ++applied; return true;
    };
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        try {
            control<QComboBox>(dialog, QStringLiteral("application.settings-dialog.audio.local.device-setup"))->setCurrentIndex(1);
            auto* output = control<QComboBox>(dialog, QStringLiteral("application.settings-dialog.audio.local.device"));
            output->setCurrentIndex(output->findData(QStringLiteral("1")));
            control<QPushButton>(dialog, QStringLiteral("application.settings-dialog.audio.local.apply"))->click();
            control<QPushButton>(dialog, QStringLiteral("application.settings-dialog.save"))->click();
        } catch (const std::exception& e) { std::cerr << e.what() << '\n'; dialog->reject(); }
    });
    const auto result = SettingsDialog::run(initial, callbacks);
    require(result && applied == 1 && result->preferences.localAudio.deviceSetup == AudioDeviceSetup::SeparateInputOutput &&
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
}
int main(int argc, char** argv) {
    QApplication app(argc, argv); installJam2Style();
    try { startup(); settings(); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    std::cout << "CoreAudio setup controls, modal actions and persistence passed\n";
}

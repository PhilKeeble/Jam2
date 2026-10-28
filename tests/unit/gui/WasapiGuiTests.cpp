#include "AudioDeviceUiSupport.hpp"
#include "SessionStartupDialogs.hpp"
#include "SettingsDialog.hpp"
#include "GuiControlContract.hpp"
#include "GuiPresentation.hpp"

#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLineEdit>
#include <QLabel>
#include <QFontMetrics>
#include <QFormLayout>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>

#include <iostream>
#include <stdexcept>

namespace {
using namespace jam2::gui;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T* control(QWidget* parent, const QString& id) {
    for (auto* item : parent->findChildren<T*>()) if (guiControlId(*item) == id || item->objectName() == id) return item;
    throw std::runtime_error("missing actual GUI control: " + id.toStdString());
}
std::vector<jam2::audio::DeviceInfo> devices() {
    using namespace jam2::audio;
    return {
        {0, "ASIO", "Interface ASIO", "asio-id", {}},
        {1, "WASAPI", "Headphones", {}, {}, "headphones-id", EndpointDirection::Output, 2, 128, 480},
        {2, "WASAPI", "Surround", {}, {}, "surround-id", EndpointDirection::Output, 8, 240, 480},
        {3, "WASAPI", "USB microphone", {}, {}, "usb-microphone-id", EndpointDirection::Input, 2},
    };
}
AudioDevicePreference wasapiPreference() {
    AudioDevicePreference result;
    result.backend = QStringLiteral("WASAPI"); result.stableId = QStringLiteral("headphones-id");
    result.name = QStringLiteral("Headphones"); result.wasapiInputId = QStringLiteral("usb-microphone-id");
    result.wasapiInputName = QStringLiteral("USB microphone");
    result.bufferSize = 480; // Legacy saved period choices must become automatic.
    return result;
}
void channelTextFits(QLabel& summary, const QString& expected)
{
    QApplication::processEvents();
    require(summary.text() == expected && !summary.wordWrap(),
        "channel summary must use complete, explicit input/output lines");
    const auto textSize = summary.fontMetrics().size(Qt::TextExpandTabs, summary.text());
    require(summary.contentsRect().width() >= textSize.width() && summary.contentsRect().height() >= textSize.height(),
        "channel summary must have room to display every line without clipping");
}
void startup() {
    const SessionAudioDeviceList list{audioDeviceChoices(devices()), QStringLiteral("1")};
    LocalEngineDialogState initial;
    initial.selectedDeviceId = QStringLiteral("1"); initial.audio = wasapiPreference();
    initial.inputChannels = QStringLiteral("1"); initial.outputChannels = QStringLiteral("1,2");
    int tests = 0;
    LocalEngineDialog dialog(initial, list, {[&](QComboBox* output, QPushButton*, QWidget*) {
        require(output->currentData().toString() == QStringLiteral("2"), "Test Device must receive chosen WASAPI output");
        ++tests;
    }});
    dialog.show(); QApplication::processEvents();
    auto* driver = control<QComboBox>(&dialog, QStringLiteral("application.local-engine.driver"));
    auto* input = control<QComboBox>(&dialog, QStringLiteral("application.local-engine.input-device"));
    auto* output = control<QComboBox>(&dialog, QStringLiteral("application.local-engine.device"));
    auto* channels = control<QLineEdit>(&dialog, QStringLiteral("application.local-engine.input-channels"));
    require(driver->currentData() == QStringLiteral("WASAPI") && input->isVisible() && !channels->isVisible(),
        "saved WASAPI selection must show endpoint controls and hide channel editing");
    require(output->count() == 2 && input->count() == 2 &&
        input->currentData() == QStringLiteral("usb-microphone-id"), "input/output endpoints must be filtered and restored independently");
    auto* period = control<QComboBox>(&dialog, QStringLiteral("application.local-engine.buffer-size"));
    require(period->isHidden() && period->currentData().toInt() == 0 && dialog.state().bufferSize == 0,
        "WASAPI must hide the period selector and return automatic buffering");
    auto* form = qobject_cast<QFormLayout*>(dialog.layout());
    require(form && form->labelForField(period)->isHidden(), "the unused WASAPI period caption must also be hidden");
    auto* summary = control<QLabel>(&dialog, QStringLiteral("application.local-engine.channel-summary"));
    channelTextFits(*summary, QStringLiteral("Input: 2 channels\nOutput: Stereo"));
    input->setCurrentIndex(0); output->setCurrentIndex(1);
    require(period->isHidden() && period->currentData().toInt() == 0,
        "changing WASAPI endpoints must keep the automatic period without exposing a selector");
    channelTextFits(*summary, QStringLiteral("Input: None\nOutput: Stereo"));
    control<QPushButton>(&dialog, QStringLiteral("application.local-engine.test-device"))->click();
    require(tests == 1 && dialog.state().audio.wasapiInputId.isEmpty() &&
        dialog.state().audio.stableId == QStringLiteral("surround-id"), "optional input and edited output must reach typed startup state");
    driver->setCurrentIndex(driver->findData(QStringLiteral("ASIO")));
    require(!input->isVisible() && channels->isVisible() && output->count() == 1,
        "ASIO selection must restore channel controls and exclude WASAPI endpoints");
    require(period->isVisible() && period->findData(32) >= 0 && period->findData(480) < 0,
        "switching to ASIO must restore the ASIO buffer editor");
    period->setCurrentIndex(period->findData(128));
    driver->setCurrentIndex(driver->findData(QStringLiteral("WASAPI")));
    require(output->currentData() == QStringLiteral("2") && input->currentData().toString().isEmpty(),
        "switching drivers must preserve each in-dialog endpoint choice");
    require(period->isHidden() && period->currentData().toInt() == 0, "returning to WASAPI must always use automatic buffering");
    driver->setCurrentIndex(driver->findData(QStringLiteral("ASIO")));
    require(period->currentData().toInt() == 128, "ASIO must retain its own selected buffer size");
    driver->setCurrentIndex(driver->findData(QStringLiteral("WASAPI")));
    QTimer::singleShot(0, &dialog, [&] { dialog.reject(); });
    require(dialog.exec() == QDialog::Rejected, "audio selection must retain normal modal cancellation");

    StartJamDialogState create; create.audio = wasapiPreference();
    StartJamDialog creator(create, list, {});
    require(creator.state().audio.wasapiInputId == create.audio.wasapiInputId &&
        creator.state().audio.backend == QStringLiteral("WASAPI"), "create dialog must return WASAPI preferences");
    require(control<QComboBox>(&creator, QStringLiteral("session.start-dialog.buffer-size"))->isHidden() &&
        creator.state().create.tuning.bufferSize == 0, "Start Jam must hide the WASAPI period and use automatic buffering");
    JoinJamDialogState join; join.audio = wasapiPreference();
    JoinJamDialog listener(join, list, {});
    require(listener.state().audio.stableId == join.audio.stableId &&
        listener.state().audio.wasapiInputId == join.audio.wasapiInputId, "join dialog must return WASAPI preferences");
    require(control<QComboBox>(&listener, QStringLiteral("session.join-dialog.buffer-size"))->isHidden() &&
        listener.state().join.tuning.bufferSize == 0, "Join Jam must hide the WASAPI period and use automatic buffering");
    auto missing = initial; missing.selectedDeviceId.clear();
    missing.audio.stableId = QStringLiteral("unplugged-output");
    LocalEngineDialog unplugged(missing, list, {});
    require(unplugged.state().selectedDeviceId.isEmpty(), "an unavailable saved endpoint must not silently select another output");
}

void settings_and_persistence() {
    QTemporaryDir directory; require(directory.isValid(), "temporary preferences directory required");
    QString error;
    require(UserPreferencesStore::setFilePathForTesting(directory.filePath(QStringLiteral("wasapi.ini")), error),
        "test preference destination required");
    SettingsDialogInput initial;
    initial.devices = devices(); initial.localAudio = wasapiPreference();
    initial.preferences.localAudio = initial.localAudio;
    initial.preferences.networkAudio = initial.localAudio;
    auto savedInput = initial;
    int applied = 0, tested = 0;
    SettingsDialogCallbacks callbacks;
    callbacks.applyLocalAudio = [&](const AudioDevicePreference& value, const QString& id, QWidget*) {
        require(value.backend == QStringLiteral("WASAPI") && value.wasapiInputId.isEmpty() && id == QStringLiteral("2"),
            "Settings Apply Audio must dispatch typed output and optional input");
        ++applied; return true;
    };
    callbacks.testDevice = [&](QComboBox* output, QPushButton* button, QWidget* parent) {
        require(output->currentData() == QStringLiteral("2"), "Settings Test Device must select the displayed output");
        button->setEnabled(false);
        QTimer::singleShot(0, parent, [button] { button->setEnabled(true); });
        ++tested;
    };
    bool modalChecked = false;
    QTimer::singleShot(0, [&] {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!dialog) return;
        try {
            auto* input = control<QComboBox>(dialog, QStringLiteral("application.settings-dialog.audio.local.input-device"));
            auto* output = control<QComboBox>(dialog, QStringLiteral("application.settings-dialog.audio.local.device"));
            require(input->currentData() == QStringLiteral("usb-microphone-id"), "Settings must restore saved capture endpoint");
            require(control<QComboBox>(dialog, QStringLiteral("application.settings-dialog.audio.local.buffer-size"))->isHidden() &&
                control<QComboBox>(dialog, QStringLiteral("application.settings-dialog.create.tuning.buffer-size"))->isHidden() &&
                control<QComboBox>(dialog, QStringLiteral("application.settings-dialog.join.tuning.buffer-size"))->isHidden(),
                "Settings must hide local and shared Create/Join WASAPI period selectors");
            channelTextFits(*control<QLabel>(dialog, QStringLiteral("application.settings-dialog.audio.local.channel-summary")),
                QStringLiteral("Input: 2 channels\nOutput: Stereo"));
            input->setCurrentIndex(0); output->setCurrentIndex(output->findData(QStringLiteral("2")));
            control<QPushButton>(dialog, QStringLiteral("application.settings-dialog.audio.local.test-device"))->click();
            control<QPushButton>(dialog, QStringLiteral("application.settings-dialog.audio.local.apply"))->click();
            modalChecked = true;
            control<QPushButton>(dialog, QStringLiteral("application.settings-dialog.save"))->click();
        } catch (const std::exception& e) { std::cerr << e.what() << '\n'; dialog->reject(); }
    });
    const auto result = SettingsDialog::run(initial, callbacks);
    require(result && modalChecked && applied == 1 && tested == 1,
        "real Settings modal must expose WASAPI editors, Test Device, Apply Audio and Save");
    require(result->preferences.localAudio.wasapiInputId.isEmpty() &&
        result->preferences.localAudio.stableId == QStringLiteral("surround-id") &&
        result->preferences.networkAudio.wasapiInputId == QStringLiteral("usb-microphone-id"),
        "Settings must preserve independent local and network WASAPI preferences");
    require(UserPreferencesStore::saveChecked(result->preferences), "WASAPI preferences must save");
    const auto loaded = UserPreferencesStore::load();
    require(loaded.localAudio.backend == QStringLiteral("WASAPI") && loaded.localAudio.wasapiInputId.isEmpty() &&
        loaded.localAudio.stableId == QStringLiteral("surround-id") &&
        loaded.networkAudio.wasapiInputId == QStringLiteral("usb-microphone-id"), "WASAPI endpoint IDs must survive persistence");
    QTimer::singleShot(0, [] {
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
            control<QComboBox>(dialog, QStringLiteral("application.settings-dialog.audio.local.input-device"))->setCurrentIndex(0);
            dialog->reject();
        }
    });
    require(!SettingsDialog::run(savedInput, {}), "Settings Cancel must not return edited preferences");
    require(savedInput.preferences.localAudio.wasapiInputId == QStringLiteral("usb-microphone-id"),
        "cancelled WASAPI edits must leave the caller's preferences untouched");

    QTimer::singleShot(0, [] {
        if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
            try {
                control<QCheckBox>(dialog, QStringLiteral("application.settings-dialog.audio.local.split-network-by-role"))->setChecked(true);
                auto* createDriver = control<QComboBox>(dialog, QStringLiteral("application.settings-dialog.audio.create.driver"));
                createDriver->setCurrentIndex(createDriver->findData(QStringLiteral("ASIO")));
                control<QComboBox>(dialog, QStringLiteral("application.settings-dialog.audio.join.input-device"))->setCurrentIndex(0);
                auto* createPeriod = control<QComboBox>(dialog, QStringLiteral("application.settings-dialog.create.tuning.buffer-size"));
                auto* joinPeriod = control<QComboBox>(dialog, QStringLiteral("application.settings-dialog.join.tuning.buffer-size"));
                require(!createPeriod->isHidden() && createPeriod->findData(32) >= 0 &&
                    joinPeriod->isHidden() && joinPeriod->currentData().toInt() == 0,
                    "per-role Settings must show ASIO buffering and hide automatic WASAPI buffering");
                control<QPushButton>(dialog, QStringLiteral("application.settings-dialog.save"))->click();
            } catch (const std::exception& e) { std::cerr << e.what() << '\n'; dialog->reject(); }
        }
    });
    const auto split = SettingsDialog::run(savedInput, {});
    require(split && split->preferences.splitNetworkAudioByRole &&
        split->preferences.createJamAudio.backend == QStringLiteral("ASIO") &&
        split->preferences.joinJamAudio.backend == QStringLiteral("WASAPI") &&
        split->preferences.joinJamAudio.wasapiInputId.isEmpty() && split->preferences.join.tuning.bufferSize == 0,
        "Settings must return independent create/join drivers, optional inputs and automatic WASAPI buffering");
    require(UserPreferencesStore::saveChecked(split->preferences) &&
        UserPreferencesStore::load().join.tuning.bufferSize == 0,
        "automatic WASAPI buffering must survive persistence");
    jam2::audio::StreamInfo stream;
    stream.device.backend = "WASAPI"; stream.input_device_name = "<microphone>"; stream.device.name = "Headphones";
    stream.sample_rate = 48000; stream.output_period_frames = 480;
    jam2::audio::DeviceStreamStatus status; status.capture_queued_frames = 321; status.capture_correction_ppm = -17;
    const auto diagnostics = audioDeviceRuntimeText(stream, status);
    require(diagnostics.contains(QStringLiteral("&lt;microphone&gt;")) &&
        diagnostics.contains(QStringLiteral("Capture queue 321 frames")) && diagnostics.contains(QStringLiteral("Correction -17 ppm")),
        "GUI device diagnostics must present escaped endpoint names and measured queue/correction data");
}
} // namespace

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    installJam2Style();
    try { startup(); settings_and_persistence(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "WASAPI startup, Settings actions and preferences passed\n";
}

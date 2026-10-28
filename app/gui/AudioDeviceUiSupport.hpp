#pragma once

#include "UserPreferences.hpp"

#include "audio_device.hpp"

#include <QString>
#include <QObject>
#include <QVector>

#include <vector>
#include <optional>
#include <functional>

class QComboBox;
class QWidget;
class QFormLayout;
class QLineEdit;
class QLabel;

namespace jam2::gui {

struct AudioDeviceChoice {
    QString label;
    QString id;
    QString backend;
    QString stableId;
    jam2::audio::EndpointDirection direction = jam2::audio::EndpointDirection::Duplex;
    std::size_t channels = 0;
    std::size_t inputChannels = 0;
    std::size_t outputChannels = 0;
};

QVector<AudioDeviceChoice> audioDeviceChoices(const std::vector<jam2::audio::DeviceInfo>& devices);
bool audioDeviceUsesInput(const AudioDevicePreference& preference);
QString preferredAudioDeviceId(const QVector<AudioDeviceChoice>& choices,
    const AudioDevicePreference& preference, const QString& requestedId = {});

struct AudioDeviceTestRequest {
    int outputDeviceId = -1;
    std::optional<int> inputDeviceId;
    bool separate = false;
};
struct AudioDeviceTestReport {
    QString text;
    std::vector<jam2::audio::DeviceTestResult> devices;
};
using AudioDeviceProbe = std::function<jam2::audio::DeviceTestResult(int)>;
AudioDeviceTestReport testAudioDeviceSelection(const AudioDeviceTestRequest& request,
    const AudioDeviceProbe& probe = jam2::audio::test_device);

// One implementation of driver/endpoint selection shared by Settings and the
// local/create/join dialogs. Existing device/channel controls retain their IDs.
class AudioDeviceControls final : public QObject {
public:
    AudioDeviceControls(QFormLayout* form, QComboBox* device, QLineEdit* inputChannels,
        QLineEdit* outputChannels, QVector<AudioDeviceChoice> choices,
        const AudioDevicePreference& preference, const QString& selectedId, const QString& controlPrefix,
        QComboBox* bufferSize = nullptr);
    AudioDeviceTestRequest testRequest() const;
    void setDevices(QVector<AudioDeviceChoice> choices, const QString& selectedId);
    void applyPreference(const AudioDevicePreference& preference, const QString& selectedId = {});
    void addBufferControl(QComboBox* buffer, QFormLayout* form);
    void clearBufferControls() { buffers_.clear(); }
    AudioDevicePreference preference(AudioDevicePreference original) const;
private:
    void populate(const QString& selectedId);
    void updateView();
    QFormLayout* form_;
    QComboBox* device_;
    QComboBox* driver_;
    QComboBox* deviceSetup_;
    QComboBox* input_;
    QLineEdit* inputChannels_;
    QLineEdit* outputChannels_;
    QLabel* channels_;
    QLabel* noDuplexDevice_;
    struct BufferControl {
        QComboBox* editor;
        QFormLayout* form;
        int nativeFrames = 64;
        bool showingWasapi = false;
    };
    QVector<BufferControl> buffers_;
    QVector<AudioDeviceChoice> choices_;
    QString nativeId_, wasapiId_, inputId_;
    QString coreOutputId_, coreInputId_;
};

QString audioDevicePreferenceKey(const jam2::audio::DeviceInfo& device);
bool storeSelectedDevicePreference(
    AudioDevicePreference& preference,
    const QString& selectedDeviceId,
    const std::vector<jam2::audio::DeviceInfo>& devices);
bool storeSelectedDevicePreference(
    AudioDevicePreference& preference,
    const QComboBox* combo,
    const std::vector<jam2::audio::DeviceInfo>& devices);
QString audioDeviceCapabilitiesText(
    const jam2::audio::DeviceTestResult& capabilities);
QString audioDeviceRuntimeText(const jam2::audio::StreamInfo& stream,
    const jam2::audio::DeviceStreamStatus& status);
void showAudioDeviceTestMessage(QWidget* parent, const QString& text);

} // namespace jam2::gui

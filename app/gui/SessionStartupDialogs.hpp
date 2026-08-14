#pragma once

#include "UserPreferences.hpp"

#include <QDialog>
#include <QString>
#include <QVector>

#include <functional>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QWidget;

namespace jam2::gui {

struct SessionAudioDeviceChoice {
    QString label;
    QString id;
};

struct SessionAudioDeviceList {
    QVector<SessionAudioDeviceChoice> devices;
    QString selectedId;
};

struct StartJamDialogState {
    CreatePreference create;
    AudioDevicePreference audio;
    QString selectedDeviceId;
};

struct JoinJamDialogState {
    QString inviteUrl;
    JoinPreference join;
    AudioDevicePreference audio;
    QString selectedDeviceId;
};

// Non-widget state consumed by MainWindow's runtime/session orchestration.
// Startup dialogs edit copies of this data; no hidden Qt controls are used as
// a configuration model.
struct SessionRuntimeDraft {
    CreatePreference configuration;
    AudioDevicePreference audio;
    QString selectedDeviceId;
    QString inviteUrl;
    QString joinProfileName = QStringLiteral("fast");
};

using SessionDeviceRefresh =
    std::function<SessionAudioDeviceList(const QString& currentDeviceId)>;
using SessionDeviceTest =
    std::function<void(QComboBox* device, QPushButton* button, QWidget* parent)>;

struct LocalEngineDialogState {
    QString selectedDeviceId;
    int sampleRate = 48000;
    int bufferSize = 64;
    QString inputChannels;
    QString outputChannels;
    bool saveDefaults = false;
};

struct LocalEngineDialogCallbacks {
    SessionDeviceTest testDevice;
};

struct StartJamDialogCallbacks {
    SessionDeviceRefresh refreshDevices;
    SessionDeviceTest testDevice;
    std::function<void(const StartJamDialogState&)> saveDefaults;
    std::function<void()> generateSession;
};

struct JoinJamDialogCallbacks {
    SessionDeviceRefresh refreshDevices;
    SessionDeviceTest testDevice;
    std::function<void(const JoinJamDialogState&)> saveDefaults;
};

class LocalEngineDialog final : public QDialog {
public:
    LocalEngineDialog(
        LocalEngineDialogState initial,
        SessionAudioDeviceList devices,
        LocalEngineDialogCallbacks callbacks,
        QWidget* parent = nullptr);

    LocalEngineDialogState state() const;

private:
    LocalEngineDialogCallbacks callbacks_;
    QComboBox* device_ = nullptr;
    QComboBox* sampleRate_ = nullptr;
    QComboBox* bufferSize_ = nullptr;
    QLineEdit* inputChannels_ = nullptr;
    QLineEdit* outputChannels_ = nullptr;
    QCheckBox* saveDefaults_ = nullptr;
};

class StartJamDialog final : public QDialog {
public:
    StartJamDialog(
        StartJamDialogState initial,
        SessionAudioDeviceList devices,
        StartJamDialogCallbacks callbacks,
        QWidget* parent = nullptr);

    StartJamDialogState state() const;

private:
    void applyCreateProfile(const QString& name);
    void setDeviceList(SessionAudioDeviceList devices);
    void updateStunAvailability(bool noStun);

    StartJamDialogCallbacks callbacks_;
    CreatePreference createTemplate_;
    AudioDevicePreference audioTemplate_;
    QLineEdit* bindHost_ = nullptr;
    QSpinBox* port_ = nullptr;
    QLineEdit* publicHost_ = nullptr;
    QLineEdit* stunServer_ = nullptr;
    QSpinBox* stunTimeout_ = nullptr;
    QSpinBox* stunRetries_ = nullptr;
    QCheckBox* noStun_ = nullptr;
    QSpinBox* maximumPeers_ = nullptr;
    QComboBox* profile_ = nullptr;
    QComboBox* device_ = nullptr;
    QLineEdit* inputChannels_ = nullptr;
    QLineEdit* outputChannels_ = nullptr;
    QComboBox* sampleRate_ = nullptr;
    QComboBox* bufferSize_ = nullptr;
    QSpinBox* frameSize_ = nullptr;
    QComboBox* audioFormat_ = nullptr;
    QSpinBox* prefill_ = nullptr;
    QSpinBox* playbackMaximum_ = nullptr;
    QSpinBox* captureRing_ = nullptr;
    QSpinBox* playbackRing_ = nullptr;
    QSpinBox* waitMs_ = nullptr;
    QSpinBox* streamMs_ = nullptr;
    QSpinBox* streamLingerMs_ = nullptr;
    QCheckBox* diagnostics_ = nullptr;
    QSpinBox* diagnosticsWarmup_ = nullptr;
    QLineEdit* diagnosticsFolder_ = nullptr;
    QSpinBox* socketSendBuffer_ = nullptr;
    QSpinBox* socketReceiveBuffer_ = nullptr;
    QComboBox* osPriority_ = nullptr;
    QCheckBox* driftCorrection_ = nullptr;
    QDoubleSpinBox* driftSmoothing_ = nullptr;
    QSpinBox* driftDeadband_ = nullptr;
    QSpinBox* driftMaximum_ = nullptr;
    QCheckBox* sampleTimePlayout_ = nullptr;
    QSpinBox* playoutDelay_ = nullptr;
    QSpinBox* jitterBuffer_ = nullptr;
    QSpinBox* jitterBufferMaximum_ = nullptr;
    QCheckBox* adaptiveCushion_ = nullptr;
    QSpinBox* adaptiveTarget_ = nullptr;
    QSpinBox* adaptiveMinimum_ = nullptr;
    QSpinBox* adaptiveMaximum_ = nullptr;
    QSpinBox* adaptiveRelease_ = nullptr;
    QSpinBox* adaptiveRatioRamp_ = nullptr;
};

class JoinJamDialog final : public QDialog {
public:
    JoinJamDialog(
        JoinJamDialogState initial,
        SessionAudioDeviceList devices,
        JoinJamDialogCallbacks callbacks,
        QWidget* parent = nullptr);

    JoinJamDialogState state() const;

private:
    void applyJoinProfile(const QString& name);
    void setDeviceList(SessionAudioDeviceList devices);

    JoinJamDialogCallbacks callbacks_;
    JoinPreference joinTemplate_;
    AudioDevicePreference audioTemplate_;
    QLineEdit* inviteUrl_ = nullptr;
    QLineEdit* bindHost_ = nullptr;
    QSpinBox* port_ = nullptr;
    QComboBox* profile_ = nullptr;
    QComboBox* device_ = nullptr;
    QLineEdit* inputChannels_ = nullptr;
    QLineEdit* outputChannels_ = nullptr;
    QComboBox* bufferSize_ = nullptr;
    QSpinBox* prefill_ = nullptr;
    QSpinBox* playbackMaximum_ = nullptr;
    QSpinBox* captureRing_ = nullptr;
    QSpinBox* playbackRing_ = nullptr;
    QCheckBox* diagnostics_ = nullptr;
    QSpinBox* diagnosticsWarmup_ = nullptr;
    QLineEdit* diagnosticsFolder_ = nullptr;
    QComboBox* osPriority_ = nullptr;
    QSpinBox* waitMs_ = nullptr;
    QSpinBox* streamMs_ = nullptr;
    QSpinBox* streamLingerMs_ = nullptr;
    QCheckBox* driftCorrection_ = nullptr;
    QDoubleSpinBox* driftSmoothing_ = nullptr;
    QSpinBox* driftDeadband_ = nullptr;
    QSpinBox* driftMaximum_ = nullptr;
    QCheckBox* sampleTimePlayout_ = nullptr;
    QSpinBox* playoutDelay_ = nullptr;
    QSpinBox* jitterBuffer_ = nullptr;
    QSpinBox* jitterBufferMaximum_ = nullptr;
    QCheckBox* adaptiveCushion_ = nullptr;
    QSpinBox* adaptiveTarget_ = nullptr;
    QSpinBox* adaptiveMinimum_ = nullptr;
    QSpinBox* adaptiveMaximum_ = nullptr;
    QSpinBox* adaptiveRelease_ = nullptr;
    QSpinBox* adaptiveRatioRamp_ = nullptr;
};

} // namespace jam2::gui

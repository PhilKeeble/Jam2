#pragma once

#include "UserPreferences.hpp"

#include "audio_device.hpp"

#include <QString>
#include <QVector>

#include <functional>
#include <optional>
#include <vector>

class QComboBox;
class QPushButton;
class QWidget;

namespace jam2::gui {

struct SettingsLoopbackSourceChoice {
    QString label;
    QString id;
};

struct SettingsDialogInput {
    UserPreferences preferences;
    AudioDevicePreference localAudio;
    std::vector<jam2::audio::DeviceInfo> devices;
    QVector<SettingsLoopbackSourceChoice> loopbackSources;
    bool networkActive = false;
};

struct SettingsDialogCallbacks {
    std::function<void(QComboBox*, QPushButton*, QWidget*)> testDevice;
    std::function<bool(
        const AudioDevicePreference&,
        const QString& selectedDeviceId,
        QWidget*)> applyLocalAudio;
    std::function<QVector<SettingsLoopbackSourceChoice>()> refreshLoopbackSources;
};

struct SettingsDialogResult {
    UserPreferences preferences;
    QString selectedLocalDeviceId;
};

// Owns the transient multi-page Settings editor and its typed preference
// result. Application-wide persistence and live audio restart/rollback remain
// MainWindow responsibilities supplied through callbacks.
class SettingsDialog final {
public:
    static std::optional<SettingsDialogResult> run(
        SettingsDialogInput input,
        SettingsDialogCallbacks callbacks,
        QWidget* parent = nullptr);

    SettingsDialog() = delete;
};

} // namespace jam2::gui

#pragma once

#include "UserPreferences.hpp"

#include "audio_device.hpp"

#include <QString>

#include <vector>

class QComboBox;
class QWidget;

namespace jam2::gui {

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
void showAudioDeviceTestMessage(QWidget* parent, const QString& text);

} // namespace jam2::gui

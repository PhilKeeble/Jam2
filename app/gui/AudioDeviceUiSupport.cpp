#include "AudioDeviceUiSupport.hpp"

#include "GuiControlContract.hpp"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>

#include <algorithm>

namespace jam2::gui {

QString audioDevicePreferenceKey(const jam2::audio::DeviceInfo& device)
{
    return QString::fromStdString(device.backend) + QLatin1Char('|') +
        QString::fromStdString(device.clsid.empty() ? device.name : device.clsid);
}

bool storeSelectedDevicePreference(
    AudioDevicePreference& preference,
    const QString& selectedDeviceId,
    const std::vector<jam2::audio::DeviceInfo>& devices)
{
    bool ok = false;
    const int id = selectedDeviceId.toInt(&ok);
    if (!ok) return false;
    const auto selected = std::find_if(
        devices.cbegin(), devices.cend(),
        [id](const auto& item) { return item.id == id; });
    if (selected == devices.cend()) return false;
    preference.backend = QString::fromStdString(selected->backend);
    preference.stableId = QString::fromStdString(
        selected->clsid.empty() ? selected->name : selected->clsid);
    preference.name = QString::fromStdString(selected->name);
    return true;
}

bool storeSelectedDevicePreference(
    AudioDevicePreference& preference,
    const QComboBox* combo,
    const std::vector<jam2::audio::DeviceInfo>& devices)
{
    return combo != nullptr && storeSelectedDevicePreference(
        preference, combo->currentData().toString(), devices);
}

QString audioDeviceCapabilitiesText(
    const jam2::audio::DeviceTestResult& capabilities)
{
    QStringList lines{
        QStringLiteral("Device: %1 %2")
            .arg(QString::fromStdString(capabilities.device.backend),
                 QString::fromStdString(capabilities.device.name)),
        QStringLiteral("Current device sample rate: %1 Hz")
            .arg(capabilities.current_sample_rate, 0, 'f', 0),
        QString(),
        QStringLiteral("Sample rates:"),
    };
    for (std::size_t index = 0; index < jam2::audio::kTestSampleRates.size(); ++index) {
        lines.append(QStringLiteral("  %1 Hz: %2")
            .arg(jam2::audio::kTestSampleRates[index])
            .arg(capabilities.sample_rate_supported[index]
                ? QStringLiteral("supported") : QStringLiteral("not supported")));
    }
    lines.append(QString());
    lines.append(QStringLiteral("Buffer sizes:"));
    for (std::size_t index = 0; index < jam2::audio::kTestBufferSizes.size(); ++index) {
        lines.append(QStringLiteral("  %1 frames: %2")
            .arg(jam2::audio::kTestBufferSizes[index])
            .arg(capabilities.buffer_size_supported[index]
                ? QStringLiteral("supported") : QStringLiteral("not supported")));
    }
    return lines.join(QLatin1Char('\n'));
}

void showAudioDeviceTestMessage(QWidget* parent, const QString& text)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("Test Device"));
    dialog.setModal(true);
    auto* message = new QLabel(text, &dialog);
    message->setObjectName(QStringLiteral("AudioDeviceTestMessage"));
    message->setTextInteractionFlags(
        Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    message->setWordWrap(true);
    message->setMinimumWidth(430);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok, &dialog);
    auto* ok = buttons->button(QDialogButtonBox::Ok);
    registerGuiControl(
        *ok,
        QStringLiteral("application.device-test-dialog.ok"),
        QStringLiteral("application.audio-device-test"),
        GuiControlAvailability::Modal);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(message);
    layout->addWidget(buttons);
    dialog.exec();
}

} // namespace jam2::gui

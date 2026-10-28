#include "AudioDeviceUiSupport.hpp"

#include "GuiControlContract.hpp"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QStringList>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QLineEdit>
#include <QSignalBlocker>

#include <algorithm>

namespace jam2::gui {

QVector<AudioDeviceChoice> audioDeviceChoices(const std::vector<jam2::audio::DeviceInfo>& devices)
{
    QVector<AudioDeviceChoice> result;
    for (const auto& device : devices) result.push_back({
        QString::fromStdString(device.name), QString::number(device.id),
        QString::fromStdString(device.backend), QString::fromStdString(jam2::audio::stable_device_id(device)),
        device.direction, device.channel_count});
    return result;
}

AudioDeviceControls::AudioDeviceControls(QFormLayout* form, QComboBox* device,
    QLineEdit* inputChannels, QLineEdit* outputChannels, QVector<AudioDeviceChoice> choices,
    const AudioDevicePreference& preference, const QString& selectedId, const QString& prefix, QComboBox* bufferSize)
    : QObject(device), form_(form), device_(device), driver_(new QComboBox(device->parentWidget())),
      input_(new QComboBox(device->parentWidget())), inputChannels_(inputChannels),
      outputChannels_(outputChannels), channels_(new QLabel(device->parentWidget())), choices_(std::move(choices))
{
#ifdef Q_OS_WIN
    driver_->addItem(QStringLiteral("ASIO"), QStringLiteral("ASIO"));
    driver_->addItem(QStringLiteral("WASAPI"), QStringLiteral("WASAPI"));
#else
    driver_->addItem(QStringLiteral("CoreAudio"), QStringLiteral("CoreAudio"));
#endif
    int row = 0; QFormLayout::ItemRole role{}; form_->getWidgetPosition(device_, &row, &role);
    form_->insertRow(row, QStringLiteral("Driver"), driver_);
    form_->insertRow(row + 2, QStringLiteral("Input device"), input_);
    form_->insertRow(row + 3, QStringLiteral("Channels"), channels_);
    for (const auto& item : {std::pair<QComboBox*, QString>{driver_, QStringLiteral("driver")},
             {input_, QStringLiteral("input-device")}}) {
        item.first->setObjectName(prefix + QLatin1Char('.') + item.second);
        registerGuiControl(*item.first, item.first->objectName(), QStringLiteral("application.audio-driver"),
            GuiControlAvailability::Modal);
    }
    channels_->setObjectName(prefix + QStringLiteral(".channel-summary"));
    channels_->setTextFormat(Qt::PlainText);
    channels_->setWordWrap(false);
    channels_->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Minimum);
    channels_->setToolTip(QStringLiteral(
        "All channels on the input device are used automatically. "
        "Jam2's mono output plays through both front stereo channels, or the single mono channel. "
        "Other surround channels stay silent."));
    if (bufferSize) buffers_.push_back({bufferSize, form_,
        preference.bufferSize > 0 && preference.backend != QStringLiteral("WASAPI") ? preference.bufferSize : 64,
        preference.backend == QStringLiteral("WASAPI")});
    applyPreference(preference, selectedId);
    QObject::connect(driver_, &QComboBox::currentIndexChanged, this, [this] { populate({}); });
    QObject::connect(device_, &QComboBox::currentIndexChanged, this, [this] {
        if (driver_->currentData() == QStringLiteral("WASAPI")) wasapiId_ = device_->currentData().toString();
        else nativeId_ = device_->currentData().toString();
        updateView();
    });
    QObject::connect(input_, &QComboBox::currentIndexChanged, this, [this] {
        inputId_ = input_->currentData().toString(); updateView();
    });
}

void AudioDeviceControls::applyPreference(const AudioDevicePreference& preference, const QString& selectedId)
{
    const QSignalBlocker blocker(driver_);
    QString backend = preference.backend;
    if (backend.isEmpty() && !selectedId.isEmpty()) {
        for (const auto& choice : choices_) if (choice.id == selectedId) backend = choice.backend;
    }
    driver_->setCurrentIndex(qMax(0, driver_->findData(backend)));
    for (auto& buffer : buffers_) {
        if (backend != QStringLiteral("WASAPI")) buffer.nativeFrames = preference.bufferSize > 0 ? preference.bufferSize : 64;
    }
    inputId_ = preference.wasapiInputId;
    QString selected = selectedId;
    if (selected.isEmpty()) for (const auto& choice : choices_)
        if (choice.stableId == preference.stableId && choice.backend == preference.backend) selected = choice.id;
    if (selected.isEmpty() && !preference.stableId.isEmpty()) selected = QStringLiteral("unavailable");
    device_->setPlaceholderText(QStringLiteral("Select an available audio device"));
    inputChannels_->setText(preference.inputChannels); outputChannels_->setText(preference.outputChannels);
    populate(selected);
}

void AudioDeviceControls::addBufferControl(QComboBox* buffer, QFormLayout* form)
{
    const bool wasapi = driver_->currentData() == QStringLiteral("WASAPI");
    buffers_.push_back({buffer, form, !wasapi && buffer->currentData().toInt() > 0 ? buffer->currentData().toInt() : 64, wasapi});
    updateView();
}

void AudioDeviceControls::setDevices(QVector<AudioDeviceChoice> choices, const QString& selectedId)
{
    choices_ = std::move(choices); populate(selectedId);
}

void AudioDeviceControls::populate(const QString& selectedId)
{
    const QSignalBlocker outputBlocker(device_), inputBlocker(input_);
    const bool wasapi = driver_->currentData() == QStringLiteral("WASAPI");
    const auto requested = !selectedId.isEmpty() ? selectedId : (wasapi ? wasapiId_ : nativeId_);
    device_->clear(); input_->clear(); input_->addItem(QStringLiteral("None"), QString());
    for (const auto& choice : choices_) {
        if (wasapi && choice.backend == QStringLiteral("WASAPI")) {
            if (choice.direction == jam2::audio::EndpointDirection::Input)
                input_->addItem(choice.label, choice.stableId);
            else device_->addItem(choice.label, choice.id);
        } else if (!wasapi && choice.backend != QStringLiteral("WASAPI")) {
            device_->addItem(choice.label, choice.id);
        }
    }
    if (!requested.isEmpty()) {
        const auto index = device_->findData(requested);
        device_->setCurrentIndex(index); // Missing saved endpoints remain visibly unselected.
    }
    const auto inputIndex = input_->findData(inputId_);
    if (inputIndex >= 0) input_->setCurrentIndex(inputIndex);
    else if (!inputId_.isEmpty()) {
        input_->addItem(QStringLiteral("Unavailable saved input"), inputId_);
        input_->setCurrentIndex(input_->count() - 1);
    }
    if (wasapi) wasapiId_ = device_->currentData().toString();
    else nativeId_ = device_->currentData().toString();
    updateView();
}

void AudioDeviceControls::updateView()
{
    const bool wasapi = driver_->currentData() == QStringLiteral("WASAPI");
    for (auto& buffer : buffers_) {
        auto* bufferSize_ = buffer.editor;
        if (!buffer.showingWasapi && bufferSize_->currentData().toInt() > 0)
            buffer.nativeFrames = bufferSize_->currentData().toInt();
        const QSignalBlocker blocker(bufferSize_);
        bufferSize_->clear();
        if (wasapi) {
            bufferSize_->addItem(QStringLiteral("Automatic"), 0);
        } else for (auto period : jam2::audio::kTestBufferSizes)
            bufferSize_->addItem(QString::number(period), static_cast<int>(period));
        bufferSize_->setCurrentIndex(qMax(0, bufferSize_->findData(wasapi ? 0 : buffer.nativeFrames)));
        buffer.showingWasapi = wasapi;
        buffer.form->setRowVisible(bufferSize_, !wasapi);
    }
    form_->setRowVisible(input_, wasapi); form_->setRowVisible(channels_, wasapi);
    form_->setRowVisible(inputChannels_, !wasapi); form_->setRowVisible(outputChannels_, !wasapi);
    if (auto* label = qobject_cast<QLabel*>(form_->labelForField(device_)))
        label->setText(wasapi ? QStringLiteral("Output device") : QStringLiteral("Device"));
    std::size_t inputs = 0, outputs = 0;
    for (const auto& choice : choices_) {
        if (choice.stableId == input_->currentData().toString()) inputs = choice.channels;
        if (choice.id == device_->currentData().toString()) outputs = choice.channels;
    }
    channels_->setText(QStringLiteral("Input: %1\nOutput: %2")
        .arg(inputs == 0 ? QStringLiteral("None") : inputs == 1 ? QStringLiteral("1 channel") : QStringLiteral("%1 channels").arg(inputs),
             outputs == 0 ? QStringLiteral("None") : outputs == 1 ? QStringLiteral("Mono") : QStringLiteral("Stereo")));
}

AudioDevicePreference AudioDeviceControls::preference(AudioDevicePreference original) const
{
    original.inputChannels = inputChannels_->text().trimmed();
    original.outputChannels = outputChannels_->text().trimmed();
    original.backend = driver_->currentData().toString();
    for (const auto& choice : choices_) if (choice.id == device_->currentData().toString()) {
        original.stableId = choice.stableId; original.name = choice.label; break;
    }
    if (original.backend == QStringLiteral("WASAPI")) {
        original.bufferSize = 0;
        original.wasapiInputId = input_->currentData().toString();
        original.wasapiInputName = original.wasapiInputId.isEmpty() ? QString() : input_->currentText();
    }
    return original;
}

QString audioDevicePreferenceKey(const jam2::audio::DeviceInfo& device)
{
    return QString::fromStdString(device.backend) + QLatin1Char('|') +
        QString::fromStdString(jam2::audio::stable_device_id(device));
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
        jam2::audio::stable_device_id(*selected));
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
    if (capabilities.device.backend == "WASAPI") {
        lines.append(QStringLiteral("Shared mode; input channels automatic; mono/front-stereo output."));
        lines.append(QStringLiteral("Native minimum/default periods: %1 / %2 frames")
            .arg(capabilities.minimum_period_frames).arg(capabilities.default_period_frames));
        lines.append(QStringLiteral("The actual period is negotiated at start. Windows converts the jam rate when needed."));
        return lines.join(QLatin1Char('\n'));
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

QString audioDeviceRuntimeText(const jam2::audio::StreamInfo& stream,
    const jam2::audio::DeviceStreamStatus& status)
{
    if (stream.device.backend != "WASAPI") return {};
    return QStringLiteral("<b>WASAPI shared</b><br>Input: %1 | Output: %2<br>"
        "Jam %3 Hz | Windows input/output %4/%5 Hz<br>"
        "Input/output periods %6/%7 frames | Endpoint capacities %8/%9 frames<br>"
        "Capture queue %10 frames (target %11 ms, capacity %12 ms) | Correction %13 ppm (limit %14 ppm)<br>"
        "Render padding %15 frames | Capture underrun/overrun %16/%17 frames<br>"
        "Discontinuities %18 | Invalid timestamps %19 | Low-period active %20 | Compatibility HRESULT %21")
        .arg(QString::fromStdString(stream.input_device_name).toHtmlEscaped(),
             QString::fromStdString(stream.device.name).toHtmlEscaped())
        .arg(stream.sample_rate, 0, 'f', 0).arg(stream.input_mix_sample_rate, 0, 'f', 0)
        .arg(stream.output_mix_sample_rate, 0, 'f', 0)
        .arg(stream.input_period_frames).arg(stream.output_period_frames)
        .arg(stream.input_buffer_frames).arg(stream.output_buffer_frames)
        .arg(status.capture_queued_frames).arg(stream.wasapi.capture_target_ms).arg(stream.wasapi.capture_capacity_ms)
        .arg(status.capture_correction_ppm).arg(stream.wasapi.capture_max_correction_ppm)
        .arg(status.render_padding_frames).arg(status.capture_underrun_frames).arg(status.capture_overrun_frames)
        .arg(status.capture_discontinuities).arg(status.invalid_timestamps)
        .arg(stream.low_latency_shared ? QStringLiteral("yes") : QStringLiteral("no"))
        .arg(stream.shared_fallback_error);
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

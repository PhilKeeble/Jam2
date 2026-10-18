#include "RecordingCalibrationDialog.hpp"
#include "GuiControlContract.hpp"
#include "GuiPresentation.hpp"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QStringList>
#include <QRunnable>
#include <QThreadPool>
#include <QVBoxLayout>

namespace {
void control(QObject& object, const char* id) {
    jam2::gui::registerGuiControl(object, QStringLiteral("looper.calibration.") + QString::fromLatin1(id),
        QStringLiteral("looper.recording-calibration"), jam2::gui::GuiControlAvailability::Modal);
}
}
RecordingCalibrationDialog::RecordingCalibrationDialog(RecordingCalibrationController& controller,
    std::size_t channel, QWidget* parent) : QDialog(parent), controller_(controller), channel_(channel)
{
    setObjectName(QStringLiteral("RecordingCalibrationDialog"));
    setWindowTitle(QStringLiteral("Calibrate Recording Alignment")); resize(620, 360);
    auto* layout = new QVBoxLayout(this); layout->setSpacing(14);
    key_ = controller_.key(channel_);
    QStringList outputs;
    for (const int output : key_.outputChannels) outputs.push_back(QString::number(output + 1));
    auto* route = new QLabel(QStringLiteral("Input %1 · Outputs %2 · %3 Hz · %4-frame buffer\n%5")
        .arg(key_.physicalInput + 1).arg(outputs.join(QStringLiteral(" / "))).arg(key_.sampleRate).arg(key_.bufferFrames)
        .arg(controller_.deviceName()), this);
    route->setWordWrap(true); layout->addWidget(route);
    method_ = new QComboBox(this); method_->addItem(QStringLiteral("Cable — recommended for audio interfaces"));
    method_->addItem(QStringLiteral("Microphone — laptop / built-in audio")); applyMutedEditorStyle(method_);
    control(*method_, "method"); layout->addWidget(method_);
    instructions_ = new QLabel(this); instructions_->setWordWrap(true); layout->addWidget(instructions_);
    status_ = new QLabel(this); status_->setWordWrap(true); status_->setMinimumHeight(68);
    control(*status_, "status"); layout->addWidget(status_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    start_ = buttons->addButton(QStringLiteral("Start Test"), QDialogButtonBox::ActionRole);
    reset_ = buttons->addButton(QStringLiteral("Reset Calibration"), QDialogButtonBox::ActionRole);
    save_ = buttons->addButton(QStringLiteral("Save Calibration"), QDialogButtonBox::AcceptRole);
    save_->setEnabled(false); start_->setDefault(true);
    control(*start_, "start"); control(*save_, "save"); control(*reset_, "reset");
    control(*buttons->button(QDialogButtonBox::Cancel), "cancel"); layout->addWidget(buttons);
    connect(method_, &QComboBox::currentIndexChanged, this, [this] { result_ = {}; save_->setEnabled(false); refreshInstructions(); });
    connect(start_, &QPushButton::clicked, this, [this] { startTest(); });
    connect(reset_, &QPushButton::clicked, this, [this] {
        QString error;
        if (controller_.reset(key_, error)) { result_ = {}; save_->setEnabled(false); status_->setText(QStringLiteral("Calibration removed. Reported device timing will be used.")); }
        else status_->setText(error);
    });
    connect(save_, &QPushButton::clicked, this, [this] {
        QString error;
        if (controller_.save(key_, static_cast<RecordingCalibrationMethod>(method_->currentIndex()), result_, error)) accept();
        else { status_->setText(error); save_->setEnabled(false); }
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(&poll_, &QTimer::timeout, this, [this] {
        if (!capture_) return;
        using State = jam2::audio::RecordingCalibrationCapture::State;
        if (capture_->state() == State::Complete) {
            poll_.stop(); status_->setText(QStringLiteral("Measuring the returned clicks…"));
            QPointer<RecordingCalibrationDialog> guard(this); auto capture = capture_;
            QThreadPool::globalInstance()->start(QRunnable::create([guard, capture] {
                jam2::audio::RecordingCalibrationCapture::Result result;
                try { result = capture->analyse(); }
                catch (const std::exception& e) { result.error = e.what(); }
                if (guard) QMetaObject::invokeMethod(guard, [guard, result] { if (guard) guard->presentResult(result); }, Qt::QueuedConnection);
            }));
        } else if (elapsed_.elapsed() > 12000) {
            stopTest(); status_->setText(QStringLiteral("No complete audio capture returned. Check that the device is still running and retry."));
        }
    });
    poll_.setInterval(50); refreshInstructions();
    status_->setText(controller_.unavailableReason().isEmpty()
        ? QStringLiteral("Press Start Test when connected. Six short clicks will play over five seconds.\nCalibration uses the raw input; input plugins are bypassed for this measurement.")
        : controller_.unavailableReason());
    start_->setEnabled(controller_.unavailableReason().isEmpty() && key_.valid());
}
RecordingCalibrationDialog::~RecordingCalibrationDialog() { stopTest(); }
void RecordingCalibrationDialog::reject()
{
    dismissed_ = true; stopTest(); QDialog::reject();
}
void RecordingCalibrationDialog::refreshInstructions()
{
    instructions_->setText(method_->currentIndex() == 0
        ? QStringLiteral("Connect a line output on the selected device to this line input. Use line mode, keep the level below clipping, and turn off hardware Direct Monitor. Speakers are not needed.")
        : QStringLiteral("Place the microphone close to the speaker connected to the selected output. Keep the room quiet, keep the input below clipping, and turn off hardware Direct Monitor. This measures the speaker/microphone path too."));
}
void RecordingCalibrationDialog::startTest()
{
    key_ = controller_.key(channel_); QString error;
    capture_ = controller_.begin(channel_, error);
    if (!capture_) { status_->setText(error); return; }
    result_ = {}; method_->setEnabled(false); start_->setEnabled(false);
    reset_->setEnabled(false); save_->setEnabled(false);
    status_->setText(QStringLiteral("Playing and recording six test clicks…")); elapsed_.start(); poll_.start();
}
void RecordingCalibrationDialog::stopTest()
{
    poll_.stop(); if (capture_) capture_->cancel(); controller_.finishTest();
    method_->setEnabled(true); start_->setEnabled(true); reset_->setEnabled(true);
}
void RecordingCalibrationDialog::presentResult(jam2::audio::RecordingCalibrationCapture::Result result)
{
    if (dismissed_) return;
    stopTest(); result_ = std::move(result); save_->setEnabled(result_.ok);
    if (!result_.ok) { status_->setText(QString::fromStdString(result_.error)); return; }
    const double ms = 1000. / key_.sampleRate;
    status_->setText(QStringLiteral("Measured %1 ms · Reported %2 ms\nAdjustment %3 ms (%4 frames)\n%5 clicks detected · Repeat spread %6 ms · Peak %7 dBFS")
        .arg(result_.measured_frames * ms, 0, 'f', 3).arg(result_.reported_frames * ms, 0, 'f', 3)
        .arg(result_.adjustment_frames * ms, 0, 'f', 3).arg(result_.adjustment_frames)
        .arg(result_.detected_clicks).arg(result_.spread_frames * ms, 0, 'f', 3)
        .arg(result_.peak > 0 ? 20 * std::log10(result_.peak) : -120., 0, 'f', 1));
}

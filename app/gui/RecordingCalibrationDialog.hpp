#pragma once
#include "RecordingCalibrationController.hpp"
#include <QDialog>
#include <QElapsedTimer>
#include <QTimer>
class QComboBox;
class QLabel;
class QPushButton;

class RecordingCalibrationDialog final : public QDialog {
public:
    RecordingCalibrationDialog(RecordingCalibrationController& controller,
        std::size_t channel, QWidget* parent = nullptr);
    ~RecordingCalibrationDialog() override;
    // The asynchronous analysis completion boundary. Also allows native tests
    // to exercise the real dialog without opening an audio device.
    void presentResult(jam2::audio::RecordingCalibrationCapture::Result result);
    void reject() override;
private:
    void startTest();
    void stopTest();
    void refreshInstructions();
    RecordingCalibrationController& controller_;
    std::size_t channel_;
    RecordingCalibrationKey key_;
    std::shared_ptr<jam2::audio::RecordingCalibrationCapture> capture_;
    jam2::audio::RecordingCalibrationCapture::Result result_;
    QTimer poll_;
    QElapsedTimer elapsed_;
    QComboBox* method_ = nullptr;
    QLabel* instructions_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* start_ = nullptr;
    QPushButton* save_ = nullptr;
    QPushButton* reset_ = nullptr;
    bool dismissed_ = false;
};

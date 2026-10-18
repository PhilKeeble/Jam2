#pragma once
#include "UserPreferences.hpp"
#include "engine.hpp"
#include <functional>
#include <optional>
class QWidget;

class RecordingCalibrationController final {
public:
    struct Backend {
        std::function<jam2::EngineSnapshot()> snapshot;
        std::function<jam2::audio::StreamInfo()> stream;
        std::function<jam2::audio::InputSourceSlotSnapshot(std::size_t)> source;
        std::function<bool()> networkRunning;
        std::function<bool(const jam2::EngineCommand&)> submit;
        std::function<std::shared_ptr<jam2::audio::RecordingCalibrationCapture>(std::size_t, QString&)> begin;
    };
    RecordingCalibrationController(UserPreferences& preferences, Backend backend);
    RecordingCalibrationKey key(std::size_t relativeChannel) const;
    std::optional<std::size_t> channelForSource(std::optional<std::size_t> slot) const;
    QString status(std::optional<std::size_t> slot) const;
    QString unavailableReason() const;
    QString deviceName() const { return QString::fromStdString(backend_.stream().device.name); }
    bool showDialog(QWidget* parent, std::optional<std::size_t> slot);
    std::shared_ptr<jam2::audio::RecordingCalibrationCapture> begin(std::size_t channel, QString& error);
    bool save(const RecordingCalibrationKey& key, RecordingCalibrationMethod method,
        const jam2::audio::RecordingCalibrationCapture::Result& result, QString& error);
    bool reset(const RecordingCalibrationKey& key, QString& error);
    void update();
    bool prepareForSource(std::optional<std::size_t> slot);
    void finishTest();
private:
    bool applyForDevice();
    UserPreferences& preferences_;
    Backend backend_;
    std::optional<RecordingCalibrationKey> appliedKey_;
    std::int64_t appliedFrames_ = 0;
    std::uint64_t previousEngineFrame_ = 0;
    bool testActive_ = false, monitorBefore_ = false, metronomeBefore_ = false;
    RecordingCalibrationKey testKey_;
};

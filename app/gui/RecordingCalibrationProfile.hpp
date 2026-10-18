#pragma once
#include <QString>
#include <QVector>
#include <cmath>
#include <cstdint>

struct RecordingCalibrationKey {
    QString backend;
    QString deviceId;
    int sampleRate = 0;
    int bufferFrames = 0;
    QVector<int> inputChannels;
    QVector<int> outputChannels;
    int physicalInput = -1; // Input used to measure this device configuration.
    bool operator==(const RecordingCalibrationKey&) const = default;
    bool matchesDeviceConfiguration(const RecordingCalibrationKey& other) const {
        return backend == other.backend && deviceId == other.deviceId &&
            sampleRate == other.sampleRate && bufferFrames == other.bufferFrames &&
            inputChannels == other.inputChannels && outputChannels == other.outputChannels;
    }
    bool valid() const {
        const auto validChannels = [](const QVector<int>& channels) {
            if (channels.isEmpty() || channels.size() > 1024) return false;
            for (qsizetype i = 0; i < channels.size(); ++i) {
                if (channels[i] < 0 || channels[i] > 1023) return false;
                for (qsizetype j = 0; j < i; ++j) if (channels[j] == channels[i]) return false;
            }
            return true;
        };
        return !backend.isEmpty() && !deviceId.isEmpty() && sampleRate >= 8000 &&
            sampleRate <= 384000 && bufferFrames > 0 && bufferFrames <= 65536 &&
            validChannels(inputChannels) && validChannels(outputChannels) &&
            inputChannels.contains(physicalInput);
    }
};
enum class RecordingCalibrationMethod { Cable, Microphone };
struct RecordingCalibrationProfile {
    RecordingCalibrationKey key;
    RecordingCalibrationMethod method = RecordingCalibrationMethod::Cable;
    std::int64_t adjustmentFrames = 0;
    double measuredFrames = 0;
    double reportedFrames = 0;
    double spreadFrames = 0;
    int clicks = 0;
    QString calibratedAt;
    bool valid() const {
        return key.valid() && (method == RecordingCalibrationMethod::Cable || method == RecordingCalibrationMethod::Microphone) &&
            adjustmentFrames >= -1000000 && adjustmentFrames <= 1000000 &&
            std::isfinite(measuredFrames) && measuredFrames > 0 && measuredFrames <= key.sampleRate / 4. &&
            std::isfinite(reportedFrames) && reportedFrames >= 0 && reportedFrames <= key.sampleRate * 2. &&
            std::isfinite(spreadFrames) && spreadFrames >= 0 && spreadFrames <= key.sampleRate * .001 &&
            clicks >= 5 && clicks <= 6 &&
            adjustmentFrames == std::llround(measuredFrames - reportedFrames);
    }
};

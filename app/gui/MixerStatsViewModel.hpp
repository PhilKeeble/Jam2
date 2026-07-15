#pragma once

#include "engine.hpp"

#include <QJsonObject>
#include <QString>

#include <cstdint>

struct MixerStatsLabels {
    QString latency;
    QString jitter;
    QString loss;
    QString depth;
    QString reorder;
    QString underrun;
    QString stalls;
    QString drift;
    QString diagnosis;
};

struct MixerMeterLevels {
    double input = 0.0;
    double send = 0.0;
    double monitor = 0.0;
    double remote = 0.0;
    double metronome = 0.0;
    double output = 0.0;
    std::uint64_t outputClippedSamples = 0;
};

class MixerStatsViewModel {
public:
    MixerStatsLabels present(const QJsonObject& stats) const;
    MixerMeterLevels consume(
        const jam2::EngineGuiPeakSnapshot& intervalPeaks,
        double sendGain,
        std::uint64_t outputClippedSamples);
    MixerMeterLevels consumeStructured(const QJsonObject& stats);
    void reset() noexcept;

private:
    static double normalized(int peakPpm) noexcept;
    static double decay(double previous, double intervalPeak) noexcept;
    MixerMeterLevels levels_;
};

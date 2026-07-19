#pragma once

#include "RuntimeContracts.hpp"
#include "engine.hpp"

#include <QString>

#include <cstdint>

struct MixerStatsLabels {
    QString latency;
    QString latencyTooltip;
    QString jitter;
    QString loss;
    QString underrun;
    QString diagnosis;
};

struct MixerMeterLevels {
    double input = 0.0;
    double send = 0.0;
    double monitor = 0.0;
    double remote = 0.0;
    double track = 0.0;
    double metronome = 0.0;
    double output = 0.0;
    std::uint64_t outputClippedSamples = 0;
};

class MixerStatsViewModel {
public:
    MixerStatsLabels present(const ConnectionDiagnosticsSnapshot* stats) const;
    MixerMeterLevels consume(
        const jam2::EngineGuiPeakSnapshot& intervalPeaks,
        double sendGain,
        std::uint64_t outputClippedSamples);
    void reset() noexcept;

private:
    static double normalized(int peakPpm) noexcept;
    static double decay(double previous, double intervalPeak) noexcept;
    MixerMeterLevels levels_;
};

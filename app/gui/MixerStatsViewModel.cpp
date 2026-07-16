#include "MixerStatsViewModel.hpp"

#include <QStringList>

#include <algorithm>
#include <cmath>

namespace {

QString diagnosis(const ConnectionDiagnosticsSnapshot& stats)
{
    if (stats.output_underrun_events > 0) {
        return QStringLiteral("Diagnosis: output underruns (%1, %2 ms); increase local prefill or buffer")
            .arg(stats.output_underrun_events)
            .arg(stats.output_underrun_ms, 0, 'f', 1);
    }
    if (stats.packet_loss_percent >= 0.5) {
        return QStringLiteral("Diagnosis: packet loss %1%; try wired networking or a larger frame size")
            .arg(stats.packet_loss_percent, 0, 'f', 2);
    }
    const double burstPercent = stats.packet_gap_samples > 0
        ? static_cast<double>(stats.packet_gap_over_4x) * 100.0 /
            static_cast<double>(stats.packet_gap_samples)
        : 0.0;
    const double latePercent = stats.received_packets > 0
        ? static_cast<double>(stats.reordered_or_late_packets) * 100.0 /
            static_cast<double>(stats.received_packets)
        : 0.0;
    if (burstPercent >= 0.5 || latePercent >= 0.25) {
        return QStringLiteral("Diagnosis: jitter/reordering pressure; increase local playout or prefill");
    }
    if (stats.callback_gap_over_2x > 0) {
        return QStringLiteral("Diagnosis: audio callback gaps; try a larger device buffer");
    }
    if (stats.drift_clamped_samples > 0 || stats.drift_abs_ppm_max >= 200.0) {
        return QStringLiteral("Diagnosis: clock drift correction is under pressure; increase local buffering");
    }
    const auto highRtt = std::find_if(stats.peers.begin(), stats.peers.end(), [](const auto& peer) {
        return peer.has_rtt && peer.rtt_ms >= 100.0;
    });
    if (highRtt != stats.peers.end()) {
        return QStringLiteral("Diagnosis: high network RTT; physical distance is limiting latency");
    }
    return QStringLiteral("Diagnosis OK");
}

} // namespace

MixerStatsLabels MixerStatsViewModel::present(const ConnectionDiagnosticsSnapshot* stats) const
{
    if (stats == nullptr) {
        return {
            QStringLiteral("RTT -"), {}, QStringLiteral("Jitter -"),
            QStringLiteral("Loss -"), QStringLiteral("Underruns -"),
            QStringLiteral("Diagnosis -"),
        };
    }

    QStringList visiblePeers;
    QStringList allPeers;
    for (std::size_t index = 0; index < stats->peers.size(); ++index) {
        const auto& peer = stats->peers[index];
        const QString value = peer.has_rtt
            ? QStringLiteral("P%1 %2 ms").arg(index + 1).arg(peer.rtt_ms, 0, 'f', 1)
            : QStringLiteral("P%1 -").arg(index + 1);
        allPeers.push_back(value);
        if (index < 4) visiblePeers.push_back(value);
    }
    QString latency = visiblePeers.isEmpty()
        ? QStringLiteral("RTT -")
        : QStringLiteral("RTT ") + visiblePeers.join(QStringLiteral(" · "));
    if (stats->peers.size() > 4) {
        latency += QStringLiteral(" · +%1").arg(stats->peers.size() - 4);
    }
    QString underrun = QStringLiteral("Underruns %1").arg(stats->output_underrun_events);
    if (stats->output_underrun_frames > 0) {
        underrun += QStringLiteral(" (%1 ms)").arg(stats->output_underrun_ms, 0, 'f', 1);
    }
    return {
        latency,
        allPeers.join(QStringLiteral("\n")),
        QStringLiteral("Jitter %1 ms").arg(stats->jitter_average_ms, 0, 'f', 1),
        QStringLiteral("Loss %1%").arg(stats->packet_loss_percent, 0, 'f', 2),
        underrun,
        diagnosis(*stats),
    };
}

MixerMeterLevels MixerStatsViewModel::consume(
    const jam2::EngineGuiPeakSnapshot& peaks,
    double sendGain,
    std::uint64_t outputClippedSamples)
{
    levels_.input = decay(levels_.input, normalized(peaks.input_peak_ppm));
    levels_.send = decay(levels_.send, std::min(1.0, normalized(peaks.input_peak_ppm) * sendGain));
    levels_.monitor = decay(levels_.monitor, normalized(peaks.monitor_peak_ppm));
    levels_.remote = decay(levels_.remote, normalized(peaks.remote_peak_ppm));
    levels_.metronome = decay(levels_.metronome, normalized(peaks.metronome_peak_ppm));
    levels_.output = decay(levels_.output, normalized(peaks.output_peak_ppm));
    levels_.outputClippedSamples = outputClippedSamples;
    return levels_;
}

void MixerStatsViewModel::reset() noexcept { levels_ = {}; }

double MixerStatsViewModel::normalized(int peakPpm) noexcept
{
    return std::clamp(static_cast<double>(peakPpm) / 1000000.0, 0.0, 1.0);
}

double MixerStatsViewModel::decay(double previous, double intervalPeak) noexcept
{
    constexpr double kVisibleDecayPerPoll = 0.78;
    const double value = std::max(intervalPeak, previous * kVisibleDecayPerPoll);
    return value < 0.001 ? 0.0 : value;
}

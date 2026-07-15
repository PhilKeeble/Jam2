#include "MixerStatsViewModel.hpp"

#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace {

bool hasNumber(const QJsonObject& object, const QString& key)
{
    return object.contains(key) && object.value(key).isDouble();
}

QString numberText(
    const QJsonObject& object,
    const QString& key,
    const QString& suffix = QString(),
    int precision = 1)
{
    if (!hasNumber(object, key)) {
        return QStringLiteral("-");
    }
    return QString::number(object.value(key).toDouble(), 'f', precision) + suffix;
}

QString integerText(
    const QJsonObject& object,
    const QString& key,
    const QString& fallbackKey = QString())
{
    QString actualKey = key;
    if (!hasNumber(object, actualKey) && !fallbackKey.isEmpty()) {
        actualKey = fallbackKey;
    }
    if (!hasNumber(object, actualKey)) {
        return QStringLiteral("-");
    }
    return QString::number(static_cast<qlonglong>(object.value(actualKey).toDouble()));
}

QString durationText(double milliseconds)
{
    if (milliseconds >= 1000.0) {
        return QString::number(milliseconds / 1000.0, 'f', 2) + QStringLiteral(" s");
    }
    return QString::number(milliseconds, 'f', 1) + QStringLiteral(" ms");
}

QString metricText(
    const QJsonObject& object,
    const QString& key,
    const QString& suffix = QString(),
    int precision = 1,
    const QString& fallbackKey = QString())
{
    QString actualKey = key;
    if (!hasNumber(object, actualKey) && !fallbackKey.isEmpty()) {
        actualKey = fallbackKey;
    }
    return numberText(object, actualKey, suffix, precision);
}

double metricValue(const QJsonObject& object, const QString& key, double fallback = 0.0)
{
    return hasNumber(object, key) ? object.value(key).toDouble() : fallback;
}

double percentOf(double part, double whole)
{
    return whole > 0.0 ? part * 100.0 / whole : 0.0;
}

double perMinute(double count, double elapsedMilliseconds)
{
    return elapsedMilliseconds > 0.0 ? count * 60000.0 / elapsedMilliseconds : 0.0;
}

QString percentText(double value, int precision = 2)
{
    return QString::number(value, 'f', precision) + QStringLiteral("%");
}

QString diagnoseStats(const QJsonObject& stats)
{
    if (!hasNumber(stats, QStringLiteral("elapsed_ms"))) {
        return QStringLiteral("Diagnosis -");
    }

    QStringList findings;
    const double elapsedMs = metricValue(stats, QStringLiteral("elapsed_ms"));
    const double recvPackets = metricValue(stats, QStringLiteral("recv_packets"));
    const double frameSize = metricValue(stats, QStringLiteral("frame_size"));
    const double receivedFrames = recvPackets * frameSize;
    const double packetGapSamples = metricValue(stats, QStringLiteral("audio_packet_gap_samples"));
    const double lossPercent = metricValue(stats, QStringLiteral("sequence_loss_percent"));
    const double underrunMs = metricValue(stats, QStringLiteral("playback_ring_underrun_time_ms"));
    const double underrunEvents = metricValue(stats, QStringLiteral("playback_ring_underrun_events"));
    const double underrunBurstMaxMs = metricValue(stats, QStringLiteral("playback_ring_underrun_burst_max_ms"));
    const double gapOver4x = metricValue(stats, QStringLiteral("audio_packet_gap_over_4x_count"));
    const double reorderedLost = metricValue(stats, QStringLiteral("reordered_lost"));
    const double sequenceLate = metricValue(stats, QStringLiteral("sequence_late"));
    const double lateFrames = metricValue(stats, QStringLiteral("late_audio_frames_dropped"));
    const double missingFrames = metricValue(stats, QStringLiteral("missing_audio_frames_inserted"));
    const double driftPpm = std::abs(metricValue(stats, QStringLiteral("drift_ppm")));

    const double underrunPercent = percentOf(underrunMs, elapsedMs);
    const double underrunEventsPerMinute = perMinute(underrunEvents, elapsedMs);
    const double gapOver4xPercent = percentOf(gapOver4x, packetGapSamples);
    const double gapOver4xPerMinute = perMinute(gapOver4x, elapsedMs);
    const double reorderLatePercent = percentOf(reorderedLost + sequenceLate, recvPackets);
    const double lateFramePercent = percentOf(lateFrames, lateFrames + missingFrames + receivedFrames);
    const double missingPressurePercent = percentOf(missingFrames, missingFrames + lateFrames + receivedFrames);

    if (underrunPercent >= 0.10 || underrunEventsPerMinute >= 2.0 || underrunBurstMaxMs >= 10.0) {
        findings << QStringLiteral("Underrun %1: +prefill/+max").arg(percentText(underrunPercent));
    }
    if (gapOver4xPercent >= 0.50 || gapOver4xPerMinute >= 2.0) {
        findings << QStringLiteral("Bursts %1: +prefill/+adaptive max").arg(percentText(gapOver4xPercent));
    }
    if (lossPercent >= 0.50) {
        findings << QStringLiteral("Loss %1: +frame/wired").arg(
            metricText(stats, QStringLiteral("sequence_loss_percent"), QStringLiteral("%"), 2));
    }
    if (reorderLatePercent >= 0.25 || lateFramePercent >= 0.25) {
        findings << QStringLiteral("Late/reorder %1: +playout").arg(
            percentText(qMax(reorderLatePercent, lateFramePercent)));
    }
    if (missingPressurePercent >= 0.25 && findings.size() < 2) {
        findings << QStringLiteral("Missing %1: +playout/prefill").arg(
            percentText(missingPressurePercent));
    }
    if (driftPpm >= 200.0 && findings.size() < 2) {
        findings << QStringLiteral("Drift %1ppm: +correction").arg(
            metricText(stats, QStringLiteral("drift_ppm"), QString{}, 0));
    }

    if (findings.isEmpty()) {
        return QStringLiteral("Diagnosis OK");
    }
    while (findings.size() > 2) {
        findings.removeLast();
    }
    return QStringLiteral("Diagnosis ") + findings.join(QStringLiteral(" | "));
}

double jsonPeak(const QJsonObject& stats, const char* name)
{
    return std::clamp(stats.value(QLatin1StringView(name)).toDouble(0.0), 0.0, 1.0);
}

} // namespace

MixerStatsLabels MixerStatsViewModel::present(const QJsonObject& stats) const
{
    MixerStatsLabels labels;
    QString latency = metricText(stats, QStringLiteral("estimated_one_way_ms"), QStringLiteral(" ms"));
    if (latency == QStringLiteral("-")) {
        latency = metricText(stats, QStringLiteral("rtt_avg_ms"), QStringLiteral(" ms"));
    }
    labels.latency = QStringLiteral("Latency ") + latency;
    labels.jitter = QStringLiteral("Jitter %1/%2 gap %3").arg(
        metricText(stats, QStringLiteral("jitter_avg_ms"), QStringLiteral(" ms")),
        metricText(stats, QStringLiteral("jitter_max_ms"), QStringLiteral(" ms")),
        metricText(stats, QStringLiteral("audio_packet_gap_max_ms"), QStringLiteral(" ms")));
    labels.loss = QStringLiteral("Loss %1 lost %2 ev %3 max %4p")
        .arg(metricText(stats, QStringLiteral("sequence_loss_percent"), QStringLiteral("%"), 2))
        .arg(integerText(stats, QStringLiteral("sequence_lost")))
        .arg(integerText(stats, QStringLiteral("sequence_loss_events")))
        .arg(integerText(stats, QStringLiteral("sequence_loss_max_gap")));
    labels.depth = QStringLiteral("Depth ") + metricText(
        stats,
        QStringLiteral("playback_depth_avg_ms"),
        QStringLiteral(" ms"),
        1,
        QStringLiteral("playback_depth_ms"));

    QString outOfOrder = integerText(stats, QStringLiteral("sequence_out_of_order"));
    if (outOfOrder == QStringLiteral("-")) {
        outOfOrder = integerText(stats, QStringLiteral("out_of_order"));
    }
    QString latePackets = integerText(stats, QStringLiteral("sequence_late"));
    if (latePackets == QStringLiteral("-")) {
        latePackets = integerText(stats, QStringLiteral("late"));
    }
    labels.reorder = QStringLiteral("Reorder oo %1 rec %2 lost %3 late %4 max %5p")
        .arg(outOfOrder)
        .arg(integerText(stats, QStringLiteral("reordered_recovered")))
        .arg(integerText(stats, QStringLiteral("reordered_lost")))
        .arg(latePackets)
        .arg(integerText(stats, QStringLiteral("reordered_max_distance_packets")));

    QString underrun = QStringLiteral("-");
    if (hasNumber(stats, QStringLiteral("playback_ring_underrun_time_ms"))) {
        underrun = durationText(stats.value(QStringLiteral("playback_ring_underrun_time_ms")).toDouble());
        if (hasNumber(stats, QStringLiteral("playback_ring_underrun_events"))) {
            underrun += QStringLiteral(" ev %1").arg(
                integerText(stats, QStringLiteral("playback_ring_underrun_events")));
        }
        if (hasNumber(stats, QStringLiteral("playback_ring_underrun_event_max_ms"))) {
            underrun += QStringLiteral(" max %1").arg(metricText(
                stats,
                QStringLiteral("playback_ring_underrun_event_max_ms"),
                QStringLiteral(" ms")));
        }
        if (hasNumber(stats, QStringLiteral("playback_ring_underrun_burst_max_ms"))) {
            underrun += QStringLiteral(" burst %1").arg(metricText(
                stats,
                QStringLiteral("playback_ring_underrun_burst_max_ms"),
                QStringLiteral(" ms")));
        }
    } else if (hasNumber(stats, QStringLiteral("playback_ring_underruns"))) {
        underrun = integerText(stats, QStringLiteral("playback_ring_underruns")) + QStringLiteral(" fr");
    }
    labels.underrun = QStringLiteral("Underrun ") + underrun;
    labels.stalls = QStringLiteral("Stall loop %1 burst %2p gap>2x %3 missing %4 latefr %5")
        .arg(metricText(stats, QStringLiteral("receive_loop_gap_max_ms"), QStringLiteral(" ms")))
        .arg(integerText(stats, QStringLiteral("receive_burst_packets_max")))
        .arg(integerText(stats, QStringLiteral("audio_packet_gap_over_2x_count")))
        .arg(integerText(stats, QStringLiteral("missing_audio_frames_inserted")))
        .arg(integerText(stats, QStringLiteral("late_audio_frames_dropped")));
    labels.drift = QStringLiteral("Drift ") +
        metricText(stats, QStringLiteral("drift_ppm"), QStringLiteral(" ppm"));
    labels.diagnosis = diagnoseStats(stats);
    return labels;
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

MixerMeterLevels MixerStatsViewModel::consumeStructured(const QJsonObject& stats)
{
    levels_.input = decay(levels_.input, jsonPeak(stats, "input_peak"));
    levels_.send = decay(levels_.send, jsonPeak(stats, "send_peak"));
    levels_.monitor = decay(levels_.monitor, jsonPeak(stats, "monitor_peak"));
    levels_.remote = decay(levels_.remote, jsonPeak(stats, "remote_peak"));
    levels_.metronome = decay(levels_.metronome, jsonPeak(stats, "metronome_peak"));
    levels_.output = decay(levels_.output, jsonPeak(stats, "output_peak"));
    levels_.outputClippedSamples = static_cast<std::uint64_t>(std::max(
        0.0,
        stats.value(QStringLiteral("output_clipped_samples")).toDouble(0.0)));
    return levels_;
}

void MixerStatsViewModel::reset() noexcept
{
    levels_ = {};
}

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

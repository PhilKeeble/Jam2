#include "ArtifactReader.hpp"
#include "LoopbackPortReservations.hpp"
#include "TestTiming.hpp"
#include "UdpImpairmentProxy.hpp"
#include "UdpSecurityFixtures.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace {

using jam2::test::CsvTable;
using jam2::test::DirectionImpairment;
using jam2::test::DirectionProxyStats;
using jam2::test::Pcm16MonoWav;
using jam2::test::UdpImpairmentProxy;
using jam2::test::UdpProxyStats;
using jam2::test::UdpProxyDirection;
using jam2::test::UdpSequenceSecurityTransformer;

constexpr int kPeerCount = 4;
constexpr int kSampleRate = 48000;
constexpr std::int64_t kAudioCallbackFrames = 256;
constexpr int kInitialBpm = 120;
constexpr int kFinalBpm = 150;
constexpr std::int64_t kBpmChangeFrames = 3LL * kSampleRate;
constexpr std::int64_t kRecordingStartFrames = 5LL * kSampleRate;
constexpr std::int64_t kRecordingStopFrames = 15LL * kSampleRate;
constexpr std::int64_t kLeaderAudioLossRecordingStopFrames = 17LL * kSampleRate;
constexpr std::int64_t kFinalBeatIntervalFrames = 60LL * kSampleRate / kFinalBpm;
constexpr std::int64_t kMaximumLeaderAudioGapBeats = 3;
enum class MetronomeMode {
    SharedGrid,
    LeaderAudio,
    ListenerCompensated,
};

enum class NetworkCondition {
    Clean,
    Delay,
    Jitter,
    Loss,
    Duplication,
    Reordering,
    BurstLoss,
    Security,
    SequenceSecurity,
};

struct UdpInjectionEvidence {
    std::uint64_t malformedRequested = 0;
    std::uint64_t malformedInjected = 0;
    std::uint64_t replayRequested = 0;
    std::uint64_t replayInjected = 0;
    std::uint64_t floodRequested = 0;
    std::uint64_t floodInjected = 0;
};

struct EdgeProxy {
    int peerA = 0;
    int peerB = 0;
    std::unique_ptr<UdpImpairmentProxy> proxy;
};

struct PeerProcess {
    std::unique_ptr<QProcess> process;
    QString root;
    QString scenarioPath;
    QString stdoutPath;
    QString stderrPath;
    QString readyPath;
    QString barrierPath;
};

struct PeerEvidence {
    QJsonObject manifest;
    QJsonObject sidecar;
    CsvTable csv;
    QVector<qsizetype> periodicRows;
    qsizetype initialRow = -1;
    qsizetype finalRow = -1;
    std::uint64_t localPeerId = 0;
    std::array<Pcm16MonoWav, 5> stems;
};

constexpr std::array<const char*, 5> kStemNames{
    "mix.wav",
    "my-input.wav",
    "their-input.wav",
    "inputs-mix.wav",
    "metronome.wav",
};

constexpr std::size_t kMyInputStem = 1;
constexpr std::size_t kTheirInputStem = 2;
constexpr std::size_t kMetronomeStem = 4;

QString modeText(MetronomeMode mode)
{
    switch (mode) {
    case MetronomeMode::SharedGrid: return QStringLiteral("shared-grid");
    case MetronomeMode::LeaderAudio: return QStringLiteral("leader-audio");
    case MetronomeMode::ListenerCompensated: return QStringLiteral("listener-compensated");
    }
    return {};
}

int modeId(MetronomeMode mode)
{
    return mode == MetronomeMode::SharedGrid ? 0 :
        mode == MetronomeMode::LeaderAudio ? 1 : 2;
}

std::optional<MetronomeMode> parseMode(const QString& text)
{
    if (text == QStringLiteral("shared-grid")) return MetronomeMode::SharedGrid;
    if (text == QStringLiteral("leader-audio")) return MetronomeMode::LeaderAudio;
    if (text == QStringLiteral("listener-compensated")) return MetronomeMode::ListenerCompensated;
    return std::nullopt;
}

QString conditionText(NetworkCondition condition)
{
    switch (condition) {
    case NetworkCondition::Clean: return QStringLiteral("clean");
    case NetworkCondition::Delay: return QStringLiteral("delay");
    case NetworkCondition::Jitter: return QStringLiteral("jitter");
    case NetworkCondition::Loss: return QStringLiteral("loss");
    case NetworkCondition::Duplication: return QStringLiteral("duplication");
    case NetworkCondition::Reordering: return QStringLiteral("reordering");
    case NetworkCondition::BurstLoss: return QStringLiteral("burst-loss");
    case NetworkCondition::Security: return QStringLiteral("security");
    case NetworkCondition::SequenceSecurity: return QStringLiteral("sequence-security");
    }
    return {};
}

std::optional<NetworkCondition> parseCondition(const QString& text)
{
    if (text == QStringLiteral("clean")) return NetworkCondition::Clean;
    if (text == QStringLiteral("delay")) return NetworkCondition::Delay;
    if (text == QStringLiteral("jitter")) return NetworkCondition::Jitter;
    if (text == QStringLiteral("loss")) return NetworkCondition::Loss;
    if (text == QStringLiteral("duplication")) return NetworkCondition::Duplication;
    if (text == QStringLiteral("reordering")) return NetworkCondition::Reordering;
    if (text == QStringLiteral("burst-loss")) return NetworkCondition::BurstLoss;
    if (text == QStringLiteral("security")) return NetworkCondition::Security;
    if (text == QStringLiteral("sequence-security")) return NetworkCondition::SequenceSecurity;
    return std::nullopt;
}

DirectionImpairment impairmentFor(NetworkCondition condition)
{
    DirectionImpairment result;
    switch (condition) {
    case NetworkCondition::Clean:
        break;
    case NetworkCondition::Delay:
        result.fixedDelayMs = 20.0;
        break;
    case NetworkCondition::Jitter:
        result.jitterMs = 40.0;
        break;
    case NetworkCondition::Loss:
        result.lossPercent = 2.0;
        break;
    case NetworkCondition::Duplication:
        result.duplicatePercent = 2.0;
        break;
    case NetworkCondition::Reordering:
        result.jitterMs = 8.0;
        result.reorderPercent = 2.0;
        result.preserveOrder = false;
        break;
    case NetworkCondition::BurstLoss:
        result.burstLossAfterMs = 2500.0;
        result.burstLossDurationMs = 400.0;
        break;
    case NetworkCondition::Security:
        result.corruptPercent = 1.0;
        break;
    case NetworkCondition::SequenceSecurity:
        break;
    }
    return result;
}

std::optional<std::array<std::uint8_t, 16>> sessionKeyBytes(const QString& text)
{
    const QByteArray decoded = QByteArray::fromHex(text.toLatin1());
    if (decoded.size() != 16) return std::nullopt;
    std::array<std::uint8_t, 16> result{};
    std::copy(decoded.cbegin(), decoded.cend(), result.begin());
    return result;
}

bool injectMalformedAndReplay(
    UdpImpairmentProxy& proxy,
    UdpInjectionEvidence& evidence,
    QString& error)
{
    const auto clientToServer = proxy.capturedAudioClientToServer();
    const auto serverToClient = proxy.capturedAudioServerToClient();
    if (!clientToServer || !serverToClient) {
        error = QStringLiteral("security proxy did not capture audio in both directions");
        return false;
    }
    const auto injectDirection = [&](const QByteArray& valid, auto&& inject) {
        const auto malformed = jam2::test::malformedUdpVariants(valid);
        evidence.malformedRequested += malformed.size();
        for (const auto& variant : malformed) {
            if (inject(variant.bytes)) ++evidence.malformedInjected;
        }
        ++evidence.replayRequested;
        if (inject(valid)) ++evidence.replayInjected;
    };
    injectDirection(*clientToServer, [&](const QByteArray& bytes) {
        return proxy.injectClientToServer(bytes);
    });
    injectDirection(*serverToClient, [&](const QByteArray& bytes) {
        return proxy.injectServerToClient(bytes);
    });
    if (evidence.malformedRequested != 14 ||
        evidence.malformedInjected != evidence.malformedRequested ||
        evidence.replayRequested != 2 ||
        evidence.replayInjected != evidence.replayRequested) {
        error = QStringLiteral(
            "security malformed/replay injection was incomplete: malformed=%1/%2 replay=%3/%4")
            .arg(evidence.malformedInjected).arg(evidence.malformedRequested)
            .arg(evidence.replayInjected).arg(evidence.replayRequested);
        return false;
    }
    return true;
}

bool injectShortFloodBatch(
    UdpImpairmentProxy& proxy,
    UdpInjectionEvidence& evidence,
    std::uint64_t& nextPacket)
{
    constexpr std::uint64_t kPacketsPerDirection = 4096;
    constexpr std::uint64_t kPacketsPerPump = 64;
    const std::uint64_t endPacket = std::min(
        nextPacket + kPacketsPerPump, kPacketsPerDirection);
    for (; nextPacket < endPacket; ++nextPacket) {
        QByteArray packet(8, Qt::Uninitialized);
        for (int byte = 0; byte < packet.size(); ++byte) {
            packet[byte] = static_cast<char>((nextPacket >> (byte * 8)) & 0xffU);
        }
        ++evidence.floodRequested;
        if (proxy.injectClientToServer(packet)) ++evidence.floodInjected;
        ++evidence.floodRequested;
        if (proxy.injectServerToClient(packet)) ++evidence.floodInjected;
    }
    return nextPacket == kPacketsPerDirection;
}

QString deterministicHex(int seed, int bytes)
{
    QByteArray data;
    data.reserve(bytes);
    for (int index = 0; index < bytes; ++index) {
        data.push_back(static_cast<char>((seed * 53 + index * 29 + 17) & 0xff));
    }
    return QString::fromLatin1(data.toHex());
}

QJsonObject scheduledAction(
    const QString& id,
    const QString& type,
    std::int64_t delayFrames)
{
    return {
        {QStringLiteral("id"), id},
        {QStringLiteral("type"), type},
        {QStringLiteral("after_event"), QStringLiteral("network.connected")},
        {QStringLiteral("delay_frames"), static_cast<qint64>(delayFrames)},
    };
}

bool writeJson(const QString& path, const QJsonObject& object, QString& error)
{
    error.clear();
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        error = QStringLiteral("opening scenario for write failed: ") + file.errorString();
        return false;
    }
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size()) {
        error = QStringLiteral("writing scenario failed: ") + file.errorString();
        return false;
    }
    return true;
}

std::uint64_t jsonUnsigned(const QJsonValue& value)
{
    bool ok = false;
    const qulonglong fromText = value.toString().toULongLong(&ok);
    if (ok) return static_cast<std::uint64_t>(fromText);
    if (!value.isDouble() || value.toDouble() < 0.0) return 0;
    return static_cast<std::uint64_t>(value.toDouble());
}

QString findOnlyCsv(const QString& root, QString& error)
{
    QStringList paths;
    QDirIterator iterator(
        root,
        QStringList{QStringLiteral("*.csv")},
        QDir::Files,
        QDirIterator::Subdirectories);
    while (iterator.hasNext()) paths.push_back(iterator.next());
    if (paths.size() != 1) {
        error = QStringLiteral("expected exactly one stats CSV beneath %1, found %2")
            .arg(root).arg(paths.size());
        return {};
    }
    return paths.front();
}

void pumpProxies(std::vector<EdgeProxy>& edges)
{
    for (auto& edge : edges) edge.proxy->pump();
}

void stopProcesses(std::array<PeerProcess, kPeerCount>& peers)
{
    for (auto& peer : peers) {
        if (!peer.process || peer.process->state() == QProcess::NotRunning) continue;
        peer.process->terminate();
    }
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 2000) {
        bool anyRunning = false;
        for (auto& peer : peers) {
            anyRunning |= peer.process && peer.process->state() != QProcess::NotRunning;
        }
        if (!anyRunning) return;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(5);
    }
    for (auto& peer : peers) {
        if (peer.process && peer.process->state() != QProcess::NotRunning) peer.process->kill();
    }
    for (auto& peer : peers) {
        if (peer.process) (void)peer.process->waitForFinished(2000);
    }
}

bool directionStatsValid(
    const DirectionProxyStats& stats,
    NetworkCondition condition,
    QString& reason)
{
    if (stats.packets < 1000 || stats.forwarded == 0) {
        reason = QStringLiteral("proxy direction did not carry sustained Jam2 traffic");
        return false;
    }
    if (stats.capacityDropped != 0 || stats.receiveErrors != 0 || stats.sendErrors != 0) {
        reason = QStringLiteral(
            "proxy direction encountered capacity or socket errors "
            "packets=%1 forwarded=%2 dropped=%3 duplicated=%4 corrupted=%5 "
            "transformed=%6 injected=%7 injection_errors=%8 delayed=%9 reordered=%10 "
            "burst_events=%11 burst_dropped=%12 capacity_dropped=%13 recv_errors=%14 "
            "send_errors=%15 destination_unreachable=%16 unroutable=%17")
            .arg(stats.packets).arg(stats.forwarded).arg(stats.dropped)
            .arg(stats.duplicated).arg(stats.corrupted).arg(stats.transformed)
            .arg(stats.injected).arg(stats.injectionErrors).arg(stats.delayed)
            .arg(stats.reordered).arg(stats.burstLossEvents).arg(stats.burstDropped)
            .arg(stats.capacityDropped).arg(stats.receiveErrors).arg(stats.sendErrors)
            .arg(stats.destinationUnreachable)
            .arg(stats.unroutable);
        return false;
    }
    if (stats.injectionErrors != 0) {
        reason = QStringLiteral("proxy direction encountered direct injection errors");
        return false;
    }
    const auto unrequestedMutation = [&] {
        return stats.dropped != 0 || stats.duplicated != 0 ||
            stats.corrupted != 0 || stats.transformed != 0 || stats.injected != 0 ||
            stats.reordered != 0 || stats.delayed != 0 ||
            stats.burstLossEvents != 0 || stats.burstDropped != 0;
    };
    switch (condition) {
    case NetworkCondition::Clean:
        if (unrequestedMutation()) reason = QStringLiteral("clean proxy altered traffic");
        break;
    case NetworkCondition::Delay:
    case NetworkCondition::Jitter:
        if (stats.delayed == 0 || stats.dropped != 0 || stats.duplicated != 0 ||
            stats.reordered != 0 || stats.burstDropped != 0) {
            reason = QStringLiteral("delay/jitter proxy counters do not prove the requested condition");
        }
        break;
    case NetworkCondition::Loss:
        if (stats.dropped == 0 || stats.burstDropped != 0 ||
            stats.duplicated != 0 || stats.reordered != 0) {
            reason = QStringLiteral("loss proxy counters do not prove random loss");
        }
        break;
    case NetworkCondition::Duplication:
        if (stats.duplicated == 0 || stats.dropped != 0 || stats.reordered != 0) {
            reason = QStringLiteral("duplication proxy counters do not prove duplication");
        }
        break;
    case NetworkCondition::Reordering:
        if (stats.reordered == 0 || stats.delayed == 0 || stats.dropped != 0) {
            reason = QStringLiteral("reorder proxy counters do not prove reordering");
        }
        break;
    case NetworkCondition::BurstLoss:
        if (stats.burstLossEvents != 1 || stats.burstDropped == 0 ||
            stats.dropped != stats.burstDropped) {
            reason = QStringLiteral("burst-loss proxy counters do not prove one bounded blackout");
        }
        break;
    case NetworkCondition::Security:
        if (stats.corrupted == 0 || stats.dropped != 0 || stats.duplicated != 0 ||
            stats.reordered != 0 || stats.delayed != 0 || stats.transformed != 0) {
            reason = QStringLiteral("security proxy counters do not prove deterministic corruption");
        }
        break;
    case NetworkCondition::SequenceSecurity:
        if (stats.corrupted != 0 || stats.injected != 0 || stats.dropped != 0 ||
            stats.duplicated != 0 || stats.reordered != 0 || stats.delayed != 0) {
            reason = QStringLiteral("sequence-security proxy applied an unrelated mutation");
        }
        break;
    }
    return reason.isEmpty();
}

std::optional<qsizetype> latestRow(
    const PeerEvidence& evidence,
    std::int64_t revision,
    bool requireActive,
    std::int64_t maximumAudioFrame = std::numeric_limits<std::int64_t>::max())
{
    std::optional<qsizetype> result;
    for (const qsizetype row : evidence.periodicRows) {
        const std::int64_t callbackFrame = evidence.csv.integer(
            row, QStringLiteral("audio_callbacks"), -1) * kAudioCallbackFrames;
        if (evidence.csv.integer(row, QStringLiteral("grid_revision"), -1) != revision ||
            callbackFrame < 0 || callbackFrame > maximumAudioFrame) {
            continue;
        }
        if (requireActive && evidence.csv.integer(
                row, QStringLiteral("network_active_peer_count"), -1) < 3) {
            continue;
        }
        result = row;
    }
    return result;
}

std::optional<qsizetype> earliestRow(
    const PeerEvidence& evidence,
    std::int64_t revision,
    bool requireActive)
{
    for (const qsizetype row : evidence.periodicRows) {
        if (evidence.csv.integer(row, QStringLiteral("grid_revision"), -1) != revision) {
            continue;
        }
        if (requireActive && evidence.csv.integer(
                row, QStringLiteral("network_active_peer_count"), -1) != 3) {
            continue;
        }
        return row;
    }
    return std::nullopt;
}

std::vector<std::int64_t> detectToneRejectedEvents(
    const std::vector<std::int16_t>& samples,
    double rejectedHz,
    int threshold,
    std::int64_t refractoryFrames)
{
    std::vector<std::int64_t> events;
    if (samples.size() < 3 || rejectedHz <= 0.0) return events;
    constexpr double twoPi = 6.283185307179586476925286766559;
    const double coefficient = 2.0 * std::cos(twoPi * rejectedHz / kSampleRate);
    std::int64_t holdoff = 0;
    for (std::size_t index = 2; index < samples.size(); ++index) {
        if (holdoff > 0) {
            --holdoff;
            continue;
        }
        const double rejected = static_cast<double>(samples[index]) -
            coefficient * static_cast<double>(samples[index - 1]) +
            static_cast<double>(samples[index - 2]);
        if (std::abs(rejected) >= threshold) {
            events.push_back(static_cast<std::int64_t>(index));
            holdoff = std::max<std::int64_t>(0, refractoryFrames);
        }
    }
    return events;
}

struct FittedGrid {
    std::size_t events = 0;
    std::int64_t maximumPhaseErrorFrames = 0;
    std::int64_t maximumIntervalMultiple = 0;
};

FittedGrid fitEventGrid(
    const std::vector<std::int64_t>& candidates,
    std::int64_t intervalFrames,
    std::int64_t toleranceFrames)
{
    FittedGrid best;
    if (intervalFrames <= 0) return best;
    // Leader audio passes through the listener's explicitly bounded playback
    // resampler. Validate the observable interval from each accepted click
    // instead of accumulating that intentional recovery ratio against one
    // fixed recording-phase anchor.
    const auto better = [](const FittedGrid& candidate, const FittedGrid& current) {
        return candidate.events > current.events ||
            (candidate.events == current.events &&
             candidate.maximumIntervalMultiple < current.maximumIntervalMultiple) ||
            (candidate.events == current.events &&
             candidate.maximumIntervalMultiple == current.maximumIntervalMultiple &&
             candidate.maximumPhaseErrorFrames < current.maximumPhaseErrorFrames);
    };
    std::vector<FittedGrid> chains(candidates.size(), FittedGrid{1, 0, 0});
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        for (std::size_t previous = 0; previous < index; ++previous) {
            const std::int64_t delta = candidates[index] - candidates[previous];
            const std::int64_t intervalMultiple = std::llround(
                static_cast<double>(delta) / intervalFrames);
            // The contract asks whether a sufficiently long bounded-gap chain
            // exists. Do not let a longer chain with an invalid gap eclipse a
            // valid chain at the same candidate endpoint.
            if (intervalMultiple < 1 ||
                intervalMultiple > kMaximumLeaderAudioGapBeats) {
                continue;
            }
            const std::int64_t error = std::llabs(
                delta - intervalMultiple * intervalFrames);
            if (error > toleranceFrames) continue;
            FittedGrid candidate = chains[previous];
            ++candidate.events;
            candidate.maximumIntervalMultiple = std::max(
                candidate.maximumIntervalMultiple, intervalMultiple);
            candidate.maximumPhaseErrorFrames = std::max(
                candidate.maximumPhaseErrorFrames, error);
            if (better(candidate, chains[index])) chains[index] = candidate;
        }
        if (better(chains[index], best)) best = chains[index];
    }
    return best;
}

bool eventContractValid(
    const QJsonObject& result,
    int peerIndex,
    MetronomeMode mode,
    QString& reason)
{
    std::set<QString> applied;
    const QJsonArray events = result.value(QStringLiteral("events")).toArray();
    for (const QJsonValue& value : events) {
        const QJsonObject event = value.toObject();
        if (event.value(QStringLiteral("event")).toString() == QStringLiteral("command_rejected")) {
            reason = QStringLiteral("native debug action was rejected: ") +
                event.value(QStringLiteral("id")).toString();
            return false;
        }
        if (event.value(QStringLiteral("event")).toString() == QStringLiteral("command_applied") ||
            event.value(QStringLiteral("event")).toString() == QStringLiteral("snapshot")) {
            applied.insert(event.value(QStringLiteral("id")).toString());
        }
    }
    std::vector<QString> required{
        QStringLiteral("record-start-%1").arg(peerIndex),
        QStringLiteral("record-stop-%1").arg(peerIndex),
        QStringLiteral("snapshot-%1").arg(peerIndex),
        QStringLiteral("shutdown-%1").arg(peerIndex),
    };
    if (peerIndex == kPeerCount - 1 && mode == MetronomeMode::LeaderAudio) {
        required.push_back(QStringLiteral("leader-release"));
        required.push_back(QStringLiteral("leader-claim"));
    }
    for (const QString& id : required) {
        if (!applied.contains(id)) {
            reason = QStringLiteral("native debug action did not apply: ") + id;
            return false;
        }
    }
    if (peerIndex == kPeerCount - 1 &&
        !applied.contains(QStringLiteral("bpm-hard-reset"))) {
        reason = QStringLiteral("joiner-originated BPM hard reset did not apply");
        return false;
    }
    return true;
}

bool recordingContractValid(
    const PeerEvidence& evidence,
    int peerIndex,
    std::int64_t expectedRecordingFrames,
    QString& reason)
{
    const QJsonObject& sidecar = evidence.sidecar;
    const std::int64_t queued = static_cast<std::int64_t>(
        sidecar.value(QStringLiteral("frames_queued")).toDouble(-1));
    const std::int64_t written = static_cast<std::int64_t>(
        sidecar.value(QStringLiteral("frames_written")).toDouble(-1));
    const std::int64_t start = static_cast<std::int64_t>(
        sidecar.value(QStringLiteral("start_audio_frame")).toDouble(-1));
    const std::int64_t stop = static_cast<std::int64_t>(
        sidecar.value(QStringLiteral("stop_audio_frame")).toDouble(-1));
    if (sidecar.value(QStringLiteral("format")).toString() != QStringLiteral("pcm16_mono_wav") ||
        sidecar.value(QStringLiteral("sample_rate")).toInt() != kSampleRate ||
        sidecar.value(QStringLiteral("bpm")).toInt() != kFinalBpm ||
        !sidecar.value(QStringLiteral("metronome_epoch_valid")).toBool() ||
        sidecar.value(QStringLiteral("metronome_epoch_sample_time")).toDouble() <= 0.0 ||
        queued != written || written < expectedRecordingFrames * 9 / 10 ||
        stop <= start || stop - start != written ||
        sidecar.value(QStringLiteral("dropped_frames")).toDouble(-1) != 0.0 ||
        sidecar.value(QStringLiteral("drop_events")).toDouble(-1) != 0.0 ||
        sidecar.value(QStringLiteral("writer_errors")).toDouble(-1) != 0.0) {
        reason = QStringLiteral("peer %1 recording sidecar failed exact writer/epoch contract")
            .arg(peerIndex + 1);
        return false;
    }
    for (std::size_t stem = 0; stem < evidence.stems.size(); ++stem) {
        if (evidence.stems[stem].sampleRate != kSampleRate ||
            static_cast<std::int64_t>(evidence.stems[stem].samples.size()) != written) {
            reason = QStringLiteral("peer %1 stem %2 does not match finalized PCM16 frame count")
                .arg(peerIndex + 1).arg(QString::fromLatin1(kStemNames[stem]));
            return false;
        }
    }
    return true;
}

bool sharedAudioValid(const PeerEvidence& evidence, int peerIndex, QString& reason)
{
    struct FramedOffset {
        double rawFrame = 0.0;
        double frames = 0.0;
    };
    std::vector<FramedOffset> offsets;
    for (const qsizetype row : evidence.periodicRows) {
        if (evidence.csv.integer(row, QStringLiteral("grid_revision"), -1) != 2) continue;
        offsets.push_back({
            evidence.csv.number(row, QStringLiteral("audio_callbacks"), -1.0) *
                static_cast<double>(kAudioCallbackFrames),
            evidence.csv.number(
                row, QStringLiteral("metronome_compensation_offset_frames"), 0.0),
        });
    }
    if (offsets.size() < 2) {
        reason = QStringLiteral("peer %1 lacks replacement-epoch render-offset history")
            .arg(peerIndex + 1);
        return false;
    }

    const auto offsetAt = [&](double rawFrame) {
        const auto upper = std::lower_bound(
            offsets.begin(), offsets.end(), rawFrame,
            [](const FramedOffset& sample, double value) {
                return sample.rawFrame < value;
            });
        if (upper == offsets.begin()) return upper->frames;
        if (upper == offsets.end()) return offsets.back().frames;
        const FramedOffset& after = *upper;
        const FramedOffset& before = *(upper - 1);
        const double duration = after.rawFrame - before.rawFrame;
        if (duration <= 0.0) return before.frames;
        const double alpha = std::clamp(
            (rawFrame - before.rawFrame) / duration, 0.0, 1.0);
        return before.frames + (after.frames - before.frames) * alpha;
    };

    const auto detected = jam2::test::detectEvents(
        evidence.stems[kMetronomeStem].samples, 1800, 900);
    const std::int64_t startFrame = static_cast<std::int64_t>(
        evidence.sidecar.value(QStringLiteral("start_audio_frame")).toDouble(-1));
    const std::int64_t epoch = static_cast<std::int64_t>(
        evidence.sidecar.value(QStringLiteral("metronome_epoch_sample_time")).toDouble(-1));
    if (detected.size() < 20 || startFrame < 0 || epoch <= 0) {
        reason = QStringLiteral("peer %1 lacks shared-grid clicks or mapped epoch evidence")
            .arg(peerIndex + 1);
        return false;
    }
    std::optional<std::int64_t> previousStep;
    for (std::size_t index = 0; index < detected.size(); ++index) {
        const std::int64_t rawFrame = startFrame + detected[index];
        const double renderOffset = offsetAt(static_cast<double>(rawFrame));
        const double musicalFrame = static_cast<double>(rawFrame) + renderOffset;
        const std::int64_t step = std::llround(
            (musicalFrame - static_cast<double>(epoch)) / kFinalBeatIntervalFrames);
        const double expected = static_cast<double>(epoch) +
            static_cast<double>(step * kFinalBeatIntervalFrames);
        const double phaseError = std::abs(musicalFrame - expected);
        const bool clippedStartupClick = index == 0 && detected[index] == 0 &&
            phaseError <= kSampleRate * 0.010;
        if (step < 0 || (!clippedStartupClick && phaseError > 128.0) ||
            (previousStep && step != *previousStep + 1)) {
            reason = QStringLiteral(
                "peer %1 shared-grid click %2 violated dynamic epoch model: "
                "step=%3 phase_error_frames=%4 render_offset_frames=%5 raw_frame=%6")
                .arg(peerIndex + 1).arg(index).arg(step)
                .arg(phaseError, 0, 'f', 2)
                .arg(renderOffset, 0, 'f', 2)
                .arg(rawFrame);
            return false;
        }
        previousStep = step;
    }
    if (jam2::test::rootMeanSquare(
            evidence.stems[kMyInputStem].samples,
            0,
            static_cast<std::int64_t>(evidence.stems[kMyInputStem].samples.size())) > 1.0) {
        reason = QStringLiteral("shared-grid silence input unexpectedly contains audio");
        return false;
    }
    return true;
}

bool leaderAudioValid(
    const std::array<PeerEvidence, kPeerCount>& peers,
    std::uint64_t authorityPeerId,
    QString& reason)
{
    int audibleLocalMetronomes = 0;
    for (int index = 0; index < kPeerCount; ++index) {
        const PeerEvidence& evidence = peers[static_cast<std::size_t>(index)];
        const bool authority = evidence.localPeerId == authorityPeerId;
        const auto localClicks = jam2::test::detectEvents(
            evidence.stems[kMetronomeStem].samples, 1800, 900);
        if (authority) {
            const auto expected = jam2::test::analyzeExpectedClicks(
                evidence.sidecar, evidence.stems[kMetronomeStem].samples);
            if (!expected.ok || expected.detectedClicks < 20) {
                reason = QStringLiteral("leader authority local click stem is not epoch-exact");
                return false;
            }
            ++audibleLocalMetronomes;
        } else if (!localClicks.empty()) {
            reason = QStringLiteral("leader-audio listener rendered a competing local click");
            return false;
        }

        for (const std::size_t stem : {kMyInputStem, kTheirInputStem}) {
            const auto& samples = evidence.stems[stem].samples;
            const std::int64_t start = std::min<std::int64_t>(
                kSampleRate, static_cast<std::int64_t>(samples.size()));
            const std::int64_t stop = std::max<std::int64_t>(
                start, static_cast<std::int64_t>(samples.size()) - kSampleRate / 2);
            const double rms = jam2::test::rootMeanSquare(samples, start, stop);
            const double tone = jam2::test::estimateToneHz(samples, kSampleRate, start, stop);
            bool continuousMixedTone = true;
            std::size_t completeToneWindows = 0;
            if (stem == kTheirInputStem) {
                for (std::int64_t window = start;
                     window + kSampleRate <= stop;
                     window += kSampleRate) {
                    const double windowTone = jam2::test::estimateToneHz(
                        samples, kSampleRate, window, window + kSampleRate);
                    continuousMixedTone = continuousMixedTone &&
                        std::abs(windowTone - 440.0) <= 5.0;
                    ++completeToneWindows;
                }
            }
            const bool contentValid = std::abs(tone - 440.0) <= 5.0 &&
                (stem == kMyInputStem
                    ? rms >= 500.0
                    : continuousMixedTone && completeToneWindows >= 8);
            if (!contentValid) {
                reason = QStringLiteral("leader-audio tone continuity/content check failed on peer %1 stem %2")
                    .arg(index + 1).arg(QString::fromLatin1(kStemNames[stem]));
                return false;
            }
        }
        if (!authority) {
            const auto candidates = detectToneRejectedEvents(
                evidence.stems[kTheirInputStem].samples, 440.0, 100, 900);
            const auto received = fitEventGrid(
                candidates, kFinalBeatIntervalFrames, 480);
            if (received.events < 20 ||
                received.maximumIntervalMultiple > kMaximumLeaderAudioGapBeats) {
                reason = QStringLiteral(
                    "peer %1 did not receive grid-timed leader clicks under impairment: "
                    "candidates=%2 fitted=%3 max_interval_error_frames=%4 max_gap_beats=%5")
                    .arg(index + 1).arg(candidates.size()).arg(received.events)
                    .arg(received.maximumPhaseErrorFrames)
                    .arg(received.maximumIntervalMultiple);
                return false;
            }
        }
    }
    if (audibleLocalMetronomes != 1) {
        reason = QStringLiteral("leader-audio did not have exactly one local click renderer");
        return false;
    }
    return true;
}

bool listenerAudioValid(const PeerEvidence& evidence, int peerIndex, QString& reason)
{
    const auto inputEpoch = jam2::test::analyzeExpectedClicks(
        evidence.sidecar,
        evidence.stems[kMyInputStem].samples,
        2500,
        900,
        96);
    if (!inputEpoch.ok || inputEpoch.detectedClicks < 20) {
        reason = QStringLiteral("peer %1 metro-pulse input does not prove the replacement epoch")
            .arg(peerIndex + 1);
        return false;
    }
    const auto clickGrid = jam2::test::analyzeIntervalGrid(
        evidence.stems[kMetronomeStem].samples,
        1800,
        900,
        kFinalBeatIntervalFrames);
    if (clickGrid.clicks < 20 || clickGrid.maximumPhaseErrorFrames > 1200) {
        reason = QStringLiteral("peer %1 compensated click interval left the bounded grid")
            .arg(peerIndex + 1);
        return false;
    }
    const auto remotePulses = jam2::test::detectEvents(
        evidence.stems[kTheirInputStem].samples, 2500, 18000);
    const auto localClicks = jam2::test::detectEvents(
        evidence.stems[kMetronomeStem].samples, 1800, 900);
    const auto matches = jam2::test::nearestEvents(
        remotePulses, localClicks, kSampleRate / 4);
    constexpr std::size_t steadySkip = 8;
    const std::size_t matchable = std::min(remotePulses.size(), localClicks.size());
    const std::size_t requiredMatches = matchable > 2 ? matchable - 2 : matchable;
    if (remotePulses.size() < 20 || localClicks.size() < 20 ||
        matches.signedErrors.size() < std::max(steadySkip + 10, requiredMatches) ||
        matches.missingReferences > 2) {
        reason = QStringLiteral(
            "peer %1 lacks enough steady remote-pulse/local-click matches: "
            "remote=%2 local=%3 matched=%4 missing=%5")
            .arg(peerIndex + 1).arg(remotePulses.size()).arg(localClicks.size())
            .arg(matches.signedErrors.size()).arg(matches.missingReferences);
        return false;
    }

    // Listener compensation targets the average phase of all remote inputs.
    // A threshold edge finds the first arriving peer, not that average.  The
    // energy centroid of each mixed remote click is the observable WAV-domain
    // equivalent, and remains meaningful when one peer's click is damaged by
    // packet loss.
    std::vector<double> signedCentroidErrors;
    std::vector<double> centroidErrors;
    const auto& remoteSamples = evidence.stems[kTheirInputStem].samples;
    constexpr std::int64_t centroidRadius = 8000;
    for (std::size_t index = steadySkip; index < localClicks.size(); ++index) {
        const std::int64_t local = localClicks[index];
        const std::int64_t begin = std::max<std::int64_t>(0, local - centroidRadius);
        const std::int64_t end = std::min<std::int64_t>(
            static_cast<std::int64_t>(remoteSamples.size()), local + centroidRadius);
        long double energy = 0.0L;
        long double weightedOffset = 0.0L;
        for (std::int64_t frame = begin; frame < end; ++frame) {
            const long double sample = remoteSamples[static_cast<std::size_t>(frame)];
            const long double weight = sample * sample;
            energy += weight;
            weightedOffset += static_cast<long double>(frame - local) * weight;
        }
        if (energy > 0.0L) {
            const double signedError = static_cast<double>(weightedOffset / energy);
            signedCentroidErrors.push_back(signedError);
            centroidErrors.push_back(std::abs(signedError));
        }
    }
    if (centroidErrors.size() < 10) {
        reason = QStringLiteral("peer %1 lacks enough remote click energy windows")
            .arg(peerIndex + 1);
        return false;
    }
    std::sort(centroidErrors.begin(), centroidErrors.end());
    std::sort(signedCentroidErrors.begin(), signedCentroidErrors.end());
    const double medianFrames = centroidErrors[centroidErrors.size() / 2];
    const double maximumFrames = centroidErrors.back();
    if (medianFrames > kSampleRate * 0.010 || maximumFrames > kSampleRate * 0.050) {
        long double signedTotal = 0.0L;
        for (const double value : signedCentroidErrors) signedTotal += value;
        const double signedMedianFrames =
            signedCentroidErrors[signedCentroidErrors.size() / 2];
        const double signedMeanFrames = static_cast<double>(
            signedTotal / static_cast<long double>(signedCentroidErrors.size()));
        const double finalOffset = evidence.csv.number(
            evidence.finalRow, QStringLiteral("metronome_compensation_offset_frames"));
        const double finalTarget = evidence.csv.number(
            evidence.finalRow, QStringLiteral("metronome_compensation_target_frames"));
        const double finalAverageLatency = evidence.csv.number(
            evidence.finalRow,
            QStringLiteral("metronome_compensation_average_latency_frames"));
        reason = QStringLiteral(
            "peer %1 compensated click/remote-energy alignment exceeded bounds: "
            "median_frames=%2 maximum_frames=%3 signed_median_frames=%4 "
            "signed_mean_frames=%5 windows=%6 final_offset_frames=%7 "
            "final_target_frames=%8 final_average_latency_frames=%9")
            .arg(peerIndex + 1)
            .arg(medianFrames, 0, 'f', 2)
            .arg(maximumFrames, 0, 'f', 2)
            .arg(signedMedianFrames, 0, 'f', 2)
            .arg(signedMeanFrames, 0, 'f', 2)
            .arg(centroidErrors.size())
            .arg(finalOffset, 0, 'f', 2)
            .arg(finalTarget, 0, 'f', 2)
            .arg(finalAverageLatency, 0, 'f', 2);
        return false;
    }

    const std::int64_t revision = evidence.csv.integer(
        evidence.finalRow, QStringLiteral("grid_revision"), -1);
    std::optional<std::int64_t> convergenceEvidenceStartFrame;
    std::int64_t convergenceEvidenceStopFrame = 0;
    bool convergedOffsetObserved = false;
    for (const qsizetype row : evidence.periodicRows) {
        const std::int64_t callbacks = evidence.csv.integer(
            row, QStringLiteral("audio_callbacks"), -1);
        const std::int64_t callbackFrame = callbacks * kAudioCallbackFrames;
        if (callbackFrame < 10LL * kSampleRate ||
            callbackFrame > kRecordingStopFrames ||
            evidence.csv.integer(row, QStringLiteral("grid_revision"), -1) != revision ||
            evidence.csv.integer(row, QStringLiteral("network_active_peer_count"), -1) != 3) {
            continue;
        }
        const double base = evidence.csv.number(
            row, QStringLiteral("metronome_compensation_base_frames"));
        const double target = evidence.csv.number(
            row, QStringLiteral("metronome_compensation_target_frames"));
        const double offset = evidence.csv.number(
            row, QStringLiteral("metronome_compensation_offset_frames"));
        const double averageLatency = evidence.csv.number(
            row, QStringLiteral("metronome_compensation_average_latency_frames"));
        const double appliedLatency = std::min(averageLatency, kSampleRate * 0.250);
        if (!evidence.csv.yes(row, QStringLiteral("metronome_compensation_active")) ||
            evidence.csv.integer(
                row, QStringLiteral("metronome_compensation_peer_count"), -1) != 3 ||
            averageLatency <= 0.0 || std::abs((target - base) + appliedLatency) > 2.0) {
            reason = QStringLiteral(
                "peer %1 listener-compensation formula or three-peer input failed at callback %2")
                .arg(peerIndex + 1).arg(callbacks);
            return false;
        }
        if (!convergenceEvidenceStartFrame) {
            convergenceEvidenceStartFrame = callbackFrame;
        }
        convergenceEvidenceStopFrame = callbackFrame;
        convergedOffsetObserved = convergedOffsetObserved ||
            std::abs(offset - target) <= kSampleRate * 0.005;
    }
    if (!convergenceEvidenceStartFrame ||
        convergenceEvidenceStopFrame - *convergenceEvidenceStartFrame <
            4LL * kSampleRate ||
        !convergedOffsetObserved) {
        reason = QStringLiteral(
            "peer %1 listener-compensation lacks frame-spanning convergence evidence: "
            "start_frame=%2 stop_frame=%3 converged=%4")
            .arg(peerIndex + 1)
            .arg(convergenceEvidenceStartFrame.value_or(-1))
            .arg(convergenceEvidenceStopFrame)
            .arg(convergedOffsetObserved);
        return false;
    }
    return true;
}

} // namespace

enum class AttemptOutcome {
    Success,
    // The harness itself could not produce valid evidence. Keep the cause
    // classified internally, but never retry a CTest run.
    HarnessInvalid,
    // A product or evidence-contract check failed.
    ProductInvalid,
};

struct AttemptResult {
    AttemptOutcome outcome = AttemptOutcome::ProductInvalid;
    QString message;
};

AttemptResult runAttempt(
    const QString& executable,
    MetronomeMode mode,
    NetworkCondition condition,
    const std::array<quint16, kPeerCount>& ports,
    LoopbackPortReservations& reservations,
    const QString& sessionId,
    const QString& sessionKey,
    const std::array<QString, kPeerCount>& tokens,
    const DirectionImpairment& impairment,
    const QString& temporaryRoot)
{
    const QString root = temporaryRoot;
    if (!QDir().mkpath(root)) {
        return {AttemptOutcome::HarnessInvalid,
                QStringLiteral("creating the attempt root failed")};
    }
    std::array<PeerProcess, kPeerCount> processes{};
    auto die = [&](const QString& message,
                   AttemptOutcome outcome = AttemptOutcome::ProductInvalid) {
        stopProcesses(processes);
        return AttemptResult{outcome, message};
    };
    QString error;
    std::optional<UdpSequenceSecurityTransformer> sequenceTransformer;
    if (condition == NetworkCondition::SequenceSecurity) {
        bool sessionIdOk = false;
        const std::uint64_t numericSessionId = sessionId.toULongLong(&sessionIdOk, 16);
        const auto key = sessionKeyBytes(sessionKey);
        if (!sessionIdOk || numericSessionId == 0 || !key) {
            return die(QStringLiteral("could not decode deterministic UDP security session"));
        }
        sequenceTransformer.emplace(
            *key, numericSessionId, jam2::NetworkAudioFormat::Pcm16Mono);
    }

    std::vector<EdgeProxy> edges;
    QJsonObject topology;
    for (const QString& token : tokens) topology.insert(token, QJsonObject{});
    int edgeIndex = 0;
    for (int peerA = 0; peerA < kPeerCount; ++peerA) {
        for (int peerB = peerA + 1; peerB < kPeerCount; ++peerB) {
            auto proxy = std::make_unique<UdpImpairmentProxy>(
                QHostAddress::LocalHost,
                ports[static_cast<std::size_t>(peerA)],
                impairment,
                impairment,
                0x4a414d3200000000ULL + static_cast<std::uint64_t>(edgeIndex),
                16384);
            if (edgeIndex == 0 && sequenceTransformer) {
                proxy->setDatagramTransformer(
                    [&](UdpProxyDirection direction, const QByteArray& packet) {
                        return sequenceTransformer->transform(direction, packet);
                    });
            }
            if (!proxy->start(error)) return die(error, AttemptOutcome::HarnessInvalid);
            QJsonObject fromA = topology.value(tokens[static_cast<std::size_t>(peerA)]).toObject();
            QJsonObject fromB = topology.value(tokens[static_cast<std::size_t>(peerB)]).toObject();
            fromA.insert(tokens[static_cast<std::size_t>(peerB)], proxy->serverPublicEndpoint());
            fromB.insert(tokens[static_cast<std::size_t>(peerA)], proxy->publicEndpoint());
            topology.insert(tokens[static_cast<std::size_t>(peerA)], fromA);
            topology.insert(tokens[static_cast<std::size_t>(peerB)], fromB);
            edges.push_back({peerA, peerB, std::move(proxy)});
            ++edgeIndex;
        }
    }
    if (edges.size() != 6) return die(QStringLiteral("exactly six edge proxies were not created"));
    std::vector<std::array<std::optional<std::uint64_t>, 2>> burstForwardBaselines(
        edges.size());
    std::vector<std::array<bool, 2>> burstForwardRecovered(edges.size());

    const std::int64_t recordingStopFrames =
        mode == MetronomeMode::LeaderAudio && condition == NetworkCondition::Loss
        ? kLeaderAudioLossRecordingStopFrames
        : kRecordingStopFrames;
    const std::int64_t expectedRecordingFrames =
        recordingStopFrames - kRecordingStartFrames;
    const std::int64_t snapshotFrames = recordingStopFrames + kSampleRate / 4;
    const std::int64_t shutdownFrames = recordingStopFrames + kSampleRate;
    const QString osPriority = QStringLiteral("off");
    const QString invite = QStringLiteral("jam2://v1?endpoint=127.0.0.1:%1&session=%2&key=%3")
        .arg(ports[0]).arg(sessionId, sessionKey);
    for (int index = 0; index < kPeerCount; ++index) {
        PeerProcess& peer = processes[static_cast<std::size_t>(index)];
        peer.root = QDir(root).filePath(QStringLiteral("peer-%1").arg(index + 1));
        if (!QDir().mkpath(peer.root)) return die(QStringLiteral("creating peer root failed"),
                                                  AttemptOutcome::HarnessInvalid);
        peer.scenarioPath = QDir(peer.root).filePath(QStringLiteral("scenario.json"));
        peer.stdoutPath = QDir(peer.root).filePath(QStringLiteral("stdout.log"));
        peer.stderrPath = QDir(peer.root).filePath(QStringLiteral("stderr.log"));
        peer.readyPath = QDir(peer.root).filePath(QStringLiteral("start.ready"));
        peer.barrierPath = QDir(peer.root).filePath(QStringLiteral("start.barrier"));
        const QString recording = QDir(peer.root).filePath(QStringLiteral("recording"));

        QJsonArray actions;
        if (index == kPeerCount - 1) {
            if (mode == MetronomeMode::LeaderAudio) {
                QJsonObject release = scheduledAction(
                    QStringLiteral("leader-release"),
                    QStringLiteral("metronome.enabled"),
                    kBpmChangeFrames - kSampleRate);
                release.insert(QStringLiteral("enabled"), false);
                actions.push_back(release);
                QJsonObject claim = scheduledAction(
                    QStringLiteral("leader-claim"),
                    QStringLiteral("metronome.enabled"),
                    kBpmChangeFrames - kSampleRate / 2);
                claim.insert(QStringLiteral("enabled"), true);
                actions.push_back(claim);
            }
            QJsonObject bpm = scheduledAction(
                QStringLiteral("bpm-hard-reset"),
                QStringLiteral("metronome.bpm"),
                kBpmChangeFrames);
            bpm.insert(QStringLiteral("value"), kFinalBpm);
            actions.push_back(bpm);
        }
        QJsonObject start = scheduledAction(
            QStringLiteral("record-start-%1").arg(index),
            QStringLiteral("recording.start"),
            kRecordingStartFrames);
        start.insert(QStringLiteral("path"), recording);
        actions.push_back(start);
        actions.push_back(scheduledAction(
            QStringLiteral("record-stop-%1").arg(index),
            QStringLiteral("recording.stop"),
            recordingStopFrames));
        actions.push_back(scheduledAction(
            QStringLiteral("snapshot-%1").arg(index),
            QStringLiteral("snapshot"),
            snapshotFrames));
        actions.push_back(scheduledAction(
            QStringLiteral("shutdown-%1").arg(index),
            QStringLiteral("shutdown"),
            shutdownFrames + (index == 0 ? kSampleRate / 2 : 0)));

        const QString signal = mode == MetronomeMode::LeaderAudio
            ? QStringLiteral("tone-440") :
            mode == MetronomeMode::ListenerCompensated
                ? QStringLiteral("metro-pulse") : QStringLiteral("silence");
        QJsonObject network{
            {QStringLiteral("bind"), QStringLiteral("127.0.0.1:%1")
                .arg(ports[static_cast<std::size_t>(index)])},
            {QStringLiteral("no_stun"), true},
            {QStringLiteral("wait_ms"), 120000},
            {QStringLiteral("peer_token"), tokens[static_cast<std::size_t>(index)]},
            {QStringLiteral("topology"), topology},
        };
        if (index == 0) {
            network.insert(QStringLiteral("session_id"), sessionId);
            network.insert(QStringLiteral("session_key"), sessionKey);
            network.insert(QStringLiteral("max_peers"), kPeerCount);
        } else {
            network.insert(QStringLiteral("join_url"), invite);
        }
        const QJsonObject scenario{
            {QStringLiteral("schema"), QStringLiteral("jam2-debug-scenario")},
            {QStringLiteral("run_id"), QStringLiteral("native-metronome-%1-%2-peer-%3")
                .arg(modeText(mode), conditionText(condition)).arg(index + 1)},
            {QStringLiteral("operation"), index == 0
                ? QStringLiteral("network.create") : QStringLiteral("network.join")},
            {QStringLiteral("profile"), QStringLiteral("fast")},
            {QStringLiteral("runtime"), QJsonObject{
                {QStringLiteral("headless_audio"), true},
                {QStringLiteral("sample_rate"), kSampleRate},
                {QStringLiteral("audio_buffer_size"), 256},
                {QStringLiteral("frame_size"), 64},
                {QStringLiteral("network_audio_format"), QStringLiteral("pcm16")},
                {QStringLiteral("stats"), true},
                {QStringLiteral("stats_interval_ms"), 100},
                {QStringLiteral("stats_warmup_ms"), 0},
                {QStringLiteral("stream_ms"), 0},
                {QStringLiteral("stream_linger_ms"), 100},
                {QStringLiteral("test_input"), signal},
                {QStringLiteral("metronome"), true},
                {QStringLiteral("bpm"), kInitialBpm},
                {QStringLiteral("metronome_level"), 0.20},
                {QStringLiteral("metronome_mode"), modeText(mode)},
                {QStringLiteral("os_priority"), osPriority},
            }},
            {QStringLiteral("artifacts"), QJsonObject{{QStringLiteral("root"), peer.root}}},
            {QStringLiteral("network"), network},
            {QStringLiteral("actions"), actions},
        };
        if (!writeJson(peer.scenarioPath, scenario, error)) return die(error, AttemptOutcome::HarnessInvalid);
    }


    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    for (const QString& name : {
             QStringLiteral("JAM2_AUTOMATION_COMMAND_HANDLE"),
             QStringLiteral("JAM2_AUTOMATION_EVENT_HANDLE"),
             QStringLiteral("JAM2_AUTOMATION_COMMAND_FD"),
             QStringLiteral("JAM2_AUTOMATION_EVENT_FD")}) {
        environment.remove(name);
    }
    for (int index = 0; index < kPeerCount; ++index) {
        PeerProcess& peer = processes[static_cast<std::size_t>(index)];
        peer.process = std::make_unique<QProcess>();
        QProcessEnvironment peerEnvironment = environment;
        peerEnvironment.insert(
            QStringLiteral("JAM2_TEST_READY_FILE"), peer.readyPath);
        peerEnvironment.insert(
            QStringLiteral("JAM2_TEST_START_BARRIER"), peer.barrierPath);
        peer.process->setProcessEnvironment(peerEnvironment);
        peer.process->setWorkingDirectory(QFileInfo(executable).absolutePath());
        peer.process->setStandardOutputFile(peer.stdoutPath, QIODevice::Truncate);
        peer.process->setStandardErrorFile(peer.stderrPath, QIODevice::Truncate);
        peer.process->start(executable, {
            QStringLiteral("debug"),
            QStringLiteral("run"),
            peer.scenarioPath,
        });
        if (!peer.process->waitForStarted(static_cast<int>(
                jam2::test::deadmanTimeout(std::chrono::seconds(30)).count()))) {
            stopProcesses(processes);
            return die(QStringLiteral("peer %1 failed to start: %2")
                .arg(index + 1).arg(peer.process->errorString()),
                AttemptOutcome::HarnessInvalid);
        }
    }

    QElapsedTimer readyDeadline;
    readyDeadline.start();
    const qint64 readyTimeoutMs = jam2::test::deadmanTimeout(
        std::chrono::seconds(30)).count();
    while (readyDeadline.elapsed() < readyTimeoutMs) {
        const bool allReady = std::all_of(
            processes.begin(), processes.end(), [](const PeerProcess& peer) {
                return QFileInfo::exists(peer.readyPath);
            });
        if (allReady) break;
        for (int index = 0; index < kPeerCount; ++index) {
            const PeerProcess& peer = processes[static_cast<std::size_t>(index)];
            if (peer.process && peer.process->state() == QProcess::NotRunning) {
                stopProcesses(processes);
                return die(QStringLiteral(
                    "peer %1 exited before the four-peer start barrier; inspect %2")
                    .arg(index + 1).arg(peer.stderrPath),
                AttemptOutcome::HarnessInvalid);
            }
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        QThread::msleep(2);
    }
    if (!std::all_of(processes.begin(), processes.end(), [](const PeerProcess& peer) {
            return QFileInfo::exists(peer.readyPath);
        })) {
        stopProcesses(processes);
        return die(QStringLiteral("four peers did not reach the bounded start barrier"),
                   AttemptOutcome::HarnessInvalid);
    }

    const auto releasePeer = [&](int index) {
        QFile barrier(processes[static_cast<std::size_t>(index)].barrierPath);
        return barrier.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
            barrier.write("start\n") == 6;
    };
    // Keep every peer's paired TCP/UDP port owned while the six proxies bind
    // their ephemeral sockets and all four children reach the pre-operation
    // barrier. Release only at the exact handoff before Jam2 binds, so a proxy
    // cannot reclaim a peer port during parallel CTest startup.
    reservations.release();
    if (!releasePeer(0)) {
        stopProcesses(processes);
        return die(QStringLiteral("could not release the creator start barrier"),
                   AttemptOutcome::HarnessInvalid);
    }
    for (int index = 1; index < kPeerCount; ++index) {
        if (!releasePeer(index)) {
            stopProcesses(processes);
            return die(QStringLiteral("could not release peer %1 start barrier")
                .arg(index + 1),
                AttemptOutcome::HarnessInvalid);
        }
    }

    QElapsedTimer deadline;
    deadline.start();
    bool allFinished = false;
    UdpInjectionEvidence injectionEvidence;
    bool malformedAndReplayInjected = false;
    bool shortFloodInjected = false;
    std::uint64_t nextShortFloodPacket = 0;
    std::optional<std::vector<UdpProxyStats>> securityInjectionCompleteStats;
    std::optional<std::vector<UdpProxyStats>> activeProxyStats;
    const qint64 scenarioTimeoutMs =
        jam2::test::deadmanTimeout(std::chrono::seconds(120)).count();
    while (deadline.elapsed() < scenarioTimeoutMs) {
        pumpProxies(edges);
        if (condition == NetworkCondition::BurstLoss) {
            for (std::size_t edgeIndexValue = 0;
                 edgeIndexValue < edges.size(); ++edgeIndexValue) {
                const UdpProxyStats stats = edges[edgeIndexValue].proxy->stats();
                const std::array<const DirectionProxyStats*, 2> directions{
                    &stats.clientToServer,
                    &stats.serverToClient,
                };
                for (std::size_t direction = 0; direction < directions.size(); ++direction) {
                    const DirectionProxyStats& current = *directions[direction];
                    auto& baseline = burstForwardBaselines[edgeIndexValue][direction];
                    if (!baseline && current.burstLossEvents == 1 &&
                        current.burstDropped > 0) {
                        baseline = current.forwarded;
                    } else if (baseline && current.forwarded > *baseline) {
                        burstForwardRecovered[edgeIndexValue][direction] = true;
                    }
                }
            }
        }
        const UdpProxyStats securityEdgeStats = edges.front().proxy->stats();
        const bool bidirectionalTrafficObserved =
            securityEdgeStats.clientToServer.forwarded >= 128 &&
            securityEdgeStats.serverToClient.forwarded >= 128;
        if (condition == NetworkCondition::Security &&
            !malformedAndReplayInjected && bidirectionalTrafficObserved) {
            if (!injectMalformedAndReplay(
                    *edges.front().proxy, injectionEvidence, error)) {
                stopProcesses(processes);
                return die(error, AttemptOutcome::HarnessInvalid);
            }
            malformedAndReplayInjected = true;
        }
        if (condition == NetworkCondition::Security &&
            malformedAndReplayInjected && !shortFloodInjected) {
            // Keep the adversarial packet count unchanged while interleaving
            // bounded injection batches with all six proxy pumps. A single
            // synchronous 8,192-datagram fixture burst can otherwise stall
            // the harness thread and invalidate the network evidence it is
            // meant to collect.
            shortFloodInjected = injectShortFloodBatch(
                *edges.front().proxy, injectionEvidence, nextShortFloodPacket);
            if (shortFloodInjected && !securityInjectionCompleteStats) {
                securityInjectionCompleteStats.emplace();
                securityInjectionCompleteStats->reserve(edges.size());
                for (const auto& edge : edges) {
                    securityInjectionCompleteStats->push_back(edge.proxy->stats());
                }
            }
        }
        const bool anyFinished = std::any_of(
            processes.begin(), processes.end(), [](const PeerProcess& peer) {
                return peer.process && peer.process->state() == QProcess::NotRunning;
            });
        // The first frame-scheduled peer shutdown is the deterministic signal
        // that every recording and hard-reset evidence window has completed.
        // Capture before later shutdowns can produce loopback lifecycle noise.
        if (!activeProxyStats && anyFinished) {
            activeProxyStats.emplace();
            activeProxyStats->reserve(edges.size());
            for (const auto& edge : edges) activeProxyStats->push_back(edge.proxy->stats());
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 2);
        allFinished = std::all_of(
            processes.begin(), processes.end(), [](const PeerProcess& peer) {
                return peer.process && peer.process->state() == QProcess::NotRunning;
            });
        if (allFinished) break;
        QThread::usleep(500);
    }
    if (!allFinished) {
        stopProcesses(processes);
        return die(QStringLiteral("four-peer metronome case exceeded its stall deadline"),
                   AttemptOutcome::HarnessInvalid);
    }
    if (condition == NetworkCondition::Security &&
        (!malformedAndReplayInjected || !shortFloodInjected ||
         injectionEvidence.floodRequested != 8192 ||
         injectionEvidence.floodInjected < 4096)) {
        return die(QStringLiteral(
            "UDP security injection did not complete: malformed=%1/%2 replay=%3/%4 flood=%5/%6")
            .arg(injectionEvidence.malformedInjected)
            .arg(injectionEvidence.malformedRequested)
            .arg(injectionEvidence.replayInjected)
            .arg(injectionEvidence.replayRequested)
            .arg(injectionEvidence.floodInjected)
            .arg(injectionEvidence.floodRequested),
            AttemptOutcome::HarnessInvalid);
    }
    for (int index = 0; index < kPeerCount; ++index) {
        const QProcess& process = *processes[static_cast<std::size_t>(index)].process;
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
            return die(QStringLiteral("peer %1 exited abnormally with code %2")
                .arg(index + 1).arg(process.exitCode()));
        }
    }

    if (!activeProxyStats) {
        activeProxyStats.emplace();
        activeProxyStats->reserve(edges.size());
        for (const auto& edge : edges) activeProxyStats->push_back(edge.proxy->stats());
    }
    if (condition == NetworkCondition::BurstLoss) {
        for (std::size_t edgeIndexValue = 0;
             edgeIndexValue < edges.size(); ++edgeIndexValue) {
            for (std::size_t direction = 0; direction < 2; ++direction) {
                if (!burstForwardBaselines[edgeIndexValue][direction] ||
                    !burstForwardRecovered[edgeIndexValue][direction]) {
                    const EdgeProxy& edge = edges[edgeIndexValue];
                    return die(QStringLiteral(
                        "edge %1-%2 direction %3 did not resume forwarding after its observed blackout")
                        .arg(edge.peerA + 1)
                        .arg(edge.peerB + 1)
                        .arg(direction));
                }
            }
        }
    }
    for (std::size_t edgeIndexValue = 0; edgeIndexValue < edges.size(); ++edgeIndexValue) {
        const EdgeProxy& edge = edges[edgeIndexValue];
        const UdpProxyStats& stats = activeProxyStats->at(edgeIndexValue);
        if (stats.pendingHighWater > stats.pendingLimit) {
            return die(QStringLiteral("edge %1-%2 exceeded its bounded pending queue")
                .arg(edge.peerA + 1).arg(edge.peerB + 1));
        }
        for (const auto* direction : {&stats.clientToServer, &stats.serverToClient}) {
            QString reason;
            if (!directionStatsValid(*direction, condition, reason)) {
                return die(QStringLiteral("edge %1-%2: %3")
                    .arg(edge.peerA + 1).arg(edge.peerB + 1).arg(reason));
            }
        }
        if (condition == NetworkCondition::Security) {
            if (!securityInjectionCompleteStats) {
                return die(QStringLiteral(
                    "security injection completion did not expose a traffic baseline"),
                    AttemptOutcome::HarnessInvalid);
            }
            const UdpProxyStats& baseline =
                securityInjectionCompleteStats->at(edgeIndexValue);
            if (stats.clientToServer.forwarded <= baseline.clientToServer.forwarded ||
                stats.serverToClient.forwarded <= baseline.serverToClient.forwarded) {
                return die(QStringLiteral(
                    "edge %1-%2 did not carry fresh bidirectional traffic after security injection")
                    .arg(edge.peerA + 1).arg(edge.peerB + 1));
            }
        }
    }

    std::array<PeerEvidence, kPeerCount> evidence;
    for (int index = 0; index < kPeerCount; ++index) {
        const PeerProcess& process = processes[static_cast<std::size_t>(index)];
        PeerEvidence& peer = evidence[static_cast<std::size_t>(index)];
        if (!jam2::test::readJsonObject(
                QDir(process.root).filePath(QStringLiteral("native-manifest.json")),
                peer.manifest,
                error)) {
            return die(error);
        }
        if (peer.manifest.value(QStringLiteral("schema")).toString() !=
                QStringLiteral("jam2-debug-manifest") ||
            !peer.manifest.value(QStringLiteral("ok")).toBool()) {
            return die(QStringLiteral("peer %1 native manifest failed").arg(index + 1));
        }
        peer.localPeerId = jsonUnsigned(peer.manifest.value(QStringLiteral("local_peer_id")));
        const QJsonObject result = peer.manifest.value(QStringLiteral("result")).toObject();
        if (peer.localPeerId == 0 ||
            result.value(QStringLiteral("remote_peer_count")).toInt(-1) != 3 ||
            result.value(QStringLiteral("control_max_active_remote_peers")).toInt(-1) != 3 ||
            result.value(QStringLiteral("commands_rejected")).toDouble(-1) != 0.0 ||
            result.value(QStringLiteral("event_rejects")).toDouble(-1) != 0.0 ||
            result.value(QStringLiteral("event_trace_drops")).toDouble(-1) != 0.0 ||
            result.value(QStringLiteral("pending_static_actions")).toInt(-1) != 0 ||
            result.value(QStringLiteral("pending_controller_actions")).toDouble(-1) != 0.0) {
            return die(QStringLiteral("peer %1 manifest lifecycle/action contract failed")
                .arg(index + 1));
        }
        if (!eventContractValid(result, index, mode, error)) return die(error);

        const QString csvPath = findOnlyCsv(process.root, error);
        if (csvPath.isEmpty() || !peer.csv.read(csvPath, error)) return die(error);
        for (const QString& column : {
                 QStringLiteral("audio_callbacks"),
                 QStringLiteral("network_active_peer_count"),
                 QStringLiteral("grid_authority_peer_id"),
                 QStringLiteral("grid_revision"),
                 QStringLiteral("grid_mode"),
                 QStringLiteral("grid_authority_epoch_frame"),
                 QStringLiteral("grid_mapped_epoch_frame"),
                 QStringLiteral("grid_mapping_error_frames"),
                 QStringLiteral("metronome_epoch_sample_time"),
                 QStringLiteral("local_metronome_beat"),
                 QStringLiteral("remote_metronome_beat"),
                 QStringLiteral("metronome_alignment_valid"),
                 QStringLiteral("mix_deadline_slots"),
                 QStringLiteral("mix_missing_peer_frames"),
                 QStringLiteral("mix_late_after_release_frames"),
                 QStringLiteral("mix_capacity_drops"),
                 QStringLiteral("sequence_lost"),
                 QStringLiteral("sequence_duplicate"),
                 QStringLiteral("sequence_late"),
                 QStringLiteral("udp_short_packets"),
                 QStringLiteral("udp_wrong_magic"),
                 QStringLiteral("udp_wrong_version"),
                 QStringLiteral("udp_unknown_type"),
                 QStringLiteral("udp_wrong_session"),
                 QStringLiteral("udp_invalid_payload_size"),
                 QStringLiteral("udp_authentication_failed"),
                 QStringLiteral("udp_replay_rejects"),
                 QStringLiteral("udp_forward_gap_rejects"),
                 QStringLiteral("udp_sample_time_future_rejects"),
                 QStringLiteral("udp_work_budget_yields"),
                 QStringLiteral("udp_receive_batch_max")}) {
            if (!peer.csv.hasColumn(column)) {
                return die(QStringLiteral("peer %1 CSV is missing %2")
                    .arg(index + 1).arg(column));
            }
        }
        peer.periodicRows = peer.csv.rowsOfType(QStringLiteral("periodic"));
        const auto initial = latestRow(peer, 1, true);
        const std::int64_t committedRevision = mode == MetronomeMode::LeaderAudio ? 4 : 2;
        const auto final = latestRow(
            peer, committedRevision, true, kRecordingStopFrames);
        if (!initial || !final) {
            return die(QStringLiteral("peer %1 did not expose stable pre/post hard-reset epochs")
                .arg(index + 1));
        }
        peer.initialRow = *initial;
        peer.finalRow = *final;

        if (!jam2::test::readJsonObject(
                QDir(process.root).filePath(QStringLiteral("recording/recording.json")),
                peer.sidecar,
                error)) {
            return die(error);
        }
        if (peer.sidecar.value(QStringLiteral("metronome_mode")).toString() != modeText(mode)) {
            return die(QStringLiteral("peer recording sidecar has the wrong metronome model"));
        }
        for (std::size_t stem = 0; stem < kStemNames.size(); ++stem) {
            const QString path = QDir(process.root).filePath(
                QStringLiteral("recording/") + QString::fromLatin1(kStemNames[stem]));
            if (!jam2::test::readPcm16MonoWav(path, peer.stems[stem], error)) return die(error);
        }
        if (!recordingContractValid(
                peer, index, expectedRecordingFrames, error)) return die(error);
    }

    std::set<std::uint64_t> localIds;
    for (const auto& peer : evidence) localIds.insert(peer.localPeerId);
    if (localIds.size() != kPeerCount) return die(QStringLiteral("four local peer IDs are not unique"));
    const std::uint64_t creatorId = evidence.front().localPeerId;
    const std::uint64_t bpmPeerId = evidence.back().localPeerId;
    const std::uint64_t expectedFinalAuthority = mode == MetronomeMode::LeaderAudio
        ? bpmPeerId : creatorId;
    const std::int64_t expectedFinalRevision = mode == MetronomeMode::LeaderAudio ? 4 : 2;

    std::optional<std::int64_t> initialAuthorityEpoch;
    std::optional<std::int64_t> finalAuthorityEpoch;
    for (int index = 0; index < kPeerCount; ++index) {
        const PeerEvidence& peer = evidence[static_cast<std::size_t>(index)];
        const qsizetype initial = peer.initialRow;
        const qsizetype final = peer.finalRow;
        const std::uint64_t initialAuthority = static_cast<std::uint64_t>(
            peer.csv.integer(initial, QStringLiteral("grid_authority_peer_id"), 0));
        const std::uint64_t finalAuthority = static_cast<std::uint64_t>(
            peer.csv.integer(final, QStringLiteral("grid_authority_peer_id"), 0));
        const std::int64_t initialEpoch = peer.csv.integer(
            initial, QStringLiteral("grid_authority_epoch_frame"), -1);
        const std::int64_t finalEpoch = peer.csv.integer(
            final, QStringLiteral("grid_authority_epoch_frame"), -1);
        const std::int64_t finalRevision = peer.csv.integer(
            final, QStringLiteral("grid_revision"), -1);
        const std::int64_t gridMode = peer.csv.integer(
            final, QStringLiteral("grid_mode"), -1);
        const bool aligned = peer.csv.yes(
            final, QStringLiteral("metronome_alignment_valid"));
        const std::int64_t beatDelta = std::llabs(
            peer.csv.integer(final, QStringLiteral("local_metronome_beat"), -100) -
            peer.csv.integer(final, QStringLiteral("remote_metronome_beat"), 100));
        const std::int64_t mappedEpoch = peer.csv.integer(
            final, QStringLiteral("grid_mapped_epoch_frame"), 0);
        const double mappingError = peer.csv.number(
            final, QStringLiteral("grid_mapping_error_frames"), 9999.0);
        const double sentPackets = peer.csv.number(
            final, QStringLiteral("sent_packets"));
        const double receivedPackets = peer.csv.number(
            final, QStringLiteral("recv_packets"));
        const double callbacks = peer.csv.number(
            final, QStringLiteral("audio_callbacks"));
        const double mixCapacityDrops = peer.csv.number(
            final, QStringLiteral("mix_capacity_drops"), -1.0);
        const double mixCapacityDroppedFrames = peer.csv.number(
            final, QStringLiteral("mix_capacity_dropped_frames"), -1.0);
        const double mixOutputFrames = peer.csv.number(
            final, QStringLiteral("mix_output_frames"), -1.0);
        constexpr double kMaximumImpairedMixDropRatio = 0.05;
        const double maximumMixCapacityDrops =
            condition == NetworkCondition::Clean
            ? 0.0
            : mixOutputFrames * kMaximumImpairedMixDropRatio;
        const bool mixCapacityHealthy =
            mixCapacityDrops >= 0.0 &&
            mixCapacityDroppedFrames == mixCapacityDrops &&
            mixOutputFrames > 0.0 &&
            mixCapacityDrops <= maximumMixCapacityDrops;
        const double authenticationFailures = peer.csv.number(
            final, QStringLiteral("udp_authentication_failed"));
        if (initialAuthority != creatorId || finalAuthority != expectedFinalAuthority ||
            finalRevision != expectedFinalRevision || gridMode != modeId(mode) ||
            !aligned || beatDelta > 1 ||
            finalEpoch == initialEpoch ||
            mappedEpoch <= 0 || std::abs(mappingError) > 1024.0 ||
            sentPackets <= 0.0 || receivedPackets <= 0.0 || callbacks <= 0.0 ||
            !mixCapacityHealthy ||
            (condition != NetworkCondition::Security &&
             authenticationFailures != 0.0)) {
            return die(QStringLiteral(
                "peer %1 failed replacement-epoch/health proof: "
                "authority=%2/%3 expected=%4/%5 revision=%6/%7 mode=%8/%9 "
                "aligned=%10 beat_delta=%11 epochs=%12->%13 mapped=%14 "
                "mapping_error=%15 sent=%16 recv=%17 callbacks=%18 "
                "mix_capacity_drops=%19/%20 output_frames=%21 auth_failures=%22")
                .arg(index + 1)
                .arg(initialAuthority).arg(finalAuthority)
                .arg(creatorId).arg(expectedFinalAuthority)
                .arg(finalRevision).arg(expectedFinalRevision)
                .arg(gridMode).arg(modeId(mode))
                .arg(aligned).arg(beatDelta)
                .arg(initialEpoch).arg(finalEpoch).arg(mappedEpoch)
                .arg(mappingError).arg(sentPackets).arg(receivedPackets)
                .arg(callbacks).arg(mixCapacityDrops).arg(maximumMixCapacityDrops)
                .arg(mixOutputFrames).arg(authenticationFailures));
        }
        if (!initialAuthorityEpoch) initialAuthorityEpoch = initialEpoch;
        if (!finalAuthorityEpoch) finalAuthorityEpoch = finalEpoch;
        if (*initialAuthorityEpoch != initialEpoch || *finalAuthorityEpoch != finalEpoch) {
            return die(QStringLiteral("four peers did not agree on both authority epochs"));
        }
        std::int64_t previousRevision = 0;
        for (const qsizetype row : peer.periodicRows) {
            const std::int64_t revision = peer.csv.integer(
                row, QStringLiteral("grid_revision"), previousRevision);
            if (revision < previousRevision) {
                return die(QStringLiteral("grid revision regressed on peer %1").arg(index + 1));
            }
            previousRevision = revision;
        }
    }
    if (!initialAuthorityEpoch || !finalAuthorityEpoch ||
        *initialAuthorityEpoch == *finalAuthorityEpoch) {
        return die(QStringLiteral("BPM hard reset did not replace the shared epoch"));
    }
    if (evidence.back().csv.number(
            evidence.back().finalRow, QStringLiteral("grid_proposals_sent")) <= 0.0) {
        return die(QStringLiteral("joiner BPM edit did not traverse the grid proposal path"));
    }
    double authorityStatesSent = 0.0;
    double authorityStatesAccepted = 0.0;
    for (const auto& peer : evidence) {
        authorityStatesSent += peer.csv.number(
            peer.finalRow, QStringLiteral("grid_authority_states_sent"));
        authorityStatesAccepted += peer.csv.number(
            peer.finalRow, QStringLiteral("grid_authority_states_accepted"));
    }
    if (authorityStatesSent <= 0.0 || authorityStatesAccepted <= 0.0) {
        return die(QStringLiteral("authority state send/accept counters do not prove epoch distribution"));
    }

    const auto sumFinal = [&](const QString& column) {
        double total = 0.0;
        for (const auto& peer : evidence) total += peer.csv.number(peer.finalRow, column);
        return total;
    };
    const auto maxFinal = [&](const QString& column) {
        double maximum = 0.0;
        for (const auto& peer : evidence) {
            maximum = std::max(maximum, peer.csv.number(peer.finalRow, column));
        }
        return maximum;
    };
    if (condition == NetworkCondition::Security) {
        const double shortPackets = sumFinal(QStringLiteral("udp_short_packets"));
        const double wrongMagic = sumFinal(QStringLiteral("udp_wrong_magic"));
        const double wrongVersion = sumFinal(QStringLiteral("udp_wrong_version"));
        const double unknownType = sumFinal(QStringLiteral("udp_unknown_type"));
        const double wrongSession = sumFinal(QStringLiteral("udp_wrong_session"));
        const double invalidPayload = sumFinal(QStringLiteral("udp_invalid_payload_size"));
        const double authenticationFailed = sumFinal(QStringLiteral("udp_authentication_failed"));
        const double replayRejected =
            sumFinal(QStringLiteral("sequence_duplicate")) +
            sumFinal(QStringLiteral("sequence_late"));
        const double budgetYields = sumFinal(QStringLiteral("udp_work_budget_yields"));
        const double receiveBatchMaximum = maxFinal(QStringLiteral("udp_receive_batch_max"));
        if (shortPackets < static_cast<double>(injectionEvidence.floodInjected + 2) ||
            wrongMagic < 2.0 || wrongVersion < 2.0 ||
            unknownType < 2.0 || wrongSession < 2.0 || invalidPayload < 2.0 ||
            authenticationFailed < 2.0 || replayRejected < 2.0 ||
            receiveBatchMaximum <= 0.0 || receiveBatchMaximum > 64.0) {
            return die(QStringLiteral(
                "UDP security counters failed: short=%1 magic=%2 version=%3 type=%4 "
                "session=%5 payload=%6 auth=%7 replay=%8 yields=%9 batch_max=%10")
                .arg(shortPackets).arg(wrongMagic).arg(wrongVersion).arg(unknownType)
                .arg(wrongSession).arg(invalidPayload).arg(authenticationFailed)
                .arg(replayRejected).arg(budgetYields).arg(receiveBatchMaximum));
        }
    }
    if (condition == NetworkCondition::SequenceSecurity) {
        const auto& transformed = sequenceTransformer->stats();
        const bool transformedBothDirections = transformed.wrapped[0] && transformed.wrapped[1] &&
            transformed.forwardGapInjected[0] && transformed.forwardGapInjected[1] &&
            transformed.extremeSampleTimeInjected[0] &&
            transformed.extremeSampleTimeInjected[1] &&
            transformed.audioPackets[0] > 1000 && transformed.audioPackets[1] > 1000;
        const double lost = sumFinal(QStringLiteral("sequence_lost"));
        const double late = sumFinal(QStringLiteral("sequence_late"));
        const double forwardGap = sumFinal(QStringLiteral("udp_forward_gap_rejects"));
        const double futureSample = sumFinal(QStringLiteral("udp_sample_time_future_rejects"));
        const double authenticationFailed = sumFinal(QStringLiteral("udp_authentication_failed"));
        // Each extreme timestamp is an authenticated audio frame rejected at
        // the expected sequence, so PeerStream deliberately records exactly
        // one lost frame and the reused number as late. Any additional loss
        // would be attributable to wrap or forward-gap recovery.
        if (!transformedBothDirections || lost != 2.0 || late != 2.0 || forwardGap < 2.0 ||
            futureSample < 2.0 || authenticationFailed != 0.0) {
            return die(QStringLiteral(
                "UDP signed sequence boundary failed: packets=%1/%2 wrapped=%3/%4 "
                "gap_injected=%5/%6 sample_injected=%7/%8 lost=%9 gap_rejects=%10 "
                "future_rejects=%11 auth_failures=%12 late=%13")
                .arg(transformed.audioPackets[0]).arg(transformed.audioPackets[1])
                .arg(transformed.wrapped[0]).arg(transformed.wrapped[1])
                .arg(transformed.forwardGapInjected[0]).arg(transformed.forwardGapInjected[1])
                .arg(transformed.extremeSampleTimeInjected[0])
                .arg(transformed.extremeSampleTimeInjected[1])
                .arg(lost).arg(forwardGap).arg(futureSample).arg(authenticationFailed)
                .arg(late));
        }
    }

    if (mode == MetronomeMode::LeaderAudio) {
        int growingInjectors = 0;
        std::uint64_t growingSource = 0;
        for (const PeerEvidence& peer : evidence) {
            const auto stable = earliestRow(peer, expectedFinalRevision, true);
            if (!stable) {
                return die(QStringLiteral(
                    "leader-audio handoff never exposed an active final-revision sample"));
            }
            const double initial = peer.csv.number(
                *stable, QStringLiteral("leader_audio_injected_packets"));
            const double final = peer.csv.number(
                peer.finalRow, QStringLiteral("leader_audio_injected_packets"));
            if (final > initial) {
                ++growingInjectors;
                growingSource = static_cast<std::uint64_t>(peer.csv.integer(
                    peer.finalRow, QStringLiteral("leader_audio_source_peer_id"), 0));
            }
        }
        if (growingInjectors != 1 || growingSource != expectedFinalAuthority) {
            return die(QStringLiteral("leader-audio did not transfer to exactly one replacement-epoch authority"));
        }
        if (!leaderAudioValid(evidence, expectedFinalAuthority, error)) return die(error);
    } else if (mode == MetronomeMode::SharedGrid) {
        for (int index = 0; index < kPeerCount; ++index) {
            if (!sharedAudioValid(evidence[static_cast<std::size_t>(index)], index, error)) {
                return die(error);
            }
        }
    } else {
        for (int index = 0; index < kPeerCount; ++index) {
            if (!listenerAudioValid(evidence[static_cast<std::size_t>(index)], index, error)) {
                return die(error);
            }
        }
    }

    std::cout << "PASS four-peer metronome mode=" << modeText(mode).toStdString()
              << " condition=" << conditionText(condition).toStdString()
              << " initial_epoch=" << *initialAuthorityEpoch
              << " replacement_epoch=" << *finalAuthorityEpoch
              << " authority=" << expectedFinalAuthority
              << '\n';
    return AttemptResult{AttemptOutcome::Success, {}};
}

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    if (argc != 4) {
        std::cerr << "usage: jam2_four_metronome_impairment <jam2> <mode> <condition>\n";
        return 2;
    }
    const QString executable = QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath();
    const auto mode = parseMode(QString::fromLocal8Bit(argv[2]));
    const auto condition = parseCondition(QString::fromLocal8Bit(argv[3]));
    if (!QFileInfo::exists(executable) || !mode || !condition) {
        std::cerr << "Jam2 executable, metronome mode, or impairment condition is invalid\n";
        return 2;
    }
    QTemporaryDir temporary(
        QDir::temp().filePath(QStringLiteral("jam2-metronome-%1-%2-XXXXXX")
            .arg(modeText(*mode), conditionText(*condition))));
    if (!temporary.isValid()) {
        std::cerr << "could not create metronome test root\n";
        return 1;
    }
    auto fail = [&](const QString& reason) {
        temporary.setAutoRemove(false);
        std::cerr << reason.toStdString() << "\nartifacts retained at "
                  << temporary.path().toStdString() << '\n';
        return 1;
    };

    QString error;
    LoopbackPortReservations reservations;
    if (!reservations.reserve(kPeerCount, error)) return fail(error);
    std::array<quint16, kPeerCount> ports{};
    for (int index = 0; index < kPeerCount; ++index) {
        ports[static_cast<std::size_t>(index)] = reservations.port(
            static_cast<std::size_t>(index));
    }

    const QString sessionId = deterministicHex(7, 8);
    const QString sessionKey = deterministicHex(11, 16);
    std::array<QString, kPeerCount> tokens;
    for (int index = 0; index < kPeerCount; ++index) {
        tokens[static_cast<std::size_t>(index)] = deterministicHex(31 + index, 16);
    }

    const DirectionImpairment impairment = impairmentFor(*condition);

    const AttemptResult result = runAttempt(
        executable, *mode, *condition, ports, reservations, sessionId,
        sessionKey, tokens, impairment, temporary.path());
    if (result.outcome == AttemptOutcome::Success) return 0;
    temporary.setAutoRemove(false);
    std::cerr << result.message.toStdString() << "\nartifacts retained at "
              << temporary.path().toStdString() << '\n';
    return 1;
}

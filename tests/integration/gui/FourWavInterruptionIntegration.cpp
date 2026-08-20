#include "FourPeerCoordinator.hpp"
#include "LoopbackPortReservations.hpp"
#include "TestTiming.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QThread>

#include <array>
#include <chrono>
#include <iostream>
#include <optional>

namespace {

using namespace std::chrono_literals;

int failures = 0;

void fail(const QString& message)
{
    std::cerr << "FAIL: " << message.toStdString() << '\n';
    ++failures;
}

void appendU16(QByteArray& bytes, quint16 value)
{
    bytes.append(static_cast<char>(value & 0xffU));
    bytes.append(static_cast<char>((value >> 8U) & 0xffU));
}

void appendU32(QByteArray& bytes, quint32 value)
{
    for (int shift = 0; shift < 32; shift += 8) {
        bytes.append(static_cast<char>((value >> shift) & 0xffU));
    }
}

QByteArray pcm16Wav(int variant)
{
    constexpr quint32 sampleRate = 48000;
    constexpr quint32 frames = 400000;
    constexpr quint32 dataBytes = frames * 2;
    QByteArray bytes;
    bytes.reserve(static_cast<qsizetype>(44 + dataBytes));
    bytes.append("RIFF", 4);
    appendU32(bytes, 36 + dataBytes);
    bytes.append("WAVEfmt ", 8);
    appendU32(bytes, 16);
    appendU16(bytes, 1);
    appendU16(bytes, 1);
    appendU32(bytes, sampleRate);
    appendU32(bytes, sampleRate * 2);
    appendU16(bytes, 2);
    appendU16(bytes, 16);
    bytes.append("data", 4);
    appendU32(bytes, dataBytes);
    for (quint32 frame = 0; frame < frames; ++frame) {
        const quint32 phase = (frame * static_cast<quint32>(variant * 2 + 3)) % 211U;
        const qint16 sample = static_cast<qint16>(
            (static_cast<int>(phase) - 105) * (90 + variant * 7));
        appendU16(bytes, static_cast<quint16>(sample));
    }
    return bytes;
}

bool writeFixture(
    const QString& folder,
    const QString& name,
    int variant,
    QString& path,
    QString& hash)
{
    const QByteArray bytes = pcm16Wav(variant);
    path = QDir(folder).absoluteFilePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(bytes) != bytes.size() || !file.flush()) {
        fail(QStringLiteral("could not write interruption WAV fixture ") + path);
        return false;
    }
    file.close();
    hash = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    return true;
}

bool send(AutomationProcess& process, QJsonObject command)
{
    QString error;
    if (process.send(std::move(command), error)) return true;
    fail(QStringLiteral("sending automation command: ") + error);
    return false;
}

std::optional<QJsonObject> receive(
    AutomationProcess& process,
    const QString& expected,
    std::chrono::milliseconds timeout = 60s)
{
    QJsonObject event;
    QString error;
    if (!process.readEvent(event, timeout, error)) {
        fail(QStringLiteral("reading %1: %2").arg(expected, error));
        return std::nullopt;
    }
    if (event.value(QStringLiteral("event")).toString() != expected) {
        fail(QStringLiteral("expected %1, received %2 (%3)").arg(
            expected,
            event.value(QStringLiteral("event")).toString(),
            event.value(QStringLiteral("reason")).toString()));
        return std::nullopt;
    }
    return event;
}

bool apply(
    FourPeerCoordinator& coordinator,
    std::size_t peer,
    QJsonObject command)
{
    return send(coordinator.peer(peer), std::move(command)) &&
        receive(coordinator.peer(peer), QStringLiteral("command_applied")).has_value();
}

std::optional<QJsonObject> snapshot(
    AutomationProcess& process,
    int peer,
    int sequence)
{
    if (!send(process, {
            {QStringLiteral("type"), QStringLiteral("snapshot")},
            {QStringLiteral("id"), QStringLiteral("interrupt-%1-%2").arg(peer).arg(sequence)},
            {QStringLiteral("cursor"), 0},
        })) return std::nullopt;
    return receive(process, QStringLiteral("snapshot"));
}

std::optional<QJsonObject> controlState(
    AutomationProcess& process,
    const QString& controlId,
    const QString& requestPrefix)
{
    int cursor = 0;
    for (;;) {
        const QString requestId = requestPrefix + QLatin1Char('-') + QString::number(cursor);
        if (!send(process, {
                {QStringLiteral("type"), QStringLiteral("snapshot")},
                {QStringLiteral("id"), requestId},
                {QStringLiteral("cursor"), cursor},
            })) return std::nullopt;
        const auto event = receive(process, QStringLiteral("snapshot"));
        if (!event || event->value(QStringLiteral("id")).toString() != requestId) {
            fail(QStringLiteral("WAV interruption control snapshot response id mismatch"));
            return std::nullopt;
        }
        for (const QJsonValue& value : event->value(QStringLiteral("controls")).toArray()) {
            const QJsonObject control = value.toObject();
            if (control.value(QStringLiteral("test_id")).toString() == controlId) {
                return control.value(QStringLiteral("state")).toObject();
            }
        }
        cursor = event->value(QStringLiteral("next_cursor")).toInt(-1);
        if (cursor < 0) break;
    }
    fail(QStringLiteral("WAV interruption view omitted control ") + controlId);
    return std::nullopt;
}

void printState(std::size_t peer, const QJsonObject& state)
{
    const QJsonObject jam = state.value(QStringLiteral("jam")).toObject();
    const QJsonObject content = state.value(QStringLiteral("content")).toObject();
    const QJsonObject transfer = content.value(QStringLiteral("transfer")).toObject();
    std::cerr << "  peer-" << peer + 1
              << " role=" << jam.value(QStringLiteral("role")).toString().toStdString()
              << " remotes=" << jam.value(QStringLiteral("remote_peer_count")).toInt()
              << " digest=" << content.value(QStringLiteral("model_sha256")).toString().toStdString()
              << " lane_count=" << content.value(
                    QStringLiteral("lane_count")).toInt()
              << " lane_ids=" << QJsonDocument(content.value(
                    QStringLiteral("lane_ids")).toArray()).toJson(
                        QJsonDocument::Compact).toStdString()
              << " lane_names=" << QJsonDocument(content.value(
                    QStringLiteral("lane_names")).toArray()).toJson(
                        QJsonDocument::Compact).toStdString()
              << " assets=" << QJsonDocument(content.value(
                    QStringLiteral("asset_hashes")).toArray()).toJson(
                        QJsonDocument::Compact).toStdString()
              << " available=" << QJsonDocument(content.value(
                    QStringLiteral("asset_available")).toArray()).toJson(
                        QJsonDocument::Compact).toStdString()
              << " pending_assets=" << content.value(
                    QStringLiteral("pending_asset_count")).toInt()
              << " pending_contributions=" << content.value(
                    QStringLiteral("pending_track_contribution_count")).toInt()
              << " outgoing_batches=" << content.value(
                    QStringLiteral("outgoing_track_batch_count")).toInt()
              << " outgoing_detail=" << QJsonDocument(content.value(
                    QStringLiteral("outgoing_track_batches")).toArray()).toJson(
                        QJsonDocument::Compact).toStdString()
              << " retries=" << content.value(
                    QStringLiteral("incoming_asset_retry_count")).toInt()
              << " retry_sources=" << content.value(
                    QStringLiteral("incoming_asset_retry_source_count")).toInt()
              << " start_timeouts=" << content.value(
                    QStringLiteral("asset_request_start_timeouts")).toInteger()
              << " file_tasks=" << content.value(
                    QStringLiteral("file_tasks_active")).toInt()
              << " pending_detail=" << QJsonDocument(content.value(
                    QStringLiteral("pending_track_contributions")).toArray()).toJson(
                        QJsonDocument::Compact).toStdString()
              << " pending_sources=" << QJsonDocument(content.value(
                    QStringLiteral("pending_track_asset_sources")).toObject()).toJson(
                        QJsonDocument::Compact).toStdString()
              << " validated=" << QJsonDocument(content.value(
                    QStringLiteral("validated_track_asset_hashes")).toArray()).toJson(
                        QJsonDocument::Compact).toStdString()
              << " incoming=" << content.value(
                    QStringLiteral("incoming_asset_hash")).toString().toStdString()
              << '@' << content.value(
                    QStringLiteral("incoming_asset_source")).toString().toStdString()
              << " transfer=" << QJsonDocument(transfer).toJson(
                    QJsonDocument::Compact).toStdString()
              << '\n';
}

void printRelevantLog(const FourPeerCoordinator& coordinator, std::size_t peer)
{
    QDirIterator logFiles(
        coordinator.storageRoot(peer),
        {QStringLiteral("*.log")},
        QDir::Files,
        QDirIterator::Subdirectories);
    QString newestLog;
    QDateTime newestLogTime;
    while (logFiles.hasNext()) {
        const QString candidate = logFiles.next();
        const QDateTime modified = QFileInfo(candidate).lastModified();
        if (newestLog.isEmpty() || modified > newestLogTime) {
            newestLog = candidate;
            newestLogTime = modified;
        }
    }
    if (newestLog.isEmpty()) return;

    QFile file(newestLog);
    if (!file.open(QIODevice::ReadOnly)) return;
    const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    QStringList relevant;
    for (const QString& line : lines) {
        if (line.contains(QStringLiteral("asset"), Qt::CaseInsensitive) ||
            line.contains(QStringLiteral("Track Sync"), Qt::CaseInsensitive) ||
            line.contains(QStringLiteral("WAV"), Qt::CaseInsensitive)) {
            relevant.append(line);
        }
    }
    const qsizetype start = qMax<qsizetype>(0, relevant.size() - 40);
    for (qsizetype line = start; line < relevant.size(); ++line) {
        std::cerr << "    log: " << relevant.at(line).toStdString() << '\n';
    }
}

template <typename Predicate>
bool waitForAll(
    FourPeerCoordinator& coordinator,
    const QString& description,
    Predicate predicate,
    std::array<QJsonObject, FourPeerCoordinator::kPeerCount>& states,
    bool requireSameDigest,
    std::chrono::milliseconds timeout = 90s)
{
    const auto deadline = std::chrono::steady_clock::now() +
        jam2::test::deadmanTimeout(timeout);
    int sequence = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        bool ready = true;
        for (std::size_t peer = 0; peer < states.size(); ++peer) {
            const auto event = snapshot(
                coordinator.peer(peer), static_cast<int>(peer + 1), sequence);
            if (!event) return false;
            states[peer] = *event;
        }
        const QString digest = states[0].value(QStringLiteral("content")).toObject()
            .value(QStringLiteral("model_sha256")).toString();
        for (std::size_t peer = 0; peer < states.size(); ++peer) {
            const QString peerDigest = states[peer]
                .value(QStringLiteral("content")).toObject()
                .value(QStringLiteral("model_sha256")).toString();
            ready = predicate(peer, states[peer]) && ready;
            if (requireSameDigest) {
                ready = !digest.isEmpty() && peerDigest == digest && ready;
            }
        }
        if (ready) return true;
        ++sequence;
    }
    fail(QStringLiteral("timed out waiting for ") + description);
    for (std::size_t peer = 0; peer < states.size(); ++peer) {
        printState(peer, states[peer]);
        printRelevantLog(coordinator, peer);
    }
    return false;
}

template <typename Predicate>
bool waitForPeer(
    FourPeerCoordinator& coordinator,
    std::size_t peer,
    const QString& description,
    Predicate predicate,
    QJsonObject& state,
    std::chrono::milliseconds timeout = 60s)
{
    const auto deadline = std::chrono::steady_clock::now() +
        jam2::test::deadmanTimeout(timeout);
    int sequence = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto event = snapshot(
            coordinator.peer(peer), static_cast<int>(peer + 1), sequence++);
        if (!event) return false;
        state = *event;
        if (predicate(state)) return true;
    }
    fail(QStringLiteral("timed out waiting for ") + description);
    printState(peer, state);
    return false;
}

bool meshReady(std::size_t peer, const QJsonObject& state)
{
    const QJsonObject jam = state.value(QStringLiteral("jam")).toObject();
    return jam.value(QStringLiteral("role")).toString() ==
            (peer == 0 ? QStringLiteral("creator") : QStringLiteral("joiner")) &&
        jam.value(QStringLiteral("remote_peer_count")).toInt() == 3 &&
        jam.value(QStringLiteral("active_remote_peer_count")).toInt() == 3 &&
        jam.value(QStringLiteral("network_attachment_ready")).toBool() &&
        jam.value(QStringLiteral("network_running")).toBool() &&
        jam.value(QStringLiteral("failure")).toString().isEmpty();
}

int assetCount(const QJsonObject& state, const QString& hash)
{
    int count = 0;
    const QJsonArray hashes = state.value(QStringLiteral("content")).toObject()
        .value(QStringLiteral("asset_hashes")).toArray();
    for (const QJsonValue& value : hashes) {
        if (value.toString() == hash) ++count;
    }
    return count;
}

int activeBankLaneCount(const QJsonObject& state)
{
    const QJsonObject content = state.value(QStringLiteral("content")).toObject();
    const int activeBank = content.value(QStringLiteral("active_bank")).toInt(-1);
    const QJsonArray laneBanks = content.value(QStringLiteral("lane_banks")).toArray();
    QStringList bankOrder;
    for (const QJsonValue& value : laneBanks) {
        const QString bank = value.toString();
        if (!bank.isEmpty() && !bankOrder.contains(bank)) bankOrder.append(bank);
    }
    if (activeBank < 0 || activeBank >= bankOrder.size()) return -1;
    int count = 0;
    for (const QJsonValue& value : laneBanks) {
        if (value.toString() == bankOrder.at(activeBank)) ++count;
    }
    return count;
}

int assetIndex(const QJsonObject& state, const QString& hash)
{
    const QJsonArray hashes = state.value(QStringLiteral("content")).toObject()
        .value(QStringLiteral("asset_hashes")).toArray();
    for (int index = 0; index < hashes.size(); ++index) {
        if (hashes.at(index).toString() == hash) return index;
    }
    return -1;
}

int activeBankAssetLaneIndex(const QJsonObject& state, const QString& hash)
{
    const QJsonObject content = state.value(QStringLiteral("content")).toObject();
    const int activeBank = content.value(QStringLiteral("active_bank")).toInt(-1);
    const QJsonArray laneBanks = content.value(QStringLiteral("lane_banks")).toArray();
    const QJsonArray hashes = content.value(QStringLiteral("asset_hashes")).toArray();
    QStringList bankOrder;
    for (const QJsonValue& value : laneBanks) {
        const QString bank = value.toString();
        if (!bank.isEmpty() && !bankOrder.contains(bank)) bankOrder.append(bank);
    }
    if (activeBank < 0 || activeBank >= bankOrder.size() ||
        hashes.size() != laneBanks.size()) return -1;

    const QString activeBankId = bankOrder.at(activeBank);
    int localLane = 0;
    for (int index = 0; index < hashes.size(); ++index) {
        if (laneBanks.at(index).toString() != activeBankId) continue;
        if (hashes.at(index).toString() == hash) return localLane;
        ++localLane;
    }
    return -1;
}

bool assetAvailable(const QJsonObject& state, const QString& hash)
{
    const int index = assetIndex(state, hash);
    const QJsonArray available = state.value(QStringLiteral("content")).toObject()
        .value(QStringLiteral("asset_available")).toArray();
    return index >= 0 && index < available.size() && available.at(index).toBool();
}

int pendingSourcesForHash(const QJsonObject& state, const QString& hash)
{
    QSet<QString> sources;
    const QJsonArray contributions = state.value(QStringLiteral("content")).toObject()
        .value(QStringLiteral("pending_track_contributions")).toArray();
    for (const QJsonValue& value : contributions) {
        const QJsonObject contribution = value.toObject();
        if (contribution.value(QStringLiteral("hash")).toString() == hash) {
            sources.insert(contribution.value(QStringLiteral("source")).toString());
        }
    }
    return sources.size();
}

QJsonObject wavPolicy(QString id, bool automatic)
{
    return {
        {QStringLiteral("type"), QStringLiteral("jam.policy")},
        {QStringLiteral("id"), std::move(id)},
        {QStringLiteral("track_lanes"), true},
        {QStringLiteral("auto_share_wavs"), automatic},
        {QStringLiteral("global_playback"), true},
        {QStringLiteral("generated_ideas"), QStringLiteral("full")},
        {QStringLiteral("metronome_state"), false},
        {QStringLiteral("recordings"), true},
    };
}

bool transferIdle(const QJsonObject& state)
{
    const QJsonObject content = state.value(QStringLiteral("content")).toObject();
    const QJsonObject transfer = content.value(QStringLiteral("transfer")).toObject();
    return content.value(QStringLiteral("pending_asset_count")).toInt() == 0 &&
        content.value(QStringLiteral("pending_track_contribution_count")).toInt() == 0 &&
        content.value(QStringLiteral("outgoing_track_batch_count")).toInt() == 0 &&
        !content.value(QStringLiteral("incoming_asset_active")).toBool() &&
        content.value(QStringLiteral("incoming_asset_retry_count")).toInt() == 0 &&
        content.value(QStringLiteral("incoming_asset_retry_source_count")).toInt() == 0 &&
        content.value(QStringLiteral("file_tasks_active")).toInt() == 0 &&
        transfer.value(QStringLiteral("pause_armed")).toString().isEmpty() &&
        transfer.value(QStringLiteral("pause_active")).toString().isEmpty() &&
        !transfer.value(QStringLiteral("drop_outgoing_start_armed")).toBool() &&
        !transfer.value(QStringLiteral("outgoing_validation_pending")).toBool() &&
        transfer.value(QStringLiteral("outgoing_hash")).toString().isEmpty() &&
        transfer.value(QStringLiteral("outgoing_queue_count")).toInt() == 0 &&
        transfer.value(QStringLiteral("incoming_hash")).toString().isEmpty() &&
        transfer.value(QStringLiteral("incoming_queued_chunks")).toInt() == 0 &&
        !transfer.value(QStringLiteral("incoming_write_pending")).toBool() &&
        !transfer.value(QStringLiteral("incoming_done_pending")).toBool();
}

bool verifyNoPartials(
    const FourPeerCoordinator& coordinator,
    std::optional<std::size_t> onlyPeer = std::nullopt)
{
    for (std::size_t peer = 0; peer < FourPeerCoordinator::kPeerCount; ++peer) {
        if (onlyPeer && peer != *onlyPeer) continue;
        QDirIterator files(
            coordinator.storageRoot(peer),
            {QStringLiteral("*.partial.*")},
            QDir::Files,
            QDirIterator::Subdirectories);
        if (files.hasNext()) {
            fail(QStringLiteral("peer %1 retained partial WAV %2")
                .arg(peer + 1).arg(files.next()));
            return false;
        }
    }
    return true;
}

bool verifyExactFiles(
    const FourPeerCoordinator& coordinator,
    const std::array<QJsonObject, FourPeerCoordinator::kPeerCount>& states,
    const QStringList& hashes)
{
    for (const QString& hash : hashes) {
        QSet<QString> paths;
        for (std::size_t peer = 0; peer < states.size(); ++peer) {
            const QJsonObject content = states[peer]
                .value(QStringLiteral("content")).toObject();
            const int index = assetIndex(states[peer], hash);
            const QJsonArray lanePaths = content.value(
                QStringLiteral("lane_paths")).toArray();
            if (assetCount(states[peer], hash) != 1 ||
                index < 0 || index >= lanePaths.size()) {
                fail(QStringLiteral("peer %1 did not expose exactly one path for %2")
                    .arg(peer + 1).arg(hash.left(12)));
                return false;
            }
            const QString path = QDir::cleanPath(
                QFileInfo(lanePaths.at(index).toString()).canonicalFilePath());
            const QString root = QDir::cleanPath(
                QFileInfo(coordinator.storageRoot(peer)).canonicalFilePath());
            if (path.isEmpty() || root.isEmpty() ||
                !(path.compare(root, Qt::CaseInsensitive) == 0 ||
                  path.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive))) {
                fail(QStringLiteral("peer %1 WAV escaped isolation: path=%2 root=%3")
                    .arg(peer + 1).arg(path, root));
                return false;
            }
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly) ||
                QString::fromLatin1(QCryptographicHash::hash(
                    file.readAll(), QCryptographicHash::Sha256).toHex()) != hash) {
                fail(QStringLiteral("peer %1 stored incorrect bytes for %2")
                    .arg(peer + 1).arg(hash.left(12)));
                return false;
            }
            paths.insert(path.toLower());
        }
        if (paths.size() != static_cast<qsizetype>(states.size())) {
            fail(QStringLiteral("hash %1 reused storage across peers").arg(hash.left(12)));
            return false;
        }
    }
    return true;
}

bool leaveAndRejoin(
    FourPeerCoordinator& coordinator,
    std::size_t peer,
    const QString& invite,
    quint16 port,
    std::array<QJsonObject, FourPeerCoordinator::kPeerCount>& states,
    const QString& label)
{
    if (!apply(coordinator, peer, {
            {QStringLiteral("type"), QStringLiteral("jam.leave")},
            {QStringLiteral("id"), label + QStringLiteral("-leave")},
        })) return false;
    if (!waitForAll(coordinator, label + QStringLiteral(" disconnect cleanup"),
            [peer](std::size_t index, const QJsonObject& state) {
                const QJsonObject jam = state.value(QStringLiteral("jam")).toObject();
                if (index == peer) {
                    return jam.value(QStringLiteral("role")).toString() ==
                            QStringLiteral("inactive") &&
                        !jam.value(QStringLiteral("network_running")).toBool() &&
                        transferIdle(state);
                }
                return jam.value(QStringLiteral("remote_peer_count")).toInt() == 2 &&
                    jam.value(QStringLiteral("active_remote_peer_count")).toInt() == 2;
            }, states, false, 90s) || !verifyNoPartials(coordinator, peer)) {
        return false;
    }
    if (!apply(coordinator, peer, {
            {QStringLiteral("type"), QStringLiteral("jam.join")},
            {QStringLiteral("id"), label + QStringLiteral("-rejoin")},
            {QStringLiteral("port"), port},
            {QStringLiteral("invite_url"), invite},
    })) return false;
    return waitForAll(coordinator, label + QStringLiteral(" four-peer rejoin"),
        meshReady, states, false, 90s);
}

bool shutDown(FourPeerCoordinator& coordinator)
{
    for (std::size_t peer = 0; peer < FourPeerCoordinator::kPeerCount; ++peer) {
        if (!send(coordinator.peer(peer), {
                {QStringLiteral("type"), QStringLiteral("shutdown")},
                {QStringLiteral("id"), QStringLiteral("shutdown-%1").arg(peer + 1)},
            })) return false;
    }
    for (std::size_t peer = 0; peer < FourPeerCoordinator::kPeerCount; ++peer) {
        if (!receive(coordinator.peer(peer), QStringLiteral("command_applied")) ||
            !receive(coordinator.peer(peer), QStringLiteral("shutdown"))) return false;
        int exitCode = -1;
        QString error;
        if (!coordinator.peer(peer).waitForExit(60s, exitCode, error) || exitCode != 0) {
            fail(QStringLiteral("peer %1 did not exit cleanly: %2 code=%3")
                .arg(peer + 1).arg(error).arg(exitCode));
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    if (argc != 2) {
        std::cerr << "usage: jam2_four_wav_interruption_integration <release-jam2>\n";
        return 2;
    }
    QTemporaryDir fixtureFolder;
    std::array<QString, 11> paths;
    std::array<QString, 11> hashes;
    if (!fixtureFolder.isValid() ||
        !writeFixture(fixtureFolder.path(), QStringLiteral("validation.wav"), 11,
            paths[0], hashes[0]) ||
        !writeFixture(fixtureFolder.path(), QStringLiteral("offer.wav"), 12,
            paths[1], hashes[1]) ||
        !writeFixture(fixtureFolder.path(), QStringLiteral("chunk.wav"), 13,
            paths[2], hashes[2]) ||
        !writeFixture(fixtureFolder.path(), QStringLiteral("finalize.wav"), 14,
            paths[3], hashes[3]) ||
        !writeFixture(fixtureFolder.path(), QStringLiteral("start-timeout.wav"), 15,
            paths[4], hashes[4]) ||
        !writeFixture(fixtureFolder.path(), QStringLiteral("multi-batch.wav"), 16,
            paths[5], hashes[5]) ||
        !writeFixture(fixtureFolder.path(), QStringLiteral("source-handoff.wav"), 17,
            paths[6], hashes[6]) ||
        !writeFixture(fixtureFolder.path(), QStringLiteral("remove-during-transfer.wav"), 18,
            paths[7], hashes[7]) ||
        !writeFixture(fixtureFolder.path(), QStringLiteral("remove-after-chunks.wav"), 19,
            paths[8], hashes[8]) ||
        !writeFixture(fixtureFolder.path(), QStringLiteral("replacement-old.wav"), 20,
            paths[9], hashes[9]) ||
        !writeFixture(fixtureFolder.path(), QStringLiteral("replacement-new.wav"), 21,
            paths[10], hashes[10])) {
        return 1;
    }

    LoopbackPortReservations reservations;
    QString error;
    if (!reservations.reserve(FourPeerCoordinator::kPeerCount, error)) {
        fail(QStringLiteral("could not reserve interruption ports: ") + error);
        return 1;
    }
    std::array<quint16, FourPeerCoordinator::kPeerCount> ports{};
    for (std::size_t peer = 0; peer < ports.size(); ++peer) {
        ports[peer] = reservations.port(peer);
    }

    std::array<QStringList, FourPeerCoordinator::kPeerCount> arguments;
    const bool show = qEnvironmentVariableIntValue("JAM2_TEST_SHOW_GUI") == 1;
    for (std::size_t peer = 0; peer < arguments.size(); ++peer) {
        arguments[peer] << QStringLiteral("debug") << QStringLiteral("gui-agent")
            << QStringLiteral("--instance-id")
            << QStringLiteral("wav-interrupt-peer-%1").arg(peer + 1);
        if (show) arguments[peer] << QStringLiteral("--show-gui");
    }
    FourPeerCoordinator coordinator;
    if (!coordinator.launch(QString::fromLocal8Bit(argv[1]), arguments, error)) {
        fail(QStringLiteral("launching interruption peers: ") + error);
        return 1;
    }
    for (std::size_t peer = 0; peer < arguments.size(); ++peer) {
        if (!receive(coordinator.peer(peer), QStringLiteral("hello"))) return 1;
    }
    reservations.release();

    if (!send(coordinator.peer(0), {
            {QStringLiteral("type"), QStringLiteral("jam.create")},
            {QStringLiteral("id"), QStringLiteral("create")},
            {QStringLiteral("port"), ports[0]},
        })) return 1;
    const auto created = receive(coordinator.peer(0), QStringLiteral("command_applied"));
    if (!created) return 1;
    const QString invite = created->value(QStringLiteral("invite_url")).toString();
    if (invite.isEmpty()) {
        fail(QStringLiteral("interruption creator returned no invite"));
        return 1;
    }
    for (std::size_t peer = 1; peer < arguments.size(); ++peer) {
        if (!apply(coordinator, peer, {
                {QStringLiteral("type"), QStringLiteral("jam.join")},
                {QStringLiteral("id"), QStringLiteral("join-%1").arg(peer + 1)},
                {QStringLiteral("port"), ports[peer]},
                {QStringLiteral("invite_url"), invite},
            })) return 1;
    }

    std::array<QJsonObject, FourPeerCoordinator::kPeerCount> states;
    if (!waitForAll(coordinator, QStringLiteral("initial interruption mesh"),
            meshReady, states, true, 90s)) return 1;

    struct Scenario {
        const char* label;
        const char* pause;
        std::size_t pausePeer;
        std::size_t importPeer;
        std::size_t leavePeer;
        std::size_t fixture;
    };
    const std::array scenarios{
        Scenario{"validation", "outgoing-validation", 1, 1, 1, 0},
        Scenario{"offer", "offer", 2, 0, 2, 1},
        Scenario{"chunk", "incoming-chunk", 3, 0, 3, 2},
        Scenario{"finalize", "incoming-finalize", 1, 0, 1, 3},
    };

    QStringList expectedHashes;
    for (const Scenario& scenario : scenarios) {
        const QString label = QString::fromLatin1(scenario.label);
        const QString pause = QString::fromLatin1(scenario.pause);
        if (!apply(coordinator, scenario.pausePeer, {
                {QStringLiteral("type"), QStringLiteral("looper.transfer.pause")},
                {QStringLiteral("id"), label + QStringLiteral("-arm")},
                {QStringLiteral("point"), pause},
            }) || !apply(coordinator, scenario.importPeer, {
                {QStringLiteral("type"), QStringLiteral("looper.import")},
                {QStringLiteral("id"), label + QStringLiteral("-import")},
                {QStringLiteral("lane"), -1},
                {QStringLiteral("source_path"), paths[scenario.fixture]},
            })) return 1;

        QJsonObject pausedState;
        if (!waitForPeer(coordinator, scenario.pausePeer,
                label + QStringLiteral(" transfer pause"),
                [&pause](const QJsonObject& state) {
                    const QJsonObject content = state.value(
                        QStringLiteral("content")).toObject();
                    const QJsonObject transfer = content.value(
                        QStringLiteral("transfer")).toObject();
                    if (transfer.value(QStringLiteral("pause_active")).toString() != pause) {
                        return false;
                    }
                    if (pause == QStringLiteral("offer")) {
                        return content.value(QStringLiteral(
                            "pending_track_contribution_count")).toInt() > 0;
                    }
                    if (pause == QStringLiteral("outgoing-validation")) {
                        return transfer.value(QStringLiteral(
                            "outgoing_validation_pending")).toBool();
                    }
                    if (pause == QStringLiteral("incoming-chunk")) {
                        return transfer.value(QStringLiteral(
                            "incoming_queued_chunks")).toInt() > 0;
                    }
                    return transfer.value(QStringLiteral(
                        "incoming_done_pending")).toBool();
                }, pausedState)) return 1;

        if (!leaveAndRejoin(coordinator, scenario.leavePeer, invite,
                ports[scenario.leavePeer], states, label)) return 1;
        if (!apply(coordinator, scenario.importPeer, {
                {QStringLiteral("type"), QStringLiteral("looper.share")},
                {QStringLiteral("id"), label + QStringLiteral("-recover-share")},
            })) return 1;

        expectedHashes.append(hashes[scenario.fixture]);
        if (!waitForAll(coordinator, label + QStringLiteral(" exact recovery"),
                [&expectedHashes](std::size_t, const QJsonObject& state) {
                    if (!transferIdle(state)) return false;
                    for (const QString& expected : expectedHashes) {
                        if (assetCount(state, expected) != 1) return false;
                    }
                    return true;
                }, states, true, 90s) ||
            !verifyExactFiles(coordinator, states, expectedHashes) ||
            !verifyNoPartials(coordinator)) {
            return 1;
        }
        if (label == QStringLiteral("offer") &&
            qEnvironmentVariableIsSet("JAM2_TEST_TRACE")) {
            std::cerr << "TRACE: offer recovery converged\n";
            for (std::size_t peer = 0; peer < states.size(); ++peer) {
                printState(peer, states[peer]);
                printRelevantLog(coordinator, peer);
            }
        }
    }

    const int beforeRemovalRaceLaneCount = states[2]
        .value(QStringLiteral("content")).toObject()
        .value(QStringLiteral("lane_count")).toInt();
    std::array<qint64, FourPeerCoordinator::kPeerCount>
        removalRaceStartTimeoutsBefore{};
    for (std::size_t peer = 0; peer < states.size(); ++peer) {
        removalRaceStartTimeoutsBefore[peer] = states[peer]
            .value(QStringLiteral("content")).toObject()
            .value(QStringLiteral("asset_request_start_timeouts")).toInteger();
    }
    const int removalRaceLaneIndex = activeBankLaneCount(states[2]);
    if (removalRaceLaneIndex < 0 || !apply(coordinator, 2, {
            {QStringLiteral("type"), QStringLiteral("invoke")},
            {QStringLiteral("id"), QStringLiteral("remove-race-add-lane")},
            {QStringLiteral("control"), QStringLiteral("looper.lane.add-empty")},
            {QStringLiteral("operation"), QStringLiteral("click")},
        }) || !waitForAll(coordinator, QStringLiteral("remove-race lane convergence"),
            [beforeRemovalRaceLaneCount](std::size_t, const QJsonObject& state) {
                return transferIdle(state) && state.value(QStringLiteral("content")).toObject()
                    .value(QStringLiteral("lane_count")).toInt() ==
                        beforeRemovalRaceLaneCount + 1;
            }, states, true)) {
        if (removalRaceLaneIndex < 0) {
            fail(QStringLiteral("could not resolve active-bank lane for removal race"));
        }
        return 1;
    }
    const QJsonObject beforeRemovalRace = states[0]
        .value(QStringLiteral("content")).toObject();
    const int removalRaceLaneCount = beforeRemovalRace
        .value(QStringLiteral("lane_count")).toInt();
    const QJsonArray removalRaceLaneIds = beforeRemovalRace
        .value(QStringLiteral("lane_ids")).toArray();
    const QString removalRaceDropControl =
        QStringLiteral("looper.lane.%1.wav.drop").arg(removalRaceLaneIndex);
    const QString removalRaceRemoveControl =
        QStringLiteral("looper.lane.%1.wav.remove").arg(removalRaceLaneIndex);

    if (!apply(coordinator, 2, {
            {QStringLiteral("type"), QStringLiteral("looper.transfer.pause")},
            {QStringLiteral("id"), QStringLiteral("remove-race-arm")},
            {QStringLiteral("point"), QStringLiteral("outgoing-validation")},
        }) || !apply(coordinator, 2, {
            {QStringLiteral("type"), QStringLiteral("invoke")},
            {QStringLiteral("id"), QStringLiteral("remove-race-drop")},
            {QStringLiteral("control"), removalRaceDropControl},
            {QStringLiteral("operation"), QStringLiteral("drop-file")},
            {QStringLiteral("value"), paths[7]},
        })) return 1;
    QJsonObject removalRacePause;
    if (!waitForPeer(coordinator, 2, QStringLiteral("remove-race validation pause"),
            [&hashes](const QJsonObject& state) {
                const QJsonObject content = state.value(QStringLiteral("content")).toObject();
                const QJsonObject transfer = content.value(
                    QStringLiteral("transfer")).toObject();
                return assetCount(state, hashes[7]) == 1 &&
                    transfer.value(QStringLiteral("pause_active")).toString() ==
                        QStringLiteral("outgoing-validation") &&
                    transfer.value(QStringLiteral("outgoing_validation_pending")).toBool();
            }, removalRacePause)) return 1;
    const QJsonObject attachedRemovalRace = removalRacePause
        .value(QStringLiteral("content")).toObject();
    const QJsonArray removalRaceLaneNames = attachedRemovalRace
        .value(QStringLiteral("lane_names")).toArray();
    if (attachedRemovalRace.value(QStringLiteral("lane_count")).toInt() !=
            removalRaceLaneCount ||
        attachedRemovalRace.value(QStringLiteral("lane_ids")).toArray() !=
            removalRaceLaneIds ||
        removalRaceLaneIndex >= removalRaceLaneNames.size() ||
        removalRaceLaneNames.at(removalRaceLaneIndex).toString() !=
            QStringLiteral("remove-during-transfer")) {
        fail(QStringLiteral(
            "WAV drop changed lane identity or did not apply the imported filename"));
        return 1;
    }
    const QString removalRaceExpectation = QStringLiteral(
        "remove WAV during paused transfer convergence (lanes=%1 ids=%2 names=%3)")
        .arg(removalRaceLaneCount)
        .arg(QString::fromUtf8(QJsonDocument(removalRaceLaneIds).toJson(
            QJsonDocument::Compact)))
        .arg(QString::fromUtf8(QJsonDocument(removalRaceLaneNames).toJson(
            QJsonDocument::Compact)));
    if (!apply(coordinator, 2, {
            {QStringLiteral("type"), QStringLiteral("invoke")},
            {QStringLiteral("id"), QStringLiteral("remove-race-open-confirm")},
            {QStringLiteral("control"), removalRaceRemoveControl},
            {QStringLiteral("operation"), QStringLiteral("click-async")},
        })) return 1;
    const auto removalRaceAccept = controlState(
        coordinator.peer(2),
        QStringLiteral("looper.wav-remove-dialog.accept"),
        QStringLiteral("remove-race-confirm"));
    const auto removalRaceCancel = controlState(
        coordinator.peer(2),
        QStringLiteral("looper.wav-remove-dialog.cancel"),
        QStringLiteral("remove-race-cancel"));
    if (!removalRaceAccept || !removalRaceAccept->value(
            QStringLiteral("enabled")).toBool() ||
        !removalRaceCancel || !removalRaceCancel->value(
            QStringLiteral("enabled")).toBool() ||
        !apply(coordinator, 2, {
            {QStringLiteral("type"), QStringLiteral("invoke")},
            {QStringLiteral("id"), QStringLiteral("remove-race-confirm-remove")},
            {QStringLiteral("control"), QStringLiteral("looper.wav-remove-dialog.accept")},
            {QStringLiteral("operation"), QStringLiteral("click")},
        }) || !waitForAll(coordinator,
            removalRaceExpectation,
            [&hashes, &expectedHashes, removalRaceLaneCount,
             &removalRaceLaneIds, &removalRaceLaneNames](
                std::size_t, const QJsonObject& state) {
                const QJsonObject content = state.value(QStringLiteral("content")).toObject();
                if (!transferIdle(state) || assetCount(state, hashes[7]) != 0 ||
                    content.value(QStringLiteral("lane_count")).toInt() !=
                        removalRaceLaneCount ||
                    content.value(QStringLiteral("lane_ids")).toArray() !=
                        removalRaceLaneIds ||
                    content.value(QStringLiteral("lane_names")).toArray() !=
                        removalRaceLaneNames) {
                    return false;
                }
                for (const QString& expected : expectedHashes) {
                    if (assetCount(state, expected) != 1 ||
                        !assetAvailable(state, expected)) return false;
                }
                return true;
            }, states, true, 90s) ||
        !verifyExactFiles(coordinator, states, expectedHashes) ||
        !verifyNoPartials(coordinator)) {
        return 1;
    }
    for (std::size_t peer = 0; peer < FourPeerCoordinator::kPeerCount; ++peer) {
        const qint64 timeoutsAfter = states[peer]
            .value(QStringLiteral("content")).toObject()
            .value(QStringLiteral("asset_request_start_timeouts")).toInteger();
        if (timeoutsAfter != removalRaceStartTimeoutsBefore[peer]) {
            fail(QStringLiteral(
                "peer %1 retained a stale request-start deadline after in-flight WAV removal: "
                "before=%2 after=%3")
                .arg(peer + 1)
                .arg(removalRaceStartTimeoutsBefore[peer])
                .arg(timeoutsAfter));
            return 1;
        }
        const auto view = controlState(
            coordinator.peer(peer),
            removalRaceDropControl,
            QStringLiteral("remove-race-view-%1").arg(peer + 1));
        if (!view || view->value(QStringLiteral("has_wav")).toBool()) {
            fail(QStringLiteral("peer %1 painted removed in-flight WAV as present")
                .arg(peer + 1));
            return 1;
        }
    }

    if (!apply(coordinator, 0, {
            {QStringLiteral("type"), QStringLiteral("looper.transfer.pause")},
            {QStringLiteral("id"), QStringLiteral("remove-chunks-arm")},
            {QStringLiteral("point"), QStringLiteral("incoming-finalize")},
        }) || !apply(coordinator, 2, {
            {QStringLiteral("type"), QStringLiteral("invoke")},
            {QStringLiteral("id"), QStringLiteral("remove-chunks-drop")},
            {QStringLiteral("control"), removalRaceDropControl},
            {QStringLiteral("operation"), QStringLiteral("drop-file")},
            {QStringLiteral("value"), paths[8]},
        })) return 1;
    QJsonObject removeAfterChunksPause;
    if (!waitForPeer(coordinator, 0,
            QStringLiteral("remove-after-chunks finalization pause"),
            [&hashes](const QJsonObject& state) {
                const QJsonObject transfer = state.value(QStringLiteral("content"))
                    .toObject().value(QStringLiteral("transfer")).toObject();
                return transfer.value(QStringLiteral("pause_active")).toString() ==
                        QStringLiteral("incoming-finalize") &&
                    transfer.value(QStringLiteral("incoming_done_pending")).toBool() &&
                    transfer.value(QStringLiteral("incoming_hash")).toString() == hashes[8];
            }, removeAfterChunksPause) || !apply(coordinator, 2, {
            {QStringLiteral("type"), QStringLiteral("invoke")},
            {QStringLiteral("id"), QStringLiteral("remove-chunks-open-confirm")},
            {QStringLiteral("control"), removalRaceRemoveControl},
            {QStringLiteral("operation"), QStringLiteral("click-async")},
        })) return 1;
    const QJsonObject attachedAfterChunks = removeAfterChunksPause
        .value(QStringLiteral("content")).toObject();
    const QJsonArray removeAfterChunksNames = attachedAfterChunks
        .value(QStringLiteral("lane_names")).toArray();
    if (attachedAfterChunks.value(QStringLiteral("lane_count")).toInt() !=
            removalRaceLaneCount ||
        attachedAfterChunks.value(QStringLiteral("lane_ids")).toArray() !=
            removalRaceLaneIds ||
        removeAfterChunksNames != removalRaceLaneNames) {
        fail(QStringLiteral("chunked WAV replacement changed lane identity or retained name"));
        return 1;
    }
    const auto removeChunksAccept = controlState(
        coordinator.peer(2),
        QStringLiteral("looper.wav-remove-dialog.accept"),
        QStringLiteral("remove-chunks-confirm"));
    const auto removeChunksCancel = controlState(
        coordinator.peer(2),
        QStringLiteral("looper.wav-remove-dialog.cancel"),
        QStringLiteral("remove-chunks-cancel"));
    if (!removeChunksAccept || !removeChunksAccept->value(
            QStringLiteral("enabled")).toBool() ||
        !removeChunksCancel || !removeChunksCancel->value(
            QStringLiteral("enabled")).toBool() ||
        !apply(coordinator, 2, {
            {QStringLiteral("type"), QStringLiteral("invoke")},
            {QStringLiteral("id"), QStringLiteral("remove-chunks-confirm-remove")},
            {QStringLiteral("control"), QStringLiteral("looper.wav-remove-dialog.accept")},
            {QStringLiteral("operation"), QStringLiteral("click")},
        }) || !waitForAll(coordinator,
            QStringLiteral("remove WAV after chunk writes convergence"),
            [&hashes, &expectedHashes, removalRaceLaneCount,
             &removalRaceLaneIds, &removalRaceLaneNames](
                std::size_t, const QJsonObject& state) {
                const QJsonObject content = state.value(QStringLiteral("content")).toObject();
                if (!transferIdle(state) || assetCount(state, hashes[8]) != 0 ||
                    content.value(QStringLiteral("lane_count")).toInt() !=
                        removalRaceLaneCount ||
                    content.value(QStringLiteral("lane_ids")).toArray() !=
                        removalRaceLaneIds ||
                    content.value(QStringLiteral("lane_names")).toArray() !=
                        removalRaceLaneNames) {
                    return false;
                }
                for (const QString& expected : expectedHashes) {
                    if (assetCount(state, expected) != 1 ||
                        !assetAvailable(state, expected)) return false;
                }
                return true;
            }, states, true, 90s) ||
        !verifyExactFiles(coordinator, states, expectedHashes) ||
        !verifyNoPartials(coordinator)) {
        return 1;
    }
    for (std::size_t peer = 0; peer < FourPeerCoordinator::kPeerCount; ++peer) {
        const qint64 timeoutsAfter = states[peer]
            .value(QStringLiteral("content")).toObject()
            .value(QStringLiteral("asset_request_start_timeouts")).toInteger();
        const auto view = controlState(
            coordinator.peer(peer),
            removalRaceDropControl,
            QStringLiteral("remove-chunks-view-%1").arg(peer + 1));
        if (timeoutsAfter != removalRaceStartTimeoutsBefore[peer] ||
            !view || view->value(QStringLiteral("has_wav")).toBool()) {
            fail(QStringLiteral(
                "peer %1 retained timeout or painted WAV state after chunked removal")
                .arg(peer + 1));
            return 1;
        }
    }

    if (!apply(coordinator, 2, {
            {QStringLiteral("type"), QStringLiteral("looper.transfer.pause")},
            {QStringLiteral("id"), QStringLiteral("replace-race-arm")},
            {QStringLiteral("point"), QStringLiteral("outgoing-validation")},
        }) || !apply(coordinator, 2, {
            {QStringLiteral("type"), QStringLiteral("invoke")},
            {QStringLiteral("id"), QStringLiteral("replace-race-old-drop")},
            {QStringLiteral("control"), removalRaceDropControl},
            {QStringLiteral("operation"), QStringLiteral("drop-file")},
            {QStringLiteral("value"), paths[9]},
        })) return 1;
    QJsonObject replacementPause;
    if (!waitForPeer(coordinator, 2,
            QStringLiteral("replacement old-hash validation pause"),
            [&hashes](const QJsonObject& state) {
                const QJsonObject content = state.value(QStringLiteral("content")).toObject();
                const QJsonObject transfer = content.value(
                    QStringLiteral("transfer")).toObject();
                return assetCount(state, hashes[9]) == 1 &&
                    transfer.value(QStringLiteral("pause_active")).toString() ==
                        QStringLiteral("outgoing-validation") &&
                    transfer.value(QStringLiteral("outgoing_validation_pending")).toBool();
            }, replacementPause) || !apply(coordinator, 2, {
            {QStringLiteral("type"), QStringLiteral("invoke")},
            {QStringLiteral("id"), QStringLiteral("replace-race-new-drop")},
            {QStringLiteral("control"), removalRaceDropControl},
            {QStringLiteral("operation"), QStringLiteral("drop-file")},
            {QStringLiteral("value"), paths[10]},
        })) return 1;
    if (!waitForPeer(coordinator, 2,
            QStringLiteral("replacement cancels obsolete sender validation"),
            [&hashes](const QJsonObject& state) {
                const QJsonObject transfer = state.value(QStringLiteral("content"))
                    .toObject().value(QStringLiteral("transfer")).toObject();
                return assetCount(state, hashes[9]) == 0 &&
                    assetCount(state, hashes[10]) == 1 &&
                    transfer.value(QStringLiteral("pause_active")).toString().isEmpty();
            }, replacementPause, 30s)) return 1;

    expectedHashes.append(hashes[10]);
    if (!waitForAll(coordinator,
            QStringLiteral("different-byte in-flight replacement convergence"),
            [&hashes, &expectedHashes, removalRaceLaneCount,
             &removalRaceLaneIds, &removalRaceLaneNames](
                std::size_t, const QJsonObject& state) {
                const QJsonObject content = state.value(QStringLiteral("content")).toObject();
                if (!transferIdle(state) || assetCount(state, hashes[9]) != 0 ||
                    assetCount(state, hashes[10]) != 1 ||
                    !assetAvailable(state, hashes[10]) ||
                    content.value(QStringLiteral("lane_count")).toInt() !=
                        removalRaceLaneCount ||
                    content.value(QStringLiteral("lane_ids")).toArray() !=
                        removalRaceLaneIds ||
                    content.value(QStringLiteral("lane_names")).toArray() !=
                        removalRaceLaneNames) {
                    return false;
                }
                for (const QString& expected : expectedHashes) {
                    if (assetCount(state, expected) != 1 ||
                        !assetAvailable(state, expected)) return false;
                }
                return true;
            }, states, true, 90s) ||
        !verifyExactFiles(coordinator, states, expectedHashes) ||
        !verifyNoPartials(coordinator)) {
        return 1;
    }
    for (std::size_t peer = 0; peer < FourPeerCoordinator::kPeerCount; ++peer) {
        const qint64 timeoutsAfter = states[peer]
            .value(QStringLiteral("content")).toObject()
            .value(QStringLiteral("asset_request_start_timeouts")).toInteger();
        const auto view = controlState(
            coordinator.peer(peer),
            removalRaceDropControl,
            QStringLiteral("replace-race-view-%1").arg(peer + 1));
        if (timeoutsAfter != removalRaceStartTimeoutsBefore[peer] ||
            !view || !view->value(QStringLiteral("has_wav")).toBool()) {
            fail(QStringLiteral(
                "peer %1 retained timeout or omitted replacement painted WAV")
                .arg(peer + 1));
            return 1;
        }
    }

    qint64 requestStartTimeoutsBeforeDrop = 0;
    for (const QJsonObject& state : states) {
        requestStartTimeoutsBeforeDrop += state.value(QStringLiteral("content")).toObject()
            .value(QStringLiteral("asset_request_start_timeouts")).toInteger();
    }

    if (!apply(coordinator, 0, {
            {QStringLiteral("type"), QStringLiteral("looper.transfer.drop-start")},
            {QStringLiteral("id"), QStringLiteral("start-timeout-arm")},
        }) || !apply(coordinator, 0, {
            {QStringLiteral("type"), QStringLiteral("looper.import")},
            {QStringLiteral("id"), QStringLiteral("start-timeout-import")},
            {QStringLiteral("lane"), -1},
            {QStringLiteral("source_path"), paths[4]},
        })) return 1;
    QJsonObject droppedState;
    if (!waitForPeer(coordinator, 0, QStringLiteral("outgoing start drop"),
            [](const QJsonObject& state) {
                const QJsonObject transfer = state.value(QStringLiteral("content"))
                    .toObject().value(QStringLiteral("transfer")).toObject();
                return !transfer.value(QStringLiteral(
                            "drop_outgoing_start_armed")).toBool() &&
                    transfer.value(QStringLiteral(
                        "dropped_outgoing_starts")).toInteger() == 1;
            }, droppedState, 90s)) return 1;

    const QString droppedTarget = droppedState.value(QStringLiteral("content"))
        .toObject().value(QStringLiteral("transfer")).toObject()
        .value(QStringLiteral("last_dropped_outgoing_target")).toString();
    std::optional<std::size_t> droppedPeer;
    for (std::size_t peer = 0; peer < states.size(); ++peer) {
        if (states[peer].value(QStringLiteral("jam")).toObject()
                .value(QStringLiteral("local_token")).toString() == droppedTarget) {
            droppedPeer = peer;
            break;
        }
    }
    if (!droppedPeer || *droppedPeer == 0) {
        fail(QStringLiteral(
            "dropped outgoing start did not identify its remote requester"));
        return 1;
    }
    if (!apply(coordinator, *droppedPeer, {
            {QStringLiteral("type"),
                QStringLiteral("looper.transfer.expire-request-start")},
            {QStringLiteral("id"), QStringLiteral("start-timeout-expire")},
        })) return 1;

    expectedHashes.append(hashes[4]);
    if (!waitForAll(coordinator, QStringLiteral("request-start timeout recovery"),
            [&expectedHashes](std::size_t, const QJsonObject& state) {
                if (!transferIdle(state)) return false;
                for (const QString& expected : expectedHashes) {
                    if (assetCount(state, expected) != 1) return false;
                }
                return true;
            }, states, true, 90s) ||
        !verifyExactFiles(coordinator, states, expectedHashes) ||
        !verifyNoPartials(coordinator)) {
        return 1;
    }
    qint64 requestStartTimeouts = 0;
    for (const QJsonObject& state : states) {
        requestStartTimeouts += state.value(QStringLiteral("content")).toObject()
            .value(QStringLiteral("asset_request_start_timeouts")).toInteger();
    }
    const qint64 droppedStartTimeouts =
        requestStartTimeouts - requestStartTimeoutsBeforeDrop;
    if (droppedStartTimeouts != 1) {
        fail(QStringLiteral("one explicitly expired dropped start produced %1 new request-start timeouts; expected exactly 1")
            .arg(droppedStartTimeouts));
        return 1;
    }

    if (!apply(coordinator, 1, {
            {QStringLiteral("type"), QStringLiteral("looper.transfer.pause")},
            {QStringLiteral("id"), QStringLiteral("multi-batch-arm")},
            {QStringLiteral("point"), QStringLiteral("outgoing-validation")},
        }) || !apply(coordinator, 1, {
            {QStringLiteral("type"), QStringLiteral("looper.import")},
            {QStringLiteral("id"), QStringLiteral("multi-batch-import")},
            {QStringLiteral("lane"), -1},
            {QStringLiteral("source_path"), paths[5]},
        })) return 1;
    QJsonObject multiBatchPause;
    if (!waitForPeer(coordinator, 1, QStringLiteral("multi-batch validation pause"),
            [](const QJsonObject& state) {
                const QJsonObject transfer = state.value(QStringLiteral("content"))
                    .toObject().value(QStringLiteral("transfer")).toObject();
                return transfer.value(QStringLiteral("pause_active")).toString() ==
                        QStringLiteral("outgoing-validation") &&
                    transfer.value(QStringLiteral("outgoing_validation_pending")).toBool();
            }, multiBatchPause) || !apply(coordinator, 1, {
                {QStringLiteral("type"), QStringLiteral("looper.share")},
                {QStringLiteral("id"), QStringLiteral("multi-batch-second-share")},
            })) return 1;
    if (!waitForPeer(coordinator, 1, QStringLiteral("two outgoing Track Sync batches"),
            [](const QJsonObject& state) {
                return state.value(QStringLiteral("content")).toObject()
                    .value(QStringLiteral("outgoing_track_batch_count")).toInt() >= 2;
            }, multiBatchPause) ||
        !leaveAndRejoin(coordinator, 1, invite, ports[1], states,
            QStringLiteral("multi-batch")) ||
        !apply(coordinator, 1, {
            {QStringLiteral("type"), QStringLiteral("looper.share")},
            {QStringLiteral("id"), QStringLiteral("multi-batch-recover-share")},
        })) return 1;
    expectedHashes.append(hashes[5]);
    if (!waitForAll(coordinator, QStringLiteral("multi-batch exact recovery"),
            [&expectedHashes](std::size_t, const QJsonObject& state) {
                if (!transferIdle(state)) return false;
                for (const QString& expected : expectedHashes) {
                    if (assetCount(state, expected) != 1) return false;
                }
                return true;
            }, states, true, 90s) ||
        !verifyExactFiles(coordinator, states, expectedHashes) ||
        !verifyNoPartials(coordinator)) {
        return 1;
    }

    for (std::size_t peer = 1; peer < FourPeerCoordinator::kPeerCount; ++peer) {
        if (!apply(coordinator, peer, {
                {QStringLiteral("type"), QStringLiteral("jam.leave")},
                {QStringLiteral("id"), QStringLiteral("new-session-leave-%1").arg(peer + 1)},
            })) return 1;
    }
    QJsonObject creatorWithoutPeers;
    if (!waitForPeer(coordinator, 0, QStringLiteral("creator peer drain before new session"),
            [](const QJsonObject& state) {
                const QJsonObject jam = state.value(QStringLiteral("jam")).toObject();
                return jam.value(QStringLiteral("remote_peer_count")).toInt() == 0 &&
                    jam.value(QStringLiteral("active_remote_peer_count")).toInt() == 0;
            }, creatorWithoutPeers) ||
        !apply(coordinator, 0, {
            {QStringLiteral("type"), QStringLiteral("jam.leave")},
            {QStringLiteral("id"), QStringLiteral("new-session-creator-leave")},
        })) return 1;
    if (!waitForAll(coordinator, QStringLiteral("new-session reset"),
            [](std::size_t, const QJsonObject& state) {
                const QJsonObject jam = state.value(QStringLiteral("jam")).toObject();
                const QJsonObject content = state.value(QStringLiteral("content")).toObject();
                return jam.value(QStringLiteral("role")).toString() ==
                        QStringLiteral("inactive") &&
                    !jam.value(QStringLiteral("network_running")).toBool() &&
                    content.value(QStringLiteral(
                        "track_sync_local_arrangement_revision")).toInt(-1) == 0 &&
                    content.value(QStringLiteral(
                        "track_sync_last_applied_host_arrangement_revision")).toInt(-1) == 0 &&
                    transferIdle(state);
            }, states, false, 90s) || !verifyNoPartials(coordinator)) {
        return 1;
    }

    if (!send(coordinator.peer(0), {
            {QStringLiteral("type"), QStringLiteral("jam.create")},
            {QStringLiteral("id"), QStringLiteral("new-session-create")},
            {QStringLiteral("port"), ports[0]},
        })) return 1;
    const auto recreated = receive(coordinator.peer(0), QStringLiteral("command_applied"));
    if (!recreated) return 1;
    const QString newInvite = recreated->value(QStringLiteral("invite_url")).toString();
    if (newInvite.isEmpty()) {
        fail(QStringLiteral("new session returned no invite"));
        return 1;
    }
    for (std::size_t peer = 1; peer < FourPeerCoordinator::kPeerCount; ++peer) {
        if (!apply(coordinator, peer, {
                {QStringLiteral("type"), QStringLiteral("jam.join")},
                {QStringLiteral("id"), QStringLiteral("new-session-join-%1").arg(peer + 1)},
                {QStringLiteral("port"), ports[peer]},
                {QStringLiteral("invite_url"), newInvite},
            })) return 1;
    }
    if (!waitForAll(coordinator, QStringLiteral("new-session revision-one acceptance"),
            [&expectedHashes](std::size_t peer, const QJsonObject& state) {
                const QJsonObject content = state.value(QStringLiteral("content")).toObject();
                if (!meshReady(peer, state) || !transferIdle(state) ||
                    content.value(QStringLiteral("arrangement_revision")).toInteger() <= 0) {
                    return false;
                }
                if (peer == 0) {
                    if (content.value(QStringLiteral(
                            "track_sync_local_arrangement_revision")).toInt() <= 0) return false;
                } else if (content.value(QStringLiteral(
                        "track_sync_last_applied_host_arrangement_revision")).toInt() <= 0) {
                    return false;
                }
                for (const QString& expected : expectedHashes) {
                    if (assetCount(state, expected) != 1) return false;
                }
                return true;
            }, states, true, 90s) ||
        !verifyExactFiles(coordinator, states, expectedHashes) ||
        !verifyNoPartials(coordinator)) {
        return 1;
    }

    if (!apply(coordinator, 0, wavPolicy(
            QStringLiteral("source-handoff-auto-off"), false)) ||
        !waitForAll(coordinator, QStringLiteral("source-handoff policy disable"),
            [](std::size_t, const QJsonObject& state) {
                return !state.value(QStringLiteral("jam")).toObject()
                    .value(QStringLiteral("policy")).toObject()
                    .value(QStringLiteral("auto_share_wavs")).toBool();
            }, states, false)) return 1;

    if (!send(coordinator.peer(1), {
            {QStringLiteral("type"), QStringLiteral("looper.import")},
            {QStringLiteral("id"), QStringLiteral("source-handoff-import-a")},
            {QStringLiteral("lane"), -1},
            {QStringLiteral("source_path"), paths[6]},
        }) || !receive(coordinator.peer(1), QStringLiteral("command_applied")) ||
        !waitForAll(coordinator, QStringLiteral("source-handoff local metadata"),
            [&hashes](std::size_t peer, const QJsonObject& state) {
                return transferIdle(state) && assetCount(state, hashes[6]) == 1 &&
                    assetAvailable(state, hashes[6]) == (peer == 1);
            }, states, true, 90s)) return 1;

    const qint64 sourceHandoffTimeoutsBefore = states[0]
        .value(QStringLiteral("content")).toObject()
        .value(QStringLiteral("asset_request_start_timeouts")).toInteger();
    const qint64 sourceHandoffDropsBefore = states[1]
        .value(QStringLiteral("content")).toObject()
        .value(QStringLiteral("transfer")).toObject()
        .value(QStringLiteral("dropped_outgoing_starts")).toInteger();
    if (!apply(coordinator, 1, {
            {QStringLiteral("type"), QStringLiteral("looper.transfer.drop-start")},
            {QStringLiteral("id"), QStringLiteral("source-handoff-drop-a")},
            {QStringLiteral("count"), 3},
        }) || !apply(coordinator, 1, {
            {QStringLiteral("type"), QStringLiteral("looper.share")},
            {QStringLiteral("id"), QStringLiteral("source-handoff-share-a")},
        })) return 1;

    for (int expiry = 1; expiry <= 2; ++expiry) {
        QJsonObject droppedRequest;
        if (!waitForPeer(coordinator, 1,
                QStringLiteral("source-A dropped start %1").arg(expiry),
                [sourceHandoffDropsBefore, expiry](const QJsonObject& state) {
                    return state.value(QStringLiteral("content")).toObject()
                        .value(QStringLiteral("transfer")).toObject()
                        .value(QStringLiteral("dropped_outgoing_starts")).toInteger() >=
                        sourceHandoffDropsBefore + expiry;
                }, droppedRequest, 90s) ||
            !apply(coordinator, 0, {
                {QStringLiteral("type"),
                    QStringLiteral("looper.transfer.expire-request-start")},
                {QStringLiteral("id"),
                    QStringLiteral("source-handoff-expire-%1").arg(expiry)},
            })) {
            return 1;
        }
    }

    QJsonObject handoffState;
    QJsonObject thirdDroppedRequest;
    const int sourceHandoffLane = activeBankAssetLaneIndex(states[2], hashes[6]);
    if (sourceHandoffLane < 0) {
        fail(QStringLiteral("source-handoff lane was absent from source B's active bank"));
        return 1;
    }
    if (!waitForPeer(coordinator, 1, QStringLiteral("source-A dropped start 3"),
            [sourceHandoffDropsBefore](const QJsonObject& state) {
                return state.value(QStringLiteral("content")).toObject()
                    .value(QStringLiteral("transfer")).toObject()
                    .value(QStringLiteral("dropped_outgoing_starts")).toInteger() ==
                    sourceHandoffDropsBefore + 3;
            }, thirdDroppedRequest, 90s) || !apply(coordinator, 0, {
            {QStringLiteral("type"), QStringLiteral("looper.transfer.pause")},
            {QStringLiteral("id"), QStringLiteral("source-handoff-offer-pause")},
            {QStringLiteral("point"), QStringLiteral("offer")},
        }) || !send(coordinator.peer(2), {
            {QStringLiteral("type"), QStringLiteral("looper.import")},
            {QStringLiteral("id"), QStringLiteral("source-handoff-import-b")},
            {QStringLiteral("lane"), sourceHandoffLane},
            {QStringLiteral("source_path"), paths[6]},
        }) || !receive(coordinator.peer(2), QStringLiteral("command_applied"))) return 1;
    QJsonObject sourceBReadyState;
    if (!waitForPeer(coordinator, 2, QStringLiteral("source-B local asset ready"),
            [&hashes](const QJsonObject& state) {
                const QJsonObject content = state.value(QStringLiteral("content")).toObject();
                return transferIdle(state) &&
                    content.value(QStringLiteral("file_tasks_active")).toInt(-1) == 0 &&
                    assetCount(state, hashes[6]) == 1 &&
                    assetAvailable(state, hashes[6]);
            }, sourceBReadyState, 90s)) return 1;
    const qint64 sourceHandoffDropsBeforeB = sourceBReadyState
        .value(QStringLiteral("content")).toObject()
        .value(QStringLiteral("transfer")).toObject()
        .value(QStringLiteral("dropped_outgoing_starts")).toInteger();
    if (!apply(coordinator, 2, {
            {QStringLiteral("type"), QStringLiteral("looper.transfer.drop-start")},
            {QStringLiteral("id"), QStringLiteral("source-handoff-drop-b")},
        }) || !apply(coordinator, 2, {
            {QStringLiteral("type"), QStringLiteral("looper.share")},
            {QStringLiteral("id"), QStringLiteral("source-handoff-share-b")},
        }) || !waitForPeer(coordinator, 0, QStringLiteral("two-source same-hash offer"),
            [&hashes](const QJsonObject& state) {
                const QJsonObject transfer = state.value(QStringLiteral("content"))
                    .toObject().value(QStringLiteral("transfer")).toObject();
                return transfer.value(QStringLiteral("pause_active")).toString() ==
                        QStringLiteral("offer") &&
                    pendingSourcesForHash(state, hashes[6]) >= 2;
            }, handoffState) || !apply(coordinator, 0, {
            {QStringLiteral("type"),
                QStringLiteral("looper.transfer.expire-request-start")},
            {QStringLiteral("id"), QStringLiteral("source-handoff-expire-3")},
        }) || !waitForPeer(coordinator, 0, QStringLiteral("three source-A start failures"),
            [sourceHandoffTimeoutsBefore](const QJsonObject& state) {
                const QJsonObject content = state.value(QStringLiteral("content")).toObject();
                return content.value(QStringLiteral(
                           "asset_request_start_timeouts")).toInteger() ==
                        sourceHandoffTimeoutsBefore + 3 &&
                    content.value(QStringLiteral("incoming_asset_retry_count")).toInt() == 1;
            }, handoffState, 60s) || !apply(coordinator, 1, {
            {QStringLiteral("type"), QStringLiteral("jam.leave")},
            {QStringLiteral("id"), QStringLiteral("source-handoff-leave-a")},
        }) || !apply(coordinator, 0, {
            {QStringLiteral("type"), QStringLiteral("looper.transfer.release")},
            {QStringLiteral("id"), QStringLiteral("source-handoff-offer-release")},
        })) return 1;

    QJsonObject sourceBDroppedRequest;
    if (!waitForPeer(coordinator, 2, QStringLiteral("source-B dropped start"),
            [sourceHandoffDropsBeforeB](const QJsonObject& state) {
                return state.value(QStringLiteral("content")).toObject()
                    .value(QStringLiteral("transfer")).toObject()
                    .value(QStringLiteral("dropped_outgoing_starts")).toInteger() ==
                    sourceHandoffDropsBeforeB + 1;
            }, sourceBDroppedRequest, 90s) || !apply(coordinator, 0, {
            {QStringLiteral("type"),
                QStringLiteral("looper.transfer.expire-request-start")},
            {QStringLiteral("id"), QStringLiteral("source-handoff-expire-b")},
        }) || !waitForPeer(coordinator, 0, QStringLiteral("fresh source-B retry budget"),
            [&hashes, sourceHandoffTimeoutsBefore](const QJsonObject& state) {
                const QJsonObject content = state.value(QStringLiteral("content")).toObject();
                const qint64 timeouts = content
                    .value(QStringLiteral("asset_request_start_timeouts")).toInteger();
                return timeouts == sourceHandoffTimeoutsBefore + 4 &&
                    content.value(QStringLiteral("incoming_asset_retry_count")).toInt() == 0 &&
                    assetAvailable(state, hashes[6]);
            }, handoffState, 90s)) return 1;

    if (!apply(coordinator, 1, {
            {QStringLiteral("type"), QStringLiteral("jam.join")},
            {QStringLiteral("id"), QStringLiteral("source-handoff-rejoin-a")},
            {QStringLiteral("port"), ports[1]},
            {QStringLiteral("invite_url"), newInvite},
        }) || !waitForAll(coordinator, QStringLiteral("source-handoff rejoin"),
            meshReady, states, false, 90s) || !apply(coordinator, 0,
            wavPolicy(QStringLiteral("source-handoff-auto-on"), true)) ||
        !apply(coordinator, 0, {
            {QStringLiteral("type"), QStringLiteral("looper.share")},
            {QStringLiteral("id"), QStringLiteral("source-handoff-final-share")},
        })) return 1;

    expectedHashes.append(hashes[6]);
    if (!waitForAll(coordinator, QStringLiteral("source-handoff exact convergence"),
            [&expectedHashes](std::size_t, const QJsonObject& state) {
                if (!transferIdle(state)) return false;
                for (const QString& expected : expectedHashes) {
                    if (assetCount(state, expected) != 1 ||
                        !assetAvailable(state, expected)) return false;
                }
                return true;
            }, states, true, 90s) ||
        !verifyExactFiles(coordinator, states, expectedHashes) ||
        !verifyNoPartials(coordinator)) return 1;

    if (!shutDown(coordinator) || !verifyNoPartials(coordinator)) return 1;
    if (failures != 0) return 1;
    coordinator.markSuccessful();
    std::cout << "four-peer WAV interruption checks passed\n";
    return 0;
}

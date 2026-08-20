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
#include <QThread>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <chrono>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <utility>

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
    constexpr quint32 frames = 1024;
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
        const int phase = static_cast<int>((frame * static_cast<quint32>(variant + 3)) % 97U);
        const qint16 sample = static_cast<qint16>((phase - 48) * (180 + variant * 17));
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
    path = folder + QLatin1Char('/') + name;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(bytes) != bytes.size() || !file.flush()) {
        fail(QStringLiteral("could not write PCM16 fixture ") + path);
        return false;
    }
    file.close();
    hash = QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    return true;
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

bool transferIdle(const QJsonObject& state)
{
    const QJsonObject content = state.value(QStringLiteral("content")).toObject();
    const QJsonObject transfer = content.value(QStringLiteral("transfer")).toObject();
    return content.value(QStringLiteral("pending_asset_count")).toInt() == 0 &&
        content.value(QStringLiteral("pending_track_contribution_count")).toInt() == 0 &&
        content.value(QStringLiteral("outgoing_track_batch_count")).toInt() == 0 &&
        !content.value(QStringLiteral("incoming_asset_active")).toBool() &&
        content.value(QStringLiteral("incoming_asset_retry_count")).toInt() == 0 &&
        content.value(QStringLiteral("file_tasks_active")).toInt() == 0 &&
        transfer.value(QStringLiteral("pause_armed")).toString().isEmpty() &&
        transfer.value(QStringLiteral("pause_active")).toString().isEmpty() &&
        !transfer.value(QStringLiteral("outgoing_validation_pending")).toBool() &&
        transfer.value(QStringLiteral("outgoing_hash")).toString().isEmpty() &&
        transfer.value(QStringLiteral("outgoing_queue_count")).toInt() == 0 &&
        transfer.value(QStringLiteral("incoming_hash")).toString().isEmpty() &&
        transfer.value(QStringLiteral("incoming_queued_chunks")).toInt() == 0 &&
        !transfer.value(QStringLiteral("incoming_write_pending")).toBool() &&
        !transfer.value(QStringLiteral("incoming_done_pending")).toBool();
}

bool assetReady(const QJsonObject& state, const QString& hash, bool expectedAvailable)
{
    const QJsonObject content = state.value(QStringLiteral("content")).toObject();
    const int index = assetIndex(state, hash);
    const QJsonArray available = content.value(QStringLiteral("asset_available")).toArray();
    const QJsonArray bytes = content.value(QStringLiteral("asset_bytes")).toArray();
    return index >= 0 && index < available.size() && index < bytes.size() &&
        available.at(index).toBool() == expectedAvailable &&
        (expectedAvailable ? bytes.at(index).toInteger() > 44 : bytes.at(index).toInteger() == 0) &&
        transferIdle(state);
}

bool verifyAssetFiles(
    const std::array<QJsonObject, FourPeerCoordinator::kPeerCount>& states,
    const QString& expectedHash)
{
    for (std::size_t peer = 0; peer < states.size(); ++peer) {
        const QJsonObject content = states[peer].value(QStringLiteral("content")).toObject();
        const int index = assetIndex(states[peer], expectedHash);
        const QJsonArray paths = content.value(QStringLiteral("lane_paths")).toArray();
        if (index < 0 || index >= paths.size()) {
            fail(QStringLiteral("peer %1 omitted the transferred asset path").arg(peer + 1));
            return false;
        }
        QFile file(paths.at(index).toString());
        if (!file.open(QIODevice::ReadOnly) ||
            QString::fromLatin1(QCryptographicHash::hash(
                file.readAll(), QCryptographicHash::Sha256).toHex()) != expectedHash) {
            fail(QStringLiteral("peer %1 stored the wrong transferred WAV bytes").arg(peer + 1));
            return false;
        }
    }
    return true;
}

bool verifyIsolatedAssetFiles(
    const FourPeerCoordinator& coordinator,
    const std::array<QJsonObject, FourPeerCoordinator::kPeerCount>& states,
    const QStringList& expectedHashes)
{
    for (const QString& hash : expectedHashes) {
        QSet<QString> paths;
        for (std::size_t peer = 0; peer < states.size(); ++peer) {
            if (assetCount(states[peer], hash) != 1) {
                fail(QStringLiteral("peer %1 did not contain exactly one lane for hash %2")
                    .arg(peer + 1).arg(hash.left(12)));
                return false;
            }
            const QJsonObject content = states[peer].value(QStringLiteral("content")).toObject();
            const int index = assetIndex(states[peer], hash);
            const QJsonArray lanePaths = content.value(QStringLiteral("lane_paths")).toArray();
            if (index < 0 || index >= lanePaths.size()) {
                fail(QStringLiteral("peer %1 omitted an isolated path for hash %2")
                    .arg(peer + 1).arg(hash.left(12)));
                return false;
            }
            const QString path = QDir::cleanPath(
                QFileInfo(lanePaths.at(index).toString()).canonicalFilePath());
            const QString root = QDir::cleanPath(
                QFileInfo(coordinator.storageRoot(peer)).canonicalFilePath());
            const bool insideRoot = path.compare(root, Qt::CaseInsensitive) == 0 ||
                path.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive);
            if (path.isEmpty() || root.isEmpty() || !insideRoot) {
                fail(QStringLiteral(
                    "peer %1 asset escaped its isolated storage root: path=%2 root=%3")
                    .arg(peer + 1).arg(path, root));
                return false;
            }
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly) ||
                QString::fromLatin1(QCryptographicHash::hash(
                    file.readAll(), QCryptographicHash::Sha256).toHex()) != hash) {
                fail(QStringLiteral("peer %1 stored incorrect bytes for hash %2")
                    .arg(peer + 1).arg(hash.left(12)));
                return false;
            }
            paths.insert(QDir::cleanPath(path).toLower());
        }
        if (paths.size() != static_cast<qsizetype>(states.size())) {
            fail(QStringLiteral("hash %1 reused a path across isolated peers").arg(hash.left(12)));
            return false;
        }
    }
    return true;
}

bool verifySavedProjects(
    const FourPeerCoordinator& coordinator,
    const std::array<QJsonObject, FourPeerCoordinator::kPeerCount>& states,
    const QStringList& expectedHashes)
{
    QSet<QString> savedPaths;
    for (std::size_t peer = 0; peer < states.size(); ++peer) {
        const QJsonObject content = states[peer]
            .value(QStringLiteral("content")).toObject();
        const QString projectPath = content
            .value(QStringLiteral("project_path")).toString();
        const QString projectFolder = content
            .value(QStringLiteral("project_folder")).toString();
        const QString storageRoot = QDir::cleanPath(QFileInfo(
            coordinator.storageRoot(peer)).canonicalFilePath());
        const QString canonicalProject = QDir::cleanPath(
            QFileInfo(projectPath).canonicalFilePath());
        if (projectPath.isEmpty() || projectFolder.isEmpty() ||
            canonicalProject.isEmpty() || storageRoot.isEmpty() ||
            !canonicalProject.startsWith(
                storageRoot + QLatin1Char('/'), Qt::CaseInsensitive) ||
            QDir::cleanPath(QFileInfo(projectPath).absolutePath()).compare(
                QDir::cleanPath(QDir(projectFolder).absolutePath()),
                Qt::CaseInsensitive) != 0) {
            fail(QStringLiteral(
                "peer %1 saved its JamJar outside isolated storage or exposed inconsistent roots")
                .arg(peer + 1));
            return false;
        }
        savedPaths.insert(canonicalProject.toLower());

        QFile projectFile(projectPath);
        if (!projectFile.open(QIODevice::ReadOnly)) {
            fail(QStringLiteral("peer %1 JamJar could not be read").arg(peer + 1));
            return false;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(
            projectFile.readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            fail(QStringLiteral("peer %1 wrote invalid JamJar JSON: %2")
                .arg(peer + 1).arg(parseError.errorString()));
            return false;
        }

        QSet<QString> savedHashes;
        const QJsonArray banks = document.object()
            .value(QStringLiteral("looper")).toObject()
            .value(QStringLiteral("banks")).toArray();
        for (const QJsonValue& bankValue : banks) {
            const QJsonArray lanes = bankValue.toObject()
                .value(QStringLiteral("lanes")).toArray();
            for (const QJsonValue& laneValue : lanes) {
                const QJsonObject lane = laneValue.toObject();
                const QString assetPath = lane
                    .value(QStringLiteral("asset_path")).toString();
                const QString hash = lane
                    .value(QStringLiteral("asset_hash")).toString();
                if (assetPath.isEmpty()) continue;
                const QString cleanRelative = QDir::cleanPath(assetPath);
                if (QFileInfo(assetPath).isAbsolute() ||
                    cleanRelative == QStringLiteral("..") ||
                    cleanRelative.startsWith(QStringLiteral("../"))) {
                    fail(QStringLiteral(
                        "peer %1 persisted a non-portable looper asset path: %2")
                        .arg(peer + 1).arg(assetPath));
                    return false;
                }
                const QString absolute = QDir(projectFolder)
                    .absoluteFilePath(cleanRelative);
                QFile asset(absolute);
                if (!asset.open(QIODevice::ReadOnly) ||
                    QString::fromLatin1(QCryptographicHash::hash(
                        asset.readAll(), QCryptographicHash::Sha256).toHex()) != hash) {
                    fail(QStringLiteral(
                        "peer %1 saved a missing or hash-mismatched looper WAV: %2")
                        .arg(peer + 1).arg(assetPath));
                    return false;
                }
                savedHashes.insert(hash);
            }
        }
        for (const QString& hash : expectedHashes) {
            if (!savedHashes.contains(hash)) {
                fail(QStringLiteral(
                    "peer %1 JamJar omitted converged WAV %2")
                    .arg(peer + 1).arg(hash.left(12)));
                return false;
            }
        }
    }
    if (savedPaths.size() != static_cast<qsizetype>(states.size())) {
        fail(QStringLiteral("four peers reused a JamJar path across isolated roots"));
        return false;
    }
    return true;
}

bool verifyNoPartialFiles(const FourPeerCoordinator& coordinator)
{
    for (std::size_t peer = 0; peer < FourPeerCoordinator::kPeerCount; ++peer) {
        QDirIterator files(
            coordinator.storageRoot(peer),
            {QStringLiteral("*.partial.*")},
            QDir::Files,
            QDirIterator::Subdirectories);
        if (files.hasNext()) {
            fail(QStringLiteral("peer %1 retained partial asset file %2")
                .arg(peer + 1).arg(files.next()));
            return false;
        }
    }
    return true;
}

bool removeIsolatedCopies(
    const FourPeerCoordinator& coordinator,
    const std::array<QString, FourPeerCoordinator::kPeerCount>& paths,
    std::size_t retainedPeer)
{
    for (std::size_t peer = 0; peer < paths.size(); ++peer) {
        if (peer == retainedPeer) continue;
        const QString path = QDir::cleanPath(QFileInfo(paths[peer]).absoluteFilePath());
        const QString root = QDir::cleanPath(
            QFileInfo(coordinator.storageRoot(peer)).absoluteFilePath());
#if defined(Q_OS_WIN)
        constexpr Qt::CaseSensitivity pathCase = Qt::CaseInsensitive;
#else
        constexpr Qt::CaseSensitivity pathCase = Qt::CaseSensitive;
#endif
        const bool insideRoot = path.compare(root, pathCase) == 0 ||
            path.startsWith(root + QLatin1Char('/'), pathCase);
        if (path.isEmpty() || root.isEmpty() || !insideRoot) {
            fail(QStringLiteral(
                "refused to remove non-source test WAV outside peer %1 root: path=%2 root=%3")
                .arg(peer + 1).arg(path, root));
            return false;
        }
        if (QFileInfo::exists(path) && !QFile::remove(path)) {
            fail(QStringLiteral("could not remove isolated non-source WAV copy: ") + path);
            return false;
        }
        if (QFileInfo::exists(path)) {
            fail(QStringLiteral("isolated non-source WAV copy still exists: ") + path);
            return false;
        }
    }
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

std::optional<QJsonObject> snapshot(AutomationProcess& process, int peer, int sequence)
{
    if (!send(process, {
            {QStringLiteral("type"), QStringLiteral("snapshot")},
            {QStringLiteral("id"), QStringLiteral("content-%1-%2").arg(peer).arg(sequence)},
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
            fail(QStringLiteral("control snapshot response id mismatch"));
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
    fail(QStringLiteral("GUI view omitted control ") + controlId);
    return std::nullopt;
}

template <typename Predicate>
bool waitForAll(
    FourPeerCoordinator& coordinator,
    const QString& description,
    Predicate predicate,
    std::array<QJsonObject, FourPeerCoordinator::kPeerCount>& states,
    bool requireSameDigest = true)
{
    const auto deadline = std::chrono::steady_clock::now() +
        jam2::test::deadmanTimeout(90s);
    int sequence = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        bool ready = true;
        for (std::size_t index = 0; index < states.size(); ++index) {
            const auto event = snapshot(
                coordinator.peer(index), static_cast<int>(index + 1), sequence);
            if (!event || !event->value(QStringLiteral("jam")).isObject() ||
                !event->value(QStringLiteral("content")).isObject()) {
                fail(QStringLiteral("peer %1 snapshot omitted Jam/content state").arg(index + 1));
                return false;
            }
            states[index] = *event;
        }
        const QString digest = states[0].value(QStringLiteral("content")).toObject()
            .value(QStringLiteral("model_sha256")).toString();
        for (std::size_t index = 0; index < states.size(); ++index) {
            const QJsonObject content = states[index].value(QStringLiteral("content")).toObject();
            ready = (!requireSameDigest || (!digest.isEmpty() &&
                content.value(QStringLiteral("model_sha256")).toString() == digest)) &&
                predicate(index, states[index]) && ready;
        }
        if (ready) return true;
        ++sequence;
    }
    fail(QStringLiteral("timed out waiting for ") + description);
    for (std::size_t index = 0; index < states.size(); ++index) {
        const QJsonObject jam = states[index].value(QStringLiteral("jam")).toObject();
        const QJsonObject content = states[index].value(QStringLiteral("content")).toObject();
        const QJsonObject transfer = content.value(QStringLiteral("transfer")).toObject();
        std::cerr << "  peer-" << index + 1
                  << " role=" << jam.value(QStringLiteral("role")).toString().toStdString()
                  << " remotes=" << jam.value(QStringLiteral("remote_peer_count")).toInt()
                  << " arrangement=" << content.value(QStringLiteral("arrangement_revision")).toInteger()
                  << " title=" << content.value(QStringLiteral("title")).toString().toStdString()
                  << " digest=" << content.value(QStringLiteral("model_sha256")).toString().toStdString()
                  << " song=" << content.value(QStringLiteral("song_model_sha256")).toString().toStdString()
                  << " looper=" << content.value(QStringLiteral("looper_sha256")).toString().toStdString()
                  << " section_ids=" << content.value(QStringLiteral("section_ids")).toArray().size()
                  << ':' << content.value(QStringLiteral("section_ids")).toArray().at(0).toString().toStdString()
                  << " lane_ids=" << content.value(QStringLiteral("lane_ids")).toArray().size()
                  << ':' << content.value(QStringLiteral("lane_ids")).toArray().at(0).toString().toStdString()
                  << " lane_path=" << content.value(QStringLiteral("lane_paths")).toArray().at(0).toString().toStdString()
                  << " assets=" << QJsonDocument(content.value(
                        QStringLiteral("asset_hashes")).toArray()).toJson(
                            QJsonDocument::Compact).toStdString()
                  << " available=" << QJsonDocument(content.value(
                        QStringLiteral("asset_available")).toArray()).toJson(
                            QJsonDocument::Compact).toStdString()
                  << " asset_bytes=" << QJsonDocument(content.value(
                        QStringLiteral("asset_bytes")).toArray()).toJson(
                            QJsonDocument::Compact).toStdString()
                  << " pending_assets=" << content.value(
                        QStringLiteral("pending_asset_count")).toInt()
                  << " pending_contributions=" << content.value(
                        QStringLiteral("pending_track_contribution_count")).toInt()
                  << " outgoing_batches=" << content.value(
                        QStringLiteral("outgoing_track_batch_count")).toInt()
                  << " incoming_active=" << content.value(
                        QStringLiteral("incoming_asset_active")).toBool()
                  << " retry_hashes=" << content.value(
                        QStringLiteral("incoming_asset_retry_count")).toInt()
                  << " file_tasks=" << content.value(
                        QStringLiteral("file_tasks_active")).toInt()
                  << " pending_detail=" << QJsonDocument(content.value(
                        QStringLiteral("pending_track_contributions")).toArray()).toJson(
                            QJsonDocument::Compact).toStdString()
                  << " outgoing_detail=" << QJsonDocument(content.value(
                        QStringLiteral("outgoing_track_batches")).toArray()).toJson(
                            QJsonDocument::Compact).toStdString()
                  << " transfer=" << QJsonDocument(transfer).toJson(
                        QJsonDocument::Compact).toStdString()
                  << '\n';
        QDirIterator logFiles(
            coordinator.storageRoot(index),
            {QStringLiteral("*.log")}, QDir::Files, QDirIterator::Subdirectories);
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
        if (!newestLog.isEmpty()) {
            QFile file(newestLog);
            if (file.open(QIODevice::ReadOnly)) {
                const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
                QStringList relevant;
                for (const QString& line : lines) {
                    if (line.contains(QStringLiteral("asset"), Qt::CaseInsensitive) ||
                        line.contains(QStringLiteral("Track Sync"), Qt::CaseInsensitive) ||
                        line.contains(QStringLiteral("WAV"), Qt::CaseInsensitive)) {
                        relevant.append(line);
                    }
                }
                const qsizetype start = std::max<qsizetype>(0, relevant.size() - 24);
                for (qsizetype line = start; line < relevant.size(); ++line) {
                    std::cerr << "    log: " << relevant.at(line).toStdString() << '\n';
                }
            }
        }
    }
    return false;
}

bool apply(
    FourPeerCoordinator& coordinator,
    std::size_t peer,
    QJsonObject command)
{
    return send(coordinator.peer(peer), std::move(command)) &&
        receive(coordinator.peer(peer), QStringLiteral("command_applied")).has_value();
}

template <std::size_t Count>
bool applyConcurrently(
    FourPeerCoordinator& coordinator,
    const std::array<std::pair<std::size_t, QJsonObject>, Count>& commands)
{
    for (const auto& [peer, command] : commands) {
        if (!send(coordinator.peer(peer), command)) return false;
    }
    for (const auto& [peer, command] : commands) {
        const auto applied = receive(coordinator.peer(peer), QStringLiteral("command_applied"));
        if (!applied || applied->value(QStringLiteral("id")) !=
                command.value(QStringLiteral("id"))) {
            fail(QStringLiteral("peer %1 returned the wrong concurrent command completion")
                .arg(peer + 1));
            return false;
        }
    }
    return true;
}

bool meshReady(std::size_t index, const QJsonObject& state)
{
    const QJsonObject jam = state.value(QStringLiteral("jam")).toObject();
    const QJsonObject content = state.value(QStringLiteral("content")).toObject();
    return jam.value(QStringLiteral("role")).toString() ==
            (index == 0 ? QStringLiteral("creator") : QStringLiteral("joiner")) &&
        jam.value(QStringLiteral("remote_peer_count")).toInt() == 3 &&
        jam.value(QStringLiteral("active_remote_peer_count")).toInt() == 3 &&
        jam.value(QStringLiteral("network_attachment_ready")).toBool() &&
        jam.value(QStringLiteral("network_running")).toBool() &&
        jam.value(QStringLiteral("failure")).toString().isEmpty() &&
        content.value(QStringLiteral("arrangement_revision")).toInteger() >= 1 &&
        content.value(QStringLiteral("title")) == content.value(QStringLiteral("title_view"));
}

bool arrangementMatches(
    const QJsonObject& state,
    std::initializer_list<std::pair<int, int>> expectedSteps,
    bool expectedLoop)
{
    const QJsonObject arrangement = state.value(QStringLiteral("content")).toObject()
        .value(QStringLiteral("arrangement")).toObject();
    const QJsonArray steps = arrangement.value(QStringLiteral("steps")).toArray();
    if (steps.size() != static_cast<qsizetype>(expectedSteps.size()) ||
        arrangement.value(QStringLiteral("loop")).toBool() != expectedLoop ||
        arrangement.value(QStringLiteral("enabled")).toBool() ||
        arrangement.value(QStringLiteral("running")).toBool() ||
        arrangement.value(QStringLiteral("armed")).toBool()) {
        return false;
    }
    qsizetype index = 0;
    for (const auto& [bank, repeats] : expectedSteps) {
        const QJsonObject step = steps.at(index).toObject();
        if (step.value(QStringLiteral("bank")).toInt(-1) != bank ||
            step.value(QStringLiteral("repeats")).toInt(-1) != repeats) {
            return false;
        }
        ++index;
    }
    return true;
}

QJsonObject policyCommand(QString id, const QString& ideas, bool autoShareWavs = true)
{
    return {
        {QStringLiteral("type"), QStringLiteral("jam.policy")},
        {QStringLiteral("id"), std::move(id)},
        {QStringLiteral("track_lanes"), true},
        {QStringLiteral("auto_share_wavs"), autoShareWavs},
        {QStringLiteral("global_playback"), true},
        {QStringLiteral("generated_ideas"), ideas},
        {QStringLiteral("metronome_state"), false},
        {QStringLiteral("recordings"), true},
    };
}

bool shutDown(FourPeerCoordinator& coordinator)
{
    for (std::size_t index = 0; index < FourPeerCoordinator::kPeerCount; ++index) {
        if (!send(coordinator.peer(index), {
                {QStringLiteral("type"), QStringLiteral("shutdown")},
                {QStringLiteral("id"), QStringLiteral("shutdown-%1").arg(index + 1)},
            })) return false;
    }
    for (std::size_t index = 0; index < FourPeerCoordinator::kPeerCount; ++index) {
        if (!receive(coordinator.peer(index), QStringLiteral("command_applied")) ||
            !receive(coordinator.peer(index), QStringLiteral("shutdown"))) return false;
        int exitCode = -1;
        QString error;
        if (!coordinator.peer(index).waitForExit(60s, exitCode, error) || exitCode != 0) {
            fail(QStringLiteral("peer %1 did not exit cleanly: %2 code=%3")
                .arg(index + 1).arg(error).arg(exitCode));
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
        std::cerr << "usage: jam2_four_shared_content_integration <release-jam2>\n";
        return 2;
    }
    QTemporaryDir fixtureFolder;
    QString automaticWavPath;
    QString automaticWavHash;
    QString manualWavPath;
    QString manualWavHash;
    QString identicalWavPathA;
    QString identicalWavPathB;
    QString identicalWavHashA;
    QString identicalWavHashB;
    QString conflictWavPathA;
    QString conflictWavPathB;
    QString conflictWavPathC;
    QString conflictWavHashA;
    QString conflictWavHashB;
    QString conflictWavHashC;
    if (!fixtureFolder.isValid() ||
        !writeFixture(fixtureFolder.path(), QStringLiteral("automatic.wav"), 1,
            automaticWavPath, automaticWavHash) ||
        !writeFixture(fixtureFolder.path(), QStringLiteral("manual.wav"), 2,
            manualWavPath, manualWavHash) ||
        !writeFixture(fixtureFolder.path(), QStringLiteral("identical-a.wav"), 3,
            identicalWavPathA, identicalWavHashA) ||
        !writeFixture(fixtureFolder.path(), QStringLiteral("identical-b.wav"), 3,
            identicalWavPathB, identicalWavHashB) ||
        !writeFixture(fixtureFolder.path(), QStringLiteral("conflict-a.wav"), 4,
            conflictWavPathA, conflictWavHashA) ||
        !writeFixture(fixtureFolder.path(), QStringLiteral("conflict-b.wav"), 5,
            conflictWavPathB, conflictWavHashB) ||
        !writeFixture(fixtureFolder.path(), QStringLiteral("conflict-c.wav"), 6,
            conflictWavPathC, conflictWavHashC)) {
        return 1;
    }
    if (identicalWavHashA != identicalWavHashB ||
        conflictWavHashA == conflictWavHashB || conflictWavHashA == conflictWavHashC ||
        conflictWavHashB == conflictWavHashC) {
        fail(QStringLiteral("WAV concurrency fixtures do not have the intended identities"));
        return 1;
    }

    LoopbackPortReservations portReservations;
    QString portError;
    if (!portReservations.reserve(FourPeerCoordinator::kPeerCount, portError)) {
        fail(QStringLiteral("could not reserve four distinct TCP/UDP ports: ") + portError);
        return 1;
    }
    std::array<quint16, FourPeerCoordinator::kPeerCount> ports{};
    for (std::size_t index = 0; index < ports.size(); ++index) {
        ports[index] = portReservations.port(index);
    }

    std::array<QStringList, FourPeerCoordinator::kPeerCount> arguments;
    const bool show = qEnvironmentVariableIntValue("JAM2_TEST_SHOW_GUI") == 1;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        arguments[index] << QStringLiteral("debug") << QStringLiteral("gui-agent")
            << QStringLiteral("--instance-id")
            << QStringLiteral("content-peer-%1").arg(index + 1);
        if (show) arguments[index] << QStringLiteral("--show-gui");
    }
    FourPeerCoordinator coordinator;
    QString error;
    if (!coordinator.launch(QString::fromLocal8Bit(argv[1]), arguments, error)) {
        fail(QStringLiteral("launching four shared-content peers: ") + error);
        return 1;
    }
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (!receive(coordinator.peer(index), QStringLiteral("hello"))) return 1;
    }
    portReservations.release();

    if (!send(coordinator.peer(0), {
            {QStringLiteral("type"), QStringLiteral("jam.create")},
            {QStringLiteral("id"), QStringLiteral("create")},
            {QStringLiteral("port"), ports[0]},
        })) return 1;
    const auto created = receive(coordinator.peer(0), QStringLiteral("command_applied"));
    if (!created) return 1;
    const QString invite = created->value(QStringLiteral("invite_url")).toString();
    if (invite.isEmpty()) {
        fail(QStringLiteral("creator did not return its invite URL"));
        return 1;
    }
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        if (!send(coordinator.peer(index), {
                {QStringLiteral("type"), QStringLiteral("jam.join")},
                {QStringLiteral("id"), QStringLiteral("join-%1").arg(index + 1)},
                {QStringLiteral("port"), ports[index]},
                {QStringLiteral("invite_url"), invite},
            }) || !receive(coordinator.peer(index), QStringLiteral("command_applied"))) return 1;
    }

    std::array<QJsonObject, FourPeerCoordinator::kPeerCount> states;
    if (!waitForAll(coordinator, QStringLiteral("initial song/view reconciliation"),
            meshReady, states)) return 1;

    if (!apply(coordinator, 0, {
            {QStringLiteral("type"), QStringLiteral("invoke")},
            {QStringLiteral("id"), QStringLiteral("arrangement-open-creator")},
            {QStringLiteral("control"), QStringLiteral("looper.arrangement")},
            {QStringLiteral("operation"), QStringLiteral("click-async")},
        })) return 1;
    for (const QString& control : {
             QStringLiteral("looper.arrangement-dialog.row.0.section"),
             QStringLiteral("looper.arrangement-dialog.row.0.repeats"),
             QStringLiteral("looper.arrangement-dialog.add"),
             QStringLiteral("looper.arrangement-dialog.loop"),
             QStringLiteral("looper.arrangement-dialog.save")}) {
        if (!controlState(
                coordinator.peer(0), control,
                QStringLiteral("arrangement-creator-control"))) {
            fail(QStringLiteral("creator Arrangement dialog omitted ") + control);
            return 1;
        }
    }
    if (!apply(coordinator, 0, {
            {QStringLiteral("type"), QStringLiteral("invoke")},
            {QStringLiteral("id"), QStringLiteral("arrangement-creator-row-0-section")},
            {QStringLiteral("control"),
                QStringLiteral("looper.arrangement-dialog.row.0.section")},
            {QStringLiteral("operation"), QStringLiteral("set-index")},
            {QStringLiteral("value"), 1},
        }) || !apply(coordinator, 0, {
            {QStringLiteral("type"), QStringLiteral("invoke")},
            {QStringLiteral("id"), QStringLiteral("arrangement-creator-row-0-repeats")},
            {QStringLiteral("control"),
                QStringLiteral("looper.arrangement-dialog.row.0.repeats")},
            {QStringLiteral("operation"), QStringLiteral("set-value")},
            {QStringLiteral("value"), 2},
        }) || !apply(coordinator, 0, {
            {QStringLiteral("type"), QStringLiteral("invoke")},
            {QStringLiteral("id"), QStringLiteral("arrangement-creator-add")},
            {QStringLiteral("control"), QStringLiteral("looper.arrangement-dialog.add")},
            {QStringLiteral("operation"), QStringLiteral("click")},
        }) || !apply(coordinator, 0, {
            {QStringLiteral("type"), QStringLiteral("invoke")},
            {QStringLiteral("id"), QStringLiteral("arrangement-creator-row-1-section")},
            {QStringLiteral("control"),
                QStringLiteral("looper.arrangement-dialog.row.1.section")},
            {QStringLiteral("operation"), QStringLiteral("set-index")},
            {QStringLiteral("value"), 2},
        }) || !apply(coordinator, 0, {
            {QStringLiteral("type"), QStringLiteral("invoke")},
            {QStringLiteral("id"), QStringLiteral("arrangement-creator-row-1-repeats")},
            {QStringLiteral("control"),
                QStringLiteral("looper.arrangement-dialog.row.1.repeats")},
            {QStringLiteral("operation"), QStringLiteral("set-value")},
            {QStringLiteral("value"), 3},
        }) || !apply(coordinator, 0, {
            {QStringLiteral("type"), QStringLiteral("invoke")},
            {QStringLiteral("id"), QStringLiteral("arrangement-creator-loop")},
            {QStringLiteral("control"), QStringLiteral("looper.arrangement-dialog.loop")},
            {QStringLiteral("operation"), QStringLiteral("set-checked")},
            {QStringLiteral("value"), false},
        }) || !apply(coordinator, 0, {
            {QStringLiteral("type"), QStringLiteral("invoke")},
            {QStringLiteral("id"), QStringLiteral("arrangement-creator-save")},
            {QStringLiteral("control"), QStringLiteral("looper.arrangement-dialog.save")},
            {QStringLiteral("operation"), QStringLiteral("click")},
        }) || !waitForAll(coordinator,
            QStringLiteral("creator Arrangement-dialog convergence"),
            [](std::size_t, const QJsonObject& state) {
                return arrangementMatches(state, {{1, 2}, {2, 3}}, false);
            }, states)) return 1;

    if (!apply(coordinator, 3, {
            {QStringLiteral("type"), QStringLiteral("invoke")},
            {QStringLiteral("id"), QStringLiteral("arrangement-open-joiner")},
            {QStringLiteral("control"), QStringLiteral("looper.arrangement")},
            {QStringLiteral("operation"), QStringLiteral("click-async")},
        }) || !apply(coordinator, 3, {
            {QStringLiteral("type"), QStringLiteral("invoke")},
            {QStringLiteral("id"), QStringLiteral("arrangement-joiner-select-row-1")},
            {QStringLiteral("control"),
                QStringLiteral("looper.arrangement-dialog.row.1.repeats")},
            {QStringLiteral("operation"), QStringLiteral("set-value")},
            {QStringLiteral("value"), 4},
        }) || !apply(coordinator, 3, {
            {QStringLiteral("type"), QStringLiteral("invoke")},
            {QStringLiteral("id"), QStringLiteral("arrangement-joiner-remove-row-1")},
            {QStringLiteral("control"), QStringLiteral("looper.arrangement-dialog.remove")},
            {QStringLiteral("operation"), QStringLiteral("click")},
        }) || !apply(coordinator, 3, {
            {QStringLiteral("type"), QStringLiteral("invoke")},
            {QStringLiteral("id"), QStringLiteral("arrangement-joiner-select-row-0")},
            {QStringLiteral("control"),
                QStringLiteral("looper.arrangement-dialog.row.0.repeats")},
            {QStringLiteral("operation"), QStringLiteral("set-value")},
            {QStringLiteral("value"), 3},
        }) || !apply(coordinator, 3, {
            {QStringLiteral("type"), QStringLiteral("invoke")},
            {QStringLiteral("id"), QStringLiteral("arrangement-joiner-remove-row-0")},
            {QStringLiteral("control"), QStringLiteral("looper.arrangement-dialog.remove")},
            {QStringLiteral("operation"), QStringLiteral("click")},
        }) || !apply(coordinator, 3, {
            {QStringLiteral("type"), QStringLiteral("invoke")},
            {QStringLiteral("id"), QStringLiteral("arrangement-joiner-loop-restore")},
            {QStringLiteral("control"), QStringLiteral("looper.arrangement-dialog.loop")},
            {QStringLiteral("operation"), QStringLiteral("set-checked")},
            {QStringLiteral("value"), true},
        }) || !apply(coordinator, 3, {
            {QStringLiteral("type"), QStringLiteral("invoke")},
            {QStringLiteral("id"), QStringLiteral("arrangement-joiner-save")},
            {QStringLiteral("control"), QStringLiteral("looper.arrangement-dialog.save")},
            {QStringLiteral("operation"), QStringLiteral("click")},
        }) || !waitForAll(coordinator,
            QStringLiteral("joiner Arrangement-dialog clear convergence"),
            [](std::size_t, const QJsonObject& state) {
                return arrangementMatches(state, {}, true);
            }, states)) return 1;

    const QString title = QStringLiteral("Four Peer Content %1")
        .arg(QCoreApplication::applicationPid());
    if (!apply(coordinator, 0, {
            {QStringLiteral("type"), QStringLiteral("song.rename")},
            {QStringLiteral("id"), QStringLiteral("rename")},
            {QStringLiteral("title"), title},
        }) || !waitForAll(coordinator, QStringLiteral("title and title-view convergence"),
            [&title](std::size_t, const QJsonObject& state) {
                const QJsonObject content = state.value(QStringLiteral("content")).toObject();
                return content.value(QStringLiteral("title")).toString() == title &&
                    content.value(QStringLiteral("title_view")).toString() == title;
            }, states)) return 1;

    struct CellEdit {
        std::size_t peer;
        const char* lane;
        const char* value;
        const char* summaryField;
    };
    const std::array edits{
        CellEdit{0, "chord", "Cmaj7", "chord_0"},
        CellEdit{1, "target", "E4", "target_0"},
        CellEdit{2, "beat", "kick", "beat_0"},
        CellEdit{3, "lyric", "together", "lyric_0"},
    };
    int editNumber = 0;
    for (const CellEdit& edit : edits) {
        ++editNumber;
        const QString value = QString::fromLatin1(edit.value);
        const QString field = QString::fromLatin1(edit.summaryField);
        if (!apply(coordinator, edit.peer, {
                {QStringLiteral("type"), QStringLiteral("song.cell")},
                {QStringLiteral("id"), QStringLiteral("cell-%1").arg(editNumber)},
                {QStringLiteral("section"), 0},
                {QStringLiteral("lane"), QString::fromLatin1(edit.lane)},
                {QStringLiteral("beat"), 0},
                {QStringLiteral("value"), value},
            }) || !waitForAll(coordinator,
                QStringLiteral("%1 view edit convergence").arg(QString::fromLatin1(edit.lane)),
                [&field, &value](std::size_t, const QJsonObject& state) {
                    const QJsonObject content = state.value(QStringLiteral("content")).toObject();
                    const QJsonObject section = content.value(
                        QStringLiteral("first_section")).toObject();
                    return section.value(field).toString() == value &&
                        content.value(QStringLiteral("chord_view_section")).toInt() == 0 &&
                        content.value(QStringLiteral("beat_view_section")).toInt() == 0 &&
                        content.value(QStringLiteral("lyric_view_section")).toInt() == 0;
                }, states)) return 1;
    }

    if (!apply(coordinator, 2, {
            {QStringLiteral("type"), QStringLiteral("song.resize")},
            {QStringLiteral("id"), QStringLiteral("resize")},
            {QStringLiteral("section"), 0},
            {QStringLiteral("beats"), 36},
        }) || !waitForAll(coordinator, QStringLiteral("structural proposal reconciliation"),
            [](std::size_t, const QJsonObject& state) {
                return state.value(QStringLiteral("content")).toObject()
                    .value(QStringLiteral("first_section")).toObject()
                    .value(QStringLiteral("beats")).toInt() == 36;
            }, states)) return 1;

    if (!apply(coordinator, 3, {
            {QStringLiteral("type"), QStringLiteral("idea.generate")},
            {QStringLiteral("id"), QStringLiteral("idea-full")},
            {QStringLiteral("parts"), QStringLiteral("full")},
            {QStringLiteral("seed"), 101},
        }) || !waitForAll(coordinator, QStringLiteral("full generated idea convergence"),
            [](std::size_t, const QJsonObject& state) {
                const QJsonObject section = state.value(QStringLiteral("content")).toObject()
                    .value(QStringLiteral("first_section")).toObject();
                return section.value(QStringLiteral("generated_kind")).toString() ==
                        QStringLiteral("practice") &&
                    !section.value(QStringLiteral("chord_fingerprint")).toString().isEmpty() &&
                    !section.value(QStringLiteral("beat_fingerprint")).toString().isEmpty();
            }, states)) return 1;
    const QJsonObject fullSection = states[0].value(QStringLiteral("content")).toObject()
        .value(QStringLiteral("first_section")).toObject();
    const QString fullChord = fullSection.value(QStringLiteral("chord_fingerprint")).toString();
    const QString fullBeat = fullSection.value(QStringLiteral("beat_fingerprint")).toString();

    if (!apply(coordinator, 1, policyCommand(
            QStringLiteral("ideas-chords"), QStringLiteral("chords"))) ||
        !waitForAll(coordinator, QStringLiteral("chord-only policy convergence"),
            [](std::size_t, const QJsonObject& state) {
                return state.value(QStringLiteral("jam")).toObject()
                    .value(QStringLiteral("policy")).toObject()
                    .value(QStringLiteral("generated_ideas")).toString() ==
                    QStringLiteral("chords");
            }, states) ||
        !apply(coordinator, 0, {
            {QStringLiteral("type"), QStringLiteral("idea.generate")},
            {QStringLiteral("id"), QStringLiteral("idea-chords")},
            {QStringLiteral("parts"), QStringLiteral("chords")},
            {QStringLiteral("seed"), 202},
        }) || !waitForAll(coordinator, QStringLiteral("pitched-only idea convergence"),
            [&fullChord, &fullBeat](std::size_t, const QJsonObject& state) {
                const QJsonObject section = state.value(QStringLiteral("content")).toObject()
                    .value(QStringLiteral("first_section")).toObject();
                return section.value(QStringLiteral("chord_fingerprint")).toString() != fullChord &&
                    section.value(QStringLiteral("beat_fingerprint")).toString() == fullBeat;
            }, states)) return 1;
    const QString pitchedChord = states[0].value(QStringLiteral("content")).toObject()
        .value(QStringLiteral("first_section")).toObject()
        .value(QStringLiteral("chord_fingerprint")).toString();

    if (!apply(coordinator, 2, policyCommand(
            QStringLiteral("ideas-beats"), QStringLiteral("beats"))) ||
        !waitForAll(coordinator, QStringLiteral("beat-only policy convergence"),
            [](std::size_t, const QJsonObject& state) {
                return state.value(QStringLiteral("jam")).toObject()
                    .value(QStringLiteral("policy")).toObject()
                    .value(QStringLiteral("generated_ideas")).toString() ==
                    QStringLiteral("beats");
            }, states) ||
        !apply(coordinator, 1, {
            {QStringLiteral("type"), QStringLiteral("idea.generate")},
            {QStringLiteral("id"), QStringLiteral("idea-beats")},
            {QStringLiteral("parts"), QStringLiteral("beats")},
            {QStringLiteral("seed"), 303},
        }) || !waitForAll(coordinator, QStringLiteral("drums-only idea convergence"),
            [&pitchedChord, &fullBeat](std::size_t, const QJsonObject& state) {
                const QJsonObject section = state.value(QStringLiteral("content")).toObject()
                    .value(QStringLiteral("first_section")).toObject();
                return section.value(QStringLiteral("chord_fingerprint")).toString() == pitchedChord &&
                    section.value(QStringLiteral("beat_fingerprint")).toString() != fullBeat;
            }, states)) return 1;
    const QString beforeLocalIdeaDigest = states[0]
        .value(QStringLiteral("content")).toObject()
        .value(QStringLiteral("model_sha256")).toString();

    if (!apply(coordinator, 0, policyCommand(
            QStringLiteral("ideas-off"), QStringLiteral("off"))) ||
        !waitForAll(coordinator, QStringLiteral("local-idea policy convergence"),
            [](std::size_t, const QJsonObject& state) {
                return state.value(QStringLiteral("jam")).toObject()
                    .value(QStringLiteral("policy")).toObject()
                    .value(QStringLiteral("generated_ideas")).toString() ==
                    QStringLiteral("off");
            }, states) ||
        !apply(coordinator, 3, {
            {QStringLiteral("type"), QStringLiteral("idea.generate")},
            {QStringLiteral("id"), QStringLiteral("idea-local")},
            {QStringLiteral("parts"), QStringLiteral("full")},
            {QStringLiteral("seed"), 404},
        }) || !waitForAll(coordinator,
            QStringLiteral("exact local-only generated-idea divergence"),
            [&beforeLocalIdeaDigest](std::size_t index, const QJsonObject& state) {
                const QString digest = state.value(QStringLiteral("content")).toObject()
                    .value(QStringLiteral("model_sha256")).toString();
                return index == 3
                    ? digest != beforeLocalIdeaDigest
                    : digest == beforeLocalIdeaDigest;
            }, states, false)) return 1;

    if (!apply(coordinator, 3, policyCommand(
            QStringLiteral("ideas-full-again"), QStringLiteral("full"))) ||
        !waitForAll(coordinator, QStringLiteral("full-idea policy restoration"),
            [](std::size_t, const QJsonObject& state) {
                return state.value(QStringLiteral("jam")).toObject()
                    .value(QStringLiteral("policy")).toObject()
                    .value(QStringLiteral("generated_ideas")).toString() ==
                    QStringLiteral("full");
            }, states, false) ||
        !apply(coordinator, 3, {
            {QStringLiteral("type"), QStringLiteral("idea.generate")},
            {QStringLiteral("id"), QStringLiteral("idea-full-final")},
            {QStringLiteral("parts"), QStringLiteral("full")},
            {QStringLiteral("seed"), 505},
        }) || !waitForAll(coordinator, QStringLiteral("final full-idea reconciliation"),
            [](std::size_t, const QJsonObject& state) {
                return state.value(QStringLiteral("content")).toObject()
                    .value(QStringLiteral("first_section")).toObject()
                    .value(QStringLiteral("generated_kind")).toString() ==
                    QStringLiteral("practice");
            }, states)) return 1;

    if (!apply(coordinator, 0, {
            {QStringLiteral("type"), QStringLiteral("looper.import")},
            {QStringLiteral("id"), QStringLiteral("wav-automatic")},
            {QStringLiteral("lane"), -1},
            {QStringLiteral("source_path"), automaticWavPath},
        }) || !waitForAll(coordinator, QStringLiteral("automatic WAV transfer"),
            [&automaticWavHash](std::size_t, const QJsonObject& state) {
                return assetReady(state, automaticWavHash, true);
            }, states) || !verifyAssetFiles(states, automaticWavHash)) return 1;

    const int beforeManualLaneCount = states[2].value(QStringLiteral("content"))
        .toObject().value(QStringLiteral("lane_count")).toInt();
    const int manualLaneIndex = activeBankLaneCount(states[2]);
    if (manualLaneIndex < 0) {
        fail(QStringLiteral("could not resolve the active-bank lane count for UI WAV import"));
        return 1;
    }
    if (!apply(coordinator, 1, policyCommand(
             QStringLiteral("wav-auto-off"), QStringLiteral("full"), false)) ||
        !waitForAll(coordinator, QStringLiteral("automatic WAV policy disable"),
            [](std::size_t, const QJsonObject& state) {
                return !state.value(QStringLiteral("jam")).toObject()
                    .value(QStringLiteral("policy")).toObject()
                    .value(QStringLiteral("auto_share_wavs")).toBool();
            }, states) ||
        !apply(coordinator, 2, {
            {QStringLiteral("type"), QStringLiteral("invoke")},
            {QStringLiteral("id"), QStringLiteral("wav-ui-add-lane")},
            {QStringLiteral("control"), QStringLiteral("looper.lane.add-empty")},
            {QStringLiteral("operation"), QStringLiteral("click")},
        }) || !waitForAll(coordinator, QStringLiteral("painted add-lane convergence"),
            [beforeManualLaneCount](std::size_t, const QJsonObject& state) {
                return transferIdle(state) && state.value(QStringLiteral("content")).toObject()
                    .value(QStringLiteral("lane_count")).toInt() == beforeManualLaneCount + 1;
            }, states) ||
        !apply(coordinator, 2, {
            {QStringLiteral("type"), QStringLiteral("invoke")},
            {QStringLiteral("id"), QStringLiteral("wav-ui-drop")},
            {QStringLiteral("control"),
                QStringLiteral("looper.lane.%1.wav.drop").arg(manualLaneIndex)},
            {QStringLiteral("operation"), QStringLiteral("drop-file")},
            {QStringLiteral("value"), manualWavPath},
        }) || !waitForAll(coordinator,
            QStringLiteral("painted WAV drop metadata without automatic bytes"),
            [&manualWavHash](std::size_t index, const QJsonObject& state) {
                return assetReady(state, manualWavHash, index == 2);
            }, states)) return 1;
    const QString manualDropControl =
        QStringLiteral("looper.lane.%1.wav.drop").arg(manualLaneIndex);
    for (std::size_t index = 0; index < FourPeerCoordinator::kPeerCount; ++index) {
        const auto view = controlState(
            coordinator.peer(index),
            manualDropControl,
            QStringLiteral("wav-local-view-%1").arg(index + 1));
        if (!view || view->value(QStringLiteral("has_wav")).toBool() != (index == 2)) {
            fail(QStringLiteral("peer %1 painted WAV state did not match local-only availability")
                .arg(index + 1));
            return 1;
        }
    }
    if (!apply(coordinator, 2, {
            {QStringLiteral("type"), QStringLiteral("invoke")},
            {QStringLiteral("id"), QStringLiteral("wav-ui-share-now")},
            {QStringLiteral("control"), QStringLiteral("looper.share-now")},
            {QStringLiteral("operation"), QStringLiteral("click")},
        }) || !waitForAll(coordinator, QStringLiteral("manual WAV transfer"),
            [&manualWavHash](std::size_t, const QJsonObject& state) {
                return assetReady(state, manualWavHash, true);
            }, states) || !verifyAssetFiles(states, manualWavHash)) return 1;
    for (std::size_t index = 0; index < FourPeerCoordinator::kPeerCount; ++index) {
        const auto view = controlState(
            coordinator.peer(index),
            manualDropControl,
            QStringLiteral("wav-shared-view-%1").arg(index + 1));
        if (!view || !view->value(QStringLiteral("has_wav")).toBool()) {
            fail(QStringLiteral("peer %1 painted WAV state did not expose the shared asset")
                .arg(index + 1));
            return 1;
        }
    }
    const QJsonObject beforeRemoveContent =
        states[0].value(QStringLiteral("content")).toObject();
    const int beforeRemoveLaneCount =
        beforeRemoveContent.value(QStringLiteral("lane_count")).toInt();
    const QJsonArray beforeRemoveLaneIds =
        beforeRemoveContent.value(QStringLiteral("lane_ids")).toArray();
    const QJsonArray beforeRemoveLaneNames =
        beforeRemoveContent.value(QStringLiteral("lane_names")).toArray();
    std::array<QString, FourPeerCoordinator::kPeerCount> beforeRemoveManualPaths;
    for (std::size_t index = 0; index < beforeRemoveManualPaths.size(); ++index) {
        const QJsonObject content = states[index].value(QStringLiteral("content")).toObject();
        const int manualIndex = assetIndex(states[index], manualWavHash);
        const QJsonArray paths = content.value(QStringLiteral("lane_paths")).toArray();
        if (manualIndex < 0 || manualIndex >= paths.size()) {
            fail(QStringLiteral("peer %1 omitted the manual WAV path before removal")
                .arg(index + 1));
            return 1;
        }
        beforeRemoveManualPaths[index] = paths.at(manualIndex).toString();
    }
    if (!apply(coordinator, 2, {
            {QStringLiteral("type"), QStringLiteral("invoke")},
            {QStringLiteral("id"), QStringLiteral("wav-ui-open-remove")},
            {QStringLiteral("control"),
                QStringLiteral("looper.lane.%1.wav.remove").arg(manualLaneIndex)},
            {QStringLiteral("operation"), QStringLiteral("click-async")},
        })) return 1;
    const auto removeAccept = controlState(
        coordinator.peer(2),
        QStringLiteral("looper.wav-remove-dialog.accept"),
        QStringLiteral("wav-remove-confirm"));
    const auto removeCancel = controlState(
        coordinator.peer(2),
        QStringLiteral("looper.wav-remove-dialog.cancel"),
        QStringLiteral("wav-remove-cancel"));
    if (!removeAccept || !removeAccept->value(QStringLiteral("enabled")).toBool() ||
        !removeCancel || !removeCancel->value(QStringLiteral("enabled")).toBool()) {
        fail(QStringLiteral("WAV removal confirmation did not expose both live actions"));
        return 1;
    }
    if (!apply(coordinator, 2, {
            {QStringLiteral("type"), QStringLiteral("invoke")},
            {QStringLiteral("id"), QStringLiteral("wav-ui-confirm-remove")},
            {QStringLiteral("control"), QStringLiteral("looper.wav-remove-dialog.accept")},
            {QStringLiteral("operation"), QStringLiteral("click")},
        }) || !waitForAll(coordinator, QStringLiteral("confirmed WAV removal convergence"),
            [&manualWavHash, beforeRemoveLaneCount, &beforeRemoveLaneIds,
             &beforeRemoveLaneNames](std::size_t, const QJsonObject& state) {
                const QJsonObject content = state.value(QStringLiteral("content")).toObject();
                return transferIdle(state) && assetCount(state, manualWavHash) == 0 &&
                    content.value(QStringLiteral("lane_count")).toInt() == beforeRemoveLaneCount &&
                    content.value(QStringLiteral("lane_ids")).toArray() == beforeRemoveLaneIds &&
                    content.value(QStringLiteral("lane_names")).toArray() == beforeRemoveLaneNames;
            }, states) ||
        !removeIsolatedCopies(coordinator, beforeRemoveManualPaths, 2)) return 1;
    for (std::size_t index = 0; index < FourPeerCoordinator::kPeerCount; ++index) {
        const auto view = controlState(
            coordinator.peer(index),
            manualDropControl,
            QStringLiteral("wav-removed-view-%1").arg(index + 1));
        if (!view || view->value(QStringLiteral("has_wav")).toBool()) {
            fail(QStringLiteral("peer %1 painted WAV state retained the removed asset")
                .arg(index + 1));
            return 1;
        }
    }
    if (!apply(coordinator, 2, {
            {QStringLiteral("type"), QStringLiteral("invoke")},
            {QStringLiteral("id"), QStringLiteral("wav-ui-redrop-after-remove")},
            {QStringLiteral("control"), manualDropControl},
            {QStringLiteral("operation"), QStringLiteral("drop-file")},
            {QStringLiteral("value"), manualWavPath},
        }) || !waitForAll(coordinator,
            QStringLiteral("same-byte WAV re-import after removal"),
            [&manualWavHash, beforeRemoveLaneCount, &beforeRemoveLaneIds,
             &beforeRemoveLaneNames](std::size_t index, const QJsonObject& state) {
                const QJsonObject content = state.value(QStringLiteral("content")).toObject();
                return assetReady(state, manualWavHash, index == 2) &&
                    assetCount(state, manualWavHash) == 1 &&
                    content.value(QStringLiteral("lane_count")).toInt() == beforeRemoveLaneCount &&
                    content.value(QStringLiteral("lane_ids")).toArray() == beforeRemoveLaneIds &&
                    content.value(QStringLiteral("lane_names")).toArray() == beforeRemoveLaneNames;
            }, states) ||
        !apply(coordinator, 2, {
            {QStringLiteral("type"), QStringLiteral("looper.transfer.pause")},
            {QStringLiteral("id"),
                QStringLiteral("wav-reshare-pause-source-validation")},
            {QStringLiteral("point"), QStringLiteral("outgoing-validation")},
        }) ||
        !apply(coordinator, 2, {
            {QStringLiteral("type"), QStringLiteral("invoke")},
            {QStringLiteral("id"), QStringLiteral("wav-ui-reshare-after-remove")},
            {QStringLiteral("control"), QStringLiteral("looper.share-now")},
            {QStringLiteral("operation"), QStringLiteral("click")},
        }) || !waitForAll(coordinator,
            QStringLiteral("delayed source validation to become active"),
            [](std::size_t index, const QJsonObject& state) {
                if (index != 2) return true;
                return state.value(QStringLiteral("content")).toObject()
                    .value(QStringLiteral("transfer")).toObject()
                    .value(QStringLiteral("pause_active")).toString() ==
                    QStringLiteral("outgoing-validation");
            }, states, false) || !apply(coordinator, 2, {
            {QStringLiteral("type"), QStringLiteral("looper.transfer.release")},
            {QStringLiteral("id"), QStringLiteral("wav-reshare-release-source-validation")},
        }) || !waitForAll(coordinator,
            QStringLiteral("same-byte WAV re-share after removal"),
            [&manualWavHash, beforeRemoveLaneCount, &beforeRemoveLaneIds](
                std::size_t, const QJsonObject& state) {
                const QJsonObject content = state.value(QStringLiteral("content")).toObject();
                return assetReady(state, manualWavHash, true) &&
                    assetCount(state, manualWavHash) == 1 &&
                    content.value(QStringLiteral("lane_count")).toInt() == beforeRemoveLaneCount &&
                    content.value(QStringLiteral("lane_ids")).toArray() == beforeRemoveLaneIds;
            }, states) || !verifyAssetFiles(states, manualWavHash)) return 1;
    for (std::size_t index = 0; index < FourPeerCoordinator::kPeerCount; ++index) {
        const auto view = controlState(
            coordinator.peer(index),
            manualDropControl,
            QStringLiteral("wav-reimported-view-%1").arg(index + 1));
        if (!view || !view->value(QStringLiteral("has_wav")).toBool()) {
            fail(QStringLiteral("peer %1 painted WAV state omitted the re-imported asset")
                .arg(index + 1));
            return 1;
        }
    }
    if (!apply(coordinator, 0, policyCommand(
            QStringLiteral("wav-auto-restored"), QStringLiteral("full"))) ||
        !waitForAll(coordinator, QStringLiteral("automatic WAV policy restoration"),
            [](std::size_t, const QJsonObject& state) {
                return state.value(QStringLiteral("jam")).toObject()
                    .value(QStringLiteral("policy")).toObject()
                    .value(QStringLiteral("auto_share_wavs")).toBool();
            }, states)) return 1;

    const int beforeIdenticalLanes = states[0].value(QStringLiteral("content"))
        .toObject().value(QStringLiteral("lane_count")).toInt();
    if (!applyConcurrently(coordinator, std::array{
            std::pair<std::size_t, QJsonObject>{0, {
                {QStringLiteral("type"), QStringLiteral("looper.import")},
                {QStringLiteral("id"), QStringLiteral("same-bytes-peer-1")},
                {QStringLiteral("lane"), -1},
                {QStringLiteral("source_path"), identicalWavPathA},
            }},
            std::pair<std::size_t, QJsonObject>{1, {
                {QStringLiteral("type"), QStringLiteral("looper.import")},
                {QStringLiteral("id"), QStringLiteral("same-bytes-peer-2")},
                {QStringLiteral("lane"), -1},
                {QStringLiteral("source_path"), identicalWavPathB},
            }},
        }) || !waitForAll(coordinator,
            QStringLiteral("simultaneous identical-byte WAV deduplication"),
            [&identicalWavHashA, beforeIdenticalLanes](
                std::size_t, const QJsonObject& state) {
                const QJsonObject content = state.value(QStringLiteral("content")).toObject();
                return transferIdle(state) && assetCount(state, identicalWavHashA) == 1 &&
                    content.value(QStringLiteral("lane_count")).toInt() ==
                        beforeIdenticalLanes + 1;
            }, states) ||
        !verifyIsolatedAssetFiles(
            coordinator, states, {identicalWavHashA})) return 1;

    const QJsonArray hashesBeforeConflict = states[0].value(QStringLiteral("content"))
        .toObject().value(QStringLiteral("asset_hashes")).toArray();
    const int beforeConflictLanes = states[0].value(QStringLiteral("content"))
        .toObject().value(QStringLiteral("lane_count")).toInt();
    if (!applyConcurrently(coordinator, std::array{
            std::pair<std::size_t, QJsonObject>{1, {
                {QStringLiteral("type"), QStringLiteral("looper.import")},
                {QStringLiteral("id"), QStringLiteral("conflict-peer-2")},
                {QStringLiteral("lane"), 0},
                {QStringLiteral("source_path"), conflictWavPathA},
            }},
            std::pair<std::size_t, QJsonObject>{2, {
                {QStringLiteral("type"), QStringLiteral("looper.import")},
                {QStringLiteral("id"), QStringLiteral("conflict-peer-3")},
                {QStringLiteral("lane"), 0},
                {QStringLiteral("source_path"), conflictWavPathB},
            }},
            std::pair<std::size_t, QJsonObject>{3, {
                {QStringLiteral("type"), QStringLiteral("looper.import")},
                {QStringLiteral("id"), QStringLiteral("conflict-peer-4")},
                {QStringLiteral("lane"), 0},
                {QStringLiteral("source_path"), conflictWavPathC},
            }},
        }) || !waitForAll(coordinator,
            QStringLiteral("simultaneous occupied-lane conflict reconciliation"),
            [&conflictWavHashA, &conflictWavHashB, &conflictWavHashC,
             &hashesBeforeConflict, beforeConflictLanes](
                std::size_t, const QJsonObject& state) {
                const QJsonObject content = state.value(QStringLiteral("content")).toObject();
                if (!transferIdle(state) || assetCount(state, conflictWavHashA) != 1 ||
                    assetCount(state, conflictWavHashB) != 1 ||
                    assetCount(state, conflictWavHashC) != 1 ||
                    content.value(QStringLiteral("lane_count")).toInt() !=
                        beforeConflictLanes + 2) {
                    return false;
                }
                for (const QJsonValue& previous : hashesBeforeConflict) {
                    const QString hash = previous.toString();
                    if (!hash.isEmpty() && assetCount(state, hash) != 1) return false;
                }
                return true;
            }, states) ||
        !verifyIsolatedAssetFiles(coordinator, states, {
            automaticWavHash,
            manualWavHash,
            identicalWavHashA,
            conflictWavHashA,
            conflictWavHashB,
            conflictWavHashC,
        })) return 1;

    const QJsonObject stableContent = states[0].value(QStringLiteral("content")).toObject();
    const QString stableDigest = stableContent.value(QStringLiteral("model_sha256")).toString();
    const QJsonArray stableHashOrder = stableContent.value(QStringLiteral("asset_hashes")).toArray();
    const QJsonArray stableLaneOrder = stableContent.value(QStringLiteral("lane_ids")).toArray();
    const QJsonArray stableLaneNames = stableContent.value(QStringLiteral("lane_names")).toArray();
    const int stableLaneCount = stableContent.value(QStringLiteral("lane_count")).toInt();
    if (!applyConcurrently(coordinator, std::array{
            std::pair<std::size_t, QJsonObject>{0, {
                {QStringLiteral("type"), QStringLiteral("looper.share")},
                {QStringLiteral("id"), QStringLiteral("repeat-share-peer-1")},
            }},
            std::pair<std::size_t, QJsonObject>{1, {
                {QStringLiteral("type"), QStringLiteral("looper.share")},
                {QStringLiteral("id"), QStringLiteral("repeat-share-peer-2")},
            }},
            std::pair<std::size_t, QJsonObject>{2, {
                {QStringLiteral("type"), QStringLiteral("looper.share")},
                {QStringLiteral("id"), QStringLiteral("repeat-share-peer-3")},
            }},
            std::pair<std::size_t, QJsonObject>{3, {
                {QStringLiteral("type"), QStringLiteral("looper.share")},
                {QStringLiteral("id"), QStringLiteral("repeat-share-peer-4")},
            }},
        }) || !waitForAll(coordinator,
            QStringLiteral("all-peer repeated Track Sync idempotence"),
            [&stableDigest, &stableHashOrder, &stableLaneOrder,
             &stableLaneNames, stableLaneCount](std::size_t, const QJsonObject& state) {
                const QJsonObject content = state.value(QStringLiteral("content")).toObject();
                return transferIdle(state) &&
                    content.value(QStringLiteral("model_sha256")).toString() == stableDigest &&
                    content.value(QStringLiteral("lane_count")).toInt() == stableLaneCount &&
                    content.value(QStringLiteral("asset_hashes")).toArray() == stableHashOrder &&
                    content.value(QStringLiteral("lane_ids")).toArray() == stableLaneOrder &&
                    content.value(QStringLiteral("lane_names")).toArray() == stableLaneNames;
            }, states) || !verifyNoPartialFiles(coordinator)) return 1;

    if (!applyConcurrently(coordinator, std::array{
            std::pair<std::size_t, QJsonObject>{0, {
                {QStringLiteral("type"), QStringLiteral("invoke")},
                {QStringLiteral("id"), QStringLiteral("save-peer-1")},
                {QStringLiteral("control"), QStringLiteral("session.save")},
                {QStringLiteral("operation"), QStringLiteral("click")},
            }},
            std::pair<std::size_t, QJsonObject>{1, {
                {QStringLiteral("type"), QStringLiteral("invoke")},
                {QStringLiteral("id"), QStringLiteral("save-peer-2")},
                {QStringLiteral("control"), QStringLiteral("session.save")},
                {QStringLiteral("operation"), QStringLiteral("click")},
            }},
            std::pair<std::size_t, QJsonObject>{2, {
                {QStringLiteral("type"), QStringLiteral("invoke")},
                {QStringLiteral("id"), QStringLiteral("save-peer-3")},
                {QStringLiteral("control"), QStringLiteral("session.save")},
                {QStringLiteral("operation"), QStringLiteral("click")},
            }},
            std::pair<std::size_t, QJsonObject>{3, {
                {QStringLiteral("type"), QStringLiteral("invoke")},
                {QStringLiteral("id"), QStringLiteral("save-peer-4")},
                {QStringLiteral("control"), QStringLiteral("session.save")},
                {QStringLiteral("operation"), QStringLiteral("click")},
            }},
        }) || !waitForAll(coordinator,
            QStringLiteral("four-peer portable JamJar save completion"),
            [](std::size_t, const QJsonObject& state) {
                const QJsonObject content = state
                    .value(QStringLiteral("content")).toObject();
                return content.value(QStringLiteral("storage_saved")).toBool() &&
                    !content.value(QStringLiteral("storage_has_artifacts")).toBool() &&
                    !content.value(QStringLiteral("unsaved_changes")).toBool() &&
                    !content.value(QStringLiteral("project_path")).toString().isEmpty() &&
                    content.value(QStringLiteral("file_tasks_active")).toInt() == 0 &&
                    transferIdle(state);
            }, states) ||
        !verifySavedProjects(coordinator, states, {
            automaticWavHash,
            manualWavHash,
            identicalWavHashA,
            conflictWavHashA,
            conflictWavHashB,
            conflictWavHashC,
        }) || !verifyNoPartialFiles(coordinator)) return 1;

    if (!shutDown(coordinator) || !verifyNoPartialFiles(coordinator)) return 1;
    if (failures != 0) {
        std::cerr << failures << " four-peer shared-content checks failed\n";
        return 1;
    }
    coordinator.markSuccessful();
    std::cout << "four-peer shared-content checks passed\n";
    return 0;
}

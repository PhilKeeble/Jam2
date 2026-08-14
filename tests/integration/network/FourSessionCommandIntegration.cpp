#include "AutomationProcess.hpp"
#include "LoopbackPortReservations.hpp"
#include "TestTiming.hpp"

#include "engine.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>

namespace {

using namespace std::chrono_literals;
constexpr std::size_t kPeerCount = 4;
constexpr int kSampleRate = 48000;

QString deterministicHex(int seed, int bytes)
{
    QByteArray result;
    result.reserve(bytes * 2);
    for (int index = 0; index < bytes; ++index) {
        const unsigned value = static_cast<unsigned>(
            (seed + index * 37) & 0xff);
        result.append(QByteArray::number(value, 16).rightJustified(2, '0'));
    }
    return QString::fromLatin1(result);
}

bool writeJson(const QString& path, const QJsonObject& object, QString& error)
{
    QFile file(path);
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(bytes) != bytes.size() || !file.flush()) {
        error = QStringLiteral("writing %1: %2").arg(path, file.errorString());
        return false;
    }
    return true;
}

bool readJson(const QString& path, QJsonObject& object, QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        error = QStringLiteral("reading %1: %2").arg(path, file.errorString());
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(
        file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        error = QStringLiteral("parsing %1: %2").arg(
            path, parseError.errorString());
        return false;
    }
    object = document.object();
    return true;
}

bool send(AutomationProcess& process, QJsonObject command, QString& error)
{
    if (process.send(std::move(command), error)) {
        return true;
    }
    error = QStringLiteral("sending reactive command: ") + error;
    return false;
}

bool waitForEvent(
    AutomationProcess& process,
    std::chrono::milliseconds timeout,
    const std::function<bool(const QJsonObject&)>& predicate,
    QJsonObject& result,
    QString& error)
{
    const auto deadline = std::chrono::steady_clock::now() +
        jam2::test::scaledTimeout(timeout);
    QString lastEvent;
    while (std::chrono::steady_clock::now() < deadline) {
        QJsonObject event;
        QString readError;
        if (!process.readEvent(event, 250ms, readError)) {
            if (!process.isRunning()) {
                error = QStringLiteral("Jam2 exited while waiting for an event: ") +
                    readError;
                return false;
            }
            continue;
        }
        lastEvent = QString::fromUtf8(
            QJsonDocument(event).toJson(QJsonDocument::Compact));
        if (predicate(event)) {
            result = event;
            return true;
        }
    }
    error = QStringLiteral("timed out waiting for an event; last event=%1")
        .arg(lastEvent);
    return false;
}

bool waitForNamedEvent(
    AutomationProcess& process,
    const QString& eventName,
    const QString& id,
    std::chrono::milliseconds timeout,
    QJsonObject& result,
    QString& error)
{
    return waitForEvent(
        process,
        timeout,
        [&](const QJsonObject& event) {
            return event.value(QStringLiteral("event")).toString() == eventName &&
                (id.isEmpty() ||
                 event.value(QStringLiteral("id")).toString() == id);
        },
        result,
        error);
}

bool waitForNamedEventOrRejection(
    AutomationProcess& process,
    const QString& eventName,
    const QString& id,
    std::chrono::milliseconds timeout,
    QJsonObject& result,
    QString& error)
{
    if (!waitForEvent(
            process,
            timeout,
            [&](const QJsonObject& event) {
                const QString currentEvent =
                    event.value(QStringLiteral("event")).toString();
                return event.value(QStringLiteral("id")).toString() == id &&
                    (currentEvent == eventName ||
                     currentEvent == QStringLiteral("command_rejected"));
            },
            result,
            error)) {
        return false;
    }
    if (result.value(QStringLiteral("event")).toString() ==
        QStringLiteral("command_rejected")) {
        error = QStringLiteral("command was rejected: %1")
            .arg(result.value(QStringLiteral("reason")).toString());
        return false;
    }
    return true;
}

QJsonObject scenario(
    int index,
    quint16 port,
    quint16 creatorPort,
    const QString& sessionId,
    const QString& sessionKey,
    const QString& peerToken,
    const QString& artifactRoot)
{
    QJsonObject network{
        {QStringLiteral("bind"),
            QStringLiteral("127.0.0.1:%1").arg(port)},
        {QStringLiteral("no_stun"), true},
        {QStringLiteral("wait_ms"), 15000},
        {QStringLiteral("peer_token"), peerToken},
    };
    if (index == 0) {
        network.insert(QStringLiteral("session_id"), sessionId);
        network.insert(QStringLiteral("session_key"), sessionKey);
        network.insert(QStringLiteral("max_peers"), static_cast<int>(kPeerCount));
    } else {
        network.insert(
            QStringLiteral("join_url"),
            QStringLiteral(
                "jam2://v1?endpoint=127.0.0.1:%1&session=%2&key=%3")
                .arg(creatorPort)
                .arg(sessionId, sessionKey));
    }
    return {
        {QStringLiteral("schema"), QStringLiteral("jam2-debug-scenario")},
        {QStringLiteral("run_id"),
            QStringLiteral("session-command-peer-%1").arg(index + 1)},
        {QStringLiteral("operation"), index == 0
            ? QStringLiteral("network.create")
            : QStringLiteral("network.join")},
        {QStringLiteral("profile"), QStringLiteral("fast")},
        {QStringLiteral("runtime"), QJsonObject{
            {QStringLiteral("headless_audio"), true},
            {QStringLiteral("sample_rate"), kSampleRate},
            {QStringLiteral("audio_buffer_size"), 64},
            {QStringLiteral("frame_size"), 64},
            {QStringLiteral("network_audio_format"), QStringLiteral("pcm16")},
            {QStringLiteral("stats"), true},
            {QStringLiteral("stats_interval_ms"), 100},
            {QStringLiteral("stream_ms"), 0},
            {QStringLiteral("test_input"), QStringLiteral("silence")},
            {QStringLiteral("metronome"), true},
            {QStringLiteral("bpm"), 120},
            {QStringLiteral("metronome_mode"), QStringLiteral("shared-grid")},
        }},
        {QStringLiteral("artifacts"), QJsonObject{
            {QStringLiteral("root"), artifactRoot}}},
        {QStringLiteral("network"), network},
        {QStringLiteral("automation"), QJsonObject{
            {QStringLiteral("reactive"), true},
            {QStringLiteral("controller_loss"), QStringLiteral("stop")},
        }},
        {QStringLiteral("actions"), QJsonArray{}},
    };
}

bool countInSnapshotValid(
    const QJsonObject& snapshot,
    bool requireCountdownStarted)
{
    const auto frame = static_cast<std::uint64_t>(
        snapshot.value(QStringLiteral("engine_frame")).toDouble());
    const auto countdown = static_cast<std::uint64_t>(snapshot
        .value(QStringLiteral("transport_countdown_start_frame")).toDouble());
    const auto target = static_cast<std::uint64_t>(snapshot
        .value(QStringLiteral("transport_target_frame")).toDouble());
    const auto countInStart = static_cast<std::uint64_t>(snapshot
        .value(QStringLiteral("count_in_start_frame")).toDouble());
    const auto countInTarget = static_cast<std::uint64_t>(snapshot
        .value(QStringLiteral("count_in_target_frame")).toDouble());
    const auto musicalTarget = static_cast<std::uint64_t>(snapshot
        .value(QStringLiteral("transport_musical_frame")).toDouble());
    const auto epoch = static_cast<std::uint64_t>(snapshot
        .value(QStringLiteral("metronome_epoch_frame")).toDouble());
    const std::uint64_t targetPhase = musicalTarget >= epoch
        ? musicalTarget - epoch
        : 0ULL;
    const std::uint64_t barRemainder = targetPhase % 96000ULL;
    const std::uint64_t barBoundaryDifference = std::min(
        barRemainder, 96000ULL - barRemainder);
    return snapshot.value(QStringLiteral("metronome_epoch_valid")).toBool() &&
        snapshot.value(QStringLiteral("transport_pending")).toBool() &&
        snapshot.value(QStringLiteral("recording_count_in_active")).toBool() &&
        !snapshot.value(QStringLiteral("playback_count_in_active")).toBool() &&
        snapshot.value(QStringLiteral("transport_action")).toInt() ==
            static_cast<int>(jam2::EngineTransportAction::RecordStart) &&
        countdown > 0 && target > countdown && target > frame &&
        target - countdown == 96000ULL &&
        musicalTarget >= epoch && barBoundaryDifference <= 64ULL &&
        countInStart == countdown && countInTarget == target &&
        (!requireCountdownStarted || frame >= countdown);
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    if (argc != 2) {
        std::cerr << "usage: jam2_four_session_command_integration <release-jam2>\n";
        return 2;
    }
    const QString executable = QFileInfo(
        QString::fromLocal8Bit(argv[1])).absoluteFilePath();
    QTemporaryDir root(
        QDir::temp().filePath(QStringLiteral("jam2-session-command-XXXXXX")));
    auto fail = [&](const QString& message) {
        root.setAutoRemove(false);
        std::cerr << message.toStdString() << "\nartifacts retained at "
                  << root.path().toStdString() << '\n';
        return 1;
    };
    if (!QFileInfo(executable).isExecutable() || !root.isValid()) {
        return fail(QStringLiteral("Jam2 executable or test root is unavailable"));
    }

    QString error;
    LoopbackPortReservations reservations;
    if (!reservations.reserve(kPeerCount, error)) {
        return fail(error);
    }
    std::array<quint16, kPeerCount> ports{};
    for (std::size_t index = 0; index < kPeerCount; ++index) {
        ports[index] = reservations.port(index);
    }
    const QString sessionId = deterministicHex(7, 8);
    const QString sessionKey = deterministicHex(11, 16);
    std::array<QString, kPeerCount> scenarioPaths{};
    std::array<QString, kPeerCount> manifestPaths{};
    for (std::size_t index = 0; index < kPeerCount; ++index) {
        const QString peerRoot = QDir(root.path()).filePath(
            QStringLiteral("peer-%1").arg(index + 1));
        if (!QDir().mkpath(peerRoot)) {
            return fail(QStringLiteral("creating peer artifact root failed"));
        }
        scenarioPaths[index] = QDir(peerRoot).filePath(
            QStringLiteral("scenario.json"));
        manifestPaths[index] = QDir(peerRoot).filePath(
            QStringLiteral("native-manifest.json"));
        if (!writeJson(
                scenarioPaths[index],
                scenario(
                    static_cast<int>(index),
                    ports[index],
                    ports[0],
                    sessionId,
                    sessionKey,
                    deterministicHex(31 + static_cast<int>(index), 16),
                    peerRoot),
                error)) {
            return fail(error);
        }
    }

    reservations.release();
    std::array<std::unique_ptr<AutomationProcess>, kPeerCount> peers{};
    for (std::size_t index = 0; index < kPeerCount; ++index) {
        peers[index] = AutomationProcess::launch(
            executable,
            {QStringLiteral("debug"), QStringLiteral("run"), scenarioPaths[index]},
            error);
        if (!peers[index]) {
            return fail(QStringLiteral("launching peer %1: %2")
                .arg(index + 1).arg(error));
        }
    }

    for (std::size_t index = 0; index < kPeerCount; ++index) {
        QJsonObject connected;
        if (!waitForEvent(
                *peers[index],
                20s,
                [](const QJsonObject& event) {
                    return event.value(QStringLiteral("event")).toString() ==
                            QStringLiteral("peer_snapshot") &&
                        event.value(QStringLiteral("remote_peer_count")).toInt() == 3 &&
                        event.value(QStringLiteral("network_attachment_ready")).toBool();
                },
                connected,
                error)) {
            return fail(QStringLiteral("peer %1 full-mesh readiness: %2")
                .arg(index + 1).arg(error));
        }
    }

    if (!send(*peers[0], {
            {QStringLiteral("type"), QStringLiteral("unsupported")},
            {QStringLiteral("id"), QStringLiteral("invalid-frame")},
        }, error)) {
        return fail(error);
    }
    QJsonObject rejected;
    if (!waitForNamedEvent(
            *peers[0],
            QStringLiteral("command_rejected"),
            QStringLiteral("invalid-frame"),
            2s,
            rejected,
            error)) {
        return fail(QStringLiteral("invalid reactive command rejection: ") + error);
    }

    for (std::size_t index = 0; index < kPeerCount; ++index) {
        const QString id = QStringLiteral("gain-%1").arg(index + 1);
        if (!send(*peers[index], {
                {QStringLiteral("type"), QStringLiteral("action")},
                {QStringLiteral("action"), QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("remote.level")},
                    {QStringLiteral("id"), id},
                    {QStringLiteral("value"), 0.25},
                }},
            }, error)) {
            return fail(error);
        }
    }
    std::uint64_t creatorGainAppliedFrame = 0;
    for (std::size_t index = 0; index < kPeerCount; ++index) {
        QJsonObject applied;
        if (!waitForNamedEvent(
                *peers[index],
                QStringLiteral("command_applied"),
                QStringLiteral("gain-%1").arg(index + 1),
                2s,
                applied,
                error)) {
            return fail(QStringLiteral("peer %1 gain application: %2")
                .arg(index + 1).arg(error));
        }
        if (index == 0) {
            creatorGainAppliedFrame = static_cast<std::uint64_t>(
                applied.value(QStringLiteral("applied_frame")).toDouble());
        }
    }

    if (!send(*peers[0], {
            {QStringLiteral("type"), QStringLiteral("snapshot")},
            {QStringLiteral("id"), QStringLiteral("delayed-snapshot")},
            {QStringLiteral("delay_frames"), 512},
        }, error)) {
        return fail(error);
    }
    QJsonObject delayedSnapshot;
    if (!waitForNamedEventOrRejection(
            *peers[0],
            QStringLiteral("snapshot"),
            QStringLiteral("delayed-snapshot"),
            2s,
            delayedSnapshot,
            error) ||
        delayedSnapshot.value(QStringLiteral("remote_level_ppm")).toInt() != 250000 ||
        static_cast<std::uint64_t>(delayedSnapshot
            .value(QStringLiteral("engine_frame")).toDouble()) <
            creatorGainAppliedFrame + 512ULL) {
        return fail(QStringLiteral("scheduled snapshot/gain state failed: ") + error);
    }

    if (!send(*peers[0], {
            {QStringLiteral("type"), QStringLiteral("action")},
            {QStringLiteral("action"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("track.record-start")},
                {QStringLiteral("id"), QStringLiteral("count-in")},
                {QStringLiteral("count_in_bars"), 1},
            }},
        }, error)) {
        return fail(error);
    }
    QJsonObject countInApplied;
    if (!waitForNamedEvent(
            *peers[0],
            QStringLiteral("command_applied"),
            QStringLiteral("count-in"),
            5s,
            countInApplied,
            error)) {
        return fail(QStringLiteral("count-in transport application: ") + error);
    }
    const std::uint64_t countInAppliedFrame = static_cast<std::uint64_t>(
        countInApplied.value(QStringLiteral("applied_frame")).toDouble());

    std::array<QJsonObject, kPeerCount> countInSnapshots{};
    bool allScheduled = false;
    for (int attempt = 0; attempt < 8 && !allScheduled; ++attempt) {
        allScheduled = true;
        std::array<QString, kPeerCount> ids{};
        for (std::size_t index = 0; index < kPeerCount; ++index) {
            ids[index] = QStringLiteral("scheduled-count-in-%1-%2")
                .arg(attempt).arg(index + 1);
            if (!send(*peers[index], {
                    {QStringLiteral("type"), QStringLiteral("snapshot")},
                    {QStringLiteral("id"), ids[index]},
                    {QStringLiteral("delay_frames"), 512},
                }, error)) {
                return fail(error);
            }
        }
        for (std::size_t index = 0; index < kPeerCount; ++index) {
            if (!waitForNamedEventOrRejection(
                    *peers[index],
                    QStringLiteral("snapshot"),
                    ids[index],
                    1s,
                    countInSnapshots[index],
                    error)) {
                return fail(QStringLiteral("peer %1 scheduled count-in snapshot: %2")
                    .arg(index + 1).arg(error));
            }
            allScheduled = allScheduled &&
                countInSnapshotValid(countInSnapshots[index], false);
        }
    }
    if (!allScheduled) {
        QString evidence;
        for (std::size_t index = 0; index < kPeerCount; ++index) {
            evidence += QStringLiteral("\npeer %1: %2")
                .arg(index + 1)
                .arg(QString::fromUtf8(QJsonDocument(countInSnapshots[index])
                    .toJson(QJsonDocument::Compact)));
        }
        return fail(QStringLiteral(
            "the future one-bar recording schedule did not reach all four peers") +
            evidence);
    }

    const std::uint64_t creatorCountdown = static_cast<std::uint64_t>(
        countInSnapshots[0]
            .value(QStringLiteral("transport_countdown_start_frame")).toDouble());
    if (creatorCountdown <= countInAppliedFrame) {
        return fail(QStringLiteral(
            "the recording schedule was not published before its countdown boundary"));
    }
    if (!send(*peers[0], {
            {QStringLiteral("type"), QStringLiteral("snapshot")},
            {QStringLiteral("id"), QStringLiteral("countdown-boundary")},
            {QStringLiteral("apply_frame"), static_cast<double>(creatorCountdown)},
        }, error)) {
        return fail(error);
    }
    QJsonObject boundarySnapshot;
    if (!waitForNamedEventOrRejection(
            *peers[0],
            QStringLiteral("snapshot"),
            QStringLiteral("countdown-boundary"),
            10s,
            boundarySnapshot,
            error) ||
        !countInSnapshotValid(boundarySnapshot, true)) {
        return fail(QStringLiteral("creator countdown boundary: ") + error);
    }

    for (std::size_t index = 0; index < kPeerCount; ++index) {
        const QString id = QStringLiteral("active-count-in-%1").arg(index + 1);
        if (!send(*peers[index], {
                {QStringLiteral("type"), QStringLiteral("snapshot")},
                {QStringLiteral("id"), id},
                {QStringLiteral("delay_frames"), 2048},
            }, error)) {
            return fail(error);
        }
        QJsonObject activeSnapshot;
        if (!waitForNamedEventOrRejection(
                *peers[index],
                QStringLiteral("snapshot"),
                id,
                1s,
                activeSnapshot,
                error) ||
            !countInSnapshotValid(activeSnapshot, true)) {
            return fail(QStringLiteral("peer %1 active count-in: %2 snapshot=%3")
                .arg(index + 1)
                .arg(error)
                .arg(QString::fromUtf8(QJsonDocument(activeSnapshot)
                    .toJson(QJsonDocument::Compact))));
        }
    }

    const std::size_t rejectedPeer = kPeerCount - 1;
    if (!send(*peers[0], {
            {QStringLiteral("type"), QStringLiteral("debug.session-error")},
            {QStringLiteral("id"), QStringLiteral("session-error")},
            {QStringLiteral("target_peer_token"),
                deterministicHex(31 + static_cast<int>(rejectedPeer), 16)},
        }, error)) {
        return fail(error);
    }
    QJsonObject injected;
    if (!waitForNamedEvent(
            *peers[0],
            QStringLiteral("command_applied"),
            QStringLiteral("session-error"),
            2s,
            injected,
            error)) {
        return fail(QStringLiteral("injecting coordinator session error: ") + error);
    }

    for (std::size_t index = 0; index < rejectedPeer; ++index) {
        if (!send(*peers[index], {
                {QStringLiteral("type"), QStringLiteral("shutdown")},
                {QStringLiteral("id"),
                    QStringLiteral("shutdown-%1").arg(index + 1)},
            }, error)) {
            return fail(error);
        }
    }
    for (std::size_t index = 0; index < kPeerCount; ++index) {
        if (index != rejectedPeer) {
            QJsonObject shutdown;
            if (!waitForNamedEvent(
                    *peers[index],
                    QStringLiteral("shutdown"),
                    QString(),
                    10s,
                    shutdown,
                    error)) {
                return fail(QStringLiteral("peer %1 shutdown event: %2")
                    .arg(index + 1).arg(error));
            }
        }
        int exitCode = -1;
        const int expectedExitCode = index == rejectedPeer ? 4 : 0;
        if (!peers[index]->waitForExit(10s, exitCode, error) ||
            exitCode != expectedExitCode) {
            return fail(QStringLiteral("peer %1 exit: %2 code=%3")
                .arg(index + 1).arg(error).arg(exitCode));
        }
        QJsonObject manifest;
        if (!readJson(manifestPaths[index], manifest, error)) {
            return fail(error);
        }
        const QJsonObject result = manifest.value(QStringLiteral("result")).toObject();
        const int rejectedCommands = result
            .value(QStringLiteral("commands_rejected")).toInt(-1);
        if (manifest.value(QStringLiteral("ok")).toBool() !=
                (index != rejectedPeer) ||
            result.value(QStringLiteral("return_code")).toInt(-1) !=
                expectedExitCode ||
            result.value(QStringLiteral("remote_peer_count")).toInt(-1) != 3 ||
            result.value(QStringLiteral("control_max_active_remote_peers")).toInt(-1) != 3 ||
            result.value(QStringLiteral("automation_command_queue_high_water")).toInt() < 1 ||
            result.value(QStringLiteral("automation_command_queue_pending")).toInt(-1) != 0 ||
            result.value(QStringLiteral("pending_controller_actions")).toInt(-1) != 0 ||
            (index == 0 ? rejectedCommands != 1 : rejectedCommands != 0)) {
            return fail(QStringLiteral("peer %1 final reactive-controller evidence failed")
                .arg(index + 1));
        }
    }

    std::cout << "four-peer reactive session command checks passed\n";
    return 0;
}

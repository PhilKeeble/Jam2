#include "FourPeerCoordinator.hpp"
#include "LoopbackPortReservations.hpp"
#include "TestTiming.hpp"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QThread>

#include <array>
#include <chrono>
#include <cstddef>
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
    std::chrono::milliseconds timeout = 20s)
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

bool invoke(
    AutomationProcess& process,
    const QString& id,
    const QString& control,
    const QString& operation,
    QJsonValue value = {})
{
    QJsonObject command{
        {QStringLiteral("type"), QStringLiteral("invoke")},
        {QStringLiteral("id"), id},
        {QStringLiteral("control"), control},
        {QStringLiteral("operation"), operation},
    };
    if (!value.isUndefined()) command.insert(QStringLiteral("value"), std::move(value));
    if (!send(process, std::move(command))) return false;
    const auto applied = receive(process, QStringLiteral("command_applied"));
    if (!applied) return false;
    if (applied->value(QStringLiteral("id")).toString() == id) return true;
    fail(QStringLiteral("expected applied id %1, received %2")
        .arg(id, applied->value(QStringLiteral("id")).toString()));
    return false;
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

std::optional<QJsonObject> jamSnapshot(AutomationProcess& process, int peer, int sequence)
{
    if (!send(process, {
            {QStringLiteral("type"), QStringLiteral("snapshot")},
            {QStringLiteral("id"), QStringLiteral("jam-state-%1-%2").arg(peer).arg(sequence)},
            {QStringLiteral("cursor"), 0},
        })) return std::nullopt;
    const auto event = receive(process, QStringLiteral("snapshot"));
    if (!event || !event->value(QStringLiteral("jam")).isObject()) {
        fail(QStringLiteral("peer %1 snapshot omitted Jam state").arg(peer));
        return std::nullopt;
    }
    return event->value(QStringLiteral("jam")).toObject();
}

template <typename Predicate>
bool waitForAll(
    FourPeerCoordinator& coordinator,
    const QString& description,
    Predicate predicate,
    std::array<QJsonObject, FourPeerCoordinator::kPeerCount>& snapshots)
{
    const auto deadline = std::chrono::steady_clock::now() +
        jam2::test::scaledTimeout(40s);
    int sequence = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        bool ready = true;
        for (std::size_t index = 0; index < FourPeerCoordinator::kPeerCount; ++index) {
            const auto snapshot = jamSnapshot(
                coordinator.peer(index), static_cast<int>(index + 1), sequence);
            if (!snapshot) return false;
            snapshots[index] = *snapshot;
            ready = predicate(index, *snapshot) && ready;
        }
        if (ready) return true;
        ++sequence;
        QThread::msleep(75);
    }
    fail(QStringLiteral("timed out waiting for ") + description);
    for (std::size_t index = 0; index < snapshots.size(); ++index) {
        std::cerr << "  peer-" << index + 1
                  << " role=" << snapshots[index].value(QStringLiteral("role")).toString().toStdString()
                  << " remotes=" << snapshots[index].value(QStringLiteral("remote_peer_count")).toInt()
                  << " active=" << snapshots[index].value(QStringLiteral("active_remote_peer_count")).toInt()
                  << " running=" << snapshots[index].value(QStringLiteral("network_running")).toBool()
                  << " failure=" << snapshots[index].value(QStringLiteral("failure")).toString().toStdString()
                  << '\n';
    }
    return false;
}

bool matchesPolicy(const QJsonObject& jam, bool local, int expectedRevision)
{
    const QJsonObject policy = jam.value(QStringLiteral("policy")).toObject();
    return policy.value(QStringLiteral("revision")).toInt() == expectedRevision &&
        policy.value(QStringLiteral("track_lanes")).toBool() == !local &&
        policy.value(QStringLiteral("auto_share_wavs")).toBool() == !local &&
        policy.value(QStringLiteral("global_playback")).toBool() == !local &&
        policy.value(QStringLiteral("generated_ideas")).toString() ==
            (local ? QStringLiteral("chords") : QStringLiteral("full")) &&
        policy.value(QStringLiteral("metronome_state")).toBool() == local &&
        policy.value(QStringLiteral("recordings")).toBool() == !local;
}

} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);
    if (argc != 2) {
        std::cerr << "usage: jam2_four_jam_sync_integration <release-jam2>\n";
        return 2;
    }

    LoopbackPortReservations portReservations;
    QString portError;
    if (!portReservations.reserve(FourPeerCoordinator::kPeerCount, portError)) {
        fail(QStringLiteral("could not reserve four distinct local TCP/UDP ports: ") +
            portError);
        return 1;
    }
    std::array<quint16, FourPeerCoordinator::kPeerCount> ports{};
    for (std::size_t index = 0; index < ports.size(); ++index) {
        ports[index] = portReservations.port(index);
    }

    const QString executable = QString::fromLocal8Bit(argv[1]);
    std::array<QStringList, FourPeerCoordinator::kPeerCount> arguments;
    const bool show = qEnvironmentVariableIntValue("JAM2_TEST_SHOW_GUI") == 1;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        arguments[index] << QStringLiteral("debug") << QStringLiteral("gui-agent")
            << QStringLiteral("--instance-id") << QStringLiteral("jam-peer-%1").arg(index + 1);
        if (show) arguments[index] << QStringLiteral("--show-gui");
    }
    FourPeerCoordinator coordinator;
    QString launchError;
    if (!coordinator.launch(executable, arguments, launchError)) {
        fail(QStringLiteral("launching four Jam Sync peers: ") + launchError);
        return 1;
    }
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        (void)receive(coordinator.peer(index), QStringLiteral("hello"));
    }
    portReservations.release();

    (void)send(coordinator.peer(0), {
        {QStringLiteral("type"), QStringLiteral("jam.create")},
        {QStringLiteral("id"), QStringLiteral("create")},
        {QStringLiteral("port"), ports[0]},
    });
    const auto created = receive(coordinator.peer(0), QStringLiteral("command_applied"));
    if (!created) return 1;
    const QString invite = created->value(QStringLiteral("invite_url")).toString();
    if (invite.isEmpty()) {
        fail(QStringLiteral("creator did not return its invite URL"));
        return 1;
    }
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        (void)send(coordinator.peer(index), {
            {QStringLiteral("type"), QStringLiteral("jam.join")},
            {QStringLiteral("id"), QStringLiteral("join-%1").arg(index + 1)},
            {QStringLiteral("port"), ports[index]},
            {QStringLiteral("invite_url"), invite},
        });
        (void)receive(coordinator.peer(index), QStringLiteral("command_applied"));
    }

    std::array<QJsonObject, FourPeerCoordinator::kPeerCount> snapshots;
    if (!waitForAll(coordinator, QStringLiteral("one four-peer full mesh"),
        [](std::size_t index, const QJsonObject& jam) {
            return jam.value(QStringLiteral("role")).toString() ==
                    (index == 0 ? QStringLiteral("creator") : QStringLiteral("joiner")) &&
                jam.value(QStringLiteral("remote_peer_count")).toInt() == 3 &&
                jam.value(QStringLiteral("active_remote_peer_count")).toInt() == 3 &&
                jam.value(QStringLiteral("network_attachment_ready")).toBool() &&
                jam.value(QStringLiteral("network_running")).toBool() &&
                jam.value(QStringLiteral("failure")).toString().isEmpty() &&
                matchesPolicy(jam, false, 1);
        }, snapshots)) return 1;
    const int initialRevision = snapshots[0].value(QStringLiteral("policy")).toObject()
        .value(QStringLiteral("revision")).toInt();
    for (std::size_t index = 1; index < snapshots.size(); ++index) {
        if (snapshots[index].value(QStringLiteral("policy")).toObject()
                .value(QStringLiteral("revision")).toInt() != initialRevision) {
            fail(QStringLiteral("initial Jam Sync policy revision did not reconcile on four peers"));
        }
    }
    if (failures != 0) return 1;

    AutomationProcess& joiner = coordinator.peer(2);
    if (!invoke(joiner,
            QStringLiteral("joiner-policy-open"),
            QStringLiteral("session.jam-sync"),
            QStringLiteral("click-async")) ||
        !invoke(joiner,
            QStringLiteral("joiner-policy-manual-wavs"),
            QStringLiteral("session.jam-sync-dialog.automatic-wavs"),
            QStringLiteral("set-checked"), false) ||
        !invoke(joiner,
            QStringLiteral("joiner-policy-recordings-off"),
            QStringLiteral("session.jam-sync-dialog.recordings"),
            QStringLiteral("set-checked"), false) ||
        !invoke(joiner,
            QStringLiteral("joiner-policy-chords"),
            QStringLiteral("session.jam-sync-dialog.generated-ideas"),
            QStringLiteral("set-index"), 1) ||
        !invoke(joiner,
            QStringLiteral("joiner-policy-metronome-on"),
            QStringLiteral("session.jam-sync-dialog.metronome-state"),
            QStringLiteral("set-checked"), true) ||
        !invoke(joiner,
            QStringLiteral("joiner-policy-lanes-off"),
            QStringLiteral("session.jam-sync-dialog.track-lanes"),
            QStringLiteral("set-checked"), false) ||
        !invoke(joiner,
            QStringLiteral("joiner-policy-playback-off"),
            QStringLiteral("session.jam-sync-dialog.global-playback"),
            QStringLiteral("set-checked"), false) ||
        ![&] {
            const auto automatic = controlState(
                joiner,
                QStringLiteral("session.jam-sync-dialog.automatic-wavs"),
                QStringLiteral("joiner-policy-automatic-state"));
            const auto recordings = controlState(
                joiner,
                QStringLiteral("session.jam-sync-dialog.recordings"),
                QStringLiteral("joiner-policy-recordings-state"));
            if (automatic && recordings &&
                !automatic->value(QStringLiteral("enabled")).toBool() &&
                !recordings->value(QStringLiteral("enabled")).toBool() &&
                !recordings->value(QStringLiteral("checked")).toBool()) {
                return true;
            }
            fail(QStringLiteral(
                "disabled lanes/playback did not disable WAV and recording dependencies"));
            return false;
        }() ||
        !invoke(joiner,
            QStringLiteral("joiner-policy-apply"),
            QStringLiteral("session.jam-sync-dialog.apply"),
            QStringLiteral("click"))) return 1;
    if (!waitForAll(coordinator, QStringLiteral("joiner-requested policy convergence"),
        [initialRevision](std::size_t, const QJsonObject& jam) {
            return matchesPolicy(jam, true, initialRevision + 1);
        }, snapshots)) return 1;
    const int firstRevision = snapshots[0].value(QStringLiteral("policy")).toObject()
        .value(QStringLiteral("revision")).toInt();
    for (std::size_t index = 1; index < snapshots.size(); ++index) {
        if (snapshots[index].value(QStringLiteral("policy")).toObject()
                .value(QStringLiteral("revision")).toInt() != firstRevision) {
            fail(QStringLiteral("four peers did not converge on one policy revision"));
        }
    }

    AutomationProcess& creator = coordinator.peer(0);
    if (!invoke(creator,
            QStringLiteral("creator-policy-open"),
            QStringLiteral("session.jam-sync"),
            QStringLiteral("click-async")) ||
        !invoke(creator,
            QStringLiteral("creator-policy-lanes-on"),
            QStringLiteral("session.jam-sync-dialog.track-lanes"),
            QStringLiteral("set-checked"), true) ||
        !invoke(creator,
            QStringLiteral("creator-policy-playback-on"),
            QStringLiteral("session.jam-sync-dialog.global-playback"),
            QStringLiteral("set-checked"), true) ||
        ![&] {
            const auto automatic = controlState(
                creator,
                QStringLiteral("session.jam-sync-dialog.automatic-wavs"),
                QStringLiteral("creator-policy-automatic-state"));
            const auto recordings = controlState(
                creator,
                QStringLiteral("session.jam-sync-dialog.recordings"),
                QStringLiteral("creator-policy-recordings-state"));
            if (automatic && recordings &&
                automatic->value(QStringLiteral("enabled")).toBool() &&
                recordings->value(QStringLiteral("enabled")).toBool()) {
                return true;
            }
            fail(QStringLiteral(
                "enabled lanes/playback did not enable WAV and recording dependencies"));
            return false;
        }() ||
        !invoke(creator,
            QStringLiteral("creator-policy-automatic-wavs"),
            QStringLiteral("session.jam-sync-dialog.automatic-wavs"),
            QStringLiteral("set-checked"), true) ||
        !invoke(creator,
            QStringLiteral("creator-policy-full-ideas"),
            QStringLiteral("session.jam-sync-dialog.generated-ideas"),
            QStringLiteral("set-index"), 0) ||
        !invoke(creator,
            QStringLiteral("creator-policy-metronome-off"),
            QStringLiteral("session.jam-sync-dialog.metronome-state"),
            QStringLiteral("set-checked"), false) ||
        !invoke(creator,
            QStringLiteral("creator-policy-recordings-on"),
            QStringLiteral("session.jam-sync-dialog.recordings"),
            QStringLiteral("set-checked"), true) ||
        !invoke(creator,
            QStringLiteral("creator-policy-apply"),
            QStringLiteral("session.jam-sync-dialog.apply"),
            QStringLiteral("click"))) return 1;
    if (!waitForAll(coordinator, QStringLiteral("creator policy convergence"),
        [firstRevision](std::size_t, const QJsonObject& jam) {
            return matchesPolicy(jam, false, firstRevision + 1);
        }, snapshots)) return 1;
    const int finalRevision = firstRevision + 1;
    for (const QJsonObject& snapshot : snapshots) {
        if (snapshot.value(QStringLiteral("policy")).toObject()
                .value(QStringLiteral("revision")).toInt() != finalRevision) {
            fail(QStringLiteral("creator change did not leave one exact final revision"));
        }
    }

    if (!invoke(creator,
            QStringLiteral("leader-audio-mode"),
            QStringLiteral("metronome.mode"),
            QStringLiteral("set-index"), 1) ||
        !invoke(creator,
            QStringLiteral("leader-audio-policy-open"),
            QStringLiteral("session.jam-sync"),
            QStringLiteral("click-async"))) return 1;
    const auto leaderMetronome = controlState(
        creator,
        QStringLiteral("session.jam-sync-dialog.metronome-state"),
        QStringLiteral("leader-audio-metronome-state"));
    const auto leaderTrackLanes = controlState(
        creator,
        QStringLiteral("session.jam-sync-dialog.track-lanes"),
        QStringLiteral("leader-audio-track-state"));
    const auto leaderApply = controlState(
        creator,
        QStringLiteral("session.jam-sync-dialog.apply"),
        QStringLiteral("leader-audio-apply-state"));
    if (!leaderMetronome || !leaderTrackLanes || !leaderApply ||
        leaderMetronome->value(QStringLiteral("enabled")).toBool() ||
        !leaderTrackLanes->value(QStringLiteral("enabled")).toBool() ||
        !leaderApply->value(QStringLiteral("enabled")).toBool()) {
        fail(QStringLiteral(
            "Leader Audio did not selectively lock only Jam Sync metronome state"));
        return 1;
    }
    if (!invoke(creator,
            QStringLiteral("leader-audio-policy-cancel"),
            QStringLiteral("session.jam-sync-dialog.cancel"),
            QStringLiteral("click")) ||
        !invoke(creator,
            QStringLiteral("shared-grid-mode-restore"),
            QStringLiteral("metronome.mode"),
            QStringLiteral("set-index"), 0)) return 1;

    for (std::size_t index = 0; index < arguments.size(); ++index) {
        (void)send(coordinator.peer(index), {
            {QStringLiteral("type"), QStringLiteral("shutdown")},
            {QStringLiteral("id"), QStringLiteral("shutdown-%1").arg(index + 1)},
        });
    }
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        (void)receive(coordinator.peer(index), QStringLiteral("command_applied"));
        (void)receive(coordinator.peer(index), QStringLiteral("shutdown"));
        int exitCode = -1;
        QString error;
        if (!coordinator.peer(index).waitForExit(20s, exitCode, error) || exitCode != 0) {
            fail(QStringLiteral("peer %1 did not exit cleanly: %2 code=%3")
                .arg(index + 1).arg(error).arg(exitCode));
        }
    }

    if (failures != 0) {
        std::cerr << failures << " four-peer Jam Sync checks failed\n";
        return 1;
    }
    coordinator.markSuccessful();
    std::cout << "four-peer Jam Sync checks passed\n";
    return 0;
}

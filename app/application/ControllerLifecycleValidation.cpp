#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ControllerLifecycleValidation.hpp"

#include "ControlProtocol.hpp"
#include "SharedSessionController.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QTcpServer>
#include <QThread>

#include <algorithm>
#include <functional>
#include <optional>

using jam2::control_protocol::TransportEvent;
using jam2::control_protocol::TransportEventType;
using jam2::control_protocol::TransportFailure;

namespace {

struct EventCapture {
    int connectionRefused = 0;
    int authenticationRejected = 0;
    int disconnectedAuthenticated = 0;
    int refreshRequested = 0;
    int reconnectScheduled = 0;
    int reconnectAttempt = 0;
    int reconnectExhausted = 0;
    int maxReconnectAttempts = 0;
    bool sawReconnecting = false;

    void event(const TransportEvent& value)
    {
        connectionRefused += value.type == TransportEventType::Failure &&
            value.failure == TransportFailure::ConnectionRefused;
        authenticationRejected += value.type == TransportEventType::Failure &&
            value.failure == TransportFailure::AuthenticationRejected;
        disconnectedAuthenticated += value.type == TransportEventType::Disconnected &&
            value.authenticated;
        refreshRequested += value.type == TransportEventType::RefreshRequested;
        reconnectScheduled += value.type == TransportEventType::ReconnectScheduled;
        reconnectAttempt += value.type == TransportEventType::ReconnectAttempt;
        reconnectExhausted += value.type == TransportEventType::Failure &&
            value.failure == TransportFailure::ReconnectExhausted;
    }

    void snapshot(const SharedSessionController::Snapshot& value)
    {
        maxReconnectAttempts = std::max(maxReconnectAttempts, value.reconnectAttempts);
        sawReconnecting = sawReconnecting ||
            value.lifecycle == SharedSessionController::Lifecycle::Reconnecting;
    }
};

std::optional<quint16> unusedLoopbackPort()
{
    QTcpServer reservation;
    if (!reservation.listen(QHostAddress::LocalHost, 0)) {
        return std::nullopt;
    }
    const quint16 port = reservation.serverPort();
    reservation.close();
    return port == 0 ? std::nullopt : std::optional<quint16>{port};
}

bool pumpUntil(const std::function<bool()>& predicate, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(2);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    return predicate();
}

SharedSessionController::SessionContract contract()
{
    SharedSessionController::SessionContract value;
    value.protocolVersion = 1;
    value.audioFormat = QStringLiteral("pcm24-mono");
    value.profile = QStringLiteral("fast");
    value.sampleRate = 48000;
    value.frameSize = 64;
    return value;
}

SharedSessionController::CreatorConfig creatorConfig(quint16 port)
{
    SharedSessionController::CreatorConfig value;
    value.port = port;
    value.sessionHex = QStringLiteral("0102030405060708");
    value.keyHex = QStringLiteral("000102030405060708090a0b0c0d0e0f");
    value.localToken = QStringLiteral("00000000000000010000000000000001");
    value.localEndpoint = QStringLiteral("127.0.0.1:41001");
    value.contract = contract();
    return value;
}

SharedSessionController::JoinerConfig joinerConfig(
    quint16 port,
    const QString& token = QStringLiteral("00000000000000020000000000000002"))
{
    SharedSessionController::JoinerConfig value;
    value.host = QStringLiteral("127.0.0.1");
    value.port = port;
    value.sessionHex = QStringLiteral("0102030405060708");
    value.keyHex = QStringLiteral("000102030405060708090a0b0c0d0e0f");
    value.localToken = token;
    value.localEndpoint = token.endsWith(QLatin1Char('2'))
        ? QStringLiteral("127.0.0.1:41002")
        : QStringLiteral("127.0.0.1:41003");
    value.expectedContract = contract();
    value.enforceExpectedContract = true;
    return value;
}

void addCase(QJsonArray& cases, const QString& name, bool ok, const QString& detail = {})
{
    QJsonObject value{
        {QStringLiteral("name"), name},
        {QStringLiteral("ok"), ok},
    };
    if (!detail.isEmpty()) {
        value[QStringLiteral("detail")] = detail;
    }
    cases.push_back(value);
}

} // namespace

QJsonObject jam2RunControllerLifecycleValidation()
{
    QJsonArray cases;
    bool allOk = true;
    const auto check = [&](const QString& name, bool ok, const QString& detail = {}) {
        addCase(cases, name, ok, detail);
        allOk = allOk && ok;
    };

    const auto refusedPort = unusedLoopbackPort();
    EventCapture refusedCapture;
    SharedSessionController refused;
    refused.onTransportEvent = [&](const TransportEvent& event, bool) {
        refusedCapture.event(event);
    };
    refused.onSnapshot = [&](const SharedSessionController::Snapshot& snapshot) {
        refusedCapture.snapshot(snapshot);
    };
    bool refusedStarted = false;
    bool exhausted = false;
    if (refusedPort) {
        auto config = joinerConfig(*refusedPort);
        config.reconnectIntervalMs = 20;
        config.reconnectAttemptLimit = 1;
        refusedStarted = refused.startJoiner(config);
        exhausted = pumpUntil([&] {
            const auto snapshot = refused.snapshot();
            return snapshot.lifecycle == SharedSessionController::Lifecycle::Failed &&
                snapshot.failure == TransportFailure::ReconnectExhausted;
        }, 12000);
    }
    const auto refusedSnapshot = refused.snapshot();
    check(QStringLiteral("controller.initial-refusal-typed"),
        refusedPort && refusedStarted && refusedCapture.connectionRefused > 0 &&
            refusedCapture.sawReconnecting,
        refusedSnapshot.failureDetail);
    check(QStringLiteral("controller.reconnect-exhaustion-bounded"),
        exhausted && refusedCapture.reconnectAttempt == 1 &&
            refusedCapture.reconnectExhausted == 1 &&
            refusedCapture.maxReconnectAttempts == 1 &&
            refusedSnapshot.reconnectAttemptLimit == 1 &&
            refusedSnapshot.reconnectIntervalMs == 20,
        refusedSnapshot.failureDetail);
    refused.close();

    const auto sessionPort = unusedLoopbackPort();
    SharedSessionController creator;
    const bool creatorStarted = sessionPort && creator.startCreator(creatorConfig(*sessionPort));
    check(QStringLiteral("controller.creator-listening"), creatorStarted &&
        creator.snapshot().lifecycle == SharedSessionController::Lifecycle::Listening,
        creator.errorString());

    SharedSessionController conflictingCreator;
    const bool conflictingCreatorRejected = sessionPort && creatorStarted &&
        !conflictingCreator.startCreator(creatorConfig(*sessionPort));
    check(QStringLiteral("controller.creator-port-conflict-detail"),
        conflictingCreatorRejected && !conflictingCreator.errorString().trimmed().isEmpty(),
        conflictingCreator.errorString());
    conflictingCreator.close();

    EventCapture wrongKeyCapture;
    SharedSessionController wrongKey;
    wrongKey.onTransportEvent = [&](const TransportEvent& event, bool) {
        wrongKeyCapture.event(event);
    };
    bool wrongKeyStarted = false;
    bool wrongKeyFailed = false;
    if (sessionPort && creatorStarted) {
        auto config = joinerConfig(
            *sessionPort,
            QStringLiteral("00000000000000030000000000000003"));
        config.keyHex = QStringLiteral("f00102030405060708090a0b0c0d0e0f");
        config.reconnectIntervalMs = 20;
        config.reconnectAttemptLimit = 1;
        wrongKeyStarted = wrongKey.startJoiner(config);
        wrongKeyFailed = pumpUntil([&] {
            return wrongKey.snapshot().lifecycle == SharedSessionController::Lifecycle::Failed;
        }, 1000);
    }
    check(QStringLiteral("controller.authentication-failure-typed"),
        wrongKeyStarted && wrongKeyFailed && wrongKeyCapture.authenticationRejected == 1 &&
            wrongKey.snapshot().failure == TransportFailure::AuthenticationRejected &&
            !wrongKey.snapshot().failureRetryable,
        wrongKey.snapshot().failureDetail);
    wrongKey.close();

    EventCapture joinerCapture;
    SharedSessionController joiner;
    joiner.onTransportEvent = [&](const TransportEvent& event, bool) {
        joinerCapture.event(event);
    };
    joiner.onSnapshot = [&](const SharedSessionController::Snapshot& snapshot) {
        joinerCapture.snapshot(snapshot);
    };
    bool joinerStarted = false;
    bool joined = false;
    if (sessionPort && creatorStarted) {
        auto config = joinerConfig(*sessionPort);
        config.reconnectIntervalMs = 250;
        config.reconnectAttemptLimit = 3;
        joinerStarted = joiner.startJoiner(config);
        joined = pumpUntil([&] {
            const auto local = joiner.snapshot();
            const auto remote = creator.snapshot();
            return local.lifecycle == SharedSessionController::Lifecycle::Active &&
                local.contractReady && local.membershipReady && local.networkAttachmentReady &&
                local.remotePeerCount == 1 && remote.remotePeerCount == 1;
        }, 2000);
    }
    check(QStringLiteral("controller.join-contract-membership-ready"),
        joinerStarted && joined && joiner.snapshot().contractRevision == 1 &&
            joiner.snapshot().arrangementAuthorityToken == creator.snapshot().localToken);

    QString proposalSourceToken;
    QJsonObject receivedProposal;
    creator.onMessage = [&](const QString& token, const QJsonObject& message) {
        if (message.value(QStringLiteral("type")).toString() == QStringLiteral("song.set")) {
            proposalSourceToken = token;
            receivedProposal = message;
        }
    };
    bool collaborativeProposalDelivered = false;
    if (joined) {
        const bool sent = joiner.send(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("song.set")},
            {QStringLiteral("arrangement_revision"), 0},
            {QStringLiteral("host_authoritative"), false},
            {QStringLiteral("track_playing"), false},
            {QStringLiteral("song"), QJsonObject{}},
        });
        collaborativeProposalDelivered = sent && pumpUntil([&] {
            return proposalSourceToken == joiner.snapshot().localToken &&
                !receivedProposal.value(QStringLiteral("host_authoritative")).toBool(true);
        }, 1000);
    }
    check(QStringLiteral("controller.peer-arrangement-proposal-delivered"),
        collaborativeProposalDelivered);

    bool nonCoordinatorAuthority = false;
    if (joined) {
        const bool sent = joiner.send(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("metronome.settings")},
            {QStringLiteral("bpm"), 126},
        });
        nonCoordinatorAuthority = sent && pumpUntil([&] {
            return creator.snapshot().gridRevision == 1 &&
                creator.snapshot().gridAuthorityToken == joiner.snapshot().localToken &&
                joiner.snapshot().gridRevision == 1 &&
                joiner.snapshot().gridAuthorityToken == joiner.snapshot().localToken;
        }, 1000);
    }
    check(QStringLiteral("controller.non-coordinator-grid-authority"), nonCoordinatorAuthority);

    SharedSessionController lateJoiner;
    bool lateJoinReady = false;
    if (sessionPort && nonCoordinatorAuthority) {
        auto config = joinerConfig(
            *sessionPort,
            QStringLiteral("00000000000000030000000000000003"));
        config.reconnectIntervalMs = 250;
        config.reconnectAttemptLimit = 3;
        (void)lateJoiner.startJoiner(config);
        lateJoinReady = pumpUntil([&] {
            const auto snapshot = lateJoiner.snapshot();
            return snapshot.lifecycle == SharedSessionController::Lifecycle::Active &&
                snapshot.totalPeerCount == 3 && snapshot.remotePeerCount == 2 &&
                snapshot.gridRevision == 1 &&
                snapshot.gridAuthorityToken == joiner.snapshot().localToken &&
                snapshot.arrangementAuthorityToken == creator.snapshot().localToken;
        }, 2000);
    }
    check(QStringLiteral("controller.late-join-authority-snapshot"), lateJoinReady);
    lateJoiner.close();
    (void)pumpUntil([&] { return creator.snapshot().remotePeerCount == 1; }, 500);

    const QString joinerToken = joiner.snapshot().localToken;
    const quint64 revisionBeforeAutoReconnect = joiner.snapshot().membershipRevision;
    bool autoReconnected = false;
    if (joined && creator.sendTo(joinerToken, QJsonObject{
            {QStringLiteral("type"), QStringLiteral("debug.lifecycle.disconnect")}}, true)) {
        autoReconnected = pumpUntil([&] {
            return joinerCapture.disconnectedAuthenticated >= 1 &&
                joinerCapture.reconnectAttempt >= 1 &&
                joiner.snapshot().lifecycle == SharedSessionController::Lifecycle::Active &&
                joiner.snapshot().membershipRevision > revisionBeforeAutoReconnect;
        }, 2000);
    }
    check(QStringLiteral("controller.established-disconnect-auto-reconnect"),
        autoReconnected && joinerCapture.reconnectScheduled >= 1 &&
            joinerCapture.maxReconnectAttempts >= 1 && joiner.snapshot().reconnectAttempts == 0);

    const quint64 revisionBeforeRefresh = joiner.snapshot().membershipRevision;
    bool manualRefreshReconnected = false;
    if (autoReconnected && creator.sendTo(joinerToken, QJsonObject{
            {QStringLiteral("type"), QStringLiteral("debug.lifecycle.disconnect")}}, true)) {
        const bool disconnected = pumpUntil([&] {
            return joinerCapture.disconnectedAuthenticated >= 2 &&
                joiner.snapshot().lifecycle == SharedSessionController::Lifecycle::Reconnecting;
        }, 200);
        if (disconnected) {
            joiner.refresh();
            manualRefreshReconnected = pumpUntil([&] {
                return joinerCapture.refreshRequested >= 1 &&
                    joiner.snapshot().lifecycle == SharedSessionController::Lifecycle::Active &&
                    joiner.snapshot().membershipRevision > revisionBeforeRefresh;
            }, 1500);
        }
    }
    check(QStringLiteral("controller.manual-refresh-reconnect"),
        manualRefreshReconnected && joiner.snapshot().reconnectAttempts == 0);

    const auto finalJoiner = joiner.snapshot();
    check(QStringLiteral("controller.final-authoritative-snapshot"),
        finalJoiner.coordinatorToken == creator.snapshot().localToken &&
            finalJoiner.gridAuthorityToken.isEmpty() && finalJoiner.gridRevision >= 2 &&
            finalJoiner.arrangementAuthorityToken == creator.snapshot().localToken &&
            finalJoiner.contractReady && finalJoiner.membershipReady &&
            finalJoiner.failure == TransportFailure::None);

    joiner.close();
    creator.close();

    return QJsonObject{
        {QStringLiteral("event"), QStringLiteral("debug_controller_lifecycle_result")},
        {QStringLiteral("schema"), 1},
        {QStringLiteral("ok"), allOk},
        {QStringLiteral("cases"), cases},
        {QStringLiteral("refused_failures"), refusedCapture.connectionRefused},
        {QStringLiteral("bounded_reconnect_attempts"), refusedCapture.maxReconnectAttempts},
        {QStringLiteral("automatic_reconnect_events"), joinerCapture.reconnectAttempt},
        {QStringLiteral("manual_refresh_events"), joinerCapture.refreshRequested},
    };
}

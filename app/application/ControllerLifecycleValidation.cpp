#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ControllerLifecycleValidation.hpp"

#include "AssetChunkProtocol.hpp"
#include "ControlProtocol.hpp"
#include "SharedSessionController.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonArray>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include <algorithm>
#include <functional>
#include <optional>
#include <thread>

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
    int sessionEnded = 0;
    int coordinatorTimeout = 0;
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
        sessionEnded += value.type == TransportEventType::SessionEnded;
        coordinatorTimeout += value.type == TransportEventType::Failure &&
            value.failure == TransportFailure::CoordinatorTimeout;
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
    value.protocolVersion = jam2::protocol::kProtocolVersion;
    value.audioFormat = QStringLiteral("pcm24-mono");
    value.profile = QStringLiteral("fast");
    value.sampleRate = 48000;
    value.frameSize = 64;
    return value;
}

QJsonObject minimalValidSong()
{
    return QJsonObject{
        {QStringLiteral("format"), QStringLiteral("jam2.song.v1")},
        {QStringLiteral("title"), QStringLiteral("Lifecycle Fixture")},
        {QStringLiteral("lyrics_text"), QString()},
        {QStringLiteral("sections"), QJsonArray{QJsonObject{
            {QStringLiteral("label"), QStringLiteral("A")},
            {QStringLiteral("name"), QStringLiteral("Section")},
            {QStringLiteral("beats"), 4},
            {QStringLiteral("chords"), QJsonArray{}},
            {QStringLiteral("beat_notes"), QJsonArray{}},
            {QStringLiteral("lyrics"), QJsonArray{}},
            {QStringLiteral("beat_patterns"), QJsonArray{}},
        }}},
    };
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

QJsonObject jam2RunControllerLifecycleValidation(
    int heartbeatIntervalMs,
    int heartbeatMissLimit)
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
    const auto invalidContractPort = unusedLoopbackPort();
    SharedSessionController invalidContractCreator;
    bool invalidContractRejected = false;
    if (invalidContractPort) {
        auto invalidConfig = creatorConfig(*invalidContractPort);
        invalidConfig.contract.audioFormat = QStringLiteral("pcm32-mono");
        invalidContractRejected = !invalidContractCreator.startCreator(invalidConfig);
    }
    check(QStringLiteral("controller.creator-rejects-unknown-audio-format-before-listen"),
        invalidContractPort && invalidContractRejected &&
            invalidContractCreator.snapshot().failure == TransportFailure::InvalidConfiguration &&
            invalidContractCreator.snapshot().lifecycle == SharedSessionController::Lifecycle::Failed);
    invalidContractCreator.close();

    SharedSessionController creator;
    auto primaryCreatorConfig = sessionPort
        ? creatorConfig(*sessionPort) : SharedSessionController::CreatorConfig{};
    // Model a LAN invite while the creator has also retained a public/STUN UDP
    // candidate. The joiner's explicit private TCP route must win for the
    // coordinator UDP edge, including its local shared TCP/UDP port.
    primaryCreatorConfig.localEndpoint = QStringLiteral("198.51.100.10:49999");
    const bool creatorStarted = sessionPort && creator.startCreator(primaryCreatorConfig);
    check(QStringLiteral("controller.creator-listening"), creatorStarted &&
        creator.snapshot().lifecycle == SharedSessionController::Lifecycle::Listening,
        creator.errorString());
    check(QStringLiteral("controller.production-heartbeat-policy"),
        creator.snapshot().heartbeatIntervalMs == 30000 &&
            creator.snapshot().heartbeatMissLimit == 5 &&
            SharedSessionController::kDefaultHeartbeatIntervalMs == 30000 &&
            SharedSessionController::kDefaultHeartbeatMissLimit == 5);

    SharedSessionController conflictingCreator;
    const bool conflictingCreatorRejected = sessionPort && creatorStarted &&
        !conflictingCreator.startCreator(creatorConfig(*sessionPort));
    check(QStringLiteral("controller.creator-port-conflict-detail"),
        conflictingCreatorRejected && !conflictingCreator.errorString().trimmed().isEmpty(),
        conflictingCreator.errorString());
    conflictingCreator.close();

    bool repeatedPreAuthDisconnectsSafe = creatorStarted;
    int preAuthChallenges = 0;
    if (sessionPort && creatorStarted) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            QTcpSocket socket;
            socket.connectToHost(QHostAddress::LocalHost, *sessionPort);
            const bool challenged = pumpUntil([&] {
                return socket.bytesAvailable() > 0 ||
                    socket.state() == QAbstractSocket::UnconnectedState;
            }, 500) && socket.bytesAvailable() > 0;
            preAuthChallenges += challenged ? 1 : 0;
            repeatedPreAuthDisconnectsSafe = repeatedPreAuthDisconnectsSafe && challenged;
            socket.abort();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        }
    }
    const bool preAuthDisconnectsObserved = pumpUntil([&] {
        return creator.serverStats().disconnectedConnections >= 3;
    }, 500);
    check(QStringLiteral("controller.pre-auth-challenge-immediate-and-repeatable"),
        repeatedPreAuthDisconnectsSafe && preAuthChallenges == 3 &&
            creator.serverStats().acceptedConnections >= 3 &&
            preAuthDisconnectsObserved);

    const quint64 acceptedBeforeClosedBacklog = creator.serverStats().acceptedConnections;
    int closedBacklogConnections = 0;
    if (sessionPort && creatorStarted) {
        // Establish and close on another thread while deliberately not pumping
        // the server event loop. acceptPeer() must safely encounter sockets
        // whose remote side disappeared before the challenge write.
        std::thread closedBacklogClient([&] {
            for (int attempt = 0; attempt < 8; ++attempt) {
                QTcpSocket socket;
                socket.connectToHost(QHostAddress::LocalHost, *sessionPort);
                if (socket.waitForConnected(1000)) {
                    ++closedBacklogConnections;
                    socket.abort();
                }
            }
        });
        closedBacklogClient.join();
    }
    const bool closedBacklogHandled = closedBacklogConnections > 0 && pumpUntil([&] {
        return creator.serverStats().acceptedConnections > acceptedBeforeClosedBacklog;
    }, 1000);
    check(QStringLiteral("controller.closed-pre-auth-backlog-preserves-listener"),
        closedBacklogHandled &&
            creator.snapshot().lifecycle == SharedSessionController::Lifecycle::Listening);

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
    bool lanInviteCoordinatorSelected = false;
    if (joined && sessionPort) {
        for (const auto& peer : joiner.snapshot().peers) {
            lanInviteCoordinatorSelected = lanInviteCoordinatorSelected ||
                (peer.token == joiner.snapshot().coordinatorToken &&
                 peer.endpoint == QStringLiteral("127.0.0.1:%1").arg(*sessionPort) &&
                 peer.proofState == QStringLiteral("candidate-lan-invite"));
        }
    }
    check(QStringLiteral("controller.private-invite-selects-lan-coordinator-endpoint"),
        lanInviteCoordinatorSelected);
    const bool heartbeatObserved = joined && pumpUntil([&] {
        return joiner.snapshot().heartbeatsReceived > 0 &&
            creator.snapshot().heartbeatAcksReceived > 0;
    }, 1000);
    check(QStringLiteral("controller.heartbeat-authenticated-and-acknowledged"),
        heartbeatObserved && joiner.snapshot().lastHeartbeatAgeMs >= 0);

    const QByteArray binaryAsset = jam2::application::asset_chunk::encode({
        QString(64, QLatin1Char('a')), 0, 0, QByteArray("binary-asset", 12)});
    QByteArray creatorBinary;
    QByteArray joinerBinary;
    QString binarySource;
    creator.onBinaryMessage = [&](const QString& source, const QByteArray& payload) {
        binarySource = source;
        creatorBinary = payload;
    };
    joiner.onBinaryMessage = [&](const QString&, const QByteArray& payload) {
        joinerBinary = payload;
    };
    const bool joinerBinarySent = joined && !binaryAsset.isEmpty() &&
        joiner.sendBinaryTo(QString{}, binaryAsset);
    const bool creatorBinaryReceived = joinerBinarySent && pumpUntil([&] {
        return creatorBinary == binaryAsset && binarySource == joiner.snapshot().localToken;
    }, 1000);
    const bool creatorBinarySent = creatorBinaryReceived &&
        creator.sendBinaryTo(joiner.snapshot().localToken, binaryAsset);
    const bool joinerBinaryReceived = creatorBinarySent && pumpUntil([&] {
        return joinerBinary == binaryAsset;
    }, 1000);
    check(QStringLiteral("controller.authenticated-binary-asset-bidirectional"),
        creatorBinaryReceived && joinerBinaryReceived);

    bool endpointMigrated = false;
    if (joined) {
        const quint64 priorRevision = creator.snapshot().membershipRevision;
        joiner.updateLocalEndpoint(QStringLiteral("127.0.0.1:42002"));
        endpointMigrated = pumpUntil([&] {
            const auto creatorSnapshot = creator.snapshot();
            const auto joinerSnapshot = joiner.snapshot();
            bool creatorSeesEndpoint = false;
            for (const auto& peer : creatorSnapshot.peers) {
                creatorSeesEndpoint = creatorSeesEndpoint ||
                    (peer.token == joinerSnapshot.localToken &&
                     peer.endpoint == QStringLiteral("127.0.0.1:42002"));
            }
            return creatorSnapshot.membershipRevision > priorRevision && creatorSeesEndpoint;
        }, 1000);
    }
    check(QStringLiteral("controller.endpoint-update-source-bound-and-republished"), endpointMigrated);

    ControlClient directClient;
    bool directAuthenticated = false;
    directClient.onEvent = [&](const TransportEvent& event) {
        directAuthenticated = directAuthenticated || event.type == TransportEventType::Authenticated;
    };
    if (sessionPort && creatorStarted) {
        directClient.connectToHost(
            QStringLiteral("127.0.0.1"),
            *sessionPort,
            QStringLiteral("0102030405060708"),
            QStringLiteral("000102030405060708090a0b0c0d0e0f"),
            QStringLiteral("00000000000000040000000000000004"),
            QStringLiteral("127.0.0.1:41004"));
        (void)pumpUntil([&] { return directAuthenticated; }, 1000);
    }
    const auto beforeRejected = creator.snapshot();
    const bool invalidQueued = directAuthenticated && directClient.send(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("metronome.settings")},
        {QStringLiteral("bpm"), 126},
    });
    const bool invalidRejected = invalidQueued && pumpUntil([&] {
        return creator.snapshot().validationRejections > beforeRejected.validationRejections;
    }, 1000);
    const auto afterInvalid = creator.snapshot();
    check(QStringLiteral("controller.invalid-peer-message-cannot-mutate-authority"),
        invalidRejected && afterInvalid.gridRevision == beforeRejected.gridRevision &&
            afterInvalid.gridAuthorityToken == beforeRejected.gridAuthorityToken);

    const QJsonObject unauthorizedMembership{
        {QStringLiteral("type"), QStringLiteral("session.membership")},
        {QStringLiteral("revision"), 1},
        {QStringLiteral("page_index"), 0},
        {QStringLiteral("page_count"), 1},
        {QStringLiteral("coordinator_token"), creator.snapshot().localToken},
        {QStringLiteral("peers"), QJsonArray{}},
    };
    const quint64 membershipBeforeUnauthorized = creator.snapshot().membershipRevision;
    const quint64 authorizationBefore = creator.snapshot().authorizationRejections;
    const bool unauthorizedQueued = directAuthenticated && directClient.send(unauthorizedMembership);
    const bool unauthorizedRejected = unauthorizedQueued && pumpUntil([&] {
        return creator.snapshot().authorizationRejections > authorizationBefore;
    }, 1000);
    check(QStringLiteral("controller.peer-cannot-originate-membership"),
        unauthorizedRejected && creator.snapshot().membershipRevision == membershipBeforeUnauthorized);
    directClient.close();
    (void)pumpUntil([&] { return creator.snapshot().remotePeerCount == 1; }, 500);

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
            {QStringLiteral("song"), minimalValidSong()},
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
            {QStringLiteral("running"), true},
            {QStringLiteral("leader"), true},
            {QStringLiteral("mode"), QStringLiteral("shared-grid")},
            {QStringLiteral("beats"), 4},
            {QStringLiteral("division"), 4},
            {QStringLiteral("play_mask_low"), QStringLiteral("ffff")},
            {QStringLiteral("play_mask_high"), QStringLiteral("0")},
            {QStringLiteral("accent_mask_low"), QStringLiteral("1111")},
            {QStringLiteral("accent_mask_high"), QStringLiteral("0")},
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
    const bool ordinaryLeavePreservedSession = pumpUntil([&] {
        return creator.snapshot().remotePeerCount == 1 &&
            creator.snapshot().lifecycle == SharedSessionController::Lifecycle::Active;
    }, 500);
    check(QStringLiteral("controller.ordinary-peer-leave-preserves-session"),
        ordinaryLeavePreservedSession);

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
            joinerCapture.maxReconnectAttempts >= 1 && joiner.snapshot().reconnectAttempts == 0 &&
            joiner.clientStats().completedConnections >= 2 &&
            joiner.clientStats().disconnectedConnections >= 1);

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

    const bool endQueued = creator.endSession(QStringLiteral("test creator end"));
    const bool gracefulEnd = endQueued && pumpUntil([&] {
        return joinerCapture.sessionEnded == 1 &&
            joiner.snapshot().lifecycle == SharedSessionController::Lifecycle::Inactive;
    }, 1000);
    check(QStringLiteral("controller.creator-end-propagates-typed-session-end"), gracefulEnd);
    joiner.close();
    creator.close();

    const auto heartbeatPort = unusedLoopbackPort();
    SharedSessionController heartbeatCreator;
    SharedSessionController heartbeatJoiner;
    EventCapture heartbeatCapture;
    heartbeatJoiner.onTransportEvent = [&](const TransportEvent& event, bool) {
        heartbeatCapture.event(event);
    };
    bool heartbeatPairReady = false;
    if (heartbeatPort) {
        auto creatorHeartbeatConfig = creatorConfig(*heartbeatPort);
        creatorHeartbeatConfig.heartbeatIntervalMs = heartbeatIntervalMs;
        creatorHeartbeatConfig.heartbeatMissLimit = heartbeatMissLimit;
        if (heartbeatCreator.startCreator(creatorHeartbeatConfig)) {
            auto joinerHeartbeatConfig = joinerConfig(*heartbeatPort);
            joinerHeartbeatConfig.reconnectIntervalMs = 20;
            joinerHeartbeatConfig.reconnectAttemptLimit = 2;
            joinerHeartbeatConfig.heartbeatIntervalMs = heartbeatIntervalMs;
            joinerHeartbeatConfig.heartbeatMissLimit = heartbeatMissLimit;
            (void)heartbeatJoiner.startJoiner(joinerHeartbeatConfig);
            heartbeatPairReady = pumpUntil([&] {
                return heartbeatJoiner.snapshot().lifecycle ==
                        SharedSessionController::Lifecycle::Active &&
                    heartbeatJoiner.snapshot().heartbeatsReceived > 0;
            }, 1000);
        }
    }
    if (heartbeatPairReady) {
        heartbeatCreator.close();
    }
    const bool heartbeatExpired = heartbeatPairReady && pumpUntil([&] {
        return heartbeatJoiner.snapshot().failure == TransportFailure::CoordinatorTimeout &&
            heartbeatJoiner.snapshot().lifecycle == SharedSessionController::Lifecycle::Failed;
    }, 1000);
    check(QStringLiteral("controller.creator-loss-expires-after-native-heartbeat-grace"),
        heartbeatExpired && heartbeatCapture.coordinatorTimeout == 1 &&
            heartbeatJoiner.snapshot().heartbeatIntervalMs == heartbeatIntervalMs &&
            heartbeatJoiner.snapshot().heartbeatMissLimit == heartbeatMissLimit);
    heartbeatJoiner.close();
    heartbeatCreator.close();

    return QJsonObject{
        {QStringLiteral("event"), QStringLiteral("debug_controller_lifecycle_result")},
        {QStringLiteral("schema"), 1},
        {QStringLiteral("ok"), allOk},
        {QStringLiteral("cases"), cases},
        {QStringLiteral("refused_failures"), refusedCapture.connectionRefused},
        {QStringLiteral("bounded_reconnect_attempts"), refusedCapture.maxReconnectAttempts},
        {QStringLiteral("automatic_reconnect_events"), joinerCapture.reconnectAttempt},
        {QStringLiteral("manual_refresh_events"), joinerCapture.refreshRequested},
        {QStringLiteral("debug_heartbeat_interval_ms"), heartbeatIntervalMs},
        {QStringLiteral("debug_heartbeat_miss_limit"), heartbeatMissLimit},
    };
}

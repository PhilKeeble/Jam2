#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ControllerLifecycleValidation.hpp"

#include "AssetChunkProtocol.hpp"
#include "ControlServer.hpp"
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
#include <memory>
#include <optional>
#include <thread>
#include <vector>

using jam2::control_protocol::TransportEvent;
using jam2::control_protocol::TransportEventType;
using jam2::control_protocol::TransportFailure;

namespace {

constexpr int kFailedKeyHandshakeTimeoutMs = 1000;
constexpr int kFailedKeyCleanupTimeoutMs = 500;
constexpr int kFailedKeyRateLimitTimeoutMs = 1000;

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
    int preAuthenticationDisconnect = 0;
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
        preAuthenticationDisconnect += value.type == TransportEventType::Failure &&
            value.failure == TransportFailure::PreAuthenticationDisconnect;
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

bool readHandshakeFrame(
    QTcpSocket& socket,
    QJsonObject& message,
    int timeoutMs = 1000)
{
    QByteArray buffer;
    return pumpUntil([&] {
        buffer += socket.readAll();
        QByteArray probe = buffer;
        QByteArray body;
        QString error;
        return jam2::control_protocol::takeFrame(probe, body, error) ==
                jam2::control_protocol::TakeFrameResult::Ready &&
            jam2::control_protocol::decodeHandshake(body, message, error);
    }, timeoutMs);
}

bool connectRawAuthenticated(
    QTcpSocket& socket,
    quint16 port,
    const QString& token,
    QByteArray& clientToServerKey)
{
    socket.connectToHost(QHostAddress::LocalHost, port);
    QJsonObject challenge;
    if (!readHandshakeFrame(socket, challenge) ||
        challenge.value(QStringLiteral("type")).toString() !=
            QStringLiteral("hello.challenge")) {
        return false;
    }
    const QString session = QStringLiteral("0102030405060708");
    const QString endpoint = QStringLiteral("127.0.0.1:43001");
    const QByteArray clientNonce = jam2::control_protocol::randomNonce();
    const QByteArray masterKey = jam2::control_protocol::decodeHex(
        QStringLiteral("000102030405060708090a0b0c0d0e0f"), 16);
    const QByteArray transcript = jam2::control_protocol::makeTranscript(
        session,
        jam2::control_protocol::decodeHex(
            challenge.value(QStringLiteral("server_nonce")).toString(), 16),
        clientNonce,
        token,
        endpoint);
    const QByteArray proof = jam2::control_protocol::keyedValue(
        masterKey, QByteArrayLiteral("jam2-control-client-proof"), transcript).left(16);
    const QByteArray proofFrame = jam2::control_protocol::encodeHandshake(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("hello.proof")},
        {QStringLiteral("version"), jam2::control_protocol::kControlProtocolVersion},
        {QStringLiteral("session"), session},
        {QStringLiteral("client_nonce"),
            jam2::control_protocol::encodeHex(clientNonce)},
        {QStringLiteral("proof"), jam2::control_protocol::encodeHex(proof)},
        {QStringLiteral("peer_token"), token},
        {QStringLiteral("udp_endpoint"), endpoint},
        {QStringLiteral("channel"), QStringLiteral("control")},
    });
    if (proofFrame.isEmpty() || socket.write(proofFrame) != proofFrame.size()) return false;
    QJsonObject response;
    if (!readHandshakeFrame(socket, response) ||
        response.value(QStringLiteral("type")).toString() != QStringLiteral("hello.ok")) {
        return false;
    }
    clientToServerKey = jam2::control_protocol::keyedValue(
        masterKey, QByteArrayLiteral("jam2-control-c2s"), transcript);
    return clientToServerKey.size() == 32;
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
        {QStringLiteral("title"), QStringLiteral("Lifecycle Fixture")},
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

    const auto securityPort = unusedLoopbackPort();
    ControlServer securityServer;
    const bool securityListening = securityPort && securityServer.listen(
        *securityPort,
        QStringLiteral("0102030405060708"),
        QStringLiteral("000102030405060708090a0b0c0d0e0f"));
    check(QStringLiteral("controller.security-test-listener"), securityListening,
        securityServer.errorString());

    std::vector<std::unique_ptr<QTcpSocket>> pendingSockets;
    if (securityListening) {
        pendingSockets.reserve(
            static_cast<std::size_t>(jam2::control_protocol::kMaxPendingPeers + 4));
        for (int index = 0; index < jam2::control_protocol::kMaxPendingPeers + 4; ++index) {
            auto socket = std::make_unique<QTcpSocket>();
            socket->connectToHost(QHostAddress::LocalHost, *securityPort);
            pendingSockets.push_back(std::move(socket));
        }
    }
    const bool pendingCapObserved = securityListening && pumpUntil([&] {
        const auto stats = securityServer.stats();
        return stats.activeConnectionHighWater ==
                static_cast<quint64>(jam2::control_protocol::kMaxPendingPeers) &&
            stats.pendingCapRejects > 0 &&
            stats.activeConnections <=
                static_cast<quint64>(jam2::control_protocol::kMaxPendingPeers);
    }, 1500);
    const auto pendingCapStats = securityServer.stats();
    check(QStringLiteral("controller.pending-authentication-work-is-bounded"),
        pendingCapObserved &&
            pendingCapStats.activeConnectionHighWater ==
                static_cast<quint64>(jam2::control_protocol::kMaxPendingPeers),
        QStringLiteral("active=%1 high_water=%2 cap_rejects=%3 accepted=%4")
            .arg(pendingCapStats.activeConnections)
            .arg(pendingCapStats.activeConnectionHighWater)
            .arg(pendingCapStats.pendingCapRejects)
            .arg(pendingCapStats.acceptedConnections));
    for (const auto& socket : pendingSockets) socket->abort();
    pendingSockets.clear();
    const bool pendingDrained = securityListening && pumpUntil([&] {
        return securityServer.stats().activeConnections == 0;
    }, 1000);
    check(QStringLiteral("controller.pending-authentication-cleanup-drains"), pendingDrained);

    const quint64 authTimeoutsBefore = securityServer.stats().authenticationTimeouts;
    QTcpSocket silentSocket;
    QJsonObject silentChallenge;
    if (securityListening) {
        silentSocket.connectToHost(QHostAddress::LocalHost, *securityPort);
        (void)readHandshakeFrame(silentSocket, silentChallenge);
    }
    const bool authenticationTimedOut = securityListening && pumpUntil([&] {
        return securityServer.stats().authenticationTimeouts > authTimeoutsBefore &&
            securityServer.stats().activeConnections == 0;
    }, jam2::control_protocol::kAuthenticationDeadlineMs + 1000);
    check(QStringLiteral("controller.silent-authentication-has-bounded-deadline"),
        authenticationTimedOut);

    const QString rawToken = QStringLiteral("00000000000000050000000000000005");
    const quint64 frameTimeoutsBefore = securityServer.stats().frameTimeouts;
    QTcpSocket incompleteSocket;
    QByteArray incompleteKey;
    const bool incompleteAuthenticated = securityListening && connectRawAuthenticated(
        incompleteSocket, *securityPort, rawToken, incompleteKey);
    if (incompleteAuthenticated) incompleteSocket.write(QByteArray(2, '\0'));
    const bool incompleteTimedOut = incompleteAuthenticated && pumpUntil([&] {
        return securityServer.stats().frameTimeouts > frameTimeoutsBefore &&
            securityServer.stats().activeConnections == 0;
    }, jam2::control_protocol::kIncompleteFrameDeadlineMs + 1000);
    check(QStringLiteral("controller.incomplete-authenticated-frame-has-bounded-deadline"),
        incompleteTimedOut);

    const QString rejectedToken =
        QStringLiteral("00000000000000060000000000000006");
    QTcpSocket rejectedSocket;
    QByteArray rejectedKey;
    const bool rejectionPeerAuthenticated = securityListening &&
        connectRawAuthenticated(
            rejectedSocket, *securityPort, rejectedToken, rejectedKey);
    const bool rejectionQueued = rejectionPeerAuthenticated &&
        securityServer.rejectAuthenticatedPeer(
            rejectedToken, QStringLiteral("Validation peer limit"));
    const bool rejectedPeerClosed = rejectionQueued && pumpUntil([&] {
        return securityServer.stats().activeConnections == 0;
    }, 1000);
    check(QStringLiteral("controller.authenticated-peer-rejection-is-delivered-and-closed"),
        rejectionPeerAuthenticated && rejectionQueued && rejectedPeerClosed &&
            securityServer.stats().authenticatedCapRejects == 1);

    int deliveredRawMessages = 0;
    securityServer.onMessage = [&](const QString&, const QJsonObject&) {
        ++deliveredRawMessages;
    };
    const quint64 replayRejectsBefore = securityServer.stats().sequenceOrTagRejects;
    QTcpSocket replaySocket;
    QByteArray replayKey;
    const bool replayAuthenticated = securityListening && connectRawAuthenticated(
        replaySocket, *securityPort, rawToken, replayKey);
    const QByteArray replayFrame = jam2::control_protocol::encodeAuthenticated(
        QJsonObject{{QStringLiteral("type"), QStringLiteral("test.probe")}},
        replayKey,
        1);
    if (replayAuthenticated && !replayFrame.isEmpty()) {
        replaySocket.write(replayFrame + replayFrame);
    }
    const bool replayRejected = replayAuthenticated && pumpUntil([&] {
        return deliveredRawMessages == 1 &&
            securityServer.stats().sequenceOrTagRejects > replayRejectsBefore &&
            securityServer.stats().activeConnections == 0;
    }, 1000);
    check(QStringLiteral("controller.authenticated-frame-replay-is-rejected-once"),
        replayRejected);

    const quint64 tagRejectsBefore = securityServer.stats().sequenceOrTagRejects;
    QTcpSocket tagSocket;
    QByteArray tagKey;
    const bool tagAuthenticated = securityListening && connectRawAuthenticated(
        tagSocket, *securityPort, rawToken, tagKey);
    QByteArray invalidTagFrame = jam2::control_protocol::encodeAuthenticated(
        QJsonObject{{QStringLiteral("type"), QStringLiteral("test.probe")}},
        tagKey,
        1);
    if (!invalidTagFrame.isEmpty()) invalidTagFrame[invalidTagFrame.size() - 1] ^= 0x01;
    if (tagAuthenticated && !invalidTagFrame.isEmpty()) tagSocket.write(invalidTagFrame);
    const bool invalidTagRejected = tagAuthenticated && pumpUntil([&] {
        return securityServer.stats().sequenceOrTagRejects > tagRejectsBefore &&
            securityServer.stats().activeConnections == 0;
    }, 1000);
    check(QStringLiteral("controller.authenticated-frame-tag-corruption-is-rejected"),
        invalidTagRejected && deliveredRawMessages == 1);

    const quint64 frameRejectsBefore = securityServer.stats().frameRejects;
    QTcpSocket oversizedSocket;
    QJsonObject oversizedChallenge;
    if (securityListening) {
        oversizedSocket.connectToHost(QHostAddress::LocalHost, *securityPort);
        if (readHandshakeFrame(oversizedSocket, oversizedChallenge)) {
            QByteArray invalidLength(4, '\0');
            invalidLength[0] = static_cast<char>(0x7f);
            oversizedSocket.write(invalidLength);
        }
    }
    const bool oversizedRejected = securityListening && pumpUntil([&] {
        return securityServer.stats().frameRejects > frameRejectsBefore &&
            securityServer.stats().activeConnections == 0;
    }, 1000);
    check(QStringLiteral("controller.oversized-frame-prefix-fails-closed"), oversizedRejected);

    // Own a fresh failure window for the exact 64-plus-one limiter proof. The
    // preceding deadline/replay/oversize cases deliberately keep one listener
    // alive for several seconds and must not consume this case's window age.
    securityServer.close();
    const auto rateLimitPort = unusedLoopbackPort();
    ControlServer rateLimitServer;
    const bool rateLimitListening = rateLimitPort && rateLimitServer.listen(
        *rateLimitPort,
        QStringLiteral("0102030405060708"),
        QStringLiteral("000102030405060708090a0b0c0d0e0f"));
    const quint64 authenticationRejectsBefore =
        rateLimitServer.stats().authenticationRejects;
    int completedFailedKeyAttempts = 0;
    bool failedKeyChallengeRead = true;
    bool failedKeyCleanupObserved = true;
    for (int attempt = 0;
         rateLimitListening &&
             attempt < jam2::control_protocol::kMaxAuthenticationFailuresPerWindow;
         ++attempt) {
        QTcpSocket socket;
        QJsonObject challenge;
        socket.connectToHost(QHostAddress::LocalHost, *rateLimitPort);
        if (!readHandshakeFrame(socket, challenge, kFailedKeyHandshakeTimeoutMs)) {
            failedKeyChallengeRead = false;
            break;
        }
        const QByteArray invalidProof = jam2::control_protocol::encodeHandshake(
            QJsonObject{{QStringLiteral("type"), QStringLiteral("hello.proof")}});
        socket.write(invalidProof);
        const quint64 target = authenticationRejectsBefore +
            static_cast<quint64>(attempt + 1);
        if (!pumpUntil([&] {
                return rateLimitServer.stats().authenticationRejects >= target &&
                    rateLimitServer.stats().activeConnections == 0;
            }, kFailedKeyCleanupTimeoutMs)) {
            failedKeyCleanupObserved = false;
            break;
        }
        ++completedFailedKeyAttempts;
    }
    QTcpSocket rateLimitedSocket;
    if (rateLimitListening) {
        rateLimitedSocket.connectToHost(QHostAddress::LocalHost, *rateLimitPort);
    }
    const bool authenticationRateLimited = rateLimitListening && pumpUntil([&] {
        return rateLimitServer.stats().authenticationRateLimitRejects > 0 &&
            rateLimitServer.stats().activeConnections == 0;
    }, kFailedKeyRateLimitTimeoutMs);
    const auto finalSecurityStats = rateLimitServer.stats();
    const quint64 authenticationRejectDelta =
        finalSecurityStats.authenticationRejects - authenticationRejectsBefore;
    check(QStringLiteral("controller.failed-key-work-is-rate-limited-and-bounded"),
        authenticationRateLimited &&
            authenticationRejectDelta ==
                static_cast<quint64>(
                    jam2::control_protocol::kMaxAuthenticationFailuresPerWindow) &&
            finalSecurityStats.activeConnectionHighWater <=
                static_cast<quint64>(jam2::control_protocol::kMaxPendingPeers) &&
            finalSecurityStats.maxBufferedInputBytes <=
                static_cast<quint64>(jam2::control_protocol::kMaxJsonBytes +
                    jam2::control_protocol::kAuthenticatedHeaderBytes + 4),
        QStringLiteral(
            "rate_limited=%1 rate_limit_rejects=%2 authentication_reject_delta=%3 "
            "attempts=%4 challenge_read=%5 cleanup_observed=%6 active=%7 "
            "active_high_water=%8 max_buffered_input_bytes=%9")
            .arg(authenticationRateLimited)
            .arg(finalSecurityStats.authenticationRateLimitRejects)
            .arg(authenticationRejectDelta)
            .arg(completedFailedKeyAttempts)
            .arg(failedKeyChallengeRead)
            .arg(failedKeyCleanupObserved)
            .arg(finalSecurityStats.activeConnections)
            .arg(finalSecurityStats.activeConnectionHighWater)
            .arg(finalSecurityStats.maxBufferedInputBytes));
    rateLimitServer.close();

    SharedSessionController creator;
    EventCapture creatorCapture;
    int creatorPeerDisconnectCallbacks = 0;
    int creatorAssetDisconnectCallbacks = 0;
    creator.onTransportEvent = [&](const TransportEvent& event, bool) {
        creatorCapture.event(event);
    };
    creator.onPeerDisconnected = [&](const QString&) {
        ++creatorPeerDisconnectCallbacks;
    };
    creator.onAssetDisconnected = [&](const QString&) {
        ++creatorAssetDisconnectCallbacks;
    };
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
            if (attempt == 2) {
                // Exercise a client that transmits before receiving the
                // required server-first challenge. The native transport must
                // still put the complete challenge on the wire first.
                socket.write(QByteArray(1, '\0'));
            }
            QByteArray challengeBytes;
            bool completeChallenge = false;
            const bool challenged = pumpUntil([&] {
                challengeBytes += socket.readAll();
                QByteArray probe = challengeBytes;
                QByteArray body;
                QString error;
                if (jam2::control_protocol::takeFrame(probe, body, error) ==
                    jam2::control_protocol::TakeFrameResult::Ready) {
                    QJsonObject message;
                    completeChallenge = jam2::control_protocol::decodeHandshake(
                        body, message, error) &&
                        message.value(QStringLiteral("type")).toString() ==
                            QStringLiteral("hello.challenge");
                }
                return completeChallenge ||
                    socket.state() == QAbstractSocket::UnconnectedState;
            }, 500) && completeChallenge;
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
            preAuthDisconnectsObserved &&
            creator.serverStats().preAuthenticationDisconnects >= 2 &&
            creatorCapture.preAuthenticationDisconnect >= 2);

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

    const QByteArray controlBinary("control-binary", 14);
    QByteArray creatorControlBinary;
    QByteArray joinerControlBinary;
    QString controlBinarySource;
    creator.onBinaryMessage =
        [&](const QString& source, const QByteArray& payload) {
            controlBinarySource = source;
            creatorControlBinary = payload;
        };
    joiner.onBinaryMessage =
        [&](const QString&, const QByteArray& payload) {
            joinerControlBinary = payload;
        };
    const bool controlQueuesReady = joined && creator.isConnected() &&
        joiner.isConnected() &&
        creator.canQueueTo(joiner.snapshot().localToken, controlBinary.size()) &&
        joiner.canQueueTo(QString{}, controlBinary.size());
    const bool joinerControlSent = controlQueuesReady &&
        joiner.sendBinaryTo(QString{}, controlBinary);
    const bool creatorControlReceived = joinerControlSent && pumpUntil([&] {
        return creatorControlBinary == controlBinary &&
            controlBinarySource == joiner.snapshot().localToken;
    }, 1000);
    const bool creatorControlSent = creatorControlReceived &&
        creator.sendBinaryTo(joiner.snapshot().localToken, controlBinary);
    const bool joinerControlReceived = creatorControlSent && pumpUntil([&] {
        return joinerControlBinary == controlBinary;
    }, 1000);
    const auto assetClientDiagnostics = joiner.assetClientStats();
    check(QStringLiteral("controller.authenticated-control-binary-roundtrip"),
        controlQueuesReady && creatorControlReceived && joinerControlReceived &&
            assetClientDiagnostics.framesSent > 0);

    const QByteArray binaryAsset = jam2::application::asset_chunk::encode({
        QString(64, QLatin1Char('a')), 0, 0, QByteArray("binary-asset", 12)});
    QByteArray creatorBinary;
    QByteArray joinerBinary;
    QString binarySource;
    creator.onAssetBinaryMessage = [&](const QString& source, const QByteArray& payload) {
        binarySource = source;
        creatorBinary = payload;
    };
    joiner.onAssetBinaryMessage = [&](const QString&, const QByteArray& payload) {
        joinerBinary = payload;
    };
    const bool assetChannelReady = joined && pumpUntil([&] {
        return joiner.canQueueAssetTo(QString{}, 1024) &&
            creator.serverStats().assetActiveConnections > 0;
    }, 1000);
    check(QStringLiteral("controller.dedicated-asset-channel-authenticated"),
        assetChannelReady);
    const bool joinerBinarySent = assetChannelReady && !binaryAsset.isEmpty() &&
        joiner.sendAssetBinaryTo(QString{}, binaryAsset);
    check(QStringLiteral("controller.dedicated-asset-channel-upload-queued"),
        joinerBinarySent);
    const bool creatorBinaryReceived = joinerBinarySent && pumpUntil([&] {
        return creatorBinary == binaryAsset && binarySource == joiner.snapshot().localToken;
    }, 1000);
    check(QStringLiteral("controller.dedicated-asset-channel-upload"),
        creatorBinaryReceived);
    const bool creatorBinarySent = creatorBinaryReceived &&
        creator.sendAssetBinaryTo(joiner.snapshot().localToken, binaryAsset);
    check(QStringLiteral("controller.dedicated-asset-channel-download-queued"),
        creatorBinarySent);
    const bool joinerBinaryReceived = creatorBinarySent && pumpUntil([&] {
        return joinerBinary == binaryAsset;
    }, 1000);
    check(QStringLiteral("controller.dedicated-asset-channel-download"),
        joinerBinaryReceived);

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
        invalidRejected && afterInvalid.editorRevision == beforeRejected.editorRevision &&
            afterInvalid.editorAuthorityToken == beforeRejected.editorAuthorityToken);

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
        QJsonObject largeSong = minimalValidSong();
        // Exercise the real authenticated TCP client/server path with the
        // same payload shape that previously made generated ideas disappear
        // above the ordinary 64 KiB frame bound.
        largeSong.insert(
            QStringLiteral("generated_recipe_blob"),
            QString(140 * 1024, QLatin1Char('x')));
        const bool sent = joiner.send(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("song.set")},
            {QStringLiteral("arrangement_revision"), 0},
            {QStringLiteral("host_authoritative"), false},
            {QStringLiteral("track_playing"), false},
            {QStringLiteral("song"), largeSong},
        });
        collaborativeProposalDelivered = sent && pumpUntil([&] {
            return proposalSourceToken == joiner.snapshot().localToken &&
                !receivedProposal.value(QStringLiteral("host_authoritative")).toBool(true) &&
                receivedProposal.value(QStringLiteral("song")).toObject() == largeSong;
        }, 2000);
    }
    check(QStringLiteral("controller.large-peer-arrangement-proposal-delivered-atomically"),
        collaborativeProposalDelivered);

    bool nonCoordinatorAuthority = false;
    if (joined) {
        const bool sent = joiner.send(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("beat.set")},
            {QStringLiteral("section"), 0},
            {QStringLiteral("beat"), 0},
            {QStringLiteral("lane"), QStringLiteral("chord")},
            {QStringLiteral("text"), QStringLiteral("Cmaj7")},
        });
        nonCoordinatorAuthority = sent && pumpUntil([&] {
            return creator.snapshot().editorRevision == 1 &&
                creator.snapshot().editorAuthorityToken == joiner.snapshot().localToken &&
                joiner.snapshot().editorRevision == 1 &&
                joiner.snapshot().editorAuthorityToken == joiner.snapshot().localToken;
        }, 1000);
    }
    check(QStringLiteral("controller.non-coordinator-editor-authority"), nonCoordinatorAuthority);

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
                snapshot.editorRevision == 1 &&
                snapshot.editorAuthorityToken == joiner.snapshot().localToken &&
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
    const auto statsBeforeAutoReconnect = creator.serverStats();
    const int peerDisconnectsBeforeAutoReconnect = creatorPeerDisconnectCallbacks;
    const int assetDisconnectsBeforeAutoReconnect = creatorAssetDisconnectCallbacks;
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

    creatorBinary.clear();
    joinerBinary.clear();
    binarySource.clear();
    const bool sameTokenAssetReauthenticated = autoReconnected && pumpUntil([&] {
        return creator.serverStats().assetActiveConnections == 1 &&
            creator.serverStats().assetAcceptedConnections >
                statsBeforeAutoReconnect.assetAcceptedConnections &&
            joiner.canQueueAssetTo(QString{}, 1024);
    }, 1500);
    const bool postReconnectUploadQueued = sameTokenAssetReauthenticated &&
        joiner.sendAssetBinaryTo(QString{}, binaryAsset);
    const bool postReconnectUploadReceived = postReconnectUploadQueued && pumpUntil([&] {
        return creatorBinary == binaryAsset && binarySource == joinerToken;
    }, 1000);
    const bool postReconnectDownloadQueued = postReconnectUploadReceived &&
        creator.sendAssetBinaryTo(joinerToken, binaryAsset);
    const bool postReconnectDownloadReceived = postReconnectDownloadQueued && pumpUntil([&] {
        return joinerBinary == binaryAsset;
    }, 1000);
    const auto statsAfterAutoReconnect = creator.serverStats();
    check(QStringLiteral(
        "controller.same-token-control-disconnect-removes-and-reauthenticates-asset"),
        postReconnectDownloadReceived &&
            creatorPeerDisconnectCallbacks == peerDisconnectsBeforeAutoReconnect + 1 &&
            creatorAssetDisconnectCallbacks == assetDisconnectsBeforeAutoReconnect + 1 &&
            statsAfterAutoReconnect.assetDisconnectedConnections ==
                statsBeforeAutoReconnect.assetDisconnectedConnections + 1 &&
            statsAfterAutoReconnect.assetActiveConnections == 1 &&
            statsAfterAutoReconnect.authenticationRejects ==
                statsBeforeAutoReconnect.authenticationRejects &&
            statsAfterAutoReconnect.authenticationRateLimitRejects ==
                statsBeforeAutoReconnect.authenticationRateLimitRejects);

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
            finalJoiner.editorAuthorityToken.isEmpty() && finalJoiner.editorRevision >= 2 &&
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

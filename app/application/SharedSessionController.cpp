#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "SharedSessionController.hpp"

#include "ApplicationRuntime.hpp"
#include "ControlMessageValidation.hpp"
#include "ControlProtocol.hpp"

#include <QHostAddress>
#include <QJsonArray>
#include <QNetworkInterface>

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <stdexcept>

using namespace jam2::control_protocol;

namespace {

constexpr int kMembershipEntriesPerPage = 64;
constexpr double kMaxExactJsonInteger = 9007199254740991.0;

bool isGridAction(const QString& type)
{
    return jam2::application::isGridControlMessageType(type);
}

bool isArrangementAction(const QString& type)
{
    return jam2::application::isArrangementControlMessageType(type);
}

bool validToken(const QString& token)
{
    return decodeHex(token, 16).size() == 16;
}

bool parseRevision(const QJsonValue& value, quint64& revision, bool allowZero)
{
    if (!value.isDouble()) {
        return false;
    }
    const double number = value.toDouble(-1.0);
    if (!std::isfinite(number) || std::floor(number) != number || number < 0.0 ||
        number > kMaxExactJsonInteger) {
        return false;
    }
    revision = static_cast<quint64>(number);
    return allowZero || revision != 0;
}

QString normalizedHost(QString host)
{
    if (host.startsWith(QStringLiteral("::ffff:"), Qt::CaseInsensitive)) {
        host = host.mid(7);
    }
    return host;
}

bool isSameMachine(const QString& host)
{
    const QHostAddress address(normalizedHost(host));
    if (address.isNull()) {
        return false;
    }
    if (address.isLoopback()) {
        return true;
    }
    const QList<QHostAddress> local = QNetworkInterface::allAddresses();
    return std::any_of(local.cbegin(), local.cend(), [&address](const QHostAddress& candidate) {
        return candidate == address;
    });
}

bool isWildcardEndpoint(const QString& endpoint, int& separator)
{
    separator = endpoint.lastIndexOf(QLatin1Char(':'));
    if (separator <= 0) {
        return false;
    }
    const QString host = endpoint.left(separator);
    return host == QStringLiteral("0.0.0.0") || host == QStringLiteral("::") ||
        host == QStringLiteral("[::]");
}

bool matchesContractSampleRate(const QJsonObject& message, int expectedSampleRate)
{
    if (expectedSampleRate <= 0) {
        return true;
    }
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("looper.recording.offer")) {
        return message.value(QStringLiteral("sample_rate")).toInt() == expectedSampleRate;
    }
    if (type != QStringLiteral("song.set")) {
        return true;
    }
    const QJsonArray banks = message.value(QStringLiteral("song")).toObject()
        .value(QStringLiteral("looper")).toObject()
        .value(QStringLiteral("banks")).toArray();
    for (const QJsonValue& bankValue : banks) {
        for (const QJsonValue& laneValue : bankValue.toObject()
                 .value(QStringLiteral("lanes")).toArray()) {
            const QJsonObject lane = laneValue.toObject();
            if (!lane.value(QStringLiteral("asset_hash")).toString().isEmpty() &&
                lane.value(QStringLiteral("sample_rate")).toInt() != expectedSampleRate) {
                return false;
            }
        }
    }
    return true;
}

}

SharedSessionController::SharedSessionController(QObject* parent)
    : QObject(parent), server_(this), client_(this)
{
    reconnectTimer_.setSingleShot(true);
    QObject::connect(&reconnectTimer_, &QTimer::timeout, this, [this] {
        if (!reconnectEnabled_ || closing_ || role_ != Role::Joiner || client_.isConnected()) {
            return;
        }
        ++reconnectAttempts_;
        setLifecycle(Lifecycle::Reconnecting);
        publishTransportEvent(TransportEvent{
            TransportEventType::ReconnectAttempt,
            TransportFailure::None,
            QStringLiteral("TCP control reconnect attempt"),
            true}, false);
        connectJoiner();
    });
    heartbeatTimer_.setSingleShot(false);
    QObject::connect(&heartbeatTimer_, &QTimer::timeout, this, [this] {
        sendHeartbeat();
    });
    heartbeatDeadlineTimer_.setSingleShot(false);
    QObject::connect(&heartbeatDeadlineTimer_, &QTimer::timeout, this, [this] {
        checkHeartbeatDeadline();
    });

    server_.onEvent = [this](const TransportEvent& event) { handleServerEvent(event); };
    server_.onMessage = [this](const QString& token, const QJsonObject& message) {
        const QString type = message.value(QStringLiteral("type")).toString();
        const auto decision = jam2::application::evaluateControlMessage(
            message, jam2::application::ControlMessageSource::AuthenticatedPeer);
        if (!decision.accepted) {
            if (decision.rejection == jam2::application::ControlMessageRejection::Authorization) {
                ++authorizationRejections_;
            } else {
                ++validationRejections_;
            }
            return;
        }
        if (!matchesContractSampleRate(message, contract_.sampleRate)) {
            ++validationRejections_;
            return;
        }
        if (type == QStringLiteral("session.heartbeat.ack")) {
            ++heartbeatAcksReceived_;
            publishSnapshot();
            return;
        }
        if (type == QStringLiteral("session.endpoint.update")) {
            const QString endpoint = message.value(QStringLiteral("udp_endpoint")).toString();
            auto peer = peers_.find(token);
            if (peer == peers_.end() || peer->endpoint == endpoint) {
                return;
            }
            try {
                (void)jam2::parse_endpoint(endpoint.toStdString());
            } catch (const std::exception&) {
                ++validationRejections_;
                return;
            }
            peer->endpoint = endpoint;
            peer->edgeState = EdgeState::Candidate;
            peer->proofState = QStringLiteral("endpoint-updated");
            peer->streamState = QStringLiteral("candidate");
            broadcastMembership();
            return;
        }
        QJsonObject routed = message;
        if (isGridAction(type) && role_ == Role::Creator) {
            if (gridRevision_ == std::numeric_limits<quint64>::max()) {
                server_.sendTo(token, QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("session.error")},
                    {QStringLiteral("message"), QStringLiteral("Grid revision exhausted")},
                });
                return;
            }
            routed[QStringLiteral("revision")] = static_cast<qint64>(++gridRevision_);
            routed[QStringLiteral("authority_token")] = token;
            gridAuthorityToken_ = token;
            gridState_ = routed;
            server_.send(routed);
            publishSnapshot();
        }
        if (onMessage) {
            onMessage(token, routed);
        }
    };
    server_.onAuthenticated = [this](const QString& token, const QJsonObject& message) {
        handleAuthenticatedPeer(token, message);
    };
    server_.onDisconnected = [this](const QString& token) { handleDisconnectedPeer(token); };
    client_.onEvent = [this](const TransportEvent& event) { handleClientEvent(event); };
    client_.onMessage = [this](const QJsonObject& message) { handleClientMessage(message); };
}

void SharedSessionController::bindRuntime(
    ApplicationRuntime& runtime,
    std::function<Jam2RuntimeOptions(const Snapshot&)> networkOptionsFactory)
{
    runtime_ = &runtime;
    networkOptionsFactory_ = std::move(networkOptionsFactory);
}

bool SharedSessionController::startLocal(const Jam2RuntimeOptions& options)
{
    close();
    closing_ = false;
    role_ = Role::Local;
    failure_ = TransportFailure::None;
    failureDetail_.clear();
    failureRetryable_ = false;
    if (runtime_ == nullptr || !runtime_->startLocal(options)) {
        const QString detail = QStringLiteral("Local audio runtime failed to start");
        fail(TransportFailure::RuntimeStartFailed, detail, false);
        publishTransportEvent(TransportEvent{
            TransportEventType::Failure,
            TransportFailure::RuntimeStartFailed,
            detail}, false);
        return false;
    }
    setLifecycle(Lifecycle::Active);
    return true;
}

bool SharedSessionController::startCreator(const CreatorConfig& config)
{
    // The headless creator starts UDP first to discover its advertised
    // endpoint. Preserve that already-running runtime while taking ownership
    // of its TCP/bootstrap lifecycle.
    reset(false);
    closing_ = false;
    role_ = Role::Creator;
    runtimeAttachmentEnabled_ = false;
    creator_ = config;
    creator_.sessionPeerLimit = std::max(0, creator_.sessionPeerLimit);
    heartbeatIntervalMs_ = std::clamp(config.heartbeatIntervalMs, 10, 60000);
    heartbeatMissLimit_ = std::clamp(config.heartbeatMissLimit, 1, 20);
    heartbeatTimer_.setInterval(heartbeatIntervalMs_);
    coordinatorToken_ = config.localToken;
    contract_ = config.contract;
    contractReady_ = contract_.protocolVersion == 1 && !contract_.audioFormat.isEmpty() &&
        contract_.sampleRate > 0 && contract_.frameSize > 0;
    contractRevision_ = contractReady_ ? 1 : 0;
    gridAuthorityToken_ = config.localToken;
    arrangementAuthorityToken_ = config.localToken;
    Peer local;
    local.endpoint = config.localEndpoint;
    local.peerId = peerIdForToken(config.localToken);
    local.edgeState = EdgeState::Active;
    local.proofState = QStringLiteral("local");
    local.streamState = QStringLiteral("local");
    peers_.insert(config.localToken, local);
    reconnectEnabled_ = true;
    if (!server_.listen(config.port, config.sessionHex, config.keyHex)) {
        fail(
            failure_ == TransportFailure::None ? TransportFailure::TransportError : failure_,
            server_.errorString(),
            false);
        return false;
    }
    runtimeAttachmentEnabled_ = true;
    heartbeatTimer_.start();
    setLifecycle(Lifecycle::Listening);
    return true;
}

bool SharedSessionController::startJoiner(const JoinerConfig& config)
{
    close();
    closing_ = false;
    role_ = Role::Joiner;
    runtimeAttachmentEnabled_ = true;
    joiner_ = config;
    joiner_.reconnectIntervalMs = std::clamp(config.reconnectIntervalMs, 10, 60000);
    joiner_.reconnectAttemptLimit = std::clamp(config.reconnectAttemptLimit, 1, 100);
    heartbeatIntervalMs_ = std::clamp(config.heartbeatIntervalMs, 10, 60000);
    heartbeatMissLimit_ = std::clamp(config.heartbeatMissLimit, 1, 20);
    reconnectTimer_.setInterval(joiner_.reconnectIntervalMs);
    heartbeatDeadlineTimer_.setInterval(std::clamp(heartbeatIntervalMs_ / 2, 10, 1000));
    reconnectAttemptLimit_ = joiner_.reconnectAttemptLimit;
    reconnectEnabled_ = true;
    reconnectAttempts_ = 0;
    failure_ = TransportFailure::None;
    failureDetail_.clear();
    failureRetryable_ = false;
    setLifecycle(Lifecycle::Connecting);
    connectJoiner();
    return true;
}

bool SharedSessionController::endSession(const QString& detail)
{
    if (role_ != Role::Creator) {
        return false;
    }
    const QJsonObject message{
        {QStringLiteral("type"), QStringLiteral("session.end")},
        {QStringLiteral("detail"), detail.left(512)},
    };
    const auto decision = jam2::application::evaluateControlMessage(
        message, jam2::application::ControlMessageSource::LocalCreator);
    if (!decision.accepted) {
        ++validationRejections_;
        publishSnapshot();
        return false;
    }
    server_.send(message);
    return true;
}

void SharedSessionController::close()
{
    reset(true);
}

void SharedSessionController::reset(bool stopRuntime)
{
    closing_ = true;
    runtimeAttachmentEnabled_ = false;
    runtimePeers_.clear();
    reconnectEnabled_ = false;
    reconnectTimer_.stop();
    heartbeatTimer_.stop();
    heartbeatDeadlineTimer_.stop();
    server_.close();
    client_.close();
    if (stopRuntime && runtime_ != nullptr && runtime_->isNetworkRunning()) {
        runtime_->stopNetwork();
    }
    role_ = Role::Inactive;
    lifecycle_ = Lifecycle::Inactive;
    creator_ = {};
    joiner_ = {};
    peers_.clear();
    membershipAssembly_ = {};
    coordinatorToken_.clear();
    gridAuthorityToken_.clear();
    arrangementAuthorityToken_.clear();
    membershipRevision_ = 0;
    contractRevision_ = 0;
    gridRevision_ = 0;
    arrangementRevision_ = 0;
    contractReady_ = false;
    contract_ = {};
    reconnectAttempts_ = 0;
    reconnectAttemptLimit_ = 15;
    everAuthenticated_ = false;
    failure_ = TransportFailure::None;
    failureDetail_.clear();
    failureRetryable_ = false;
    gridState_ = {};
    arrangementState_ = {};
    heartbeatIntervalMs_ = kDefaultHeartbeatIntervalMs;
    heartbeatMissLimit_ = kDefaultHeartbeatMissLimit;
    heartbeatSequence_ = 0;
    heartbeatsSent_ = 0;
    heartbeatsReceived_ = 0;
    heartbeatAcksReceived_ = 0;
    validationRejections_ = 0;
    authorizationRejections_ = 0;
    lastHeartbeat_.invalidate();
    publishSnapshot();
}

void SharedSessionController::refresh()
{
    if (!reconnectEnabled_ || closing_) {
        return;
    }
    if (role_ == Role::Creator) {
        publishTransportEvent(TransportEvent{
            TransportEventType::RefreshRequested,
            TransportFailure::None,
            QStringLiteral("TCP control refresh requested")}, true);
        if (server_.hasPeer()) {
            publishTransportEvent(TransportEvent{
                TransportEventType::AlreadyConnected,
                TransportFailure::None,
                QStringLiteral("TCP control already connected"),
                false,
                true}, true);
        }
        return;
    }
    if (role_ == Role::Joiner && !client_.isConnected()) {
        publishTransportEvent(TransportEvent{
            TransportEventType::RefreshRequested,
            TransportFailure::None,
            QStringLiteral("TCP control refresh requested"),
            true}, false);
        reconnectTimer_.stop();
        failure_ = TransportFailure::None;
        failureDetail_.clear();
        failureRetryable_ = false;
        ++reconnectAttempts_;
        setLifecycle(Lifecycle::Reconnecting);
        connectJoiner();
    }
}

void SharedSessionController::setReconnectEnabled(bool enabled)
{
    reconnectEnabled_ = enabled;
    if (!enabled) {
        reconnectTimer_.stop();
    }
}

void SharedSessionController::updateLocalEndpoint(const QString& endpoint)
{
    const QString token = role_ == Role::Creator ? creator_.localToken : joiner_.localToken;
    if (token.isEmpty() || endpoint.isEmpty()) {
        return;
    }
    if (role_ == Role::Creator) {
        creator_.localEndpoint = endpoint;
    } else {
        joiner_.localEndpoint = endpoint;
    }
    auto it = peers_.find(token);
    if (it != peers_.end() && it->endpoint != endpoint) {
        it->endpoint = endpoint;
        if (role_ == Role::Creator) {
            broadcastMembership();
        } else {
            publishSnapshot();
            if (client_.isConnected()) {
                (void)send(QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("session.endpoint.update")},
                    {QStringLiteral("udp_endpoint"), endpoint},
                });
            }
        }
    }
}

void SharedSessionController::updatePeerEdgeState(
    quint64 peerId,
    EdgeState state,
    const QString& proofState,
    const QString& streamState)
{
    for (auto it = peers_.begin(); it != peers_.end(); ++it) {
        if (it->peerId != peerId) {
            continue;
        }
        const EdgeState beforeState = it->edgeState;
        const QString beforeProof = it->proofState;
        const QString beforeStream = it->streamState;
        it->edgeState = state;
        if (!proofState.isEmpty()) {
            it->proofState = proofState;
        }
        if (!streamState.isEmpty()) {
            it->streamState = streamState;
        }
        if (it->edgeState != beforeState || it->proofState != beforeProof ||
            it->streamState != beforeStream) {
            publishSnapshot();
        }
        return;
    }
}

bool SharedSessionController::send(const QJsonObject& message)
{
    const auto source = role_ == Role::Creator
        ? jam2::application::ControlMessageSource::LocalCreator
        : jam2::application::ControlMessageSource::LocalJoiner;
    const auto decision = jam2::application::evaluateControlMessage(message, source);
    if (!decision.accepted) {
        if (decision.rejection == jam2::application::ControlMessageRejection::Authorization) {
            ++authorizationRejections_;
        } else {
            ++validationRejections_;
        }
        publishSnapshot();
        return false;
    }
    if (!matchesContractSampleRate(message, contract_.sampleRate)) {
        ++validationRejections_;
        publishSnapshot();
        return false;
    }
    if (role_ == Role::Creator) {
        QJsonObject routed = message;
        const QString type = routed.value(QStringLiteral("type")).toString();
        if (isGridAction(type)) {
            if (gridRevision_ == std::numeric_limits<quint64>::max()) {
                return false;
            }
            routed[QStringLiteral("revision")] = static_cast<qint64>(++gridRevision_);
            routed[QStringLiteral("authority_token")] = creator_.localToken;
            gridAuthorityToken_ = creator_.localToken;
            gridState_ = routed;
            publishSnapshot();
        } else if (isArrangementAction(type)) {
            if (arrangementRevision_ == std::numeric_limits<quint64>::max()) {
                return false;
            }
            routed[QStringLiteral("arrangement_revision")] =
                static_cast<qint64>(++arrangementRevision_);
            routed[QStringLiteral("authority_token")] = creator_.localToken;
            arrangementAuthorityToken_ = creator_.localToken;
            arrangementState_ = routed;
            publishSnapshot();
        }
        server_.send(routed);
        return server_.hasPeer();
    }
    return role_ == Role::Joiner && client_.send(message);
}

bool SharedSessionController::sendTo(
    const QString& token,
    const QJsonObject& message,
    bool closeAfterWrite)
{
    if (role_ == Role::Creator) {
        const auto source = message.value(QStringLiteral("type")).toString() ==
                QStringLiteral("debug.lifecycle.disconnect")
            ? jam2::application::ControlMessageSource::Internal
            : jam2::application::ControlMessageSource::LocalCreator;
        const auto decision = jam2::application::evaluateControlMessage(message, source);
        if (!decision.accepted) {
            if (decision.rejection == jam2::application::ControlMessageRejection::Authorization) {
                ++authorizationRejections_;
            } else {
                ++validationRejections_;
            }
            publishSnapshot();
            return false;
        }
        return server_.sendTo(token, message, closeAfterWrite);
    }
    return role_ == Role::Joiner && token.isEmpty() && !closeAfterWrite && send(message);
}

bool SharedSessionController::canQueueTo(const QString& token, qint64 additionalBytes) const
{
    return role_ == Role::Creator
        ? server_.canQueueTo(token, additionalBytes)
        : role_ == Role::Joiner && token.isEmpty() && client_.canQueue(additionalBytes);
}

bool SharedSessionController::hasPeer() const
{
    return role_ == Role::Creator ? server_.hasPeer() : client_.isConnected();
}

bool SharedSessionController::isConnected() const
{
    return role_ == Role::Creator ? server_.hasPeer() :
        role_ == Role::Joiner && client_.isConnected();
}

QString SharedSessionController::errorString() const
{
    return role_ == Role::Creator ? server_.errorString() : QString{};
}

SharedSessionController::Snapshot SharedSessionController::snapshot() const
{
    Snapshot result;
    result.role = role_;
    result.lifecycle = lifecycle_;
    result.localToken = role_ == Role::Creator ? creator_.localToken : joiner_.localToken;
    result.coordinatorToken = coordinatorToken_;
    result.gridAuthorityToken = gridAuthorityToken_;
    result.arrangementAuthorityToken = arrangementAuthorityToken_;
    result.membershipRevision = membershipRevision_;
    result.contractRevision = contractRevision_;
    result.gridRevision = gridRevision_;
    result.arrangementRevision = arrangementRevision_;
    result.sessionPeerLimit = role_ == Role::Creator ? creator_.sessionPeerLimit : 0;
    result.reconnectAttempts = reconnectAttempts_;
    result.reconnectAttemptLimit = role_ == Role::Joiner ? reconnectAttemptLimit_ : 0;
    result.reconnectIntervalMs = role_ == Role::Joiner ? reconnectTimer_.interval() : 0;
    result.heartbeatIntervalMs = heartbeatIntervalMs_;
    result.heartbeatMissLimit = heartbeatMissLimit_;
    result.lastHeartbeatAgeMs = lastHeartbeat_.isValid() ? lastHeartbeat_.elapsed() : -1;
    result.heartbeatMissed = result.lastHeartbeatAgeMs < 0 ? 0 : std::min(
        heartbeatMissLimit_, static_cast<int>(result.lastHeartbeatAgeMs / heartbeatIntervalMs_));
    result.heartbeatsSent = heartbeatsSent_;
    result.heartbeatsReceived = heartbeatsReceived_;
    result.heartbeatAcksReceived = heartbeatAcksReceived_;
    result.validationRejections = validationRejections_;
    result.authorizationRejections = authorizationRejections_;
    result.totalPeerCount = static_cast<int>(peers_.size());
    const QString localToken = result.localToken;
    result.remotePeerCount = result.totalPeerCount - (peers_.contains(localToken) ? 1 : 0);
    result.contractReady = contractReady_;
    result.membershipReady = role_ == Role::Local ||
        (role_ == Role::Creator && peers_.contains(creator_.localToken)) ||
        (role_ == Role::Joiner && membershipRevision_ > 0 &&
         peers_.contains(joiner_.localToken) && peers_.contains(coordinatorToken_));
    result.networkAttachmentReady = contractReady_ && result.membershipReady;
    result.failure = failure_;
    result.failureDetail = failureDetail_;
    result.failureRetryable = failureRetryable_;
    result.contract = contract_;
    result.peers.reserve(peers_.size());
    for (auto it = peers_.cbegin(); it != peers_.cend(); ++it) {
        result.peers.push_back(PeerSnapshot{
            it.key(), it->peerId, it->endpoint, it->edgeState,
            it->proofState, it->streamState, it->sameMachine});
    }
    return result;
}

quint64 SharedSessionController::peerIdForToken(const QString& token)
{
    bool ok = false;
    const quint64 value = token.left(16).toULongLong(&ok, 16);
    return ok && value != 0 ? value : 1ULL;
}

void SharedSessionController::handleServerEvent(const TransportEvent& event)
{
    if (event.type == TransportEventType::Authenticated) {
        failure_ = TransportFailure::None;
        failureDetail_.clear();
        failureRetryable_ = false;
        setLifecycle(Lifecycle::Authenticated);
    } else if (event.type == TransportEventType::Listening) {
        failure_ = TransportFailure::None;
        failureDetail_.clear();
        failureRetryable_ = false;
        setLifecycle(Lifecycle::Listening);
    } else if (event.type == TransportEventType::Disconnected && role_ == Role::Creator) {
        setLifecycle(peers_.size() > 1 ? Lifecycle::Active : Lifecycle::Listening);
    } else if (event.type == TransportEventType::Failure &&
               (event.failure == TransportFailure::InvalidConfiguration ||
                event.failure == TransportFailure::TransportError) &&
               !server_.hasPeer()) {
        failure_ = event.failure;
        failureDetail_ = event.detail;
        failureRetryable_ = event.retryable;
        publishSnapshot();
    }
    publishTransportEvent(event, true);
}

void SharedSessionController::handleClientEvent(const TransportEvent& event)
{
    if (event.type == TransportEventType::Authenticated) {
        const bool firstAuthentication = !everAuthenticated_;
        everAuthenticated_ = true;
        reconnectAttempts_ = 0;
        reconnectTimer_.stop();
        if (firstAuthentication || !lastHeartbeat_.isValid()) {
            lastHeartbeat_.start();
        }
        heartbeatDeadlineTimer_.start();
        failure_ = TransportFailure::None;
        failureDetail_.clear();
        failureRetryable_ = false;
        setLifecycle(Lifecycle::Authenticated);
    } else if (event.type == TransportEventType::Disconnected && !closing_) {
        scheduleReconnect();
    } else if (event.type == TransportEventType::Failure) {
        if (event.retryable) {
            failure_ = event.failure;
            failureDetail_ = event.detail;
            failureRetryable_ = true;
            publishSnapshot();
            scheduleReconnect();
        } else {
            reconnectEnabled_ = false;
            fail(event.failure, event.detail, false);
        }
    } else if (event.type == TransportEventType::Connecting ||
               event.type == TransportEventType::Connected ||
               event.type == TransportEventType::ProofSent) {
        if (lifecycle_ != Lifecycle::Reconnecting) {
            setLifecycle(Lifecycle::Connecting);
        }
    }
    publishTransportEvent(event, false);
}

void SharedSessionController::handleAuthenticatedPeer(
    const QString& token,
    const QJsonObject& message)
{
    if (role_ != Role::Creator || token.isEmpty()) {
        return;
    }
    const int remotePeers = peers_.size() - (peers_.contains(creator_.localToken) ? 1 : 0);
    if (creator_.sessionPeerLimit > 0 && !peers_.contains(token) &&
        remotePeers >= creator_.sessionPeerLimit) {
        server_.rejectAuthenticatedPeer(token, QStringLiteral("Session peer limit reached"));
        return;
    }

    QString endpoint = message.value(QStringLiteral("udp_endpoint")).toString();
    const QString tcpHost = normalizedHost(message.value(QStringLiteral("tcp_peer_host")).toString());
    int separator = -1;
    if (isWildcardEndpoint(endpoint, separator)) {
        if (tcpHost.isEmpty()) {
            server_.sendTo(token, QJsonObject{
                {QStringLiteral("type"), QStringLiteral("session.error")},
                {QStringLiteral("message"), QStringLiteral("Wildcard UDP candidate could not be resolved")},
            }, true);
            return;
        }
        endpoint = tcpHost + endpoint.mid(separator);
    }
    if (endpoint.isEmpty()) {
        server_.sendTo(token, QJsonObject{
            {QStringLiteral("type"), QStringLiteral("session.error")},
            {QStringLiteral("message"), QStringLiteral("Authenticated peer omitted its UDP endpoint")},
        }, true);
        return;
    }

    Peer peer;
    peer.endpoint = endpoint;
    peer.peerId = peerIdForToken(token);
    peer.edgeState = EdgeState::Authenticated;
    peer.proofState = QStringLiteral("authenticated");
    peer.streamState = QStringLiteral("candidate");
    peer.sameMachine = isSameMachine(tcpHost);
    peers_[token] = peer;
    if (contractReady_) {
        server_.sendTo(token, contractMessage());
    }
    broadcastMembership();
    if (!gridState_.isEmpty()) {
        server_.sendTo(token, gridState_);
    }
    if (!arrangementState_.isEmpty()) {
        server_.sendTo(token, arrangementState_);
    }
    sendHeartbeat(token);
    if (onPeerAuthenticated) {
        onPeerAuthenticated(token, message);
    }
}

void SharedSessionController::handleDisconnectedPeer(const QString& token)
{
    if (role_ == Role::Creator && peers_.remove(token) > 0) {
        if (gridAuthorityToken_ == token) {
            gridAuthorityToken_.clear();
            gridState_ = {};
            if (gridRevision_ != std::numeric_limits<quint64>::max()) {
                ++gridRevision_;
            }
        }
        broadcastMembership();
    }
    if (onPeerDisconnected) {
        onPeerDisconnected(token);
    }
}

void SharedSessionController::handleClientMessage(const QJsonObject& message)
{
    const QString type = message.value(QStringLiteral("type")).toString();
    const auto decision = jam2::application::evaluateControlMessage(
        message, jam2::application::ControlMessageSource::Coordinator);
    if (!decision.accepted) {
        if (decision.rejection == jam2::application::ControlMessageRejection::Authorization) {
            ++authorizationRejections_;
        } else {
            ++validationRejections_;
        }
        publishTransportEvent(TransportEvent{
            TransportEventType::Failure,
            TransportFailure::AuthenticatedFrameRejected,
            decision.reason}, false);
        publishSnapshot();
        return;
    }
    if (!matchesContractSampleRate(message, contract_.sampleRate)) {
        ++validationRejections_;
        publishTransportEvent(TransportEvent{
            TransportEventType::Failure,
            TransportFailure::AuthenticatedFrameRejected,
            QStringLiteral("Control message WAV sample rate does not match the session contract")}, false);
        publishSnapshot();
        return;
    }
    if (type == QStringLiteral("session.heartbeat")) {
        ++heartbeatsReceived_;
        lastHeartbeat_.restart();
        (void)send(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("session.heartbeat.ack")},
            {QStringLiteral("sequence"), message.value(QStringLiteral("sequence"))},
        });
        publishSnapshot();
        return;
    }
    if (type == QStringLiteral("session.end")) {
        reconnectEnabled_ = false;
        reconnectTimer_.stop();
        heartbeatDeadlineTimer_.stop();
        runtimeAttachmentEnabled_ = false;
        if (runtime_ != nullptr && runtime_->isNetworkRunning()) {
            runtime_->stopNetwork();
        }
        client_.close();
        lifecycle_ = Lifecycle::Inactive;
        const QString detail = message.value(QStringLiteral("detail")).toString(
            QStringLiteral("The jam creator ended the session"));
        publishSnapshot();
        publishTransportEvent(TransportEvent{
            TransportEventType::SessionEnded,
            TransportFailure::None,
            detail}, false);
        return;
    }
    if (type == QStringLiteral("session.contract")) {
        if (!acceptContract(message)) {
            client_.send(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("session.error")},
                {QStringLiteral("message"), QStringLiteral("Session contract is invalid or incompatible")},
            });
            reconnectEnabled_ = false;
            const QString detail =
                QStringLiteral("TCP session contract is invalid or incompatible");
            fail(TransportFailure::ContractRejected, detail, false);
            publishTransportEvent(TransportEvent{
                TransportEventType::Failure,
                TransportFailure::ContractRejected,
                detail}, false);
            client_.close();
        }
        return;
    }
    if (type == QStringLiteral("session.membership")) {
        if (!acceptMembershipPage(message)) {
            reconnectEnabled_ = false;
            const QString detail = QStringLiteral("TCP coordinator membership is invalid");
            fail(TransportFailure::MembershipRejected, detail, false);
            publishTransportEvent(TransportEvent{
                TransportEventType::Failure,
                TransportFailure::MembershipRejected,
                detail}, false);
            client_.close();
        }
        return;
    }
    if ((isGridAction(type) || isArrangementAction(type)) &&
        !acceptAuthorityUpdate(message)) {
        publishTransportEvent(TransportEvent{
            TransportEventType::Failure,
            TransportFailure::AuthenticatedFrameRejected,
            QStringLiteral("Rejected stale, malformed, or unauthorized authority update")},
            false);
        return;
    }
    if (onMessage) {
        onMessage(QString{}, message);
    }
}

bool SharedSessionController::acceptContract(const QJsonObject& message)
{
    SessionContract next;
    next.protocolVersion = message.value(QStringLiteral("protocol_version")).toInt();
    next.audioFormat = message.value(QStringLiteral("audio_format")).toString();
    next.profile = message.value(QStringLiteral("profile")).toString();
    next.sampleRate = message.value(QStringLiteral("sample_rate")).toInt();
    next.frameSize = message.value(QStringLiteral("frame_size")).toInt();
    const QString coordinator = message.value(QStringLiteral("coordinator_token")).toString();
    if (next.protocolVersion != 1 || next.audioFormat != QStringLiteral("pcm24-mono") ||
        next.sampleRate <= 0 || next.frameSize <= 0 ||
        !validToken(coordinator)) {
        return false;
    }
    if (joiner_.enforceExpectedContract &&
        (next.protocolVersion != joiner_.expectedContract.protocolVersion ||
         next.audioFormat != joiner_.expectedContract.audioFormat ||
         next.sampleRate != joiner_.expectedContract.sampleRate ||
         next.frameSize != joiner_.expectedContract.frameSize)) {
        return false;
    }
    if (contractReady_) {
        if (next.protocolVersion != contract_.protocolVersion ||
            next.audioFormat != contract_.audioFormat || next.profile != contract_.profile ||
            next.sampleRate != contract_.sampleRate || next.frameSize != contract_.frameSize ||
            coordinator != coordinatorToken_) {
            return false;
        }
    } else {
        contract_ = next;
        contractReady_ = true;
        ++contractRevision_;
        coordinatorToken_ = coordinator;
        if (onContract) {
            onContract(contract_);
        }
    }
    publishSnapshot();
    return true;
}

bool SharedSessionController::acceptAuthorityUpdate(const QJsonObject& message)
{
    const QString type = message.value(QStringLiteral("type")).toString();
    const QString authority = message.value(QStringLiteral("authority_token")).toString();
    if (!validToken(authority) || !peers_.contains(authority)) {
        return false;
    }
    if (isGridAction(type)) {
        quint64 revision = 0;
        if (!parseRevision(message.value(QStringLiteral("revision")), revision, false) ||
            revision < gridRevision_ ||
            (revision == gridRevision_ && authority != gridAuthorityToken_)) {
            return false;
        }
        if (revision > gridRevision_) {
            gridAuthorityToken_ = authority;
            gridRevision_ = revision;
            publishSnapshot();
        }
        return true;
    }
    if (isArrangementAction(type)) {
        quint64 revision = 0;
        if (!parseRevision(
                message.value(QStringLiteral("arrangement_revision")), revision, false) ||
            revision < arrangementRevision_ || authority != coordinatorToken_ ||
            (revision == arrangementRevision_ && authority != arrangementAuthorityToken_)) {
            return false;
        }
        if (revision > arrangementRevision_) {
            arrangementAuthorityToken_ = authority;
            arrangementRevision_ = revision;
            publishSnapshot();
        }
        return true;
    }
    return false;
}

QJsonObject SharedSessionController::contractMessage() const
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("session.contract")},
        {QStringLiteral("protocol_version"), contract_.protocolVersion},
        {QStringLiteral("audio_format"), contract_.audioFormat},
        {QStringLiteral("profile"), contract_.profile},
        {QStringLiteral("sample_rate"), contract_.sampleRate},
        {QStringLiteral("frame_size"), contract_.frameSize},
        {QStringLiteral("coordinator_token"), coordinatorToken_},
    };
}

bool SharedSessionController::acceptMembershipPage(const QJsonObject& message)
{
    if (role_ != Role::Joiner) {
        return false;
    }
    quint64 revision = 0;
    quint64 gridRevision = 0;
    quint64 arrangementRevision = 0;
    const int pageIndex = message.value(QStringLiteral("page_index")).toInt(-1);
    const int pageCount = message.value(QStringLiteral("page_count")).toInt(-1);
    const QString coordinator = message.value(QStringLiteral("coordinator_token")).toString();
    const QString gridAuthority =
        message.value(QStringLiteral("grid_authority_token")).toString();
    const QString arrangementAuthority =
        message.value(QStringLiteral("arrangement_authority_token")).toString();
    const QJsonArray entries = message.value(QStringLiteral("peers")).toArray();
    if (!parseRevision(message.value(QStringLiteral("revision")), revision, false) ||
        !parseRevision(message.value(QStringLiteral("grid_revision")), gridRevision, true) ||
        !parseRevision(
            message.value(QStringLiteral("arrangement_revision")), arrangementRevision, true) ||
        !message.value(QStringLiteral("page_index")).isDouble() ||
        !message.value(QStringLiteral("page_count")).isDouble() ||
        !message.value(QStringLiteral("peers")).isArray() ||
        pageIndex < 0 || pageCount <= 0 ||
        pageIndex >= pageCount || entries.size() > kMembershipEntriesPerPage ||
        !validToken(coordinator) || (!gridAuthority.isEmpty() && !validToken(gridAuthority)) ||
        !validToken(arrangementAuthority)) {
        return false;
    }
    if (membershipAssembly_.revision != revision) {
        if (revision <= membershipRevision_) {
            return false;
        }
        membershipAssembly_ = {};
        membershipAssembly_.revision = revision;
        membershipAssembly_.pageCount = pageCount;
        membershipAssembly_.coordinatorToken = coordinator;
        membershipAssembly_.gridAuthorityToken = gridAuthority;
        membershipAssembly_.arrangementAuthorityToken = arrangementAuthority;
        membershipAssembly_.gridRevision = gridRevision;
        membershipAssembly_.arrangementRevision = arrangementRevision;
    }
    if (membershipAssembly_.pageCount != pageCount ||
        membershipAssembly_.coordinatorToken != coordinator ||
        membershipAssembly_.gridAuthorityToken != gridAuthority ||
        membershipAssembly_.arrangementAuthorityToken != arrangementAuthority ||
        membershipAssembly_.gridRevision != gridRevision ||
        membershipAssembly_.arrangementRevision != arrangementRevision) {
        membershipAssembly_ = {};
        return false;
    }
    membershipAssembly_.pages[pageIndex] = message;
    if (membershipAssembly_.pages.size() != pageCount) {
        return true;
    }

    QMap<QString, Peer> next;
    for (int page = 0; page < pageCount; ++page) {
        const QJsonArray values = membershipAssembly_.pages.value(page)
            .value(QStringLiteral("peers")).toArray();
        for (const QJsonValue& value : values) {
            const QJsonObject object = value.toObject();
            const QString token = object.value(QStringLiteral("token")).toString();
            const QString endpoint = object.value(QStringLiteral("endpoint")).toString();
            bool idOk = false;
            const quint64 peerId = object.value(QStringLiteral("peer_id")).toString().toULongLong(&idOk);
            if (!validToken(token) ||
                endpoint.isEmpty() || endpoint.size() > 255 || !idOk || peerId == 0 ||
                next.contains(token)) {
                membershipAssembly_ = {};
                return false;
            }
            Peer peer;
            peer.endpoint = endpoint;
            peer.peerId = peerId;
            peer.edgeState = token == joiner_.localToken ? EdgeState::Active : EdgeState::Candidate;
            peer.proofState = token == joiner_.localToken ? QStringLiteral("local") : QStringLiteral("candidate");
            peer.streamState = token == joiner_.localToken ? QStringLiteral("local") : QStringLiteral("candidate");
            next.insert(token, peer);
        }
    }
    if (!next.contains(joiner_.localToken) || !next.contains(coordinator) ||
        coordinator == joiner_.localToken || coordinator != coordinatorToken_ ||
        (!gridAuthority.isEmpty() && !next.contains(gridAuthority)) ||
        !next.contains(arrangementAuthority) || arrangementAuthority != coordinator ||
        gridRevision < gridRevision_ || arrangementRevision < arrangementRevision_ ||
        (membershipRevision_ > 0 && gridRevision == gridRevision_ &&
         gridAuthorityToken_ != gridAuthority) ||
        (membershipRevision_ > 0 && arrangementRevision == arrangementRevision_ &&
         arrangementAuthorityToken_ != arrangementAuthority)) {
        membershipAssembly_ = {};
        return false;
    }
    peers_ = std::move(next);
    coordinatorToken_ = coordinator;
    gridAuthorityToken_ = gridAuthority;
    arrangementAuthorityToken_ = arrangementAuthority;
    gridRevision_ = gridRevision;
    arrangementRevision_ = arrangementRevision;
    membershipRevision_ = revision;
    membershipAssembly_ = {};
    setLifecycle(Lifecycle::Active);
    return true;
}

QJsonObject SharedSessionController::membershipPageFor(
    const QString& observerToken,
    int pageIndex,
    int pageCount,
    const QList<QString>& tokens) const
{
    QJsonArray entries;
    const qsizetype begin = static_cast<qsizetype>(pageIndex) * kMembershipEntriesPerPage;
    const qsizetype end = std::min<qsizetype>(begin + kMembershipEntriesPerPage, tokens.size());
    for (qsizetype index = begin; index < end; ++index) {
        const QString& token = tokens[index];
        const Peer& peer = peers_[token];
        QString endpoint = peer.endpoint;
        const auto observer = peers_.constFind(observerToken);
        if (observer != peers_.cend() && observer->sameMachine && token == creator_.localToken) {
            const int separator = endpoint.lastIndexOf(QLatin1Char(':'));
            if (separator > 0) {
                endpoint = QStringLiteral("127.0.0.1") + endpoint.mid(separator);
            }
        }
        if (endpointOverride) {
            const QString overridden = endpointOverride(observerToken, token);
            if (!overridden.isEmpty()) {
                endpoint = overridden;
            }
        }
        entries.append(QJsonObject{
            {QStringLiteral("token"), token},
            {QStringLiteral("peer_id"), QString::number(peer.peerId)},
            {QStringLiteral("endpoint"), endpoint},
        });
    }
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("session.membership")},
        {QStringLiteral("revision"), static_cast<qint64>(membershipRevision_)},
        {QStringLiteral("page_index"), pageIndex},
        {QStringLiteral("page_count"), pageCount},
        {QStringLiteral("coordinator_token"), coordinatorToken_},
        {QStringLiteral("grid_authority_token"), gridAuthorityToken_},
        {QStringLiteral("arrangement_authority_token"), arrangementAuthorityToken_},
        {QStringLiteral("grid_revision"), static_cast<qint64>(gridRevision_)},
        {QStringLiteral("arrangement_revision"), static_cast<qint64>(arrangementRevision_)},
        {QStringLiteral("peers"), entries},
    };
}

void SharedSessionController::broadcastMembership()
{
    if (role_ != Role::Creator) {
        return;
    }
    ++membershipRevision_;
    const QList<QString> tokens = peers_.keys();
    const int pageCount = std::max(1, static_cast<int>(
        (tokens.size() + kMembershipEntriesPerPage - 1) / kMembershipEntriesPerPage));
    for (const QString& observerToken : tokens) {
        if (observerToken == creator_.localToken) {
            continue;
        }
        for (int page = 0; page < pageCount; ++page) {
            if (!server_.sendTo(observerToken, membershipPageFor(observerToken, page, pageCount, tokens))) {
                break;
            }
        }
    }
    setLifecycle(peers_.size() > 1 ? Lifecycle::Active : Lifecycle::Listening);
}

void SharedSessionController::scheduleReconnect()
{
    if (!reconnectEnabled_ || closing_ || role_ != Role::Joiner || reconnectTimer_.isActive()) {
        return;
    }
    if (everAuthenticated_ && lastHeartbeat_.isValid() &&
        lastHeartbeat_.elapsed() >=
            static_cast<qint64>(heartbeatIntervalMs_) * heartbeatMissLimit_) {
        expireCoordinatorHeartbeat();
        return;
    }
    if (!everAuthenticated_ && reconnectAttempts_ >= reconnectAttemptLimit_) {
        const QString detail = QStringLiteral("TCP control reconnect attempts exhausted");
        reconnectEnabled_ = false;
        fail(TransportFailure::ReconnectExhausted, detail, false);
        publishTransportEvent(TransportEvent{
            TransportEventType::Failure,
            TransportFailure::ReconnectExhausted,
            detail}, false);
        return;
    }
    setLifecycle(Lifecycle::Reconnecting);
    publishTransportEvent(TransportEvent{
        TransportEventType::ReconnectScheduled,
        TransportFailure::None,
        QStringLiteral("TCP control reconnect scheduled"),
        true}, false);
    reconnectTimer_.start();
}

void SharedSessionController::connectJoiner()
{
    if (role_ != Role::Joiner || closing_) {
        return;
    }
    client_.connectToHost(
        joiner_.host,
        joiner_.port,
        joiner_.sessionHex,
        joiner_.keyHex,
        joiner_.localToken,
        joiner_.localEndpoint);
}

void SharedSessionController::sendHeartbeat(const QString& targetToken)
{
    if (role_ != Role::Creator || closing_) {
        return;
    }
    heartbeatSequence_ = heartbeatSequence_ >= (std::numeric_limits<int>::max)()
        ? 1 : heartbeatSequence_ + 1;
    const QJsonObject heartbeat{
        {QStringLiteral("type"), QStringLiteral("session.heartbeat")},
        {QStringLiteral("sequence"), heartbeatSequence_},
    };
    bool queued = false;
    if (targetToken.isEmpty()) {
        server_.send(heartbeat);
        queued = server_.hasPeer();
    } else {
        queued = server_.sendTo(targetToken, heartbeat);
    }
    if (queued) {
        ++heartbeatsSent_;
        publishSnapshot();
    }
}

void SharedSessionController::checkHeartbeatDeadline()
{
    if (role_ != Role::Joiner || closing_ || !everAuthenticated_ ||
        !lastHeartbeat_.isValid()) {
        return;
    }
    if (lastHeartbeat_.elapsed() >=
        static_cast<qint64>(heartbeatIntervalMs_) * heartbeatMissLimit_) {
        expireCoordinatorHeartbeat();
    } else {
        publishSnapshot();
    }
}

void SharedSessionController::expireCoordinatorHeartbeat()
{
    if (role_ != Role::Joiner || closing_ || !reconnectEnabled_) {
        return;
    }
    reconnectEnabled_ = false;
    reconnectTimer_.stop();
    heartbeatDeadlineTimer_.stop();
    client_.close();
    runtimeAttachmentEnabled_ = false;
    if (runtime_ != nullptr && runtime_->isNetworkRunning()) {
        runtime_->stopNetwork();
    }
    const QString detail = QStringLiteral(
        "Jam creator heartbeat timed out after %1 missed check-ins (%2 ms)")
        .arg(heartbeatMissLimit_)
        .arg(static_cast<qint64>(heartbeatIntervalMs_) * heartbeatMissLimit_);
    fail(TransportFailure::CoordinatorTimeout, detail, false);
    publishTransportEvent(TransportEvent{
        TransportEventType::Failure,
        TransportFailure::CoordinatorTimeout,
        detail}, false);
}

void SharedSessionController::publishSnapshot()
{
    Snapshot current = snapshot();
    reconcileRuntime(current);
    current = snapshot();
    if (onSnapshot) {
        onSnapshot(current);
    }
}

void SharedSessionController::setLifecycle(Lifecycle lifecycle)
{
    lifecycle_ = lifecycle;
    publishSnapshot();
}

void SharedSessionController::fail(
    TransportFailure failure,
    const QString& detail,
    bool retryable)
{
    failure_ = failure;
    failureDetail_ = detail;
    failureRetryable_ = retryable;
    if (!retryable) {
        lifecycle_ = Lifecycle::Failed;
    }
    publishSnapshot();
}

void SharedSessionController::publishTransportEvent(
    TransportEvent event,
    bool serverSide)
{
    if (onTransportEvent) {
        onTransportEvent(event, serverSide);
    }
}

void SharedSessionController::reconcileRuntime(const Snapshot& snapshot)
{
    if (reconcilingRuntime_ || !runtimeAttachmentEnabled_ || runtime_ == nullptr ||
        !networkOptionsFactory_ || !snapshot.networkAttachmentReady ||
        (snapshot.role != Role::Creator && snapshot.role != Role::Joiner)) {
        return;
    }

    Snapshot runtimeSnapshot = snapshot;
    if (endpointOverride) {
        for (PeerSnapshot& peer : runtimeSnapshot.peers) {
            const QString overridden = endpointOverride(snapshot.localToken, peer.token);
            if (!overridden.isEmpty()) {
                peer.endpoint = overridden;
            }
        }
    }

    std::vector<Jam2RuntimePeer> peers;
    peers.reserve(static_cast<std::size_t>(runtimeSnapshot.remotePeerCount));
    for (const PeerSnapshot& peer : runtimeSnapshot.peers) {
        if (peer.token == runtimeSnapshot.localToken || peer.endpoint.isEmpty()) {
            continue;
        }
        peers.push_back(Jam2RuntimePeer{
            peer.peerId,
            jam2::parse_endpoint(peer.endpoint.toStdString()),
        });
    }

    reconcilingRuntime_ = true;
    try {
        if (runtime_->isNetworkRunning()) {
            if (peers != runtimePeers_ && runtime_->updatePeers(peers)) {
                runtimePeers_ = std::move(peers);
            }
        } else {
            Jam2RuntimeOptions options = networkOptionsFactory_(runtimeSnapshot);
            if (!runtime_->startNetwork(options)) {
                throw std::runtime_error("network audio runtime failed to start");
            }
            runtimePeers_ = std::move(peers);
        }
        reconcilingRuntime_ = false;
    } catch (const std::exception& error) {
        reconcilingRuntime_ = false;
        runtimeAttachmentEnabled_ = false;
        const QString detail = QStringLiteral("Network attachment failed: ") +
            QString::fromUtf8(error.what());
        fail(TransportFailure::RuntimeStartFailed, detail, false);
        publishTransportEvent(TransportEvent{
            TransportEventType::Failure,
            TransportFailure::RuntimeStartFailed,
            detail}, false);
    }
}

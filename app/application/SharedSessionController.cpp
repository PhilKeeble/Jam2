#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "SharedSessionController.hpp"

#include "ApplicationRuntime.hpp"
#include "ContentLimits.hpp"
#include "ControlMessageValidation.hpp"
#include "ControlProtocol.hpp"

#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonArray>
#include <QNetworkInterface>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <stdexcept>

using jam2::application::UdpMultiplexTransport;
using namespace jam2::control_protocol;

namespace {

constexpr int kAssetIdleCloseDelayMs = 1000;

constexpr int kMembershipEntriesPerPage = 64;
constexpr double kMaxExactJsonInteger = 9007199254740991.0;

bool isEditorAction(const QString& type)
{
    return jam2::application::isEditorControlMessageType(type);
}

bool isArrangementAction(const QString& type)
{
    return jam2::application::isArrangementControlMessageType(type);
}

bool validToken(const QString& token)
{
    return peerIdFromToken(token).has_value();
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

bool matchesContractSampleRate(const QJsonObject& message, int expectedSampleRate)
{
    if (expectedSampleRate <= 0) {
        return true;
    }
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("looper.track.batch.offer")) {
        for (const QJsonValue& track : message.value(QStringLiteral("tracks")).toArray()) {
            if (track.toObject().value(QStringLiteral("sample_rate")).toInt() !=
                expectedSampleRate) {
                return false;
            }
        }
        return true;
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
    : QObject(parent), server_(this), client_(this), assetClient_(this)
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
            QStringLiteral("UDP multiplex control reconnect attempt"),
            true}, false);
        connectJoiner();
    });
    assetReconnectTimer_.setSingleShot(true);
    assetReconnectTimer_.setInterval(1000);
    QObject::connect(&assetReconnectTimer_, &QTimer::timeout, this, [this] {
        if (assetChannelRequired_ && !closing_ && role_ == Role::Joiner &&
            client_.isConnected() &&
            !assetClient_.isConnected()) {
            connectAssetJoiner();
        }
    });
    assetIdleCloseTimer_.setSingleShot(true);
    assetIdleCloseTimer_.setInterval(kAssetIdleCloseDelayMs);
    QObject::connect(&assetIdleCloseTimer_, &QTimer::timeout, this, [this] {
        if (closing_ || assetChannelRequired_ || role_ != Role::Joiner ||
            !assetClient_.isConnected()) {
            return;
        }
        assetClient_.close();
        publishTransportEvent(TransportEvent{
            TransportEventType::Disconnected,
            TransportFailure::None,
            QStringLiteral("UDP multiplex asset stream closed after track transfer became idle"),
            false,
            true,
            true}, false);
    });
    QObject::connect(&multiplexDiagnosticsTimer_, &QTimer::timeout, this, [this] {
        if (!closing_ && multiplex_ && onSignalerStatus)
            onSignalerStatus(QStringLiteral("UDP multiplex stats ") +
                QString::fromUtf8(QJsonDocument(multiplexDiagnostics()).toJson(QJsonDocument::Compact)));
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
    const auto wireDecision = [this](const QJsonObject& message, jam2::application::ControlMessageSource source) {
        using namespace jam2::application;
        const auto decision = evaluateControlMessage(message, source);
        if (!decision.accepted) {
            if (decision.rejection == ControlMessageRejection::Authorization) ++authorizationRejections_;
            else ++validationRejections_;
            return control_wire::DispatchDecision{decision.rejection == ControlMessageRejection::Authorization
                ? control_wire::Rejection::Unauthorized : control_wire::Rejection::InvalidValue, decision.reason};
        }
        if (!matchesContractSampleRate(message, contract_.sampleRate)) {
            ++validationRejections_;
            return control_wire::DispatchDecision{control_wire::Rejection::InvalidValue, QStringLiteral("WAV sample rate does not match session contract")};
        }
        return control_wire::DispatchDecision{};
    };
    server_.validateMessage = [wireDecision](const QJsonObject& value) {
        return wireDecision(value, jam2::application::ControlMessageSource::AuthenticatedPeer);
    };
    client_.validateMessage = [wireDecision](const QJsonObject& value) {
        return wireDecision(value, jam2::application::ControlMessageSource::Coordinator);
    };
    server_.onMessage = [this](const QString& token, const QJsonObject& message) {
        const QString type = message.value(QStringLiteral("type")).toString();
        // Typed-channel dispatch already applied source and contract validation.
        if (type == QStringLiteral("session.udp.recover")) {
            const auto target = message.value(QStringLiteral("peer")).toString();
            if (multiplex_ && peers_.contains(token) && peers_.contains(target) && token != target)
                introducePair(token, target);
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
        if (type == QStringLiteral("looper.recording.state") &&
            role_ == Role::Creator) {
            routed[QStringLiteral("source_peer_token")] = token;
            server_.send(routed);
            if (onMessage) {
                onMessage(token, routed);
            }
            return;
        }
        if (isEditorAction(type) && role_ == Role::Creator) {
            if (editorRevision_ == std::numeric_limits<quint64>::max()) {
                server_.sendTo(token, QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("session.error")},
                    {QStringLiteral("message"), QStringLiteral("Editor revision exhausted")},
                });
                return;
            }
            routed[QStringLiteral("revision")] = static_cast<qint64>(++editorRevision_);
            routed[QStringLiteral("authority_token")] = token;
            editorAuthorityToken_ = token;
            editorState_ = routed;
            server_.send(routed);
            publishSnapshot();
        }
        if (onMessage) {
            onMessage(token, routed);
        }
    };
    server_.onBinaryMessage = [this](const QString& token, const QByteArray& payload) {
        if (onBinaryMessage) {
            onBinaryMessage(token, payload);
        }
    };
    server_.onAssetMessage = [this](const QString& token, const QJsonObject& message) {
        if (onAssetMessage) {
            onAssetMessage(token, message);
        }
    };
    server_.onAssetBinaryMessage = [this](const QString& token, const QByteArray& payload) {
        if (onAssetBinaryMessage) {
            onAssetBinaryMessage(token, payload);
        }
    };
    server_.onAuthenticated = [this](const QString& token, const ControlServer::AuthenticatedPeer& message) {
        handleAuthenticatedPeer(token, message);
    };
    server_.onDisconnected = [this](const QString& token) { handleDisconnectedPeer(token); };
    server_.onAssetDisconnected = [this](const QString& token) {
        if (onAssetDisconnected) onAssetDisconnected(token);
    };
    client_.onEvent = [this](const TransportEvent& event) { handleClientEvent(event); };
    client_.onMessage = [this](const QJsonObject& message) { handleClientMessage(message); };
    client_.onBinaryMessage = [this](const QByteArray& payload) {
        if (onBinaryMessage) {
            onBinaryMessage(QString{}, payload);
        }
    };
    assetClient_.onEvent = [this](const TransportEvent& event) {
        handleAssetClientEvent(event);
    };
    assetClient_.onMessage = [this](const QJsonObject& message) {
        if (onAssetMessage) {
            onAssetMessage(QString{}, message);
        }
    };
    assetClient_.onBinaryMessage = [this](const QByteArray& payload) {
        if (onAssetBinaryMessage) {
            onAssetBinaryMessage(QString{}, payload);
        }
    };
}

void SharedSessionController::bindRuntime(
    ApplicationRuntime& runtime,
    std::function<Jam2RuntimeOptions(const Snapshot&)> networkOptionsFactory)
{
    runtime_ = &runtime;
    networkOptionsFactory_ = std::move(networkOptionsFactory);
}


bool SharedSessionController::startMultiplex(bool creator)
{
    using namespace jam2::application;
    multiplex_ = std::make_shared<UdpMultiplexTransport>();
    multiplex_->onBootstrapFailure = [this](const QString& detail) {
        if (closing_) return;
        fail(TransportFailure::TransportError,detail,false);
        publishTransportEvent(TransportEvent{TransportEventType::Failure,TransportFailure::TransportError,detail},role_==Role::Creator);
    };
    multiplex_->onRegistered = [this] {
        if (closing_ || !multiplex_) return;
        if (role_==Role::Creator) if (const auto endpoint=multiplex_->bootstrapEndpoint()) {
            creator_.localEndpoint=QString::fromStdString(jam2::endpoint_to_string(*endpoint));
            auto local=peers_.find(creator_.localToken);
            if (local!=peers_.end()) local->endpoint=creator_.localEndpoint;
        }
        publishSnapshot();
    };
    multiplex_->onJoinRequest = [this](const QString& peer) {
        if (closing_ || role_ != Role::Creator) return;
        const int remote = peers_.size() - (peers_.contains(creator_.localToken) ? 1 : 0);
        if (!peers_.contains(peer) && creator_.sessionPeerLimit > 0 && remote >= creator_.sessionPeerLimit) {
            multiplex_->rejectPeer(peer); return;
        }
        introducePair(creator_.localToken, peer);
    };
    multiplex_->onLinkReady = [this](const QString& peer) {
        if (closing_ || !multiplex_) return;
        if (role_ == Role::Creator && peers_.size() == 1) {
            if (const auto endpoint = multiplex_->observedEndpoint(peer)) {
                creator_.localEndpoint = QString::fromStdString(jam2::endpoint_to_string(*endpoint));
                auto local = peers_.find(creator_.localToken);
                if (local != peers_.end()) local->endpoint = creator_.localEndpoint;
            }
        }
        if (role_ == Role::Joiner && peer == multiplex_->coordinatorToken()) {
            coordinatorToken_ = peer;
            if (const auto endpoint = multiplex_->observedEndpoint(peer))
                joiner_.localEndpoint = QString::fromStdString(jam2::endpoint_to_string(*endpoint));
            connectJoiner();
        }
        publishSnapshot();
    };
    multiplex_->onStreamReady = [this](const QString& peer, UdpMuxChannel channel,
            std::shared_ptr<ControlConnection> connection, bool incoming) {
        if (closing_) { connection->close(); return; }
        if (incoming && role_ == Role::Creator) {
            server_.acceptTransport(connection);
        } else if (!incoming && role_ == Role::Joiner && peer == coordinatorToken_) {
            if (channel == UdpMuxChannel::Asset) {
                if (!assetChannelRequired_) { connection->close(); return; }
                assetClient_.connectViaTransport(connection, joiner_.sessionHex, joiner_.keyHex,
                    joiner_.localToken, {}, ControlClient::Channel::Asset);
            } else {
                client_.connectViaTransport(connection, joiner_.sessionHex, joiner_.keyHex,
                    joiner_.localToken, joiner_.localEndpoint);
            }
        } else connection->close();
    };
    multiplex_->onPeerLeft = [this](const QString& peer) {
        if (closing_) return;
        controlRecoveryDeadlines_.remove(peer);
        if (role_ == Role::Creator && peers_.contains(peer)) handleDisconnectedPeer(peer);
        else if (peer == coordinatorToken_) handleClientMessage(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("session.end")},
            {QStringLiteral("detail"), QStringLiteral("The jam creator left the session")}});
    };
    multiplex_->onLinkStalled = [this](const QString& peer) { recoverMultiplexPeer(peer); };
    multiplex_->onStreamFailed = [this](const QString& peer, UdpMuxChannel channel, const QString& detail) {
        if (closing_ || role_ != Role::Joiner || peer != coordinatorToken_) return;
        if (onSignalerStatus) onSignalerStatus(detail);
        if (channel == UdpMuxChannel::Control) scheduleReconnect();
        else if (assetChannelRequired_ && !assetReconnectTimer_.isActive()) assetReconnectTimer_.start();
    };
    multiplex_->onSignalerStatus = [this](const QString& detail) { if (!closing_ && onSignalerStatus) onSignalerStatus(detail); };
    multiplex_->onPairFailure = [this](const QString& a, const QString& b, const QString& detail) {
        if (!closing_) connectionWarning(a, b, detail, role_ == Role::Creator);
        if (!closing_ && role_ == Role::Creator && peers_.contains(a) && peers_.contains(b) &&
            recoveringUdpPairs_.contains({(std::min)(a, b), (std::max)(a, b)})) {
            const std::weak_ptr<UdpMultiplexTransport> transport = multiplex_;
            QTimer::singleShot(5000, this, [this, transport, a, b] {
                if (transport.lock() == multiplex_ && multiplex_ && !closing_ &&
                    peers_.contains(a) && peers_.contains(b)) introducePair(a, b);
            });
        }
    };
    UdpMultiplexTransport::Config config;
    config.creator = creator;
    config.strategy = creator ? creator_.strategy : joiner_.strategy;
    config.bindHost = creator ? creator_.bindHost : joiner_.bindHost;
    config.bindPort = creator ? creator_.port : joiner_.localPort;
    config.socketBuffers=creator ? creator_.socketBuffers : joiner_.socketBuffers;
    if (creator) {
        config.publicEndpoint=creator_.publicEndpoint; config.stunServer=creator_.stunServer;
        config.stunTimeoutMs=creator_.stunTimeoutMs; config.stunRetries=creator_.stunRetries;
    }
    config.sessionHex = creator ? creator_.sessionHex : joiner_.sessionHex;
    config.keyHex = creator ? creator_.keyHex : joiner_.keyHex;
    config.localToken = creator ? creator_.localToken : joiner_.localToken;
    config.server = creator ? creator_.rendezvousServer
        : jam2::Endpoint{joiner_.host.toStdString(), joiner_.port};
    config.maximumPeers = creator ? creator_.sessionPeerLimit : 0;
    config.receiveModel = serverReceiveModel_;
    QString error;
    if (multiplex_->start(config, error)) {
        multiplexDiagnosticsTimer_.setInterval(2000);
        multiplexDiagnosticsTimer_.start();
        return true;
    }
    multiplex_.reset();
    fail(TransportFailure::TransportError, error, false);
    publishTransportEvent(TransportEvent{TransportEventType::Failure,
        TransportFailure::TransportError, error}, creator);
    return false;
}

void SharedSessionController::recoverMultiplexPeer(const QString& peer)
{
    if (!multiplex_ || closing_ || !peers_.contains(peer)) return;
    if (onSignalerStatus) onSignalerStatus(QStringLiteral("Repairing UDP peer link: %1").arg(peerName(peer)));
    if (role_ == Role::Creator) introducePair(creator_.localToken, peer);
    else if (peer == coordinatorToken_) {
        multiplex_->recoverPeer(peer);
        setLifecycle(Lifecycle::Reconnecting);
    } else if (client_.isConnected()) {
        send(QJsonObject{{QStringLiteral("type"), QStringLiteral("session.udp.recover")},
            {QStringLiteral("peer"), peer}});
    }
}

void SharedSessionController::introducePair(const QString& a, const QString& b)
{
    if (!multiplex_ || role_ != Role::Creator || a == b) return;
    if (peers_.contains(a) && peers_.contains(b))
        recoveringUdpPairs_.insert({(std::min)(a, b), (std::max)(a, b)});
    const auto request = QString::fromLatin1(QUuid::createUuid().toRfc4122().toHex());
    for (const auto& target : {a, b}) {
        if (target == creator_.localToken) continue;
        // The creator's own peer can reach rendezvous without its failed
        // control stream. Other mesh edges use the surviving star links.
        server_.sendTo(target, QJsonObject{{QStringLiteral("type"), QStringLiteral("session.udp.introduce")},
                {QStringLiteral("nonce"), request}, {QStringLiteral("peer"), target == a ? b : a}});
    }
    multiplex_->introduce(a, b, request);
}

void SharedSessionController::introduceMeshPeer(const QString& token)
{
    for (auto it = peers_.cbegin(); it != peers_.cend(); ++it) {
        if (it.key() == creator_.localToken || it.key() == token) continue;
        const auto nonce = QString::fromLatin1(QUuid::createUuid().toRfc4122().toHex());
        const auto notify = [&](const QString& target, const QString& partner) {
            return server_.sendTo(target, QJsonObject{
                {QStringLiteral("type"), QStringLiteral("session.udp.introduce")},
                {QStringLiteral("nonce"), nonce}, {QStringLiteral("peer"), partner}});
        };
        if (notify(it.key(), token) && notify(token, it.key()))
            multiplex_->introduce(it.key(), token, nonce);
        else connectionWarning(it.key(), token, QStringLiteral("Mesh introduction could not be queued"), true);
    }
}

void SharedSessionController::connectionWarning(
    const QString& a, const QString& b, const QString& detail, bool broadcast)
{
    const QString first = a < b ? a : b, second = a < b ? b : a;
    const auto id = first + QLatin1Char('/') + second;
    if (introductionWarnings_.contains(id)) return;
    introductionWarnings_.insert(id);
    const auto aName=peerName(a), bName=peerName(b);
    const QString connection = aName.isEmpty() || bName.isEmpty()
        ? QStringLiteral("An incoming peer connection could not be established.")
        : QStringLiteral("The connection between %1 and %2 could not be established.").arg(aName,bName);
    const auto message = detail + QLatin1Char('\n') + connection + QStringLiteral(
        " The current jam remains running. Direct and STUN require the creator UDP port to be forwarded; "
        "peer-to-peer links require compatible router hole punching.");
    if (broadcast && validToken(a) && validToken(b))
        server_.send(QJsonObject{{QStringLiteral("type"), QStringLiteral("session.connection.failure")},
            {QStringLiteral("a"), a}, {QStringLiteral("b"), b},
            {QStringLiteral("detail"), detail.left(2048)}});
    if (onConnectionWarning) onConnectionWarning(message);
}

jam2::application::UdpMultiplexTransport::Snapshot SharedSessionController::multiplexSnapshot() const
{
    return multiplex_ ? multiplex_->snapshot() : jam2::application::UdpMultiplexTransport::Snapshot{};
}

QJsonObject SharedSessionController::multiplexDiagnostics() const
{
    const auto stats = multiplexSnapshot();
    QJsonArray links;
    const auto endpoint = [](const jam2::Endpoint& value) { return value.port ? QString::fromStdString(jam2::endpoint_to_string(value)) : QString{}; };
    for (const auto& link : stats.links) links.append(QJsonObject{
        {QStringLiteral("peer"), link.peer}, {QStringLiteral("phase"), static_cast<int>(link.phase)},
        {QStringLiteral("route"), link.route},
        {QStringLiteral("attempt"), link.attempt}, {QStringLiteral("hello_sent"), link.helloSent},
        {QStringLiteral("local"), endpoint(link.local)}, {QStringLiteral("remote"), endpoint(link.remote)},
        {QStringLiteral("observed"), endpoint(link.observed)},
        {QStringLiteral("received_hello"), link.receivedHello}, {QStringLiteral("received_ack"), link.receivedAck},
        {QStringLiteral("control_queued_bytes"), static_cast<qint64>(link.controlQueued)},
        {QStringLiteral("asset_queued_bytes"), static_cast<qint64>(link.assetQueued)},
        {QStringLiteral("retransmissions"), static_cast<qint64>(link.retransmissions)},
        {QStringLiteral("control_stream"), QString::number(link.controlStream)},
        {QStringLiteral("asset_stream"), QString::number(link.assetStream)},
        {QStringLiteral("control_rtt_us"), static_cast<qint64>(link.controlRttUs)},
        {QStringLiteral("asset_rtt_us"), static_cast<qint64>(link.assetRttUs)},
        {QStringLiteral("control_window_packets"), link.controlWindow},
        {QStringLiteral("asset_window_packets"), link.assetWindow},
        {QStringLiteral("input_deferrals"), static_cast<qint64>(link.inputDeferrals)},
        {QStringLiteral("ack_datagrams_sent"),static_cast<qint64>(link.ackDatagramsSent)},
        {QStringLiteral("fragment_acks_coalesced"),static_cast<qint64>(link.acksCoalesced)},
        {QStringLiteral("ack_transmit_deferrals"),static_cast<qint64>(link.ackTransmitDeferrals)},
        {QStringLiteral("invalid_ack_windows"),static_cast<qint64>(link.invalidAckWindows)},
        {QStringLiteral("in_order_direct_fragments"),static_cast<qint64>(link.inOrderDirect)},
        {QStringLiteral("reorder_insertions"),static_cast<qint64>(link.reorderInsertions)},
        {QStringLiteral("tail_bytes_packed"),static_cast<qint64>(link.tailBytesPacked)},
        {QStringLiteral("fragments_avoided"),static_cast<qint64>(link.fragmentsAvoided)},
        {QStringLiteral("fast_retransmissions"),static_cast<qint64>(link.fastRetransmissions)},
        {QStringLiteral("sack_gap_reports"),static_cast<qint64>(link.sackGapReports)},
        {QStringLiteral("ack_pairs"),static_cast<qint64>(link.ackPairs)}, {QStringLiteral("piggybacked_acks"),static_cast<qint64>(link.piggybackedAcks)},
        {QStringLiteral("compact_packets"),static_cast<qint64>(link.compactPackets)}, {QStringLiteral("compact_bytes_saved"),static_cast<qint64>(link.compactBytesSaved)},
        {QStringLiteral("native_intent_sent"),link.nativeIntentSent}, {QStringLiteral("native_intent_acknowledged"),link.nativeIntentAcknowledged},
        {QStringLiteral("native_intent_receipts_allowed"),link.nativeIntentReceiptsAllowed},
        {QStringLiteral("last_receive_age_ms"), static_cast<qint64>(link.lastReceiveAgeMs)},
        {QStringLiteral("socket_send_buffer_bytes"),link.sendBufferBytes},
        {QStringLiteral("socket_receive_buffer_bytes"),link.receiveBufferBytes},
        {QStringLiteral("retained_audio_local"), endpoint(link.retainedAudioLocal)}});
    QJsonObject result{{QStringLiteral("bootstrap_repeat_ms"),500}, {QStringLiteral("bootstrap_join_attempt_ms"),3000},
        {QStringLiteral("bootstrap_join_timeout_ms"),45000}, {QStringLiteral("bootstrap_maximum_attempts"),10},
        {QStringLiteral("introduction_phase_attempt_ms"),4000}, {QStringLiteral("introduction_completion_timeout_ms"),8000},
        {QStringLiteral("bootstrap_pending_authentication_limit"),kMaxPendingPeers},
        {QStringLiteral("bootstrap_authentication_work_window_ms"),kAuthenticationFailureWindowMs},
        {QStringLiteral("bootstrap_authentication_failure_limit"),kMaxAuthenticationFailuresPerWindow},
        {QStringLiteral("enabled"), multiplex_ != nullptr}, {QStringLiteral("running"), stats.running},
        {QStringLiteral("uses_rendezvous"),stats.usesRendezvous},
        {QStringLiteral("physical_socket_count"),static_cast<qint64>(stats.physicalSocketCount)},
        {QStringLiteral("socket_send_buffer_requested_bytes"),stats.requestedBuffers.sendBytes.value_or(0)},
        {QStringLiteral("socket_receive_buffer_requested_bytes"),stats.requestedBuffers.receiveBytes.value_or(0)},
        {QStringLiteral("bootstrap_socket_send_buffer_bytes"),stats.signalSendBufferBytes},
        {QStringLiteral("bootstrap_socket_receive_buffer_bytes"),stats.signalReceiveBufferBytes},
        {QStringLiteral("bootstrap_local"),QString::fromStdString(jam2::endpoint_to_string(stats.bootstrapLocal))},
        {QStringLiteral("bootstrap_public"),QString::fromStdString(jam2::endpoint_to_string(stats.bootstrapPublic))},
        {QStringLiteral("bootstrap_rejected"),static_cast<qint64>(stats.bootstrapRejected)},
        {QStringLiteral("bootstrap_authentication_rejects"),static_cast<qint64>(stats.bootstrapAuthenticationRejects)},
        {QStringLiteral("bootstrap_attempts"),static_cast<qint64>(stats.bootstrapAttempts)},
        {QStringLiteral("pending_admission_timeout_ms"),static_cast<qint64>(stats.pendingAdmissionTimeoutMs)},
        {QStringLiteral("pending_admission_timeouts"),static_cast<qint64>(stats.pendingAdmissionTimeouts)},
        {QStringLiteral("introduction_pending"),static_cast<qint64>(stats.introductionPending)},
        {QStringLiteral("stun_requests_sent"),static_cast<qint64>(stats.stunRequestsSent)},
        {QStringLiteral("stun_responses_rejected"),static_cast<qint64>(stats.stunResponsesRejected)},
        {QStringLiteral("registered"), stats.registered}, {QStringLiteral("admission_paused"), stats.admissionPaused},
        {QStringLiteral("pause_confirmed"), stats.pauseConfirmed},
        {QStringLiteral("signal_packets_sent"), static_cast<qint64>(stats.signalPacketsSent)},
        {QStringLiteral("signal_packets_received"), static_cast<qint64>(stats.signalPacketsReceived)},
        {QStringLiteral("signal_packets_rejected"), static_cast<qint64>(stats.signalPacketsRejected)},
        {QStringLiteral("audio_packets_sent"), static_cast<qint64>(stats.audioPacketsSent)},
        {QStringLiteral("audio_packets_received"), static_cast<qint64>(stats.audioPacketsReceived)},
        {QStringLiteral("audio_queue_drops"), static_cast<qint64>(stats.audioQueueDrops)},
        {QStringLiteral("runtime_receive_enabled"), stats.runtimeReceiveEnabled},
        {QStringLiteral("runtime_receive_owner"), stats.runtimeReceiverAttached},
        {QStringLiteral("receiver_handovers"), static_cast<qint64>(stats.receiverHandovers)},
        {QStringLiteral("receiver_detachments"), static_cast<qint64>(stats.receiverDetachments)},
        {QStringLiteral("runtime_native_delivered"), static_cast<qint64>(stats.runtimeNativeDelivered)},
        {QStringLiteral("bootstrap_native_discarded"), static_cast<qint64>(stats.bootstrapNativeDiscarded)},
        {QStringLiteral("runtime_control_queued"), static_cast<qint64>(stats.runtimeControlQueued)},
        {QStringLiteral("runtime_asset_queued"), static_cast<qint64>(stats.runtimeAssetQueued)},
        {QStringLiteral("runtime_invalid_packets"), static_cast<qint64>(stats.runtimeInvalidPackets)},
        {QStringLiteral("runtime_classification_budget_yields"), static_cast<qint64>(stats.runtimeBudgetYields)},
        {QStringLiteral("ingress_queue_drops"), static_cast<qint64>(stats.ingressQueueDrops)},
        {QStringLiteral("control_ingress_depth_packets"), static_cast<qint64>(stats.controlIngressDepth)},
        {QStringLiteral("asset_ingress_depth_packets"), static_cast<qint64>(stats.assetIngressDepth)},
        {QStringLiteral("ingress_high_water_packets"), static_cast<qint64>(stats.ingressHighWater)},
        {QStringLiteral("ingress_capacity_per_queue_packets"), static_cast<qint64>(UdpMultiplexTransport::kIngressPacketsPerQueue)},
        {QStringLiteral("classification_packets_per_call"), static_cast<qint64>(UdpMultiplexTransport::kClassificationPacketsPerCall)},
        {QStringLiteral("classification_timing_stride"), static_cast<qint64>(UdpMultiplexTransport::kClassificationTimingStride)},
        {QStringLiteral("runtime_wait_maximum_us"), static_cast<qint64>(UdpMultiplexTransport::kRuntimeWaitMaximumUs)},
        {QStringLiteral("runtime_receive_buffer_bytes"), static_cast<qint64>(jam2::NetworkDatagramTransport::kReceiveBufferBytes)},
        {QStringLiteral("receiver_handover_timeout_ms"), static_cast<qint64>(UdpMultiplexTransport::kReceiverHandoverTimeoutMs)},
        {QStringLiteral("ingress_wait_samples"), static_cast<qint64>(stats.ingressWaitSamples)},
        {QStringLiteral("ingress_wait_sum_us"), static_cast<qint64>(stats.ingressWaitSumUs)},
        {QStringLiteral("ingress_wait_max_us"), static_cast<qint64>(stats.ingressWaitMaxUs)},
        {QStringLiteral("classification_samples"), static_cast<qint64>(stats.classificationSamples)},
        {QStringLiteral("classification_sum_ns"), static_cast<qint64>(stats.classificationSumNs)},
        {QStringLiteral("classification_max_ns"), static_cast<qint64>(stats.classificationMaxNs)},
        {QStringLiteral("worker_loop_gap_max_us"), static_cast<qint64>(stats.workerLoopGapMaxUs)},
        {QStringLiteral("worker_service_max_us"), static_cast<qint64>(stats.workerServiceMaxUs)},
        {QStringLiteral("mux_version"),jam2::application::kUdpMuxVersion},
        {QStringLiteral("native_intent_acks_sent"),static_cast<qint64>(stats.nativeIntentAcksSent)},
        {QStringLiteral("native_intent_acks_received"),static_cast<qint64>(stats.nativeIntentAcksReceived)},
        {QStringLiteral("native_intent_repeats_suppressed"),static_cast<qint64>(stats.nativeIntentRepeatsSuppressed)},
        {QStringLiteral("native_intent_ack_deferrals"),static_cast<qint64>(stats.nativeIntentAckDeferrals)},
        {QStringLiteral("native_intent_ack_invalid"),static_cast<qint64>(stats.nativeIntentAckInvalid)},
        {QStringLiteral("fast_recovery_threshold"),3}, {QStringLiteral("fast_reorder_us"),static_cast<qint64>(stats.fastReorderUs)},
        {QStringLiteral("fast_recovery_enabled"),stats.fastRecovery}, {QStringLiteral("compact_frames_enabled"),stats.compactFrames},
        {QStringLiteral("combine_acks_enabled"),stats.combineAcks}, {QStringLiteral("native_intent_receipts_enabled"),stats.acknowledgeNativeIntents},
        {QStringLiteral("native_intent_receipt_disabled_links"),static_cast<qint64>(stats.intentReceiptDisabledLinks)},
        {QStringLiteral("worker_turns"),static_cast<qint64>(stats.workerTurns)},
        {QStringLiteral("worker_send_credit_repolls"),static_cast<qint64>(stats.workerSendRepolls)},
        {QStringLiteral("worker_reuses_buffers"),stats.reusesWorkerBuffers},
        {QStringLiteral("worker_workspace_growths"),static_cast<qint64>(stats.workerWorkspaceGrowths)},
        {QStringLiteral("worker_service_sum_us"),static_cast<qint64>(stats.workerServiceSumUs)},
        {QStringLiteral("ingress_copy_bytes_avoided"),static_cast<qint64>(stats.ingressCopyBytesAvoided)},
        {QStringLiteral("mux_copy_bytes_avoided"),static_cast<qint64>(stats.muxCopyBytesAvoided)},
        {QStringLiteral("worker_lock_wait_max_us"), static_cast<qint64>(stats.workerLockWaitMaxUs)},
        {QStringLiteral("control_bytes_per_second"), static_cast<qint64>(stats.controlBytesPerSecond)},
        {QStringLiteral("asset_bytes_per_second"), static_cast<qint64>(stats.assetBytesPerSecond)},
        {QStringLiteral("control_bytes_sent"), static_cast<qint64>(stats.controlBytesSent)},
        {QStringLiteral("asset_bytes_sent"), static_cast<qint64>(stats.assetBytesSent)},
        {QStringLiteral("invalid_mux_packets"), static_cast<qint64>(stats.invalidMuxPackets)},
        {QStringLiteral("control_bytes_received"), static_cast<qint64>(stats.controlBytesReceived)},
        {QStringLiteral("asset_bytes_received"), static_cast<qint64>(stats.assetBytesReceived)},
        {QStringLiteral("mux_budget_deferrals"), static_cast<qint64>(stats.budgetDeferrals)},
        {QStringLiteral("mux_send_errors"), static_cast<qint64>(stats.sendErrors)},
        {QStringLiteral("link_repair_silence_ms"), static_cast<qint64>(stats.linkRepairSilenceMs)},
        {QStringLiteral("frame_idle_timeout_ms"), 10000},
        {QStringLiteral("same_socket_recovery_attempts"), 2},
        {QStringLiteral("links"), links}};
    if (multiplex_) result.insert(QStringLiteral("binary_control"), QJsonObject{
        {QStringLiteral("server"), jam2::application::control_wire::diagnostics(server_.stats().wire)},
        {QStringLiteral("peer_channels"), server_.wireDiagnostics()},
        {QStringLiteral("client"), jam2::application::control_wire::diagnostics(client_.stats().wire)},
        {QStringLiteral("asset_client"), jam2::application::control_wire::diagnostics(assetClient_.stats().wire)}});
    return result;
}
bool SharedSessionController::configureServerReceiveModel(UdpMultiplexTransport::ReceiveModel model)
{
    if (role_ == Role::Creator || role_ == Role::Joiner) return false;
    serverReceiveModel_ = model; return true;
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
    const auto name = jam2::application::PeerName::fromInput(config.name);
    if (!name) { fail(TransportFailure::InvalidConfiguration, jam2::application::peerNameRequirement(), false); return false; }
    // Preserve the local engine while the common transport starts discovery.
    reset(false);
    knownPeerNames_.clear();
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
    const auto audioFormat = jam2::protocol::parse_audio_format(
        contract_.audioFormat.toStdString());
    contractReady_ = contract_.protocolVersion == jam2::protocol::kProtocolVersion &&
        audioFormat.has_value() &&
        contract_.sampleRate >= jam2::application::limits::kMinimumSampleRate &&
        contract_.sampleRate <= jam2::application::limits::kMaximumSampleRate &&
        contract_.frameSize > 0 &&
        contract_.frameSize <= static_cast<int>(jam2::protocol::kMaxAudioFramesPerPacket);
    contractRevision_ = contractReady_ ? 1 : 0;
    if (!contractReady_) {
        const QString detail = QStringLiteral(
            "Creator session contract has an unsupported protocol, audio format, sample rate, or frame size");
        fail(TransportFailure::InvalidConfiguration, detail, false);
        publishTransportEvent(TransportEvent{
            TransportEventType::Failure,
            TransportFailure::InvalidConfiguration,
            detail}, false);
        return false;
    }
    const auto localPeerId = peerIdFromToken(config.localToken);
    if (!localPeerId) {
        const QString detail = QStringLiteral(
            "Creator peer token must encode a stable non-zero peer id");
        fail(TransportFailure::InvalidConfiguration, detail, false);
        publishTransportEvent(TransportEvent{
            TransportEventType::Failure,
            TransportFailure::InvalidConfiguration,
            detail}, false);
        return false;
    }
    editorAuthorityToken_ = config.localToken;
    arrangementAuthorityToken_ = config.localToken;
    Peer local;
    local.name = *name;
    local.endpoint = config.localEndpoint;
    local.peerId = *localPeerId;
    local.edgeState = EdgeState::Active;
    local.proofState = QStringLiteral("local");
    local.streamState = QStringLiteral("local");
    peers_.insert(config.localToken, local);
    knownPeerNames_.insert(config.localToken, {*localPeerId, *name});
    reconnectEnabled_ = true;
    if (!server_.listenViaTransport(config.sessionHex, config.keyHex)) {
        fail(
            failure_ == TransportFailure::None ? TransportFailure::TransportError : failure_,
            server_.errorString(),
            false);
        return false;
    }
    if (!startMultiplex(true)) return false;
    runtimeAttachmentEnabled_ = true;
    heartbeatTimer_.start();
    setLifecycle(Lifecycle::Listening);
    return true;
}

bool SharedSessionController::startJoiner(const JoinerConfig& config)
{
    const auto name = jam2::application::PeerName::fromInput(config.name);
    if (!name) { fail(TransportFailure::InvalidConfiguration, jam2::application::peerNameRequirement(), false); return false; }
    close();
    knownPeerNames_.clear();
    closing_ = false;
    role_ = Role::Joiner;
    runtimeAttachmentEnabled_ = true;
    joiner_ = config;
    joiner_.name = name->text();
    client_.setPeerName(*name);
    assetClient_.setPeerName(*name);
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
    if (!peerIdFromToken(config.localToken).has_value()) {
        const QString detail = QStringLiteral(
            "Joiner peer token must encode a stable non-zero peer id");
        fail(TransportFailure::InvalidConfiguration, detail, false);
        publishTransportEvent(TransportEvent{
            TransportEventType::Failure,
            TransportFailure::InvalidConfiguration,
            detail}, false);
        return false;
    }
    setLifecycle(Lifecycle::Connecting);
    return startMultiplex(false);
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
    runtimeAttachedForSession_ = false;
    runtimePeers_.clear();
    reconnectEnabled_ = false;
    reconnectTimer_.stop();
    assetReconnectTimer_.stop();
    assetIdleCloseTimer_.stop();
    heartbeatTimer_.stop();
    multiplexDiagnosticsTimer_.stop();
    heartbeatDeadlineTimer_.stop();
    server_.close();
    client_.close();
    assetClient_.close();
    assetChannelRequired_ = false;
    if (stopRuntime && runtime_ != nullptr && runtime_->isNetworkRunning()) {
        runtime_->stopNetwork();
    }
    if (multiplex_) { multiplex_->stop(); multiplex_.reset(); }
    introductionWarnings_.clear();
    recoveringUdpPairs_.clear();
    role_ = Role::Inactive;
    lifecycle_ = Lifecycle::Inactive;
    creator_ = {};
    joiner_ = {};
    peers_.clear();
    controlRecoveryDeadlines_.clear();
    membershipAssembly_ = {};
    coordinatorToken_.clear();
    editorAuthorityToken_.clear();
    arrangementAuthorityToken_.clear();
    membershipRevision_ = 0;
    contractRevision_ = 0;
    editorRevision_ = 0;
    arrangementRevision_ = 0;
    contractReady_ = false;
    contract_ = {};
    reconnectAttempts_ = 0;
    reconnectAttemptLimit_ = 15;
    everAuthenticated_ = false;
    failure_ = TransportFailure::None;
    failureDetail_.clear();
    failureRetryable_ = false;
    editorState_ = {};
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

bool SharedSessionController::reconnectControlStream()
{
    if (role_ != Role::Joiner || closing_ || !multiplex_ || !client_.isConnected()) return false;
    interruptAssetJoiner();
    client_.close();
    scheduleReconnect();
    return true;
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
            QStringLiteral("UDP multiplex control refresh requested")}, true);
        if (server_.hasPeer()) {
            publishTransportEvent(TransportEvent{
                TransportEventType::AlreadyConnected,
                TransportFailure::None,
                QStringLiteral("UDP multiplex control already connected"),
                false,
                true}, true);
        }
        return;
    }
    if (role_ == Role::Joiner && !client_.isConnected()) {
        publishTransportEvent(TransportEvent{
            TransportEventType::RefreshRequested,
            TransportFailure::None,
            QStringLiteral("UDP multiplex control refresh requested"),
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
        if (type == QStringLiteral("looper.recording.state")) {
            routed[QStringLiteral("source_peer_token")] = creator_.localToken;
        } else if (isEditorAction(type)) {
            if (editorRevision_ == std::numeric_limits<quint64>::max()) {
                return false;
            }
            routed[QStringLiteral("revision")] = static_cast<qint64>(++editorRevision_);
            routed[QStringLiteral("authority_token")] = creator_.localToken;
            editorAuthorityToken_ = creator_.localToken;
            editorState_ = routed;
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
        const auto source = jam2::application::ControlMessageSource::LocalCreator;
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

bool SharedSessionController::sendBinaryTo(const QString& token, const QByteArray& payload)
{
    if (role_ == Role::Creator) {
        return server_.sendBinaryTo(token, payload);
    }
    return role_ == Role::Joiner && token.isEmpty() && client_.sendBinary(payload);
}

bool SharedSessionController::canQueueTo(const QString& token, qint64 additionalBytes) const
{
    return role_ == Role::Creator
        ? server_.canQueueTo(token, additionalBytes)
        : role_ == Role::Joiner && token.isEmpty() && client_.canQueue(additionalBytes);
}

bool SharedSessionController::sendAssetTo(
    const QString& token,
    const QJsonObject& message)
{
    return role_ == Role::Creator
        ? server_.sendAssetTo(token, message)
        : role_ == Role::Joiner && token.isEmpty() && assetChannelRequired_ &&
            assetClient_.send(message);
}

bool SharedSessionController::sendAssetBinaryTo(
    const QString& token,
    const QByteArray& payload)
{
    return role_ == Role::Creator
        ? server_.sendAssetBinaryTo(token, payload)
        : role_ == Role::Joiner && token.isEmpty() && assetChannelRequired_ &&
            assetClient_.sendBinary(payload);
}

bool SharedSessionController::canQueueAssetTo(
    const QString& token,
    qint64 additionalBytes) const
{
    return role_ == Role::Creator
        ? server_.canQueueAssetTo(token, additionalBytes)
        : role_ == Role::Joiner && token.isEmpty() && assetChannelRequired_ &&
            assetClient_.canQueue(additionalBytes);
}

void SharedSessionController::setAssetChannelRequired(bool required)
{
    if (assetChannelRequired_ == required) {
        return;
    }
    assetChannelRequired_ = required;
    if (required) {
        assetIdleCloseTimer_.stop();
        if (role_ == Role::Joiner && !closing_ && client_.isConnected()) {
            connectAssetJoiner();
        }
        return;
    }
    assetReconnectTimer_.stop();
    if (role_ == Role::Joiner && assetClient_.isConnected()) {
        assetIdleCloseTimer_.start();
    }
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
    if (const auto local = peers_.constFind(result.localToken); local != peers_.cend()) {
        result.localPeerId = local->peerId;
    }
    if (const auto coordinator = peers_.constFind(result.coordinatorToken);
        coordinator != peers_.cend()) {
        result.coordinatorPeerId = coordinator->peerId;
    }
    result.editorAuthorityToken = editorAuthorityToken_;
    result.arrangementAuthorityToken = arrangementAuthorityToken_;
    result.membershipRevision = membershipRevision_;
    result.contractRevision = contractRevision_;
    result.editorRevision = editorRevision_;
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
            it->proofState, it->streamState, it->sameMachine, it->name});
    }
    return result;
}

QString SharedSessionController::peerName(const QString& token) const
{
    const auto peer = knownPeerNames_.constFind(token);
    return peer == knownPeerNames_.cend() ? QString{} : peer->name.text();
}

QString SharedSessionController::peerName(quint64 id) const
{
    for (const auto& peer : peers_) if (peer.peerId == id) return peer.name.text();
    for (const auto& peer : knownPeerNames_) if (peer.peerId == id) return peer.name.text();
    return {};
}

void SharedSessionController::handleServerEvent(const TransportEvent& event)
{
    if (event.assetChannel) {
        publishTransportEvent(event, true);
        return;
    }
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
        // A re-established control channel is fresh route evidence even when
        // the coordinator republishes the same textual endpoints. UDP is
        // connectionless, so an old NAT mapping can be dead while ordinary
        // sends still report success. Force the retained runtime to prove its
        // direct edges again instead of preserving a stale Active state.
        if (!firstAuthentication && runtime_ != nullptr && multiplex_) {
            // The multiplex retains its proven audio socket. Recover only the
            // grid control state and its applied receipt on logical reconnect.
            (void)runtime_->requestGridAssignmentRecovery();
        }
        setLifecycle(Lifecycle::Authenticated);
        if (assetChannelRequired_) {
            connectAssetJoiner();
        }
    } else if (event.type == TransportEventType::Disconnected && !closing_) {
        interruptAssetJoiner();
        scheduleReconnect();
    } else if (event.type == TransportEventType::Failure) {
        if (event.retryable) {
            if (multiplex_) interruptAssetJoiner();
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

void SharedSessionController::interruptAssetJoiner()
{
    const bool hadWork = assetChannelRequired_;
    assetReconnectTimer_.stop();
    assetClient_.close();
    if (hadWork && onAssetDisconnected) onAssetDisconnected(QString{});
}

void SharedSessionController::handleAssetClientEvent(const TransportEvent& incoming)
{
    TransportEvent event = incoming;
    event.assetChannel = true;
    if ((event.type == TransportEventType::Disconnected ||
         event.type == TransportEventType::Failure) &&
        assetChannelRequired_ && !closing_ && role_ == Role::Joiner &&
        client_.isConnected()) {
        if (event.type == TransportEventType::Disconnected && onAssetDisconnected) {
            onAssetDisconnected(QString{});
        }
        if (!assetReconnectTimer_.isActive()) {
            assetReconnectTimer_.start();
        }
    } else if (event.type == TransportEventType::Authenticated) {
        assetReconnectTimer_.stop();
        if (!assetChannelRequired_) {
            assetIdleCloseTimer_.start();
        }
    }
    publishTransportEvent(event, false);
}

void SharedSessionController::handleAuthenticatedPeer(
    const QString& token,
    const ControlServer::AuthenticatedPeer& message)
{
    if (role_ != Role::Creator || token.isEmpty()) return;
    if (!message.name.valid()) { server_.rejectAuthenticatedPeer(token,jam2::application::peerNameRequirement()); return; }
    const int remotePeers = peers_.size() - (peers_.contains(creator_.localToken) ? 1 : 0);
    if (creator_.sessionPeerLimit > 0 && !peers_.contains(token) &&
        remotePeers >= creator_.sessionPeerLimit) {
        if (multiplex_) multiplex_->rejectPeer(token);
        server_.rejectAuthenticatedPeer(token, QStringLiteral("Session peer limit reached"));
        return;
    }

    const QString endpoint = message.endpoint.text();
    const QString peerHost = normalizedHost(message.host);
    if (!message.endpoint.valid()) {
        server_.rejectAuthenticatedPeer(token, QStringLiteral("Authenticated peer omitted its UDP endpoint"));
        return;
    }

    const auto peerId = peerIdFromToken(token);
    if (!peerId) {
        server_.sendTo(token, QJsonObject{
            {QStringLiteral("type"), QStringLiteral("session.error")},
            {QStringLiteral("message"), QStringLiteral("Authenticated peer token has no valid peer id")},
        }, true);
        return;
    }

    for (auto known=peers_.cbegin(); known!=peers_.cend(); ++known) {
        if (known.key()!=token && known->peerId==*peerId) {
            server_.rejectAuthenticatedPeer(token,QStringLiteral("Peer ID collides with an existing member"));
            if (multiplex_) multiplex_->rejectPeer(token);
            return;
        }
    }
    controlRecoveryDeadlines_.remove(token);
    Peer peer;
    peer.name = message.name;
    peer.endpoint = endpoint;
    peer.peerId = *peerId;
    peer.edgeState = EdgeState::Authenticated;
    peer.proofState = QStringLiteral("authenticated");
    peer.streamState = QStringLiteral("candidate");
    peer.sameMachine = isSameMachine(peerHost);
    const bool newPeer = !peers_.contains(token);
    peers_[token] = peer;
    knownPeerNames_.insert(token, {peer.peerId, peer.name});
    if (multiplex_) {
        multiplex_->admitPeer(token);
        if (newPeer) introduceMeshPeer(token);
    }
    if (contractReady_) {
        server_.sendTo(token, contractMessage());
    }
    broadcastMembership();
    if (!editorState_.isEmpty()) {
        server_.sendTo(token, editorState_);
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
    if (!closing_ && role_ == Role::Creator && multiplex_ && peers_.contains(token) &&
        multiplex_->peerEndpoint(peers_.value(token).peerId)) {
        // A logical control failure does not invalidate this punched media
        // socket. Keep membership while the joiner reopens/authenticates a
        // fresh logical stream. Explicit peer leave bypasses this grace.
        if (!controlRecoveryDeadlines_.contains(token)) multiplex_->prepareRecovery(token);
        if (!controlRecoveryDeadlines_.contains(token))
            controlRecoveryDeadlines_[token] = jam2::monotonic_us() +
                static_cast<quint64>(heartbeatIntervalMs_) * heartbeatMissLimit_ * 1000;
        // Also notify pending asset work when no asset stream authenticated
        // yet: ControlServer cannot associate that unauthenticated stream
        // with this token, but the GUI's outstanding request already can.
        if (onAssetDisconnected) onAssetDisconnected(token);
        if (onSignalerStatus) onSignalerStatus(QStringLiteral(
            "UDP control stream lost; retaining audio socket while peer reconnects: %1").arg(peerName(token)));
        return;
    }
    controlRecoveryDeadlines_.remove(token);
    std::erase_if(recoveringUdpPairs_, [&](const auto& pair) { return pair.first == token || pair.second == token; });
    if (role_ == Role::Creator && peers_.remove(token) > 0) {
        if (multiplex_) multiplex_->removePeer(token);
        if (editorAuthorityToken_ == token) {
            editorAuthorityToken_.clear();
            editorState_ = {};
            if (editorRevision_ != std::numeric_limits<quint64>::max()) {
                ++editorRevision_;
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
    if (type == QStringLiteral("session.udp.introduce")) {
        if (multiplex_) multiplex_->prepareIntroduction(message.value(QStringLiteral("nonce")).toString());
        return;
    }
    if (type == QStringLiteral("session.connection.failure")) {
        connectionWarning(message.value(QStringLiteral("a")).toString(),
            message.value(QStringLiteral("b")).toString(),
            message.value(QStringLiteral("detail")).toString(), false);
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
                QStringLiteral("UDP multiplex session contract is invalid or incompatible");
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
            const QString detail = QStringLiteral("UDP multiplex coordinator membership is invalid");
            fail(TransportFailure::MembershipRejected, detail, false);
            publishTransportEvent(TransportEvent{
                TransportEventType::Failure,
                TransportFailure::MembershipRejected,
                detail}, false);
            client_.close();
        }
        return;
    }
    if ((isEditorAction(type) || isArrangementAction(type)) &&
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
    if (next.protocolVersion != jam2::protocol::kProtocolVersion ||
        !jam2::protocol::parse_audio_format(next.audioFormat.toStdString()).has_value() ||
        next.sampleRate < jam2::application::limits::kMinimumSampleRate ||
        next.sampleRate > jam2::application::limits::kMaximumSampleRate ||
        next.frameSize <= 0 ||
        next.frameSize > static_cast<int>(jam2::protocol::kMaxAudioFramesPerPacket) ||
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
    if (isEditorAction(type)) {
        quint64 revision = 0;
        if (!parseRevision(message.value(QStringLiteral("revision")), revision, false) ||
            revision < editorRevision_ ||
            (revision == editorRevision_ && authority != editorAuthorityToken_)) {
            return false;
        }
        if (revision > editorRevision_) {
            editorAuthorityToken_ = authority;
            editorRevision_ = revision;
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
    quint64 editorRevision = 0;
    quint64 arrangementRevision = 0;
    const int pageIndex = message.value(QStringLiteral("page_index")).toInt(-1);
    const int pageCount = message.value(QStringLiteral("page_count")).toInt(-1);
    const QString coordinator = message.value(QStringLiteral("coordinator_token")).toString();
    const QString editorAuthority =
        message.value(QStringLiteral("editor_authority_token")).toString();
    const QString arrangementAuthority =
        message.value(QStringLiteral("arrangement_authority_token")).toString();
    const QJsonArray entries = message.value(QStringLiteral("peers")).toArray();
    if (!parseRevision(message.value(QStringLiteral("revision")), revision, false) ||
        !parseRevision(message.value(QStringLiteral("editor_revision")), editorRevision, true) ||
        !parseRevision(
            message.value(QStringLiteral("arrangement_revision")), arrangementRevision, true) ||
        !message.value(QStringLiteral("page_index")).isDouble() ||
        !message.value(QStringLiteral("page_count")).isDouble() ||
        !message.value(QStringLiteral("peers")).isArray() ||
        pageIndex < 0 || pageCount <= 0 ||
        pageIndex >= pageCount || entries.size() > kMembershipEntriesPerPage ||
        !validToken(coordinator) ||
        (!editorAuthority.isEmpty() && !validToken(editorAuthority)) ||
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
        membershipAssembly_.editorAuthorityToken = editorAuthority;
        membershipAssembly_.arrangementAuthorityToken = arrangementAuthority;
        membershipAssembly_.editorRevision = editorRevision;
        membershipAssembly_.arrangementRevision = arrangementRevision;
    }
    if (membershipAssembly_.pageCount != pageCount ||
        membershipAssembly_.coordinatorToken != coordinator ||
        membershipAssembly_.editorAuthorityToken != editorAuthority ||
        membershipAssembly_.arrangementAuthorityToken != arrangementAuthority ||
        membershipAssembly_.editorRevision != editorRevision ||
        membershipAssembly_.arrangementRevision != arrangementRevision) {
        membershipAssembly_ = {};
        return false;
    }
    membershipAssembly_.pages[pageIndex] = message;
    if (membershipAssembly_.pages.size() != pageCount) {
        return true;
    }

    QMap<QString, Peer> next;
    QSet<quint64> memberIds;
    for (int page = 0; page < pageCount; ++page) {
        const QJsonArray values = membershipAssembly_.pages.value(page)
            .value(QStringLiteral("peers")).toArray();
        for (const QJsonValue& value : values) {
            const QJsonObject object = value.toObject();
            const QString token = object.value(QStringLiteral("token")).toString();
            QString endpoint = object.value(QStringLiteral("endpoint")).toString();
            bool idOk = false;
            const quint64 peerId = object.value(QStringLiteral("peer_id")).toString().toULongLong(&idOk);
            const auto derivedPeerId = peerIdFromToken(token);
            const auto name = jam2::application::PeerName::fromWire(object.value(QStringLiteral("name")).toString().toUtf8());
            if (!name || !derivedPeerId || *derivedPeerId != peerId ||
                endpoint.isEmpty() || endpoint.size() > 255 || !idOk || peerId == 0 ||
                next.contains(token) || memberIds.contains(peerId)) {
                membershipAssembly_ = {};
                return false;
            }
            memberIds.insert(peerId);
            Peer peer;
            peer.name = *name;
            peer.endpoint = endpoint;
            peer.peerId = peerId;
            peer.edgeState = token == joiner_.localToken ? EdgeState::Active : EdgeState::Candidate;
            peer.proofState = token == joiner_.localToken
                ? QStringLiteral("local")
                : QStringLiteral("candidate");
            peer.streamState = token == joiner_.localToken ? QStringLiteral("local") : QStringLiteral("candidate");
            next.insert(token, peer);
        }
    }
    if (!next.contains(joiner_.localToken) || !next.contains(coordinator) ||
        coordinator == joiner_.localToken || coordinator != coordinatorToken_ ||
        (!editorAuthority.isEmpty() && !next.contains(editorAuthority)) ||
        !next.contains(arrangementAuthority) || arrangementAuthority != coordinator ||
        editorRevision < editorRevision_ || arrangementRevision < arrangementRevision_ ||
        (membershipRevision_ > 0 && editorRevision == editorRevision_ &&
         editorAuthorityToken_ != editorAuthority) ||
        (membershipRevision_ > 0 && arrangementRevision == arrangementRevision_ &&
         arrangementAuthorityToken_ != arrangementAuthority)) {
        membershipAssembly_ = {};
        return false;
    }
    if (multiplex_) {
        for (auto it = peers_.cbegin(); it != peers_.cend(); ++it)
            if (!next.contains(it.key()) && it.key() != joiner_.localToken) multiplex_->removePeer(it.key());
    }
    peers_ = std::move(next);
    for (auto peer=peers_.cbegin();peer!=peers_.cend();++peer)
        knownPeerNames_.insert(peer.key(), {peer->peerId,peer->name});
    coordinatorToken_ = coordinator;
    editorAuthorityToken_ = editorAuthority;
    arrangementAuthorityToken_ = arrangementAuthority;
    editorRevision_ = editorRevision;
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
        entries.append(QJsonObject{
            {QStringLiteral("token"), token},
            {QStringLiteral("peer_id"), QString::number(peer.peerId)},
            {QStringLiteral("endpoint"), endpoint},
            {QStringLiteral("name"), peer.name.text()},
        });
    }
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("session.membership")},
        {QStringLiteral("revision"), static_cast<qint64>(membershipRevision_)},
        {QStringLiteral("page_index"), pageIndex},
        {QStringLiteral("page_count"), pageCount},
        {QStringLiteral("coordinator_token"), coordinatorToken_},
        {QStringLiteral("editor_authority_token"), editorAuthorityToken_},
        {QStringLiteral("arrangement_authority_token"), arrangementAuthorityToken_},
        {QStringLiteral("editor_revision"), static_cast<qint64>(editorRevision_)},
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
        const QString detail = QStringLiteral("UDP multiplex control reconnect attempts exhausted");
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
        QStringLiteral("UDP multiplex control reconnect scheduled"),
        true}, false);
    reconnectTimer_.start();
}

void SharedSessionController::connectJoiner()
{
    if (role_ != Role::Joiner || closing_) {
        return;
    }
    if (!multiplex_) return;
    if (everAuthenticated_ && reconnectAttempts_ > 2) {
        reconnectAttempts_ = 0;
        recoverMultiplexPeer(multiplex_->coordinatorToken());
    } else multiplex_->openStream(multiplex_->coordinatorToken(), jam2::application::UdpMuxChannel::Control);
}

void SharedSessionController::connectAssetJoiner()
{
    if (!assetChannelRequired_ || role_ != Role::Joiner || closing_ ||
        !client_.isConnected() ||
        assetClient_.isConnected()) {
        return;
    }
    if (multiplex_) multiplex_->openStream(multiplex_->coordinatorToken(),jam2::application::UdpMuxChannel::Asset);
}

void SharedSessionController::sendHeartbeat(const QString& targetToken)
{
    if (role_ != Role::Creator || closing_) {
        return;
    }
    const auto expired = controlRecoveryDeadlines_;
    for (auto it = expired.cbegin(); it != expired.cend(); ++it) {
        if (jam2::monotonic_us() < it.value()) continue;
        if (!multiplex_) { controlRecoveryDeadlines_.remove(it.key()); continue; }
        controlRecoveryDeadlines_[it.key()] = jam2::monotonic_us() +
            static_cast<quint64>(heartbeatIntervalMs_) * heartbeatMissLimit_ * 1000;
        introducePair(creator_.localToken, it.key());
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
    if (multiplex_) {
        recoverMultiplexPeer(coordinatorToken_);
        lastHeartbeat_.restart();
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
    if (multiplex_) {
        for (PeerSnapshot& peer : runtimeSnapshot.peers) {
            if (const auto endpoint = multiplex_->peerEndpoint(peer.peerId))
                peer.endpoint = QString::fromStdString(jam2::endpoint_to_string(*endpoint));
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
            runtimeAttachedForSession_ = true;
            if (peers != runtimePeers_ && runtime_->updatePeers(peers)) {
                runtimePeers_ = std::move(peers);
            }
        } else if (!runtimeAttachedForSession_) {
            Jam2RuntimeOptions options = networkOptionsFactory_(runtimeSnapshot);
            if (multiplex_) {
                options.datagram_transport = multiplex_;
                options.no_stun = true;
                options.bind.port = 0;
            }
            if (!runtime_->startNetwork(options)) {
                throw std::runtime_error("network audio runtime failed to start");
            }
            runtimeAttachedForSession_ = true;
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

#include "ControlServer.hpp"

#include "ControlProtocol.hpp"

#include <QTimer>

#include <algorithm>
#include <memory>
#include <utility>

using namespace jam2::control_protocol;
using jam2::application::NativeTcpConnection;

namespace {

bool validUdpEndpointText(const QString& endpoint)
{
    if (endpoint.isEmpty()) {
        return true;
    }
    const int separator = endpoint.lastIndexOf(QLatin1Char(':'));
    bool portOk = false;
    const int port = separator > 0 ? endpoint.mid(separator + 1).toInt(&portOk) : 0;
    const QString host = separator > 0 ? endpoint.left(separator) : QString{};
    if (!portOk || port < 1 || port > 65535 || host.isEmpty() || host.size() > 253) {
        return false;
    }
    for (const QChar character : host) {
        if (!(character.isLetterOrNumber() || character == QLatin1Char('.') ||
              character == QLatin1Char('-'))) {
            return false;
        }
    }
    return true;
}

} // namespace

ControlServer::ControlServer(QObject* parent)
    : QObject(parent)
{
}

ControlServer::~ControlServer()
{
    close();
}

bool ControlServer::listen(quint16 port, const QString& sessionHex, const QString& keyHex)
{
    close();
    sessionHex_ = sessionHex.toLower();
    masterKey_ = decodeHex(keyHex, 16);
    if (decodeHex(sessionHex_, 8).size() != 8 || masterKey_.size() != 16) {
        publishEvent(TransportEvent{
            TransportEventType::Failure,
            TransportFailure::InvalidConfiguration,
            QStringLiteral("TCP control session or key encoding is invalid")});
        return false;
    }
    authenticationFailuresInWindow_ = 0;
    authenticationFailureWindow_.restart();
    const quint64 generation = ++listenGeneration_;
    if (!server_.listen(
            port,
            this,
            [this, generation](const NativeTcpConnection::Pointer& connection) {
                if (generation != listenGeneration_ || !server_.isListening()) {
                    connection->close();
                    return;
                }
                acceptPeer(connection);
            },
            [this, generation](const QString& detail) {
                if (generation == listenGeneration_) {
                    publishEvent(TransportEvent{
                        TransportEventType::Failure,
                        TransportFailure::TransportError,
                        detail});
                }
            },
            kMaxPendingPeers)) {
        publishEvent(TransportEvent{
            TransportEventType::Failure,
            TransportFailure::TransportError,
            server_.errorString()});
        return false;
    }
    publishEvent(TransportEvent{
        TransportEventType::Listening,
        TransportFailure::None,
        QStringLiteral("TCP control listening")});
    return true;
}

void ControlServer::close()
{
    ++listenGeneration_;
    server_.close();
    const QList<PeerHandle> peers = std::move(peers_);
    peers_.clear();
    stats_.activeConnections = 0;
    for (const PeerHandle& peer : peers) {
        if (!peer) {
            continue;
        }
        if (peer->authenticationTimer) {
            peer->authenticationTimer->stop();
        }
        if (peer->frameTimer) {
            peer->frameTimer->stop();
        }
        if (peer->connection) {
            peer->connection->close();
        }
    }
    masterKey_.clear();
}

void ControlServer::send(const QJsonObject& message)
{
    const QList<PeerHandle> peers = peers_;
    for (const PeerHandle& peer : peers) {
        if (peer && peer->connection && peer->authenticated) {
            const QByteArray frame = encodeAuthenticated(message, peer->sendKey, peer->sendSequence);
            if (!frame.isEmpty() && writeFrame(peer, frame)) {
                ++peer->sendSequence;
            }
        }
    }
}

bool ControlServer::sendTo(const QString& token, const QJsonObject& message, bool closeAfterWrite)
{
    for (const PeerHandle& peer : peers_) {
        if (!peer || !peer->authenticated || peer->token != token) {
            continue;
        }
        const QByteArray frame = encodeAuthenticated(message, peer->sendKey, peer->sendSequence);
        if (frame.isEmpty() || !writeFrame(peer, frame, closeAfterWrite)) {
            return false;
        }
        ++peer->sendSequence;
        return true;
    }
    return false;
}

bool ControlServer::sendBinaryTo(const QString& token, const QByteArray& payload)
{
    for (const PeerHandle& peer : peers_) {
        if (!peer || !peer->authenticated || peer->token != token) {
            continue;
        }
        const QByteArray frame = encodeAuthenticatedBinary(payload, peer->sendKey, peer->sendSequence);
        if (frame.isEmpty() || !writeFrame(peer, frame)) {
            return false;
        }
        ++peer->sendSequence;
        return true;
    }
    return false;
}

bool ControlServer::canQueueTo(const QString& token, qint64 additionalBytes) const
{
    if (additionalBytes < 0 || additionalBytes > kOutputHighWaterBytes) {
        return false;
    }
    for (const PeerHandle& peer : peers_) {
        if (peer && peer->authenticated && peer->token == token && peer->connection) {
            return peer->connection->isConnected() &&
                peer->connection->bytesToWrite() <= kOutputHighWaterBytes - additionalBytes;
        }
    }
    return false;
}

bool ControlServer::rejectAuthenticatedPeer(const QString& token, const QString& reason)
{
    ++stats_.authenticatedCapRejects;
    publishEvent(TransportEvent{
        TransportEventType::Failure,
        TransportFailure::SessionPeerLimit,
        reason,
        false,
        true});
    return sendTo(token, QJsonObject{
        {QStringLiteral("type"), QStringLiteral("session.error")},
        {QStringLiteral("message"), reason},
    }, true);
}

bool ControlServer::hasPeer() const
{
    return authenticatedPeerCount() > 0;
}

QString ControlServer::errorString() const
{
    if (masterKey_.isEmpty() && !sessionHex_.isEmpty()) {
        return QStringLiteral("control session or key encoding is invalid");
    }
    return server_.errorString();
}

ControlServer::PeerHandle ControlServer::findPeer(
    const NativeTcpConnection::Pointer& connection) const
{
    for (const PeerHandle& peer : peers_) {
        if (peer && peer->connection == connection) {
            return peer;
        }
    }
    return {};
}

int ControlServer::authenticatedPeerCount() const
{
    int count = 0;
    for (const PeerHandle& peer : peers_) {
        count += peer && peer->authenticated ? 1 : 0;
    }
    return count;
}

int ControlServer::pendingPeerCount() const
{
    int count = 0;
    for (const PeerHandle& peer : peers_) {
        count += peer && !peer->authenticated ? 1 : 0;
    }
    return count;
}

void ControlServer::noteAuthenticationReject()
{
    ++stats_.authenticationRejects;
    (void)authenticationWorkAvailable();
    ++authenticationFailuresInWindow_;
}

bool ControlServer::authenticationWorkAvailable()
{
    if (!authenticationFailureWindow_.isValid() ||
        authenticationFailureWindow_.elapsed() >= kAuthenticationFailureWindowMs) {
        authenticationFailureWindow_.restart();
        authenticationFailuresInWindow_ = 0;
    }
    return authenticationFailuresInWindow_ < kMaxAuthenticationFailuresPerWindow;
}

void ControlServer::acceptPeer(const NativeTcpConnection::Pointer& connection)
{
    if (!connection || !connection->isConnected()) {
        return;
    }
    if (!authenticationWorkAvailable()) {
        ++stats_.authenticationRateLimitRejects;
        connection->close();
        return;
    }
    if (pendingPeerCount() >= kMaxPendingPeers) {
        ++stats_.pendingCapRejects;
        connection->close();
        return;
    }

    auto peer = std::make_shared<Peer>();
    peer->connection = connection;
    peer->serverNonce = randomNonce();
    peer->authenticationTimer = std::make_unique<QTimer>();
    peer->frameTimer = std::make_unique<QTimer>();
    peer->authenticationTimer->setSingleShot(true);
    peer->frameTimer->setSingleShot(true);
    peers_.push_back(peer);
    ++stats_.acceptedConnections;
    stats_.activeConnections = static_cast<quint64>(peers_.size());
    stats_.activeConnectionHighWater = std::max(
        stats_.activeConnectionHighWater, stats_.activeConnections);

    const std::weak_ptr<Peer> weakPeer = peer;
    QObject::connect(peer->authenticationTimer.get(), &QTimer::timeout, this, [this, weakPeer] {
        if (const PeerHandle current = weakPeer.lock();
            current && findPeer(current->connection) && !current->authenticated) {
            ++stats_.authenticationTimeouts;
            rejectPeer(
                current,
                QStringLiteral("TCP control authentication timeout"),
                TransportFailure::AuthenticationTimeout,
                true);
        }
    });
    QObject::connect(peer->frameTimer.get(), &QTimer::timeout, this, [this, weakPeer] {
        if (const PeerHandle current = weakPeer.lock();
            current && findPeer(current->connection) && !current->buffer.isEmpty()) {
            ++stats_.frameTimeouts;
            rejectPeer(
                current,
                QStringLiteral("TCP control incomplete-frame timeout"),
                TransportFailure::FrameTimeout,
                true);
        }
    });
    const QByteArray challenge = encodeHandshake(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("hello.challenge")},
        {QStringLiteral("version"), kControlProtocolVersion},
        {QStringLiteral("session"), sessionHex_},
        {QStringLiteral("server_nonce"), encodeHex(peer->serverNonce)},
    });
    if (!writeFrame(peer, challenge)) {
        return;
    }
    connection->start(
        this,
        [this, weakPeer](const QByteArray& bytes) {
            const PeerHandle current = weakPeer.lock();
            if (!current || !findPeer(current->connection)) {
                return;
            }
            current->buffer += bytes;
            stats_.maxBufferedInputBytes = std::max<quint64>(
                stats_.maxBufferedInputBytes, current->buffer.size());
            stats_.maxBufferedInputBytes = std::max<quint64>(
                stats_.maxBufferedInputBytes,
                static_cast<quint64>(std::max<qint64>(
                    0, current->connection->maxPendingReadBytes())));
            readPeer(current);
        },
        [this, weakPeer](const QString& detail) {
            if (const PeerHandle current = weakPeer.lock()) {
                disconnectPeer(current, detail);
            }
        });

    peer->authenticationTimer->start(kAuthenticationDeadlineMs);
    publishEvent(TransportEvent{
        TransportEventType::ChallengeSent,
        TransportFailure::None,
        QStringLiteral("TCP peer connected; sent bounded auth challenge")});
}

void ControlServer::disconnectPeer(const PeerHandle& peer, const QString& detail)
{
    if (!peer) {
        return;
    }
    for (qsizetype i = 0; i < peers_.size(); ++i) {
        if (peers_[i] != peer) {
            continue;
        }
        const QString token = peer->token;
        const bool wasAuthenticated = peer->authenticated;
        if (peer->authenticationTimer) {
            peer->authenticationTimer->stop();
        }
        if (peer->frameTimer) {
            peer->frameTimer->stop();
        }
        if (peer->connection) {
            stats_.maxBufferedInputBytes = std::max<quint64>(
                stats_.maxBufferedInputBytes,
                static_cast<quint64>(std::max<qint64>(
                    0, peer->connection->maxPendingReadBytes())));
        }
        peers_.removeAt(i);
        peer->connection.reset();
        stats_.activeConnections = static_cast<quint64>(peers_.size());
        ++stats_.disconnectedConnections;
        if (onDisconnected && wasAuthenticated) {
            onDisconnected(token);
        }
        if (!detail.isEmpty()) {
            publishEvent(TransportEvent{
                TransportEventType::Failure,
                TransportFailure::TransportError,
                detail,
                false,
                wasAuthenticated});
        }
        publishEvent(TransportEvent{
            TransportEventType::Disconnected,
            TransportFailure::None,
            QStringLiteral("TCP peer disconnected"),
            false,
            wasAuthenticated});
        return;
    }
}

void ControlServer::readPeer(const PeerHandle& peer)
{
    if (!peer || !peer->connection || !findPeer(peer->connection)) {
        return;
    }
    peer->readScheduled = false;
    int handled = 0;
    while (handled < kFramesPerTurn) {
        QByteArray body;
        QString error;
        const TakeFrameResult result = takeFrame(peer->buffer, body, error);
        if (result == TakeFrameResult::NeedMore) {
            if (peer->buffer.isEmpty()) {
                peer->frameTimer->stop();
            } else if (!peer->frameTimer->isActive()) {
                peer->frameTimer->start(kIncompleteFrameDeadlineMs);
            }
            return;
        }
        if (result == TakeFrameResult::Invalid) {
            ++stats_.frameRejects;
            rejectPeer(
                peer,
                QStringLiteral("TCP control frame rejected: ") + error,
                TransportFailure::FrameRejected,
                true);
            return;
        }
        peer->frameTimer->stop();
        ++handled;
        ++stats_.framesReceived;

        QJsonObject message;
        if (!peer->authenticated) {
            if (!decodeHandshake(body, message, error)) {
                noteAuthenticationReject();
                rejectPeer(
                    peer,
                    QStringLiteral("TCP control handshake rejected: ") + error,
                    TransportFailure::AuthenticationRejected);
                return;
            }
            const NativeTcpConnection::Pointer connection = peer->connection;
            handleHandshake(peer, message);
            if (!connection || !findPeer(connection) || !connection->isConnected()) {
                return;
            }
        } else {
            AuthenticatedPayload payload;
            if (!decodeAuthenticated(body, peer->receiveKey, peer->receiveSequence, payload, error)) {
                ++stats_.sequenceOrTagRejects;
                rejectPeer(
                    peer,
                    QStringLiteral("TCP control authenticated frame rejected: ") + error,
                    TransportFailure::AuthenticatedFrameRejected,
                    true);
                return;
            }
            ++peer->receiveSequence;
            if (payload.type == AuthenticatedPayloadType::Json) {
                if (onMessage) {
                    onMessage(peer->token, payload.message);
                }
            } else if (onBinaryMessage) {
                onBinaryMessage(peer->token, payload.binary);
            }
            if (!peer->connection || !findPeer(peer->connection)) {
                return;
            }
        }
    }

    if (!peer->buffer.isEmpty() && !peer->readScheduled) {
        peer->readScheduled = true;
        const std::weak_ptr<Peer> weakPeer = peer;
        QTimer::singleShot(0, this, [this, weakPeer] {
            if (const PeerHandle current = weakPeer.lock();
                current && current->connection && findPeer(current->connection)) {
                readPeer(current);
            }
        });
    }
}

void ControlServer::handleHandshake(const PeerHandle& peer, const QJsonObject& message)
{
    const QString type = message.value(QStringLiteral("type")).toString();
    const QString session = message.value(QStringLiteral("session")).toString().toLower();
    const QByteArray clientNonce = decodeHex(message.value(QStringLiteral("client_nonce")).toString(), 16);
    const QByteArray clientProof = decodeHex(message.value(QStringLiteral("proof")).toString(), 16);
    QString token = message.value(QStringLiteral("peer_token")).toString();
    const QString udpEndpoint = message.value(QStringLiteral("udp_endpoint")).toString();
    const bool tokenValid = token.isEmpty() || peerIdFromToken(token).has_value();
    if (type != QStringLiteral("hello.proof") ||
        message.value(QStringLiteral("version")).toInt() != kControlProtocolVersion ||
        session != sessionHex_ || clientNonce.size() != 16 || clientProof.size() != 16 ||
        !tokenValid || !validUdpEndpointText(udpEndpoint)) {
        noteAuthenticationReject();
        rejectPeer(
            peer,
            QStringLiteral("TCP control authentication fields are invalid"),
            TransportFailure::AuthenticationRejected);
        return;
    }
    if (token.isEmpty()) {
        token = randomPeerToken();
    }
    for (const PeerHandle& existing : peers_) {
        if (existing != peer && existing && existing->authenticated && existing->token == token) {
            noteAuthenticationReject();
            rejectPeer(
                peer,
                QStringLiteral("TCP control peer token is already active"),
                TransportFailure::AuthenticationRejected);
            return;
        }
    }
    peer->transcript = makeTranscript(sessionHex_, peer->serverNonce, clientNonce, token, udpEndpoint);
    const QByteArray expectedProof = keyedValue(
        masterKey_, QByteArrayLiteral("jam2-control-client-proof"), peer->transcript).left(16);
    if (!constantTimeEqual(clientProof, expectedProof)) {
        noteAuthenticationReject();
        rejectPeer(
            peer,
            QStringLiteral("TCP control client proof is invalid"),
            TransportFailure::AuthenticationRejected);
        return;
    }

    const QByteArray serverProof = keyedValue(
        masterKey_, QByteArrayLiteral("jam2-control-server-proof"), peer->transcript).left(16);
    const QByteArray response = encodeHandshake(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("hello.ok")},
        {QStringLiteral("version"), kControlProtocolVersion},
        {QStringLiteral("role"), QStringLiteral("listener")},
        {QStringLiteral("peer_token"), token},
        {QStringLiteral("proof"), encodeHex(serverProof)},
    });
    if (!writeFrame(peer, response)) {
        return;
    }

    peer->token = token;
    peer->receiveKey = keyedValue(masterKey_, QByteArrayLiteral("jam2-control-c2s"), peer->transcript);
    peer->sendKey = keyedValue(masterKey_, QByteArrayLiteral("jam2-control-s2c"), peer->transcript);
    peer->authenticated = true;
    peer->authenticationTimer->stop();
    publishEvent(TransportEvent{
        TransportEventType::Authenticated,
        TransportFailure::None,
        QStringLiteral("TCP peer authenticated"),
        false,
        true});
    const NativeTcpConnection::Pointer connection = peer->connection;
    if (connection && findPeer(connection) && onAuthenticated) {
        QJsonObject authenticatedMessage{
            {QStringLiteral("peer_token"), token},
            {QStringLiteral("udp_endpoint"), udpEndpoint},
            {QStringLiteral("tcp_peer_host"), connection->peerHost()},
        };
        onAuthenticated(token, authenticatedMessage);
    }
}

bool ControlServer::writeFrame(
    const PeerHandle& peer,
    const QByteArray& frame,
    bool closeAfterWrite)
{
    const NativeTcpConnection::Pointer connection = peer ? peer->connection : nullptr;
    if (!connection || !connection->isConnected() || frame.isEmpty()) {
        return false;
    }
    const qint64 queued = connection->bytesToWrite();
    stats_.maxQueuedOutputBytes = std::max<quint64>(stats_.maxQueuedOutputBytes, queued);
    if (queued + frame.size() > kOutputHighWaterBytes) {
        ++stats_.outputHighWaterRejects;
        rejectPeer(
            peer,
            QStringLiteral("TCP control output high-water exceeded"),
            TransportFailure::OutputHighWater,
            true);
        return false;
    }
    if (!connection->write(frame, closeAfterWrite)) {
        rejectPeer(
            peer,
            QStringLiteral("TCP control write failed"),
            TransportFailure::WriteFailed,
            true);
        return false;
    }
    ++stats_.framesSent;
    stats_.maxQueuedOutputBytes = std::max<quint64>(
        stats_.maxQueuedOutputBytes, connection->bytesToWrite());
    return true;
}

void ControlServer::rejectPeer(
    const PeerHandle& peer,
    const QString& reason,
    TransportFailure failure,
    bool abort)
{
    const NativeTcpConnection::Pointer connection = peer ? peer->connection : nullptr;
    if (!connection || !findPeer(connection)) {
        return;
    }
    publishEvent(TransportEvent{
        TransportEventType::Failure,
        failure,
        reason,
        false,
        peer->authenticated});
    if (!connection || !findPeer(connection)) {
        return;
    }
    if (abort) {
        connection->close();
        disconnectPeer(peer);
        return;
    }
    const QByteArray errorFrame = encodeHandshake(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("hello.error")},
        {QStringLiteral("reason"), reason},
    });
    if (!errorFrame.isEmpty() &&
        connection->bytesToWrite() + errorFrame.size() <= kOutputHighWaterBytes &&
        connection->write(errorFrame, true)) {
        return;
    }
    connection->close();
    disconnectPeer(peer);
}

void ControlServer::publishEvent(TransportEvent event)
{
    if (onEvent) {
        onEvent(event);
    }
}

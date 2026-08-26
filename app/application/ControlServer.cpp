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

ControlServer::Stats ControlServer::stats() const
{
    Stats snapshot = stats_;
    snapshot.pendingCapRejects += server_.pendingDeliveryRejects();
    return snapshot;
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
    stats_.assetActiveConnections = 0;
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
        if (peer && peer->connection && peer->authenticated && !peer->assetChannel) {
            const AuthenticatedJsonFrames encoded =
                encodeAuthenticatedJsonFrames(message, peer->sendKey, peer->sendSequence);
            qint64 wireBytes = 0;
            for (const QByteArray& frame : encoded.frames) wireBytes += frame.size();
            if (encoded.frames.isEmpty() || !canQueueTo(peer->token, wireBytes)) {
                ++stats_.outputHighWaterRejects;
                continue;
            }
            bool sent = true;
            for (const QByteArray& frame : encoded.frames) {
                if (!writeFrame(peer, frame)) {
                    sent = false;
                    break;
                }
                ++peer->sendSequence;
            }
            if (sent && encoded.chunked) {
                ++stats_.largeJsonMessagesSent;
                stats_.largeJsonRawBytesSent += static_cast<quint64>(encoded.rawBytes);
                stats_.largeJsonCompressedBytesSent += static_cast<quint64>(encoded.compressedBytes);
            }
        }
    }
}

bool ControlServer::sendTo(const QString& token, const QJsonObject& message, bool closeAfterWrite)
{
    for (const PeerHandle& peer : peers_) {
        if (!peer || !peer->authenticated || peer->assetChannel || peer->token != token) {
            continue;
        }
        const AuthenticatedJsonFrames encoded =
            encodeAuthenticatedJsonFrames(message, peer->sendKey, peer->sendSequence);
        qint64 wireBytes = 0;
        for (const QByteArray& frame : encoded.frames) wireBytes += frame.size();
        if (encoded.frames.isEmpty() || !canQueueTo(token, wireBytes)) {
            ++stats_.outputHighWaterRejects;
            return false;
        }
        for (qsizetype index = 0; index < encoded.frames.size(); ++index) {
            if (!writeFrame(
                    peer,
                    encoded.frames.at(index),
                    closeAfterWrite && index + 1 == encoded.frames.size())) {
                return false;
            }
            ++peer->sendSequence;
        }
        if (encoded.chunked) {
            ++stats_.largeJsonMessagesSent;
            stats_.largeJsonRawBytesSent += static_cast<quint64>(encoded.rawBytes);
            stats_.largeJsonCompressedBytesSent += static_cast<quint64>(encoded.compressedBytes);
        }
        return true;
    }
    return false;
}

bool ControlServer::sendBinaryTo(const QString& token, const QByteArray& payload)
{
    for (const PeerHandle& peer : peers_) {
        if (!peer || !peer->authenticated || peer->assetChannel || peer->token != token) {
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

bool ControlServer::sendAssetTo(const QString& token, const QJsonObject& message)
{
    const PeerHandle peer = findAuthenticatedPeer(token, true);
    if (!peer) {
        return false;
    }
    const AuthenticatedJsonFrames encoded =
        encodeAuthenticatedJsonFrames(message, peer->sendKey, peer->sendSequence);
    qint64 wireBytes = 0;
    for (const QByteArray& frame : encoded.frames) wireBytes += frame.size();
    if (encoded.frames.isEmpty() || !canQueueAssetTo(token, wireBytes)) {
        ++stats_.outputHighWaterRejects;
        return false;
    }
    for (const QByteArray& frame : encoded.frames) {
        if (!writeFrame(peer, frame)) {
            return false;
        }
        ++peer->sendSequence;
    }
    return true;
}

bool ControlServer::sendAssetBinaryTo(const QString& token, const QByteArray& payload)
{
    const PeerHandle peer = findAuthenticatedPeer(token, true);
    if (!peer) {
        return false;
    }
    const QByteArray frame = encodeAuthenticatedBinary(payload, peer->sendKey, peer->sendSequence);
    if (frame.isEmpty() || !canQueueAssetTo(token, frame.size()) || !writeFrame(peer, frame)) {
        return false;
    }
    ++peer->sendSequence;
    return true;
}

bool ControlServer::canQueueTo(const QString& token, qint64 additionalBytes) const
{
    const qint64 queueBound = std::min(
        kOutputHighWaterBytes,
        jam2::application::kNativeTcpQueueHighWaterBytes);
    if (additionalBytes < 0 || additionalBytes > queueBound) {
        return false;
    }
    for (const PeerHandle& peer : peers_) {
        if (peer && peer->authenticated && !peer->assetChannel &&
            peer->token == token && peer->connection) {
            return peer->connection->isConnected() &&
                peer->connection->bytesToWrite() <= queueBound - additionalBytes;
        }
    }
    return false;
}

bool ControlServer::canQueueAssetTo(const QString& token, qint64 additionalBytes) const
{
    const qint64 queueBound = std::min(
        kOutputHighWaterBytes,
        jam2::application::kNativeTcpQueueHighWaterBytes);
    if (additionalBytes < 0 || additionalBytes > queueBound) {
        return false;
    }
    const PeerHandle peer = findAuthenticatedPeer(token, true);
    return peer && peer->connection && peer->connection->isConnected() &&
        peer->connection->bytesToWrite() <= queueBound - additionalBytes;
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
        count += peer && peer->authenticated && !peer->assetChannel ? 1 : 0;
    }
    return count;
}

ControlServer::PeerHandle ControlServer::findAuthenticatedPeer(
    const QString& token,
    bool assetChannel) const
{
    for (const PeerHandle& peer : peers_) {
        if (peer && peer->authenticated && peer->assetChannel == assetChannel &&
            peer->token == token && peer->connection) {
            return peer;
        }
    }
    return {};
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
            current->receivedAnyInput = current->receivedAnyInput || !bytes.isEmpty();
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
        const bool wasAssetChannel = peer->assetChannel;
        const bool disconnectedBeforeAuthenticationInput =
            !wasAuthenticated && !peer->receivedAnyInput;
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
        if (wasAssetChannel) {
            stats_.assetActiveConnections = stats_.assetActiveConnections > 0
                ? stats_.assetActiveConnections - 1 : 0;
            ++stats_.assetDisconnectedConnections;
        }
        if (disconnectedBeforeAuthenticationInput) {
            ++stats_.preAuthenticationDisconnects;
        }
        if (onDisconnected && wasAuthenticated && !wasAssetChannel) {
            onDisconnected(token);
        } else if (onAssetDisconnected && wasAuthenticated && wasAssetChannel) {
            onAssetDisconnected(token);
        }
        if (disconnectedBeforeAuthenticationInput) {
            const QString reason = detail.isEmpty()
                ? QStringLiteral("TCP peer disconnected before sending authentication data")
                : QStringLiteral("TCP peer disconnected before sending authentication data: ") + detail;
            publishEvent(TransportEvent{
                TransportEventType::Failure,
                TransportFailure::PreAuthenticationDisconnect,
                reason,
                false,
                false});
        } else if (!detail.isEmpty()) {
            publishEvent(TransportEvent{
                TransportEventType::Failure,
                TransportFailure::TransportError,
                detail,
                false,
                wasAuthenticated,
                wasAssetChannel});
        }
        publishEvent(TransportEvent{
            TransportEventType::Disconnected,
            TransportFailure::None,
            wasAssetChannel
                ? QStringLiteral("TCP asset stream disconnected")
                : QStringLiteral("TCP peer disconnected"),
            false,
            wasAuthenticated,
            wasAssetChannel});
        if (wasAuthenticated && !wasAssetChannel) {
            const PeerHandle assetPeer = findAuthenticatedPeer(token, true);
            if (assetPeer && assetPeer->connection) {
                assetPeer->connection->close();
                // Closing a native connection suppresses its later read callback,
                // so remove the paired asset entry now. Otherwise a reconnect
                // with the same peer token is rejected forever as a duplicate.
                disconnectPeer(assetPeer);
            }
        }
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
            if (peer->assetChannel) {
                if (payload.type == AuthenticatedPayloadType::Json) {
                    if (onAssetMessage) {
                        onAssetMessage(peer->token, payload.message);
                    }
                } else if (payload.type == AuthenticatedPayloadType::AssetChunk) {
                    if (onAssetBinaryMessage) {
                        onAssetBinaryMessage(peer->token, payload.binary);
                    }
                } else {
                    ++stats_.sequenceOrTagRejects;
                    rejectPeer(
                        peer,
                        QStringLiteral("TCP asset stream rejected a non-asset frame"),
                        TransportFailure::AuthenticatedFrameRejected,
                        true);
                    return;
                }
            } else if (payload.type == AuthenticatedPayloadType::Json) {
                if (peer->largeJsonReceiver.active()) {
                    ++stats_.sequenceOrTagRejects;
                    rejectPeer(
                        peer,
                        QStringLiteral("TCP control large JSON transfer was interleaved"),
                        TransportFailure::AuthenticatedFrameRejected,
                        true);
                    return;
                }
                if (onMessage) {
                    onMessage(peer->token, payload.message);
                }
            } else if (payload.type == AuthenticatedPayloadType::LargeJsonChunk) {
                QJsonObject completed;
                bool ready = false;
                if (!peer->largeJsonReceiver.accept(payload.binary, completed, ready, error)) {
                    ++stats_.sequenceOrTagRejects;
                    rejectPeer(
                        peer,
                        QStringLiteral("TCP control large JSON transfer rejected: ") + error,
                        TransportFailure::AuthenticatedFrameRejected,
                        true);
                    return;
                }
                if (ready) {
                    ++stats_.largeJsonMessagesReceived;
                    if (onMessage) {
                        onMessage(peer->token, completed);
                    }
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
    const QString channel = message.value(QStringLiteral("channel"))
        .toString(QStringLiteral("control"));
    const bool assetChannel = channel == QStringLiteral("asset");
    const bool tokenValid = token.isEmpty() || peerIdFromToken(token).has_value();
    if (type != QStringLiteral("hello.proof") ||
        message.value(QStringLiteral("version")).toInt() != kControlProtocolVersion ||
        session != sessionHex_ || clientNonce.size() != 16 || clientProof.size() != 16 ||
        !tokenValid || !validUdpEndpointText(udpEndpoint) ||
        (channel != QStringLiteral("control") && !assetChannel) ||
        (assetChannel && udpEndpoint.size() > 255)) {
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
    bool controlPeerPresent = false;
    PeerHandle replacedAssetPeer;
    for (const PeerHandle& existing : peers_) {
        if (existing != peer && existing && existing->authenticated && existing->token == token) {
            controlPeerPresent = controlPeerPresent || !existing->assetChannel;
            if (existing->assetChannel != assetChannel) {
                continue;
            }
            if (assetChannel) {
                replacedAssetPeer = existing;
                continue;
            }
            noteAuthenticationReject();
            rejectPeer(
                peer,
                QStringLiteral("TCP control peer token is already active"),
                TransportFailure::AuthenticationRejected);
            return;
        }
    }
    if (assetChannel && !controlPeerPresent) {
        noteAuthenticationReject();
        rejectPeer(
            peer,
            QStringLiteral("TCP asset stream requires an authenticated control peer"),
            TransportFailure::AuthenticationRejected);
        return;
    }
    peer->transcript = makeTranscript(
        sessionHex_, peer->serverNonce, clientNonce, token, udpEndpoint, channel);
    const QByteArray clientProofDomain = assetChannel
        ? QByteArrayLiteral("jam2-asset-client-proof")
        : QByteArrayLiteral("jam2-control-client-proof");
    const QByteArray expectedProof = keyedValue(
        masterKey_, clientProofDomain, peer->transcript).left(16);
    if (!constantTimeEqual(clientProof, expectedProof)) {
        noteAuthenticationReject();
        rejectPeer(
            peer,
            QStringLiteral("TCP control client proof is invalid"),
            TransportFailure::AuthenticationRejected);
        return;
    }

    const QByteArray serverProofDomain = assetChannel
        ? QByteArrayLiteral("jam2-asset-server-proof")
        : QByteArrayLiteral("jam2-control-server-proof");
    const QByteArray serverProof = keyedValue(
        masterKey_, serverProofDomain, peer->transcript).left(16);
    const QByteArray response = encodeHandshake(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("hello.ok")},
        {QStringLiteral("version"), kControlProtocolVersion},
        {QStringLiteral("role"), QStringLiteral("listener")},
        {QStringLiteral("peer_token"), token},
        {QStringLiteral("channel"), channel},
        {QStringLiteral("proof"), encodeHex(serverProof)},
    });
    if (!writeFrame(peer, response)) {
        return;
    }

    peer->token = token;
    peer->assetChannel = assetChannel;
    peer->receiveKey = keyedValue(
        masterKey_,
        assetChannel ? QByteArrayLiteral("jam2-asset-c2s")
                     : QByteArrayLiteral("jam2-control-c2s"),
        peer->transcript);
    peer->sendKey = keyedValue(
        masterKey_,
        assetChannel ? QByteArrayLiteral("jam2-asset-s2c")
                     : QByteArrayLiteral("jam2-control-s2c"),
        peer->transcript);
    peer->authenticated = true;
    peer->authenticationTimer->stop();
    if (replacedAssetPeer && replacedAssetPeer->connection &&
        findPeer(replacedAssetPeer->connection)) {
        replacedAssetPeer->connection->close();
        disconnectPeer(replacedAssetPeer);
    }
    if (assetChannel) {
        ++stats_.assetAcceptedConnections;
        ++stats_.assetActiveConnections;
        stats_.assetConnectionHighWater = std::max(
            stats_.assetConnectionHighWater, stats_.assetActiveConnections);
    }
    publishEvent(TransportEvent{
        TransportEventType::Authenticated,
        TransportFailure::None,
        assetChannel
            ? QStringLiteral("TCP asset stream authenticated")
            : QStringLiteral("TCP peer authenticated"),
        false,
        true,
        assetChannel});
    const NativeTcpConnection::Pointer connection = peer->connection;
    if (!assetChannel && connection && findPeer(connection) && onAuthenticated) {
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
    const qint64 queueBound = std::min(
        kOutputHighWaterBytes,
        jam2::application::kNativeTcpQueueHighWaterBytes);
    if (queued + frame.size() > queueBound) {
        ++stats_.outputHighWaterRejects;
        // A full bounded queue is backpressure, not a transport failure.
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
        peer->authenticated,
        peer->assetChannel});
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

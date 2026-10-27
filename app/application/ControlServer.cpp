#include "ControlServer.hpp"
#include "PeerName.hpp"

#include "ControlProtocol.hpp"

#include <QTimer>

#include <algorithm>
#include <memory>
#include <utility>

using namespace jam2::control_protocol;
using jam2::application::ControlConnection;

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
    for (const auto& peer : peers_) if (peer && peer->wire)
        jam2::application::control_wire::accumulate(snapshot.wire, peer->wire->stats());
    return snapshot;
}
QJsonArray ControlServer::wireDiagnostics() const
{
    QJsonArray result;
    for (const auto& peer : peers_) if (peer && peer->wire) result.append(QJsonObject{
        {QStringLiteral("peer"), peer->token}, {QStringLiteral("asset_channel"), peer->assetChannel},
        {QStringLiteral("stats"), jam2::application::control_wire::diagnostics(peer->wire->stats())}});
    return result;
}

bool ControlServer::prepareSession(const QString& sessionHex, const QString& keyHex)
{
    close();
    sessionHex_ = sessionHex.toLower();
    masterKey_ = decodeHex(keyHex, 16);
    if (decodeHex(sessionHex_, 8).size() != 8 || masterKey_.size() != 16) {
        publishEvent(TransportEvent{
            TransportEventType::Failure,
            TransportFailure::InvalidConfiguration,
            QStringLiteral("UDP multiplex control session or key encoding is invalid")});
        return false;
    }
    authenticationFailuresInWindow_ = 0;
    authenticationFailureWindow_.restart();
    return true;
}

bool ControlServer::listenViaTransport(const QString& sessionHex, const QString& keyHex)
{
    if (!prepareSession(sessionHex, keyHex)) return false;
    transportListening_ = true;
    publishEvent(TransportEvent{TransportEventType::Listening, TransportFailure::None,
        QStringLiteral("UDP multiplex control listening")});
    return true;
}

void ControlServer::acceptTransport(const ControlConnection::Pointer& connection)
{
    if (transportListening_ && connection) acceptPeer(connection);
    else if (connection) connection->close();
}

void ControlServer::close()
{
    transportListening_ = false;
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
        if (peer->wire) {
            auto value = peer->wire->stats();
            value.unresolvedReceipts += value.pendingReceipts;
            value.pendingReceipts = 0; value.queuedBytes = 0;
            jam2::application::control_wire::accumulate(stats_.wire, value);
            peer->wire.reset();
        }
        if (peer->connection) {
            peer->connection->close();
        }
    }
    masterKey_.clear();
}

void ControlServer::send(const QJsonObject& message)
{
    const auto peers=peers_;
    for (const auto& peer : peers) if (peer && peer->authenticated && !peer->assetChannel && peer->wire) {
        const auto wire=peer->wire;
        const auto rejects=wire->stats().queueRejects;
        if (!wire->send(message) && wire->stats().queueRejects>rejects) peer->connection->close();
    }
}
bool ControlServer::sendTo(const QString& token,const QJsonObject& message,bool closeAfterWrite)
{
    const auto peer=findAuthenticatedPeer(token,false);
    if (!peer || !peer->wire) return false;
    const auto wire=peer->wire;
    const bool sent=wire->send(message);
    if (sent && closeAfterWrite) {
        const auto connection=peer->connection;
        wire->closeAfterDrain([connection] { connection->write({},true); });
    }
    return sent;
}
bool ControlServer::sendBinaryTo(const QString& token,const QByteArray& payload)
{
    const auto peer=findAuthenticatedPeer(token,false);
    return peer && peer->wire && peer->wire->sendBinary(payload);
}
bool ControlServer::sendAssetTo(const QString& token,const QJsonObject& message)
{
    const auto peer=findAuthenticatedPeer(token,true);
    return peer && peer->wire && peer->wire->send(message);
}
bool ControlServer::sendAssetBinaryTo(const QString& token,const QByteArray& payload)
{
    const auto peer=findAuthenticatedPeer(token,true);
    return peer && peer->wire && peer->wire->sendAsset(payload);
}
bool ControlServer::canQueueTo(const QString& token,qint64 additionalBytes) const
{
    const auto peer=findAuthenticatedPeer(token,false);
    return peer && peer->wire && peer->wire->canQueue(additionalBytes);
}
bool ControlServer::canQueueAssetTo(const QString& token,qint64 additionalBytes) const
{
    const auto peer=findAuthenticatedPeer(token,true);
    return peer && peer->wire && peer->wire->canQueue(additionalBytes);
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
    return {};
}

ControlServer::PeerHandle ControlServer::findPeer(
    const ControlConnection::Pointer& connection) const
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

void ControlServer::acceptPeer(const ControlConnection::Pointer& connection)
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
    peer->frameTimer->setTimerType(Qt::PreciseTimer);
    peer->frameClock.start();
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
                QStringLiteral("UDP multiplex control authentication timeout"),
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
                QStringLiteral("UDP multiplex control incomplete-frame timeout"),
                TransportFailure::FrameTimeout,
                true);
        }
    });
    QJsonObject challengeMessage{
        {QStringLiteral("type"), QStringLiteral("hello.challenge")},
        {QStringLiteral("version"), kControlProtocolVersion},
        {QStringLiteral("session"), sessionHex_},
        {QStringLiteral("server_nonce"), encodeHex(peer->serverNonce)},
    };
    const QByteArray challenge = encodeHandshake(challengeMessage);
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
            if (!bytes.isEmpty()) current->frameDeadline.received(current->frameClock.elapsed());
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

    peer->authenticationTimer->start(connection->authenticationDeadlineMs());
    publishEvent(TransportEvent{
        TransportEventType::ChallengeSent,
        TransportFailure::None,
        QStringLiteral("UDP multiplex peer connected; sent bounded auth challenge")});
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
        if (peer->wire) {
            auto value = peer->wire->stats();
            value.unresolvedReceipts += value.pendingReceipts;
            value.pendingReceipts = 0; value.queuedBytes = 0;
            jam2::application::control_wire::accumulate(stats_.wire, value);
            peer->wire.reset();
        }
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
                ? QStringLiteral("UDP multiplex peer disconnected before sending authentication data")
                : QStringLiteral("UDP multiplex peer disconnected before sending authentication data: ") + detail;
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
                ? QStringLiteral("UDP multiplex asset stream disconnected")
                : QStringLiteral("UDP multiplex peer disconnected"),
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
            } else {
                if (peer->frameDeadline.began < 0) peer->frameDeadline.received(peer->frameClock.elapsed());
                peer->frameTimer->start(peer->frameDeadline.remaining(peer->frameClock.elapsed()));
            }
            return;
        }
        if (result == TakeFrameResult::Invalid) {
            ++stats_.frameRejects;
            rejectPeer(
                peer,
                QStringLiteral("UDP multiplex control frame rejected: ") + error,
                TransportFailure::FrameRejected,
                true);
            return;
        }
        peer->frameTimer->stop();
        peer->frameDeadline.clear();
        ++handled;
        ++stats_.framesReceived;

        QJsonObject message;
        if (!peer->authenticated) {
            if (!decodeHandshake(body, message, error)) {
                noteAuthenticationReject();
                rejectPeer(
                    peer,
                    QStringLiteral("UDP multiplex control handshake rejected: ") + error,
                    TransportFailure::AuthenticationRejected);
                return;
            }
            const ControlConnection::Pointer connection = peer->connection;
            handleHandshake(peer, message);
            if (!connection || !findPeer(connection) || !connection->isConnected()) {
                return;
            }
        } else if (const auto wire = peer->wire) {
            if (!wire->receive(body, error)) {
                ++stats_.sequenceOrTagRejects;
                rejectPeer(peer, QStringLiteral("Typed control rejected: ") + error,
                    TransportFailure::AuthenticatedFrameRejected, true);
                return;
            }
            if (!peer->connection || !findPeer(peer->connection)) return;
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
    std::optional<jam2::application::PeerName> peerName;
    if (assetChannel) {
        for (const auto& control : peers_) if (control && control->authenticated && !control->assetChannel && control->token == token)
            peerName = control->name;
        if (message.contains(QStringLiteral("peer_name"))) peerName.reset();
    } else peerName = jam2::application::PeerName::fromWire(message.value(QStringLiteral("peer_name")).toString().toUtf8());
    const bool tokenValid = peerIdFromToken(token).has_value();
    if (type != QStringLiteral("hello.proof") ||
        message.value(QStringLiteral("version")).toInt() != kControlProtocolVersion ||
        session != sessionHex_ || clientNonce.size() != 16 || clientProof.size() != 16 ||
        !tokenValid || !peerName || !validUdpEndpointText(udpEndpoint) ||
        (channel != QStringLiteral("control") && !assetChannel) ||
        (assetChannel && udpEndpoint.size() > 255)) {
        noteAuthenticationReject();
        rejectPeer(
            peer,
            QStringLiteral("UDP multiplex control authentication fields are invalid"),
            TransportFailure::AuthenticationRejected);
        return;
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
                QStringLiteral("UDP multiplex control peer token is already active"),
                TransportFailure::AuthenticationRejected);
            return;
        }
    }
    if (assetChannel && !controlPeerPresent) {
        noteAuthenticationReject();
        rejectPeer(
            peer,
            QStringLiteral("UDP multiplex asset stream requires an authenticated control peer"),
            TransportFailure::AuthenticationRejected);
        return;
    }
    peer->transcript = makeTranscript(
        sessionHex_, peer->serverNonce, clientNonce, token, udpEndpoint, channel, peerName->text());
    const QByteArray clientProofDomain = assetChannel
        ? QByteArrayLiteral("jam2-asset-client-proof")
        : QByteArrayLiteral("jam2-control-client-proof");
    const QByteArray expectedProof = keyedValue(
        masterKey_, clientProofDomain, peer->transcript).left(16);
    if (!constantTimeEqual(clientProof, expectedProof)) {
        noteAuthenticationReject();
        rejectPeer(
            peer,
            QStringLiteral("UDP multiplex control client proof is invalid"),
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
    peer->name = *peerName;
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
    {
        const std::weak_ptr<Peer> weak = peer;
        peer->wire = std::make_shared<jam2::application::control_wire::Channel>(peer->sendKey, peer->receiveKey,
            [weak](const QByteArray& bytes) { const auto p = weak.lock(); return p && p->connection && p->connection->isConnected() && p->connection->write(bytes); },
            [weak] { const auto p = weak.lock(); return p && p->connection ? p->connection->bytesToWrite() : qint64{256 * 1024}; });
        peer->wire->onMessage = [this, weak](const QJsonObject& value) {
            if (const auto p = weak.lock()) {
                if (p->assetChannel) { if (onAssetMessage) onAssetMessage(p->token, value); }
                else if (onMessage) onMessage(p->token, value);
            }
        };
        peer->wire->validateMessage = [this, weak](const QJsonObject& value) {
            const auto p = weak.lock();
            return p && !p->assetChannel && validateMessage ? validateMessage(value) : jam2::application::control_wire::DispatchDecision{};
        };
        peer->wire->onAsset = [this, weak](const QByteArray& value) {
            if (const auto p = weak.lock()) {
                if (p->assetChannel) { if (onAssetBinaryMessage) onAssetBinaryMessage(p->token, value); }
                else if (onBinaryMessage) onBinaryMessage(p->token, value);
            }
        };
        peer->wire->onFailure = [this, weak](const QString& error) {
            if (const auto p = weak.lock()) rejectPeer(p, error, TransportFailure::AuthenticatedFrameRejected, true);
        };
        if (!assetChannel && peer->connection->hasIndependentControl()) {
            peer->wire->enableIndependent(
                [weak](const QByteArray& bytes) { const auto p = weak.lock(); return p && p->connection && p->connection->writeIndependent(bytes); },
                [weak] { const auto p = weak.lock(); return p && p->connection ? p->connection->independentBytesToWrite() : qint64{16 * 1024}; });
            peer->connection->startIndependent(this, [this, weak](const QByteArray& bytes) {
                if (const auto p = weak.lock(); p && p->wire) { QString error;
                    const auto wire = p->wire;
                    if (!wire->receiveIndependent(bytes, error)) rejectPeer(p, error, TransportFailure::AuthenticatedFrameRejected, true);
                }
            });
        }
    }
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
            ? QStringLiteral("UDP multiplex asset stream authenticated")
            : QStringLiteral("UDP multiplex peer authenticated"),
        false,
        true,
        assetChannel});
    const ControlConnection::Pointer connection = peer->connection;
    if (!assetChannel && connection && findPeer(connection) && onAuthenticated) {
        QJsonObject authenticatedMessage{
            {QStringLiteral("peer_token"), token},
            {QStringLiteral("udp_endpoint"), udpEndpoint},
            {QStringLiteral("peer_host"), connection->peerHost()},
            {QStringLiteral("peer_name"), peerName->text()},
        };
        onAuthenticated(token, authenticatedMessage);
    }
}

bool ControlServer::writeFrame(
    const PeerHandle& peer,
    const QByteArray& frame,
    bool closeAfterWrite)
{
    const ControlConnection::Pointer connection = peer ? peer->connection : nullptr;
    if (!connection || !connection->isConnected() || frame.isEmpty()) {
        return false;
    }
    const qint64 queued = connection->bytesToWrite();
    stats_.maxQueuedOutputBytes = std::max<quint64>(stats_.maxQueuedOutputBytes, queued);
    const qint64 queueBound = std::min(
        kOutputHighWaterBytes,
        jam2::application::kControlStreamQueueBytes);
    if (queued + frame.size() > queueBound) {
        ++stats_.outputHighWaterRejects;
        // A full bounded queue is backpressure, not a transport failure.
        return false;
    }
    if (!connection->write(frame, closeAfterWrite)) {
        rejectPeer(
            peer,
            QStringLiteral("UDP multiplex control write failed"),
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
    const ControlConnection::Pointer connection = peer ? peer->connection : nullptr;
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

#include "ControlServer.hpp"

#include "ControlProtocol.hpp"

#include <QHostAddress>
#include <QTimer>

#include <algorithm>

using namespace jam2::control_protocol;

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
        if (!(character.isLetterOrNumber() || character == QLatin1Char('.') || character == QLatin1Char('-'))) {
            return false;
        }
    }
    return true;
}

} // namespace

ControlServer::ControlServer(QObject* parent)
    : QObject(parent)
{
    QObject::connect(&server_, &QTcpServer::newConnection, this, [this] { acceptPeer(); });
}

bool ControlServer::listen(quint16 port, const QString& sessionHex, const QString& keyHex)
{
    close();
    sessionHex_ = sessionHex.toLower();
    masterKey_ = decodeHex(keyHex, 16);
    if (decodeHex(sessionHex_, 8).size() != 8 || masterKey_.size() != 16) {
        return false;
    }
    return server_.listen(QHostAddress::Any, port);
}

void ControlServer::close()
{
    for (Peer* peer : peers_) {
        if (peer->socket) {
            peer->socket->disconnect(this);
            peer->socket->abort();
            peer->socket->deleteLater();
        }
        delete peer;
    }
    peers_.clear();
    server_.close();
    masterKey_.clear();
}

void ControlServer::send(const QJsonObject& message)
{
    const QList<Peer*> peers = peers_;
    for (Peer* peer : peers) {
        if (peer && peer->socket && peer->authenticated) {
            const QByteArray frame = encodeAuthenticated(message, peer->sendKey, peer->sendSequence);
            if (!frame.isEmpty() && writeFrame(peer, frame)) {
                ++peer->sendSequence;
            }
        }
    }
}

bool ControlServer::sendTo(const QString& token, const QJsonObject& message, bool closeAfterWrite)
{
    for (Peer* peer : peers_) {
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

bool ControlServer::canQueueTo(const QString& token, qint64 additionalBytes) const
{
    if (additionalBytes < 0 || additionalBytes > kOutputHighWaterBytes) {
        return false;
    }
    for (const Peer* peer : peers_) {
        if (peer && peer->authenticated && peer->token == token && peer->socket) {
            return peer->socket->state() == QAbstractSocket::ConnectedState &&
                peer->socket->bytesToWrite() <= kOutputHighWaterBytes - additionalBytes;
        }
    }
    return false;
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

ControlServer::Peer* ControlServer::findPeer(QTcpSocket* socket)
{
    for (Peer* peer : peers_) {
        if (peer->socket == socket) {
            return peer;
        }
    }
    return nullptr;
}

int ControlServer::authenticatedPeerCount() const
{
    int count = 0;
    for (const Peer* peer : peers_) {
        count += peer && peer->authenticated ? 1 : 0;
    }
    return count;
}

int ControlServer::pendingPeerCount() const
{
    int count = 0;
    for (const Peer* peer : peers_) {
        count += peer && !peer->authenticated ? 1 : 0;
    }
    return count;
}

void ControlServer::acceptPeer()
{
    while (server_.hasPendingConnections()) {
        QTcpSocket* socket = server_.nextPendingConnection();
        if (!socket) {
            return;
        }
        if (pendingPeerCount() >= kMaxPendingPeers ||
            peers_.size() >= kMaxPendingPeers + kMaxAuthenticatedPeers) {
            ++stats_.pendingCapRejects;
            socket->abort();
            socket->deleteLater();
            continue;
        }

        auto* peer = new Peer;
        peer->socket = socket;
        peer->serverNonce = randomNonce();
        peer->authenticationTimer = new QTimer(socket);
        peer->frameTimer = new QTimer(socket);
        peer->authenticationTimer->setSingleShot(true);
        peer->frameTimer->setSingleShot(true);
        socket->setReadBufferSize(kMaxJsonBytes + kAuthenticatedHeaderBytes + 4);
        peers_.push_back(peer);
        ++stats_.acceptedConnections;

        QObject::connect(peer->authenticationTimer, &QTimer::timeout, this, [this, socket] {
            if (Peer* current = findPeer(socket); current && !current->authenticated) {
                ++stats_.authenticationTimeouts;
                rejectPeer(current, QStringLiteral("TCP control authentication timeout"), true);
            }
        });
        QObject::connect(peer->frameTimer, &QTimer::timeout, this, [this, socket] {
            if (Peer* current = findPeer(socket); current && !current->buffer.isEmpty()) {
                ++stats_.frameTimeouts;
                rejectPeer(current, QStringLiteral("TCP control incomplete-frame timeout"), true);
            }
        });
        QObject::connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
            if (Peer* current = findPeer(socket)) {
                readPeer(current);
            }
        });
        QObject::connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            for (qsizetype i = 0; i < peers_.size(); ++i) {
                Peer* current = peers_[i];
                if (current->socket != socket) {
                    continue;
                }
                const QString token = current->token;
                peers_.removeAt(i);
                if (onDisconnected && current->authenticated) {
                    onDisconnected(token);
                }
                if (onState) {
                    onState(QStringLiteral("TCP peer disconnected"));
                }
                socket->deleteLater();
                delete current;
                return;
            }
        });

        peer->authenticationTimer->start(kAuthenticationDeadlineMs);
        const QByteArray challenge = encodeHandshake(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("hello.challenge")},
            {QStringLiteral("version"), 1},
            {QStringLiteral("session"), sessionHex_},
            {QStringLiteral("server_nonce"), encodeHex(peer->serverNonce)},
        });
        if (!writeFrame(peer, challenge)) {
            continue;
        }
        if (onState) {
            onState(QStringLiteral("TCP peer connected; sent bounded auth challenge"));
        }
    }
}

void ControlServer::readPeer(Peer* peer)
{
    if (!peer || !peer->socket) {
        return;
    }
    peer->readScheduled = false;
    peer->buffer += peer->socket->readAll();
    stats_.maxBufferedInputBytes = std::max<quint64>(stats_.maxBufferedInputBytes, peer->buffer.size());

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
            rejectPeer(peer, QStringLiteral("TCP control frame rejected: ") + error, true);
            return;
        }
        peer->frameTimer->stop();
        ++handled;
        ++stats_.framesReceived;

        QJsonObject message;
        if (!peer->authenticated) {
            if (!decodeHandshake(body, message, error)) {
                ++stats_.authenticationRejects;
                rejectPeer(peer, QStringLiteral("TCP control handshake rejected: ") + error);
                return;
            }
            QTcpSocket* const socket = peer->socket;
            handleHandshake(peer, message);
            Peer* const current = findPeer(socket);
            if (!current || socket->state() == QAbstractSocket::UnconnectedState) {
                return;
            }
        } else {
            if (!decodeAuthenticated(body, peer->receiveKey, peer->receiveSequence, message, error)) {
                ++stats_.sequenceOrTagRejects;
                rejectPeer(peer, QStringLiteral("TCP control authenticated frame rejected: ") + error, true);
                return;
            }
            ++peer->receiveSequence;
            if (onMessage) {
                onMessage(peer->token, message);
            }
        }
    }

    if (!peer->buffer.isEmpty() && !peer->readScheduled) {
        peer->readScheduled = true;
        QTimer::singleShot(0, this, [this, socket = peer->socket] {
            if (Peer* current = findPeer(socket)) {
                readPeer(current);
            }
        });
    }
}

void ControlServer::handleHandshake(Peer* peer, const QJsonObject& message)
{
    const QString type = message.value(QStringLiteral("type")).toString();
    const QString session = message.value(QStringLiteral("session")).toString().toLower();
    const QByteArray clientNonce = decodeHex(message.value(QStringLiteral("client_nonce")).toString(), 16);
    const QByteArray clientProof = decodeHex(message.value(QStringLiteral("proof")).toString(), 16);
    QString token = message.value(QStringLiteral("mesh_peer_token")).toString();
    const QString udpEndpoint = message.value(QStringLiteral("udp_endpoint")).toString();
    const bool tokenValid = token.isEmpty() || decodeHex(token, 16).size() == 16;
    if (type != QStringLiteral("hello.proof") ||
        message.value(QStringLiteral("version")).toInt() != 1 ||
        session != sessionHex_ || clientNonce.size() != 16 || clientProof.size() != 16 ||
        !tokenValid || !validUdpEndpointText(udpEndpoint)) {
        ++stats_.authenticationRejects;
        rejectPeer(peer, QStringLiteral("TCP control authentication fields are invalid"));
        return;
    }
    if (authenticatedPeerCount() >= kMaxAuthenticatedPeers) {
        ++stats_.authenticatedCapRejects;
        rejectPeer(peer, QStringLiteral("TCP control authenticated peer cap reached"));
        return;
    }

    if (token.isEmpty()) {
        token = encodeHex(randomNonce());
    }
    for (const Peer* existing : peers_) {
        if (existing != peer && existing && existing->authenticated && existing->token == token) {
            ++stats_.authenticationRejects;
            rejectPeer(peer, QStringLiteral("TCP control peer token is already active"));
            return;
        }
    }
    peer->transcript = makeTranscript(sessionHex_, peer->serverNonce, clientNonce, token, udpEndpoint);
    const QByteArray expectedProof = keyedValue(
        masterKey_,
        QByteArrayLiteral("jam2-control-client-proof"),
        peer->transcript).left(16);
    if (!constantTimeEqual(clientProof, expectedProof)) {
        ++stats_.authenticationRejects;
        rejectPeer(peer, QStringLiteral("TCP control client proof is invalid"));
        return;
    }

    const QByteArray serverProof = keyedValue(
        masterKey_,
        QByteArrayLiteral("jam2-control-server-proof"),
        peer->transcript).left(16);
    const QByteArray response = encodeHandshake(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("hello.ok")},
        {QStringLiteral("version"), 1},
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
    if (onState) {
        onState(QStringLiteral("TCP peer authenticated"));
    }
    if (onAuthenticated) {
        QJsonObject authenticatedMessage{
            {QStringLiteral("mesh_peer_token"), token},
            {QStringLiteral("udp_endpoint"), udpEndpoint},
            {QStringLiteral("tcp_peer_host"), peer->socket->peerAddress().toString()},
        };
        onAuthenticated(token, authenticatedMessage);
    }
}

bool ControlServer::writeFrame(Peer* peer, const QByteArray& frame, bool closeAfterWrite)
{
    if (!peer || !peer->socket || frame.isEmpty()) {
        return false;
    }
    const qint64 queued = peer->socket->bytesToWrite();
    stats_.maxQueuedOutputBytes = std::max<quint64>(stats_.maxQueuedOutputBytes, queued);
    if (queued + frame.size() > kOutputHighWaterBytes) {
        ++stats_.outputHighWaterRejects;
        rejectPeer(peer, QStringLiteral("TCP control output high-water exceeded"), true);
        return false;
    }
    if (peer->socket->write(frame) != frame.size()) {
        rejectPeer(peer, QStringLiteral("TCP control write failed"), true);
        return false;
    }
    ++stats_.framesSent;
    stats_.maxQueuedOutputBytes = std::max<quint64>(
        stats_.maxQueuedOutputBytes,
        peer->socket->bytesToWrite());
    if (closeAfterWrite) {
        peer->socket->disconnectFromHost();
    }
    return true;
}

void ControlServer::rejectPeer(Peer* peer, const QString& reason, bool abort)
{
    if (!peer || !peer->socket) {
        return;
    }
    if (onState) {
        onState(reason);
    }
    if (abort) {
        peer->socket->abort();
        return;
    }
    const QByteArray errorFrame = encodeHandshake(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("hello.error")},
        {QStringLiteral("reason"), reason},
    });
    if (!errorFrame.isEmpty() && peer->socket->bytesToWrite() + errorFrame.size() <= kOutputHighWaterBytes) {
        peer->socket->write(errorFrame);
    }
    peer->socket->disconnectFromHost();
}

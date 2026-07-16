#include "ControlClient.hpp"

#include "ControlProtocol.hpp"

#include <QTimer>

#include <algorithm>

using namespace jam2::control_protocol;

namespace {

constexpr int kSocketRetirementDelayMs = 60000;

} // namespace

ControlClient::ControlClient(QObject* parent)
    : QObject(parent)
{
    authenticationTimer_.setSingleShot(true);
    frameTimer_.setSingleShot(true);
    QObject::connect(&authenticationTimer_, &QTimer::timeout, this, [this] {
        if (!isConnected()) {
            ++stats_.authenticationTimeouts;
            reject(
                QStringLiteral("TCP control authentication timeout"),
                TransportFailure::AuthenticationTimeout,
                true);
        }
    });
    QObject::connect(&frameTimer_, &QTimer::timeout, this, [this] {
        if (!buffer_.isEmpty()) {
            ++stats_.frameTimeouts;
            reject(
                QStringLiteral("TCP control incomplete-frame timeout"),
                TransportFailure::FrameTimeout,
                true);
        }
    });
}

void ControlClient::installSocket(QTcpSocket* socket)
{
    socket_ = socket;
    const QPointer<QTcpSocket> guardedSocket(socket);
    QObject::connect(socket, &QTcpSocket::connected, this, [this, guardedSocket] {
        if (!guardedSocket || socket_ != guardedSocket) {
            return;
        }
        guardedSocket->setReadBufferSize(kMaxJsonBytes + kAuthenticatedHeaderBytes + 4);
        failureReportedForAttempt_ = false;
        authenticationTimer_.start(kAuthenticationDeadlineMs);
        publishEvent(TransportEvent{
            TransportEventType::Connected,
            TransportFailure::None,
            QStringLiteral("TCP connected; waiting for bounded auth challenge")});
    });
    QObject::connect(socket, &QTcpSocket::readyRead, this, [this, guardedSocket] {
        if (guardedSocket && socket_ == guardedSocket) {
            readSocket(guardedSocket.data());
        }
    });
    QObject::connect(socket, &QTcpSocket::errorOccurred, this,
        [this, guardedSocket](QAbstractSocket::SocketError socketError) {
            if (!guardedSocket || socket_ != guardedSocket || manualClose_ ||
                failureReportedForAttempt_ ||
                socketError == QAbstractSocket::RemoteHostClosedError) {
                return;
            }
            TransportFailure failure = TransportFailure::TransportError;
            if (socketError == QAbstractSocket::ConnectionRefusedError) {
                failure = TransportFailure::ConnectionRefused;
            } else if (socketError == QAbstractSocket::HostNotFoundError) {
                failure = TransportFailure::HostNotFound;
            } else if (socketError == QAbstractSocket::NetworkError) {
                failure = TransportFailure::NetworkUnavailable;
            }
            failureReportedForAttempt_ = true;
            publishEvent(TransportEvent{
                TransportEventType::Failure,
                failure,
                QStringLiteral("TCP control transport error: ") + guardedSocket->errorString(),
                true});
        });
    QObject::connect(socket, &QTcpSocket::disconnected, this, [this, guardedSocket] {
        if (!guardedSocket || socket_ != guardedSocket) {
            return;
        }
        const bool wasAuthenticated = handshakeState_ == HandshakeState::Authenticated;
        authenticationTimer_.stop();
        frameTimer_.stop();
        handshakeState_ = HandshakeState::WaitingForChallenge;
        receiveKey_.clear();
        sendKey_.clear();
        socket_.clear();
        if (!manualClose_) {
            publishEvent(TransportEvent{
                TransportEventType::Disconnected,
                TransportFailure::None,
                QStringLiteral("TCP control disconnected"),
                true,
                wasAuthenticated});
        }
        retireSocket(guardedSocket.data());
    });
}

void ControlClient::connectToHost(
    const QString& host,
    quint16 port,
    const QString& sessionHex,
    const QString& keyHex,
    const QString& meshPeerToken,
    const QString& meshUdpEndpoint)
{
    close();
    manualClose_ = false;
    failureReportedForAttempt_ = false;
    sessionHex_ = sessionHex.toLower();
    masterKey_ = decodeHex(keyHex, 16);
    meshPeerToken_ = meshPeerToken.isEmpty() ? randomPeerToken() : meshPeerToken.toLower();
    meshUdpEndpoint_ = meshUdpEndpoint;
    if (decodeHex(sessionHex_, 8).size() != 8 || masterKey_.size() != 16 ||
        !peerIdFromToken(meshPeerToken_).has_value() || meshUdpEndpoint_.size() > 255) {
        reject(
            QStringLiteral("TCP control session, key, token, or endpoint is invalid"),
            TransportFailure::InvalidConfiguration,
            true);
        return;
    }
    handshakeState_ = HandshakeState::WaitingForChallenge;
    receiveSequence_ = 1;
    sendSequence_ = 1;
    publishEvent(TransportEvent{
        TransportEventType::Connecting,
        TransportFailure::None,
        QStringLiteral("TCP control connecting")});
    auto* socket = new QTcpSocket(this);
    installSocket(socket);
    socket->connectToHost(host, port);
}

void ControlClient::close()
{
    manualClose_ = true;
    authenticationTimer_.stop();
    frameTimer_.stop();
    buffer_.clear();
    serverNonce_.clear();
    clientNonce_.clear();
    transcript_.clear();
    receiveKey_.clear();
    sendKey_.clear();
    handshakeState_ = HandshakeState::WaitingForChallenge;
    const QPointer<QTcpSocket> socket = socket_;
    socket_.clear();
    if (socket) {
        retireSocket(socket.data());
    }
}

bool ControlClient::send(const QJsonObject& message)
{
    if (!isConnected()) {
        return false;
    }
    const QByteArray frame = encodeAuthenticated(message, sendKey_, sendSequence_);
    if (!frame.isEmpty() && writeFrame(frame)) {
        ++sendSequence_;
        return true;
    }
    return false;
}

bool ControlClient::sendBinary(const QByteArray& payload)
{
    if (!isConnected()) {
        return false;
    }
    const QByteArray frame = encodeAuthenticatedBinary(payload, sendKey_, sendSequence_);
    if (!frame.isEmpty() && writeFrame(frame)) {
        ++sendSequence_;
        return true;
    }
    return false;
}

bool ControlClient::canQueue(qint64 additionalBytes) const
{
    return isConnected() && additionalBytes >= 0 && additionalBytes <= kOutputHighWaterBytes &&
        socket_->bytesToWrite() <= kOutputHighWaterBytes - additionalBytes;
}

bool ControlClient::isConnected() const
{
    return socket_ && socket_->state() == QTcpSocket::ConnectedState &&
        handshakeState_ == HandshakeState::Authenticated;
}

void ControlClient::readSocket(QTcpSocket* expectedSocket)
{
    const QPointer<QTcpSocket> socket = socket_;
    if (!socket || socket != expectedSocket) {
        return;
    }
    readScheduled_ = false;
    buffer_ += socket->readAll();
    stats_.maxBufferedInputBytes = std::max<quint64>(stats_.maxBufferedInputBytes, buffer_.size());

    int handled = 0;
    while (handled < kFramesPerTurn) {
        QByteArray body;
        QString error;
        const TakeFrameResult result = takeFrame(buffer_, body, error);
        if (result == TakeFrameResult::NeedMore) {
            if (buffer_.isEmpty()) {
                frameTimer_.stop();
            } else if (!frameTimer_.isActive()) {
                frameTimer_.start(kIncompleteFrameDeadlineMs);
            }
            return;
        }
        if (result == TakeFrameResult::Invalid) {
            ++stats_.frameRejects;
            reject(
                QStringLiteral("TCP control frame rejected: ") + error,
                TransportFailure::FrameRejected,
                true);
            return;
        }
        frameTimer_.stop();
        ++handled;
        ++stats_.framesReceived;

        QJsonObject message;
        if (handshakeState_ != HandshakeState::Authenticated) {
            if (!decodeHandshake(body, message, error)) {
                ++stats_.authenticationRejects;
                reject(
                    QStringLiteral("TCP control handshake rejected: ") + error,
                    TransportFailure::AuthenticationRejected,
                    true);
                return;
            }
            handleHandshake(message);
            if (!socket_ || socket_ != socket ||
                socket->state() == QAbstractSocket::UnconnectedState) {
                return;
            }
        } else {
            AuthenticatedPayload payload;
            if (!decodeAuthenticated(body, receiveKey_, receiveSequence_, payload, error)) {
                ++stats_.sequenceOrTagRejects;
                reject(
                    QStringLiteral("TCP control authenticated frame rejected: ") + error,
                    TransportFailure::AuthenticatedFrameRejected,
                    true);
                return;
            }
            ++receiveSequence_;
            if (payload.type == AuthenticatedPayloadType::Json) {
                if (onMessage) {
                    onMessage(payload.message);
                }
            } else if (onBinaryMessage) {
                onBinaryMessage(payload.binary);
            }
            if (!socket_ || socket_ != socket) {
                return;
            }
        }
    }

    if (!buffer_.isEmpty() && !readScheduled_) {
        readScheduled_ = true;
        QTimer::singleShot(0, this, [this, socket] {
            if (socket && socket_ == socket &&
                socket->state() != QAbstractSocket::UnconnectedState) {
                readSocket(socket.data());
            }
        });
    }
}

void ControlClient::handleHandshake(const QJsonObject& message)
{
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type == QStringLiteral("hello.error")) {
        ++stats_.authenticationRejects;
        reject(QStringLiteral("TCP auth failed: ") +
            message.value(QStringLiteral("reason")).toString(QStringLiteral("auth failed")),
            TransportFailure::AuthenticationRejected);
        return;
    }

    if (handshakeState_ == HandshakeState::WaitingForChallenge) {
        serverNonce_ = decodeHex(message.value(QStringLiteral("server_nonce")).toString(), 16);
        if (type != QStringLiteral("hello.challenge") ||
            message.value(QStringLiteral("version")).toInt() != kControlProtocolVersion ||
            message.value(QStringLiteral("session")).toString().toLower() != sessionHex_ ||
            serverNonce_.size() != 16) {
            ++stats_.authenticationRejects;
            reject(
                QStringLiteral("TCP control server challenge is invalid"),
                TransportFailure::AuthenticationRejected,
                true);
            return;
        }
        clientNonce_ = randomNonce();
        transcript_ = makeTranscript(
            sessionHex_,
            serverNonce_,
            clientNonce_,
            meshPeerToken_,
            meshUdpEndpoint_);
        const QByteArray proof = keyedValue(
            masterKey_,
            QByteArrayLiteral("jam2-control-client-proof"),
            transcript_).left(16);
        const QByteArray response = encodeHandshake(QJsonObject{
            {QStringLiteral("type"), QStringLiteral("hello.proof")},
            {QStringLiteral("version"), kControlProtocolVersion},
            {QStringLiteral("session"), sessionHex_},
            {QStringLiteral("client_nonce"), encodeHex(clientNonce_)},
            {QStringLiteral("peer_token"), meshPeerToken_},
            {QStringLiteral("udp_endpoint"), meshUdpEndpoint_},
            {QStringLiteral("proof"), encodeHex(proof)},
        });
        if (!writeFrame(response)) {
            return;
        }
        handshakeState_ = HandshakeState::WaitingForServerProof;
        publishEvent(TransportEvent{
            TransportEventType::ProofSent,
            TransportFailure::None,
            QStringLiteral("TCP connected; sent challenge proof")});
        return;
    }

    if (handshakeState_ != HandshakeState::WaitingForServerProof ||
        type != QStringLiteral("hello.ok") ||
        message.value(QStringLiteral("version")).toInt() != kControlProtocolVersion ||
        message.value(QStringLiteral("peer_token")).toString() != meshPeerToken_) {
        ++stats_.authenticationRejects;
        reject(
            QStringLiteral("TCP control server proof state is invalid"),
            TransportFailure::AuthenticationRejected,
            true);
        return;
    }
    const QByteArray receivedProof = decodeHex(message.value(QStringLiteral("proof")).toString(), 16);
    const QByteArray expectedProof = keyedValue(
        masterKey_,
        QByteArrayLiteral("jam2-control-server-proof"),
        transcript_).left(16);
    if (!constantTimeEqual(receivedProof, expectedProof)) {
        ++stats_.authenticationRejects;
        reject(
            QStringLiteral("TCP control server proof is invalid"),
            TransportFailure::AuthenticationRejected,
            true);
        return;
    }

    receiveKey_ = keyedValue(masterKey_, QByteArrayLiteral("jam2-control-s2c"), transcript_);
    sendKey_ = keyedValue(masterKey_, QByteArrayLiteral("jam2-control-c2s"), transcript_);
    handshakeState_ = HandshakeState::Authenticated;
    authenticationTimer_.stop();
    publishEvent(TransportEvent{
        TransportEventType::Authenticated,
        TransportFailure::None,
        QStringLiteral("TCP control authenticated"),
        false,
        true});
}

bool ControlClient::writeFrame(const QByteArray& frame)
{
    const QPointer<QTcpSocket> socket = socket_;
    if (!socket || frame.isEmpty() ||
        socket->state() == QAbstractSocket::UnconnectedState) {
        return false;
    }
    const qint64 queued = socket->bytesToWrite();
    stats_.maxQueuedOutputBytes = std::max<quint64>(stats_.maxQueuedOutputBytes, queued);
    if (queued + frame.size() > kOutputHighWaterBytes) {
        ++stats_.outputHighWaterRejects;
        reject(
            QStringLiteral("TCP control output high-water exceeded"),
            TransportFailure::OutputHighWater,
            true,
            true);
        return false;
    }
    if (socket->write(frame) != frame.size()) {
        reject(
            QStringLiteral("TCP control write failed"),
            TransportFailure::WriteFailed,
            true,
            true);
        return false;
    }
    if (!socket || socket_ != socket ||
        socket->state() == QAbstractSocket::UnconnectedState) {
        return false;
    }
    ++stats_.framesSent;
    stats_.maxQueuedOutputBytes = std::max<quint64>(
        stats_.maxQueuedOutputBytes,
        socket->bytesToWrite());
    return true;
}

void ControlClient::reject(
    const QString& reason,
    TransportFailure failure,
    bool abort,
    bool retryable)
{
    const QPointer<QTcpSocket> socket = socket_;
    failureReportedForAttempt_ = true;
    publishEvent(TransportEvent{
        TransportEventType::Failure,
        failure,
        reason,
        retryable,
        handshakeState_ == HandshakeState::Authenticated});
    if (!socket || socket_ != socket) {
        return;
    }
    // Both protocol rejection modes retire this generation. A graceful close
    // avoids tearing down Cocoa's native notifier while a delivery for it may
    // already be queued; the generation is no longer used either way.
    (void)abort;
    socket_.clear();
    retireSocket(socket.data());
}

void ControlClient::retireSocket(QTcpSocket* socket)
{
    if (!socket) {
        return;
    }
    purgeRetiredSockets();
    for (const QPointer<QTcpSocket>& retired : retiredSockets_) {
        if (retired == socket) {
            return;
        }
    }

    socket->disconnect(this);
    if (socket->state() != QAbstractSocket::UnconnectedState) {
        socket->disconnectFromHost();
    }
    const QPointer<QTcpSocket> guardedSocket(socket);
    retiredSockets_.push_back(guardedSocket);
    stats_.retiredSockets = static_cast<quint64>(retiredSockets_.size());
    stats_.retiredSocketHighWater = std::max(
        stats_.retiredSocketHighWater,
        stats_.retiredSockets);
    QTimer::singleShot(kSocketRetirementDelayMs, this, [this, guardedSocket] {
        completeSocketRetirement(guardedSocket);
    });
}

void ControlClient::completeSocketRetirement(const QPointer<QTcpSocket>& socket)
{
    if (socket && socket->state() != QAbstractSocket::UnconnectedState) {
        socket->disconnectFromHost();
        QTimer::singleShot(1000, this, [this, socket] {
            completeSocketRetirement(socket);
        });
        return;
    }
    if (socket) {
        socket->deleteLater();
    }
    QTimer::singleShot(0, this, [this] { purgeRetiredSockets(); });
}

void ControlClient::purgeRetiredSockets()
{
    retiredSockets_.removeIf([](const QPointer<QTcpSocket>& socket) {
        return socket.isNull();
    });
    stats_.retiredSockets = static_cast<quint64>(retiredSockets_.size());
}

void ControlClient::publishEvent(TransportEvent event)
{
    if (onEvent) {
        onEvent(event);
    }
}

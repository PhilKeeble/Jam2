#include "ControlClient.hpp"

#include "ControlProtocol.hpp"

#include <QTimer>

#include <algorithm>
#include <memory>

using namespace jam2::control_protocol;
using jam2::application::NativeTcpConnection;
using jam2::application::NativeTcpError;

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

ControlClient::~ControlClient()
{
    close();
}

void ControlClient::installConnection(
    const NativeTcpConnection::Pointer& connection,
    quint64 generation)
{
    if (!connection || generation != connectionGeneration_ || manualClose_) {
        if (connection) {
            connection->close();
        }
        return;
    }
    connection_ = connection;
    ++stats_.completedConnections;
    const std::weak_ptr<NativeTcpConnection> weakConnection = connection;
    connection->start(
        this,
        [this, weakConnection, generation](const QByteArray& bytes) {
            const auto current = weakConnection.lock();
            if (current && generation == connectionGeneration_ && connection_ == current) {
                readConnection(current, bytes);
            }
        },
        [this, weakConnection, generation](const QString& detail) {
            const auto current = weakConnection.lock();
            if (current && generation == connectionGeneration_ && connection_ == current) {
                connectionClosed(current, detail);
            }
        });
    failureReportedForAttempt_ = false;
    authenticationTimer_.start(kAuthenticationDeadlineMs);
    publishEvent(TransportEvent{
        TransportEventType::Connected,
        TransportFailure::None,
        QStringLiteral("TCP connected; waiting for bounded auth challenge")});
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
    const quint64 generation = ++connectionGeneration_;
    ++stats_.connectionAttempts;
    publishEvent(TransportEvent{
        TransportEventType::Connecting,
        TransportFailure::None,
        QStringLiteral("TCP control connecting")});
    connector_.connectToHost(
        host,
        port,
        this,
        [this, generation](const NativeTcpConnection::Pointer& connection) {
            installConnection(connection, generation);
        },
        [this, generation](const NativeTcpError& error) {
            if (generation != connectionGeneration_ || manualClose_ || failureReportedForAttempt_) {
                return;
            }
            failureReportedForAttempt_ = true;
            TransportFailure failure = TransportFailure::TransportError;
            switch (error.code) {
            case NativeTcpError::Code::HostNotFound:
                failure = TransportFailure::HostNotFound;
                break;
            case NativeTcpError::Code::ConnectionRefused:
                failure = TransportFailure::ConnectionRefused;
                break;
            case NativeTcpError::Code::NetworkUnavailable:
                failure = TransportFailure::NetworkUnavailable;
                break;
            case NativeTcpError::Code::None:
            case NativeTcpError::Code::Timeout:
            case NativeTcpError::Code::Transport:
                break;
            }
            publishEvent(TransportEvent{
                TransportEventType::Failure,
                failure,
                error.message,
                true});
        });
}

void ControlClient::close()
{
    manualClose_ = true;
    ++connectionGeneration_;
    authenticationTimer_.stop();
    frameTimer_.stop();
    connector_.cancel();
    buffer_.clear();
    serverNonce_.clear();
    clientNonce_.clear();
    transcript_.clear();
    receiveKey_.clear();
    sendKey_.clear();
    handshakeState_ = HandshakeState::WaitingForChallenge;
    const NativeTcpConnection::Pointer connection = std::move(connection_);
    if (connection) {
        connection->close();
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
        connection_->bytesToWrite() <= kOutputHighWaterBytes - additionalBytes;
}

bool ControlClient::isConnected() const
{
    return connection_ && connection_->isConnected() &&
        handshakeState_ == HandshakeState::Authenticated;
}

void ControlClient::readConnection(
    const NativeTcpConnection::Pointer& connection,
    const QByteArray& bytes)
{
    if (!connection || connection_ != connection) {
        return;
    }
    readScheduled_ = false;
    buffer_ += bytes;
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
            if (!connection_ || connection_ != connection || !connection->isConnected()) {
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
            if (!connection_ || connection_ != connection) {
                return;
            }
        }
    }

    if (!buffer_.isEmpty() && !readScheduled_) {
        readScheduled_ = true;
        const std::weak_ptr<NativeTcpConnection> weakConnection = connection;
        QTimer::singleShot(0, this, [this, weakConnection] {
            const auto current = weakConnection.lock();
            if (current && connection_ == current && current->isConnected()) {
                readConnection(current, {});
            }
        });
    }
}

void ControlClient::connectionClosed(
    const NativeTcpConnection::Pointer& connection,
    const QString& detail)
{
    if (!connection || connection_ != connection) {
        return;
    }
    const bool wasAuthenticated = handshakeState_ == HandshakeState::Authenticated;
    authenticationTimer_.stop();
    frameTimer_.stop();
    handshakeState_ = HandshakeState::WaitingForChallenge;
    receiveKey_.clear();
    sendKey_.clear();
    connection_.reset();
    ++stats_.disconnectedConnections;
    if (!manualClose_) {
        if (!detail.isEmpty() && !failureReportedForAttempt_) {
            failureReportedForAttempt_ = true;
            publishEvent(TransportEvent{
                TransportEventType::Failure,
                TransportFailure::TransportError,
                detail,
                true,
                wasAuthenticated});
        }
        publishEvent(TransportEvent{
            TransportEventType::Disconnected,
            TransportFailure::None,
            QStringLiteral("TCP control disconnected"),
            true,
            wasAuthenticated});
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
            sessionHex_, serverNonce_, clientNonce_, meshPeerToken_, meshUdpEndpoint_);
        const QByteArray proof = keyedValue(
            masterKey_, QByteArrayLiteral("jam2-control-client-proof"), transcript_).left(16);
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
        masterKey_, QByteArrayLiteral("jam2-control-server-proof"), transcript_).left(16);
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
    const NativeTcpConnection::Pointer connection = connection_;
    if (!connection || frame.isEmpty() || !connection->isConnected()) {
        return false;
    }
    const qint64 queued = connection->bytesToWrite();
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
    if (!connection->write(frame)) {
        reject(
            QStringLiteral("TCP control write failed"),
            TransportFailure::WriteFailed,
            true,
            true);
        return false;
    }
    ++stats_.framesSent;
    stats_.maxQueuedOutputBytes = std::max<quint64>(
        stats_.maxQueuedOutputBytes, connection->bytesToWrite());
    return true;
}

void ControlClient::reject(
    const QString& reason,
    TransportFailure failure,
    bool abort,
    bool retryable)
{
    (void)abort;
    const NativeTcpConnection::Pointer connection = connection_;
    failureReportedForAttempt_ = true;
    publishEvent(TransportEvent{
        TransportEventType::Failure,
        failure,
        reason,
        retryable,
        handshakeState_ == HandshakeState::Authenticated});
    if (!connection || connection_ != connection) {
        return;
    }
    connection_.reset();
    connection->close();
}

void ControlClient::publishEvent(TransportEvent event)
{
    if (onEvent) {
        onEvent(event);
    }
}

#include "ControlClient.hpp"

#include "ControlProtocol.hpp"

#include <QTimer>

#include <algorithm>
#include <memory>

using namespace jam2::control_protocol;
using jam2::application::ControlConnection;

ControlClient::ControlClient(QObject* parent)
    : QObject(parent)
{
    authenticationTimer_.setSingleShot(true);
    frameTimer_.setSingleShot(true);
    frameTimer_.setTimerType(Qt::PreciseTimer);
    QObject::connect(&authenticationTimer_, &QTimer::timeout, this, [this] {
        if (!isConnected()) {
            ++stats_.authenticationTimeouts;
            reject(
                QStringLiteral("UDP multiplex control authentication timeout"),
                TransportFailure::AuthenticationTimeout,
                true,
                true);
        }
    });
    QObject::connect(&frameTimer_, &QTimer::timeout, this, [this] {
        if (!buffer_.isEmpty()) {
            ++stats_.frameTimeouts;
            reject(
                QStringLiteral("UDP multiplex control incomplete-frame timeout"),
                TransportFailure::FrameTimeout,
                true,
                true);
        }
    });
}

ControlClient::~ControlClient()
{
    close();
}

void ControlClient::installConnection(
    const ControlConnection::Pointer& connection,
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
    const std::weak_ptr<ControlConnection> weakConnection = connection;
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
    frameClock_.start();
    frameDeadline_.clear();
    authenticationTimer_.start(connection->authenticationDeadlineMs());
    publishEvent(TransportEvent{
        TransportEventType::Connected,
        TransportFailure::None,
        QStringLiteral("UDP multiplex connected; waiting for bounded auth challenge")});
}

bool ControlClient::prepareConnection(
    const QString& sessionHex, const QString& keyHex,
    const QString& meshPeerToken, const QString& meshUdpEndpoint, Channel channel)
{
    close();
    manualClose_ = false;
    failureReportedForAttempt_ = false;
    sessionHex_ = sessionHex.toLower();
    masterKey_ = decodeHex(keyHex, 16);
    meshPeerToken_ = meshPeerToken.toLower();
    meshUdpEndpoint_ = channel == Channel::Asset ? QString{} : jam2::application::binary_wire::Endpoint::fromText(meshUdpEndpoint).text();
    channel_ = channel;
    if (decodeHex(sessionHex_, 8).size() != 8 || masterKey_.size() != 16 ||
        !peerIdFromToken(meshPeerToken_).has_value() || !localName_.valid() ||
        (channel == Channel::Control && !jam2::application::binary_wire::Endpoint::fromText(meshUdpEndpoint_).valid())) {
        reject(
            QStringLiteral("UDP multiplex control session, key, token, or endpoint is invalid"),
            TransportFailure::InvalidConfiguration,
            true);
        return false;
    }
    handshakeState_ = HandshakeState::WaitingForChallenge;
    return true;
}

ControlClient::Stats ControlClient::stats() const
{
    auto result = stats_;
    if (wire_) jam2::application::control_wire::accumulate(result.wire, wire_->stats());
    return result;
}

void ControlClient::connectViaTransport(
    const ControlConnection::Pointer& connection,
    const QString& sessionHex, const QString& keyHex,
    const QString& meshPeerToken, const QString& meshUdpEndpoint, Channel channel)
{
    if (!prepareConnection(sessionHex, keyHex, meshPeerToken, meshUdpEndpoint, channel)) return;
    ++stats_.connectionAttempts;
    installConnection(connection, ++connectionGeneration_);
}

void ControlClient::retireWire()
{
    if (!wire_) return;
    auto value = wire_->stats();
    value.unresolvedReceipts += value.pendingReceipts;
    value.pendingReceipts = 0; value.queuedBytes = 0;
    jam2::application::control_wire::accumulate(stats_.wire, value);
    wire_.reset();
}
void ControlClient::close()
{
    retireWire();
    manualClose_ = true;
    ++connectionGeneration_;
    authenticationTimer_.stop();
    frameTimer_.stop();
    buffer_.clear();
    serverNonce_.clear();
    clientNonce_.clear();
    transcript_.clear();
    receiveKey_.clear();
    sendKey_.clear();
    handshakeState_ = HandshakeState::WaitingForChallenge;
    const ControlConnection::Pointer connection = std::move(connection_);
    if (connection) {
        connection->close();
    }
}

bool ControlClient::send(const QJsonObject& message)
{
    return isConnected() && wire_ && wire_->send(message);
}
bool ControlClient::sendBinary(const QByteArray& payload)
{
    return isConnected() && wire_ && (channel_==Channel::Asset
        ? wire_->sendAsset(payload) : wire_->sendBinary(payload));
}
bool ControlClient::canQueue(qint64 additionalBytes) const
{
    return isConnected() && wire_ && wire_->canQueue(additionalBytes);
}

bool ControlClient::isConnected() const
{
    return connection_ && connection_->isConnected() &&
        handshakeState_ == HandshakeState::Authenticated;
}

void ControlClient::readConnection(
    const ControlConnection::Pointer& connection,
    const QByteArray& bytes)
{
    if (!connection || connection_ != connection) {
        return;
    }
    readScheduled_ = false;
    buffer_ += bytes;
    if (!bytes.isEmpty()) frameDeadline_.received(frameClock_.elapsed());
    stats_.maxBufferedInputBytes = std::max<quint64>(stats_.maxBufferedInputBytes, buffer_.size());
    stats_.maxBufferedInputBytes = std::max<quint64>(
        stats_.maxBufferedInputBytes,
        static_cast<quint64>(std::max<qint64>(0, connection->maxPendingReadBytes())));

    int handled = 0;
    while (handled < kFramesPerTurn) {
        QByteArray body;
        QString error;
        const TakeFrameResult result = takeFrame(buffer_, body, error, handshakeState_ == HandshakeState::Authenticated ? kMaximumRecordBodyBytes : kMaximumHandshakeBodyBytes);
        if (result == TakeFrameResult::NeedMore) {
            if (buffer_.isEmpty()) {
                frameTimer_.stop();
            } else {
                if (frameDeadline_.began < 0) frameDeadline_.received(frameClock_.elapsed());
                frameTimer_.start(frameDeadline_.remaining(frameClock_.elapsed()));
            }
            return;
        }
        if (result == TakeFrameResult::Invalid) {
            ++stats_.frameRejects;
            reject(
                QStringLiteral("UDP multiplex control frame rejected: ") + error,
                TransportFailure::FrameRejected,
                true);
            return;
        }
        frameTimer_.stop();
        frameDeadline_.clear();
        ++handled;
        ++stats_.framesReceived;

        Handshake message;
        if (handshakeState_ != HandshakeState::Authenticated) {
            if (!decodeHandshake(body, message, error)) {
                ++stats_.authenticationRejects;
                reject(
                    QStringLiteral("UDP multiplex control handshake rejected: ") + error,
                    TransportFailure::AuthenticationRejected,
                    true);
                return;
            }
            handleHandshake(message);
            if (!connection_ || connection_ != connection || !connection->isConnected()) {
                return;
            }
        } else if (const auto wire = wire_) {
            if (!wire->receive(body, error)) {
                ++stats_.sequenceOrTagRejects;
                reject(QStringLiteral("Typed control rejected: ") + error,
                    TransportFailure::AuthenticatedFrameRejected, true);
                return;
            }
            if (!connection_ || connection_ != connection) return;
        }
    }

    if (!buffer_.isEmpty() && !readScheduled_) {
        readScheduled_ = true;
        const std::weak_ptr<ControlConnection> weakConnection = connection;
        QTimer::singleShot(0, this, [this, weakConnection] {
            const auto current = weakConnection.lock();
            if (current && connection_ == current && current->isConnected()) {
                readConnection(current, {});
            }
        });
    }
}

void ControlClient::connectionClosed(
    const ControlConnection::Pointer& connection,
    const QString& detail)
{
    if (!connection || connection_ != connection) {
        return;
    }
    retireWire();
    const bool wasAuthenticated = handshakeState_ == HandshakeState::Authenticated;
    stats_.maxBufferedInputBytes = std::max<quint64>(
        stats_.maxBufferedInputBytes,
        static_cast<quint64>(std::max<qint64>(0, connection->maxPendingReadBytes())));
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
            QStringLiteral("UDP multiplex control disconnected"),
            true,
            wasAuthenticated});
    }
}

void ControlClient::handleHandshake(const Handshake& message)
{
    if (const auto* rejected = std::get_if<Rejected>(&message)) {
        ++stats_.authenticationRejects;
        reject(QStringLiteral("UDP multiplex auth failed: ") + rejectionText(rejected->reason), TransportFailure::AuthenticationRejected);
        return;
    }
    if (handshakeState_ == HandshakeState::WaitingForChallenge) {
        const auto* challenge = std::get_if<Challenge>(&message);
        if (!challenge || challenge->session != sessionHex_) {
            ++stats_.authenticationRejects;
            reject(QStringLiteral("UDP multiplex control server challenge is invalid"), TransportFailure::AuthenticationRejected, true);
            return;
        }
        serverNonce_ = challenge->nonce;
        clientNonce_ = randomNonce();
        const QString channel = channel_ == Channel::Asset
            ? QStringLiteral("asset") : QStringLiteral("control");
        transcript_ = makeTranscript(
            sessionHex_, serverNonce_, clientNonce_, meshPeerToken_, meshUdpEndpoint_, channel, localName_.text());
        const QByteArray clientProofDomain = channel_ == Channel::Asset
            ? QByteArrayLiteral("jam2-asset-client-proof")
            : QByteArrayLiteral("jam2-control-client-proof");
        const QByteArray proof = keyedValue(
            masterKey_, clientProofDomain, transcript_).left(16);
        Proof proofMessage;
        proofMessage.nonce = clientNonce_; proofMessage.peer = meshPeerToken_; proofMessage.proof = proof;
        proofMessage.channel = channel_ == Channel::Asset ? HandshakeChannel::Asset : HandshakeChannel::Control;
        if (channel_ == Channel::Control) {
            proofMessage.endpoint = jam2::application::binary_wire::Endpoint::fromText(meshUdpEndpoint_);
            proofMessage.name = localName_;
        }
        const QByteArray response = encodeHandshake(proofMessage);
        if (!writeFrame(response)) {
            return;
        }
        handshakeState_ = HandshakeState::WaitingForServerProof;
        publishEvent(TransportEvent{
            TransportEventType::ProofSent,
            TransportFailure::None,
            QStringLiteral("UDP multiplex connected; sent challenge proof")});
        return;
    }

    const auto* accepted = std::get_if<Accepted>(&message);
    if (handshakeState_ != HandshakeState::WaitingForServerProof || !accepted) {
        ++stats_.authenticationRejects;
        reject(QStringLiteral("UDP multiplex control server proof state is invalid"), TransportFailure::AuthenticationRejected, true);
        return;
    }
    const QByteArray receivedProof = accepted->proof;
    const QByteArray serverProofDomain = channel_ == Channel::Asset
        ? QByteArrayLiteral("jam2-asset-server-proof")
        : QByteArrayLiteral("jam2-control-server-proof");
    const QByteArray expectedProof = keyedValue(
        masterKey_, serverProofDomain, transcript_).left(16);
    if (!constantTimeEqual(receivedProof, expectedProof)) {
        ++stats_.authenticationRejects;
        reject(
            QStringLiteral("UDP multiplex control server proof is invalid"),
            TransportFailure::AuthenticationRejected,
            true);
        return;
    }

    receiveKey_ = keyedValue(
        masterKey_,
        channel_ == Channel::Asset ? QByteArrayLiteral("jam2-asset-s2c")
                                   : QByteArrayLiteral("jam2-control-s2c"),
        transcript_);
    sendKey_ = keyedValue(
        masterKey_,
        channel_ == Channel::Asset ? QByteArrayLiteral("jam2-asset-c2s")
                                   : QByteArrayLiteral("jam2-control-c2s"),
        transcript_);
    {
        const std::weak_ptr<ControlConnection> weak = connection_;
        wire_ = std::make_shared<jam2::application::control_wire::Channel>(sendKey_, receiveKey_,
            [weak](const QByteArray& bytes) { const auto connection = weak.lock(); return connection && connection->isConnected() && connection->write(bytes); },
            [weak] { const auto connection = weak.lock(); return connection ? connection->bytesToWrite() : qint64{256 * 1024}; });
        wire_->onMessage = [this](const QJsonObject& value) { if (onMessage) onMessage(value); };
        wire_->validateMessage = [this](const QJsonObject& value) {
            return validateMessage ? validateMessage(value) : jam2::application::control_wire::DispatchDecision{};
        };
        wire_->onAsset = [this](const QByteArray& value) { if (onBinaryMessage) onBinaryMessage(value); };
        wire_->onFailure = [this](const QString& error) {
            reject(error, TransportFailure::AuthenticatedFrameRejected, true, true);
        };
        if (channel_ == Channel::Control && connection_->hasIndependentControl()) {
            wire_->enableIndependent(
                [weak](const QByteArray& bytes) { const auto c = weak.lock(); return c && c->writeIndependent(bytes); },
                [weak] { const auto c = weak.lock(); return c ? c->independentBytesToWrite() : qint64{16 * 1024}; });
            const std::weak_ptr<jam2::application::control_wire::Channel> wire = wire_;
            connection_->startIndependent(this, [this, wire](const QByteArray& bytes) {
                if (const auto channel = wire.lock()) { QString error;
                    if (!channel->receiveIndependent(bytes, error)) reject(error, TransportFailure::AuthenticatedFrameRejected, true, true);
                }
            });
        }
    }
    handshakeState_ = HandshakeState::Authenticated;
    authenticationTimer_.stop();
    publishEvent(TransportEvent{
        TransportEventType::Authenticated,
        TransportFailure::None,
        QStringLiteral("UDP multiplex control authenticated"),
        false,
        true});
}

bool ControlClient::writeFrame(const QByteArray& frame)
{
    const ControlConnection::Pointer connection = connection_;
    if (!connection || frame.isEmpty() || !connection->isConnected()) {
        return false;
    }
    const qint64 queued = connection->bytesToWrite();
    stats_.maxQueuedOutputBytes = std::max<quint64>(stats_.maxQueuedOutputBytes, queued);
    const qint64 queueBound = std::min(
        kOutputHighWaterBytes,
        jam2::application::kControlStreamQueueBytes);
    if (queued + frame.size() > queueBound) {
        ++stats_.outputHighWaterRejects;
        // Queue saturation is temporary backpressure.  It must never tear
        // down either the control session or the independent asset channel.
        return false;
    }
    if (!connection->write(frame)) {
        reject(
            QStringLiteral("UDP multiplex control write failed"),
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
    const ControlConnection::Pointer connection = connection_;
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
    retireWire();
    connection_.reset();
    connection->close();
}

void ControlClient::publishEvent(TransportEvent event)
{
    if (onEvent) {
        onEvent(event);
    }
}

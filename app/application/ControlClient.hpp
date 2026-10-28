#pragma once

#include "ControlProtocol.hpp"
#include "ControlConnection.hpp"
#include "ControlWire.hpp"
#include "PeerName.hpp"

#include <QJsonObject>
#include <QElapsedTimer>
#include <QTimer>

#include <functional>

class ControlClient : public QObject {
public:
    enum class Channel {
        Control,
        Asset,
    };

    struct Stats {
        quint64 authenticationRejects = 0;
        quint64 authenticationTimeouts = 0;
        quint64 frameTimeouts = 0;
        quint64 frameRejects = 0;
        quint64 sequenceOrTagRejects = 0;
        quint64 outputHighWaterRejects = 0;
        quint64 framesReceived = 0;
        quint64 framesSent = 0;
        quint64 maxBufferedInputBytes = 0;
        quint64 maxQueuedOutputBytes = 0;
        quint64 connectionAttempts = 0;
        quint64 completedConnections = 0;
        quint64 disconnectedConnections = 0;
        jam2::application::control_wire::Stats wire;
    };

    explicit ControlClient(QObject* parent = nullptr);
    void setPeerName(const jam2::application::PeerName& name) { localName_ = name; }
    ~ControlClient() override;

    void connectViaTransport(
        const jam2::application::ControlConnection::Pointer& connection,
        const QString& sessionHex, const QString& keyHex,
        const QString& meshPeerToken, const QString& meshUdpEndpoint,
        Channel channel = Channel::Control);
    void close();
    bool send(const QJsonObject& message);
    bool sendBinary(const QByteArray& payload);
    bool canQueue(qint64 additionalBytes) const;
    bool isConnected() const;
    Stats stats() const;

    std::function<void(const QJsonObject&)> onMessage;
    std::function<jam2::application::control_wire::DispatchDecision(const QJsonObject&)> validateMessage;
    std::function<void(const QByteArray&)> onBinaryMessage;
    std::function<void(const jam2::control_protocol::TransportEvent&)> onEvent;

private:
    enum class HandshakeState {
        WaitingForChallenge,
        WaitingForServerProof,
        Authenticated,
    };

    bool prepareConnection(const QString& sessionHex, const QString& keyHex,
        const QString& meshPeerToken, const QString& meshUdpEndpoint, Channel channel);
    void installConnection(
        const jam2::application::ControlConnection::Pointer& connection,
        quint64 generation);
    void readConnection(
        const jam2::application::ControlConnection::Pointer& connection,
        const QByteArray& bytes);
    void connectionClosed(
        const jam2::application::ControlConnection::Pointer& connection,
        const QString& detail);
    void handleHandshake(const jam2::control_protocol::Handshake& message);
    void retireWire();
    bool writeFrame(const QByteArray& frame);
    void reject(
        const QString& reason,
        jam2::control_protocol::TransportFailure failure,
        bool abort = false,
        bool retryable = false);
    void publishEvent(jam2::control_protocol::TransportEvent event);

    jam2::application::ControlConnection::Pointer connection_;
    std::shared_ptr<jam2::application::control_wire::Channel> wire_;
    quint64 connectionGeneration_ = 0;
    QTimer authenticationTimer_;
    QTimer frameTimer_;
    QElapsedTimer frameClock_;
    jam2::application::ControlFrameDeadline frameDeadline_;
    QByteArray buffer_;
    QByteArray masterKey_;
    QByteArray serverNonce_;
    QByteArray clientNonce_;
    QByteArray transcript_;
    QByteArray receiveKey_;
    QByteArray sendKey_;
    QString sessionHex_;
    QString meshPeerToken_;
    jam2::application::PeerName localName_;
    QString meshUdpEndpoint_;
    Channel channel_ = Channel::Control;
    HandshakeState handshakeState_ = HandshakeState::WaitingForChallenge;
    bool readScheduled_ = false;
    bool manualClose_ = true;
    bool failureReportedForAttempt_ = false;
    Stats stats_;
};

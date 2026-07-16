#pragma once

#include "ControlProtocol.hpp"

#include <QJsonObject>
#include <QList>
#include <QPointer>
#include <QTcpSocket>
#include <QTimer>

#include <functional>

class ControlClient : public QObject {
public:
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
        quint64 retiredSockets = 0;
        quint64 retiredSocketHighWater = 0;
    };

    explicit ControlClient(QObject* parent = nullptr);

    void connectToHost(
        const QString& host,
        quint16 port,
        const QString& sessionHex,
        const QString& keyHex,
        const QString& meshPeerToken = QString(),
        const QString& meshUdpEndpoint = QString());
    void close();
    bool send(const QJsonObject& message);
    bool sendBinary(const QByteArray& payload);
    bool canQueue(qint64 additionalBytes) const;
    bool isConnected() const;
    Stats stats() const { return stats_; }

    std::function<void(const QJsonObject&)> onMessage;
    std::function<void(const QByteArray&)> onBinaryMessage;
    std::function<void(const jam2::control_protocol::TransportEvent&)> onEvent;

private:
    enum class HandshakeState {
        WaitingForChallenge,
        WaitingForServerProof,
        Authenticated,
    };

    void installSocket(QTcpSocket* socket);
    void readSocket(QTcpSocket* socket);
    void handleHandshake(const QJsonObject& message);
    bool writeFrame(const QByteArray& frame);
    void retireSocket(QTcpSocket* socket);
    void completeSocketRetirement(const QPointer<QTcpSocket>& socket);
    void purgeRetiredSockets();
    void reject(
        const QString& reason,
        jam2::control_protocol::TransportFailure failure,
        bool abort = false,
        bool retryable = false);
    void publishEvent(jam2::control_protocol::TransportEvent event);

    QPointer<QTcpSocket> socket_;
    QList<QPointer<QTcpSocket>> retiredSockets_;
    QTimer authenticationTimer_;
    QTimer frameTimer_;
    QByteArray buffer_;
    QByteArray masterKey_;
    QByteArray serverNonce_;
    QByteArray clientNonce_;
    QByteArray transcript_;
    QByteArray receiveKey_;
    QByteArray sendKey_;
    QString sessionHex_;
    QString meshPeerToken_;
    QString meshUdpEndpoint_;
    HandshakeState handshakeState_ = HandshakeState::WaitingForChallenge;
    quint64 receiveSequence_ = 1;
    quint64 sendSequence_ = 1;
    bool readScheduled_ = false;
    bool manualClose_ = true;
    bool failureReportedForAttempt_ = false;
    Stats stats_;
};

#pragma once

#include "ControlProtocol.hpp"

#include <QJsonObject>
#include <QElapsedTimer>
#include <QList>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <functional>
#include <memory>

class ControlServer : public QObject {
public:
    struct Stats {
        quint64 acceptedConnections = 0;
        quint64 pendingCapRejects = 0;
        quint64 authenticationRateLimitRejects = 0;
        quint64 authenticatedCapRejects = 0;
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
    };

    explicit ControlServer(QObject* parent = nullptr);

    bool listen(quint16 port, const QString& sessionHex, const QString& keyHex);
    void close();
    void send(const QJsonObject& message);
    bool sendTo(const QString& token, const QJsonObject& message, bool closeAfterWrite = false);
    bool sendBinaryTo(const QString& token, const QByteArray& payload);
    bool rejectAuthenticatedPeer(const QString& token, const QString& reason);
    bool canQueueTo(const QString& token, qint64 additionalBytes) const;
    bool hasPeer() const;
    QString errorString() const;
    Stats stats() const { return stats_; }

    std::function<void(const QString&, const QJsonObject&)> onMessage;
    std::function<void(const QString&, const QByteArray&)> onBinaryMessage;
    std::function<void(const QString&, const QJsonObject&)> onAuthenticated;
    std::function<void(const QString&)> onDisconnected;
    std::function<void(const jam2::control_protocol::TransportEvent&)> onEvent;

private:
    struct Peer {
        QPointer<QTcpSocket> socket;
        QTimer* authenticationTimer = nullptr;
        QTimer* frameTimer = nullptr;
        QByteArray buffer;
        QByteArray serverNonce;
        QByteArray transcript;
        QByteArray receiveKey;
        QByteArray sendKey;
        bool authenticated = false;
        bool readScheduled = false;
        quint64 receiveSequence = 1;
        quint64 sendSequence = 1;
        QString token;
    };

    using PeerHandle = std::shared_ptr<Peer>;

    void acceptPeer();
    void readPeer(const PeerHandle& peer);
    void handleHandshake(const PeerHandle& peer, const QJsonObject& message);
    bool writeFrame(
        const PeerHandle& peer,
        const QByteArray& frame,
        bool closeAfterWrite = false);
    void rejectPeer(
        const PeerHandle& peer,
        const QString& reason,
        jam2::control_protocol::TransportFailure failure,
        bool abort = false);
    void publishEvent(jam2::control_protocol::TransportEvent event);
    PeerHandle findPeer(QTcpSocket* socket) const;
    int authenticatedPeerCount() const;
    int pendingPeerCount() const;
    void noteAuthenticationReject();
    bool authenticationWorkAvailable();

    QTcpServer server_;
    QList<PeerHandle> peers_;
    QString sessionHex_;
    QByteArray masterKey_;
    QElapsedTimer authenticationFailureWindow_;
    int authenticationFailuresInWindow_ = 0;
    Stats stats_;
};

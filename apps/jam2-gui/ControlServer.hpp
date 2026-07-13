#pragma once

#include <QJsonObject>
#include <QList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include <functional>

class ControlServer : public QObject {
public:
    struct Stats {
        quint64 acceptedConnections = 0;
        quint64 pendingCapRejects = 0;
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
    bool canQueueTo(const QString& token, qint64 additionalBytes) const;
    bool hasPeer() const;
    QString errorString() const;
    Stats stats() const { return stats_; }

    std::function<void(const QString&, const QJsonObject&)> onMessage;
    std::function<void(const QString&, const QJsonObject&)> onAuthenticated;
    std::function<void(const QString&)> onDisconnected;
    std::function<void(const QString&)> onState;

private:
    struct Peer {
        QTcpSocket* socket = nullptr;
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

    void acceptPeer();
    void readPeer(Peer* peer);
    void handleHandshake(Peer* peer, const QJsonObject& message);
    bool writeFrame(Peer* peer, const QByteArray& frame, bool closeAfterWrite = false);
    void rejectPeer(Peer* peer, const QString& reason, bool abort = false);
    Peer* findPeer(QTcpSocket* socket);
    int authenticatedPeerCount() const;
    int pendingPeerCount() const;

    QTcpServer server_;
    QList<Peer*> peers_;
    QString sessionHex_;
    QByteArray masterKey_;
    Stats stats_;
};

#pragma once

#include <QJsonObject>
#include <QList>
#include <QTcpServer>
#include <QTcpSocket>

#include <functional>

class ControlServer : public QObject {
public:
    explicit ControlServer(QObject* parent = nullptr);

    bool listen(quint16 port, const QString& sessionHex, const QString& keyHex);
    void close();
    void send(const QJsonObject& message);
    bool hasPeer() const;
    QString errorString() const;

    std::function<void(const QJsonObject&)> onMessage;
    std::function<void(const QString&, const QJsonObject&)> onAuthenticated;
    std::function<void(const QString&)> onDisconnected;
    std::function<void(const QString&)> onState;

private:
    struct Peer {
        QTcpSocket* socket = nullptr;
        QByteArray buffer;
        bool authenticated = false;
        QString token;
    };

    void acceptPeer();
    void readPeer(Peer* peer);
    void handleMessage(Peer* peer, const QJsonObject& message);
    Peer* findPeer(QTcpSocket* socket);

    QTcpServer server_;
    QList<Peer*> peers_;
    QString sessionHex_;
    QString keyHex_;
};

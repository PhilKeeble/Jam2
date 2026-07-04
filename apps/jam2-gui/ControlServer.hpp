#pragma once

#include <QJsonObject>
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
    std::function<void(const QString&)> onState;

private:
    void acceptPeer();
    void readPeer();
    void handleMessage(const QJsonObject& message);

    QTcpServer server_;
    QTcpSocket* peer_ = nullptr;
    QByteArray buffer_;
    QString sessionHex_;
    QString keyHex_;
    bool authenticated_ = false;
};

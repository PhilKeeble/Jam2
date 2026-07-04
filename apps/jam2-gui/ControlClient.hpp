#pragma once

#include <QJsonObject>
#include <QTcpSocket>

#include <functional>

class ControlClient : public QObject {
public:
    explicit ControlClient(QObject* parent = nullptr);

    void connectToHost(const QString& host, quint16 port, const QString& sessionHex, const QString& keyHex);
    void close();
    void send(const QJsonObject& message);
    bool isConnected() const;

    std::function<void(const QJsonObject&)> onMessage;
    std::function<void(const QString&)> onState;

private:
    void readSocket();

    QTcpSocket socket_;
    QByteArray buffer_;
    QString sessionHex_;
    QString keyHex_;
    bool authenticated_ = false;
};

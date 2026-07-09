#include "ControlClient.hpp"

#include <QJsonDocument>

ControlClient::ControlClient(QObject* parent)
    : QObject(parent)
{
    QObject::connect(&socket_, &QTcpSocket::connected, this, [this] {
        QJsonObject hello{
            {QStringLiteral("type"), QStringLiteral("hello")},
            {QStringLiteral("session"), sessionHex_},
            {QStringLiteral("key"), keyHex_},
        };
        if (!meshPeerToken_.isEmpty()) {
            hello.insert(QStringLiteral("mesh_peer_token"), meshPeerToken_);
        }
        if (!meshUdpEndpoint_.isEmpty()) {
            hello.insert(QStringLiteral("udp_endpoint"), meshUdpEndpoint_);
        }
        socket_.write(QJsonDocument(hello).toJson(QJsonDocument::Compact) + "\n");
        socket_.flush();
        if (onState) {
            onState(QStringLiteral("TCP connected; sent session auth"));
        }
    });
    QObject::connect(&socket_, &QTcpSocket::readyRead, this, [this] { readSocket(); });
    QObject::connect(&socket_, &QTcpSocket::disconnected, this, [this] {
        authenticated_ = false;
        if (onState) {
            onState(QStringLiteral("TCP control disconnected"));
        }
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
    sessionHex_ = sessionHex;
    keyHex_ = keyHex;
    meshPeerToken_ = meshPeerToken;
    meshUdpEndpoint_ = meshUdpEndpoint;
    socket_.connectToHost(host, port);
}

void ControlClient::close()
{
    authenticated_ = false;
    buffer_.clear();
    socket_.disconnectFromHost();
}

void ControlClient::send(const QJsonObject& message)
{
    if (!isConnected()) {
        return;
    }
    socket_.write(QJsonDocument(message).toJson(QJsonDocument::Compact) + "\n");
}

bool ControlClient::isConnected() const
{
    return socket_.state() == QTcpSocket::ConnectedState && authenticated_;
}

void ControlClient::readSocket()
{
    buffer_ += socket_.readAll();
    int newline = -1;
    while ((newline = buffer_.indexOf('\n')) >= 0) {
        const QByteArray line = buffer_.left(newline).trimmed();
        buffer_.remove(0, newline + 1);
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (!doc.isObject()) {
            continue;
        }
        const QJsonObject message = doc.object();
        if (!authenticated_ && message.value(QStringLiteral("type")).toString() == QStringLiteral("hello.ok")) {
            authenticated_ = true;
            if (onState) {
                onState(QStringLiteral("TCP control authenticated"));
            }
            continue;
        }
        if (!authenticated_ && message.value(QStringLiteral("type")).toString() == QStringLiteral("hello.error")) {
            const QString reason = message.value(QStringLiteral("reason")).toString(QStringLiteral("auth failed"));
            if (onState) {
                onState(QStringLiteral("TCP auth failed: ") + reason);
            }
            socket_.disconnectFromHost();
            continue;
        }
        if (onMessage) {
            onMessage(message);
        }
    }
}

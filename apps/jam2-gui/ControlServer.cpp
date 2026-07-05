#include "ControlServer.hpp"

#include <QJsonDocument>

ControlServer::ControlServer(QObject* parent)
    : QObject(parent)
{
    QObject::connect(&server_, &QTcpServer::newConnection, this, [this] { acceptPeer(); });
}

bool ControlServer::listen(quint16 port, const QString& sessionHex, const QString& keyHex)
{
    close();
    sessionHex_ = sessionHex;
    keyHex_ = keyHex;
    return server_.listen(QHostAddress::Any, port);
}

void ControlServer::close()
{
    authenticated_ = false;
    buffer_.clear();
    if (peer_) {
        peer_->disconnectFromHost();
        peer_->deleteLater();
        peer_ = nullptr;
    }
    server_.close();
}

void ControlServer::send(const QJsonObject& message)
{
    if (!peer_ || !authenticated_) {
        return;
    }
    peer_->write(QJsonDocument(message).toJson(QJsonDocument::Compact) + "\n");
}

bool ControlServer::hasPeer() const
{
    return peer_ && authenticated_;
}

QString ControlServer::errorString() const
{
    return server_.errorString();
}

void ControlServer::acceptPeer()
{
    QTcpSocket* socket = server_.nextPendingConnection();
    if (!socket) {
        return;
    }
    if (peer_) {
        socket->disconnectFromHost();
        socket->deleteLater();
        return;
    }
    peer_ = socket;
    authenticated_ = false;
    if (onState) {
        onState(QStringLiteral("TCP peer connected; waiting for session auth"));
    }
    QObject::connect(peer_, &QTcpSocket::readyRead, this, [this] { readPeer(); });
    QObject::connect(peer_, &QTcpSocket::disconnected, this, [this, socket] {
        authenticated_ = false;
        if (onState) {
            onState(QStringLiteral("TCP peer disconnected"));
        }
        socket->deleteLater();
        if (peer_ == socket) {
            peer_ = nullptr;
        }
    });
}

void ControlServer::readPeer()
{
    buffer_ += peer_->readAll();
    int newline = -1;
    while ((newline = buffer_.indexOf('\n')) >= 0) {
        const QByteArray line = buffer_.left(newline).trimmed();
        buffer_.remove(0, newline + 1);
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject()) {
            handleMessage(doc.object());
        }
    }
}

void ControlServer::handleMessage(const QJsonObject& message)
{
    if (!authenticated_) {
        if (message.value(QStringLiteral("type")).toString() == QStringLiteral("hello") &&
            message.value(QStringLiteral("session")).toString() == sessionHex_ &&
            message.value(QStringLiteral("key")).toString() == keyHex_) {
            authenticated_ = true;
            send(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("hello.ok")},
                {QStringLiteral("role"), QStringLiteral("listener")},
            });
            if (onState) {
                onState(QStringLiteral("TCP peer authenticated"));
            }
        } else {
            peer_->disconnectFromHost();
        }
        return;
    }
    if (onMessage) {
        onMessage(message);
    }
}

#include "ControlServer.hpp"

#include <QJsonDocument>
#include <QHostAddress>

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
    for (Peer* peer : peers_) {
        if (peer->socket) {
            peer->socket->disconnect(this);
            peer->socket->abort();
            peer->socket->deleteLater();
        }
        delete peer;
    }
    peers_.clear();
    server_.close();
}

void ControlServer::send(const QJsonObject& message)
{
    const QByteArray line = QJsonDocument(message).toJson(QJsonDocument::Compact) + "\n";
    for (Peer* peer : peers_) {
        if (peer->socket && peer->authenticated) {
            peer->socket->write(line);
        }
    }
}

bool ControlServer::hasPeer() const
{
    for (const Peer* peer : peers_) {
        if (peer->socket && peer->authenticated) {
            return true;
        }
    }
    return false;
}

QString ControlServer::errorString() const
{
    return server_.errorString();
}

ControlServer::Peer* ControlServer::findPeer(QTcpSocket* socket)
{
    for (Peer* peer : peers_) {
        if (peer->socket == socket) {
            return peer;
        }
    }
    return nullptr;
}

void ControlServer::acceptPeer()
{
    while (server_.hasPendingConnections()) {
        QTcpSocket* socket = server_.nextPendingConnection();
        if (!socket) {
            return;
        }
        auto* peer = new Peer;
        peer->socket = socket;
        peers_.push_back(peer);
        if (onState) {
            onState(QStringLiteral("TCP peer connected; waiting for session auth"));
        }
        QObject::connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
            if (Peer* peer = findPeer(socket)) {
                readPeer(peer);
            }
        });
        QObject::connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            for (qsizetype i = 0; i < peers_.size(); ++i) {
                Peer* peer = peers_[i];
                if (peer->socket != socket) {
                    continue;
                }
                const QString token = peer->token;
                peers_.removeAt(i);
                if (onDisconnected && !token.isEmpty()) {
                    onDisconnected(token);
                }
                if (onState) {
                    onState(QStringLiteral("TCP peer disconnected"));
                }
                socket->deleteLater();
                delete peer;
                return;
            }
        });
    }
}

void ControlServer::readPeer(Peer* peer)
{
    if (!peer || !peer->socket) {
        return;
    }
    peer->buffer += peer->socket->readAll();
    int newline = -1;
    while ((newline = peer->buffer.indexOf('\n')) >= 0) {
        const QByteArray line = peer->buffer.left(newline).trimmed();
        peer->buffer.remove(0, newline + 1);
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject()) {
            handleMessage(peer, doc.object());
        }
    }
}

void ControlServer::handleMessage(Peer* peer, const QJsonObject& message)
{
    if (!peer || !peer->socket) {
        return;
    }
    if (!peer->authenticated) {
        if (message.value(QStringLiteral("type")).toString() == QStringLiteral("hello") &&
            message.value(QStringLiteral("session")).toString() == sessionHex_ &&
            message.value(QStringLiteral("key")).toString() == keyHex_) {
            peer->authenticated = true;
            peer->token = message.value(QStringLiteral("mesh_peer_token")).toString();
            peer->socket->write(QJsonDocument(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("hello.ok")},
                {QStringLiteral("role"), QStringLiteral("listener")},
            }).toJson(QJsonDocument::Compact) + "\n");
            if (onState) {
                onState(QStringLiteral("TCP peer authenticated"));
            }
            if (onAuthenticated) {
                QJsonObject authenticatedMessage = message;
                authenticatedMessage.insert(QStringLiteral("tcp_peer_host"), peer->socket->peerAddress().toString());
                onAuthenticated(peer->token, authenticatedMessage);
            }
        } else {
            peer->socket->write(QJsonDocument(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("hello.error")},
                {QStringLiteral("reason"), QStringLiteral("incorrect session or key")},
            }).toJson(QJsonDocument::Compact) + "\n");
            peer->socket->flush();
            if (onState) {
                onState(QStringLiteral("TCP auth rejected: incorrect session or key"));
            }
            peer->socket->disconnectFromHost();
        }
        return;
    }
    if (onMessage) {
        onMessage(message);
    }
}

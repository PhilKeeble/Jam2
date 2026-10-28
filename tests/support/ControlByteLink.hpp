#pragma once
#include "ControlConnection.hpp"
#include <QPointer>
#include <QMetaObject>
#include <deque>
#include <memory>
#include <algorithm>
namespace jam2::test {
using jam2::application::ControlConnection;
// A byte-only fixture: all authentication, framing, sequencing and binary
// handling under test are the actual ControlClient/ControlServer implementations.
class Bytes final : public ControlConnection, public std::enable_shared_from_this<Bytes> {
public:
    std::weak_ptr<Bytes> remote;
    bool connected = true, paused = false;
    QPointer<QObject> context;
    DataHandler data;
    DisconnectHandler disconnected;
    std::deque<QByteArray> input;
    qint64 buffered = 0, highWater = 0;
    void drain()
    {
        if (!context || !data || !connected || paused) return;
        const std::weak_ptr<Bytes> weak = shared_from_this();
        QMetaObject::invokeMethod(context, [weak] {
            const auto self = weak.lock();
            if (!self || !self->connected || self->paused) return;
            while (!self->input.empty() && self->data) {
                const QByteArray bytes = std::move(self->input.front());
                self->input.pop_front(); self->buffered -= bytes.size();
                self->data(bytes);
            }
        }, Qt::QueuedConnection);
    }
    void start(QObject* owner, DataHandler onData, DisconnectHandler onDisconnected) override
    {
        context = owner; data = std::move(onData); disconnected = std::move(onDisconnected); drain();
    }
    bool write(const QByteArray& bytes, bool closeAfterWrite = false) override
    {
        const auto other = remote.lock();
        if (!connected || !other || !other->connected ||
            other->buffered + bytes.size() > 256 * 1024) return false;
        // Split framing boundaries without interpreting any protocol fields.
        for (qsizetype offset = 0; offset < bytes.size(); offset += 503) {
            other->input.push_back(bytes.mid(offset, 503));
        }
        other->buffered += bytes.size();
        other->highWater = std::max(other->highWater, other->buffered);
        other->drain();
        if (closeAfterWrite) {
            const std::weak_ptr<Bytes> weak = shared_from_this();
            QMetaObject::invokeMethod(context, [weak] { if (const auto self = weak.lock()) self->close(); },
                Qt::QueuedConnection);
        }
        return true;
    }
    void close() override
    {
        if (!connected) return;
        connected = false;
        if (const auto other = remote.lock(); other && other->connected) {
            other->connected = false;
            const auto callback = other->disconnected;
            if (other->context && callback) {
                QMetaObject::invokeMethod(other->context, [callback] { callback(QStringLiteral("byte link closed")); },
                    Qt::QueuedConnection);
            }
        }
        data = {}; disconnected = {}; input.clear(); buffered = 0;
    }
    bool isConnected() const override { return connected; }
    qint64 bytesToWrite() const override { return 0; }
    qint64 maxPendingReadBytes() const override { return highWater; }
    QString peerHost() const override { return QStringLiteral("127.0.0.1"); }
};

inline auto endpoints()
{
    auto first = std::make_shared<Bytes>(), second = std::make_shared<Bytes>();
    first->remote = second; second->remote = first;
    return std::make_pair(first, second);
}

}

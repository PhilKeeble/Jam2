#pragma once
#include <QByteArray>
#include <QObject>
#include <QString>
#include <functional>
#include <memory>

namespace jam2::application {
// Ordered reliable bytes for the existing authenticated control/asset protocol.
// Implementations provide TCP I/O or a logical stream of the punched UDP link.
class ControlConnection {
public:
    using Pointer = std::shared_ptr<ControlConnection>;
    using DataHandler = std::function<void(const QByteArray&)>;
    using DisconnectHandler = std::function<void(const QString&)>;
    virtual ~ControlConnection() = default;
    virtual void start(QObject* context, DataHandler onData, DisconnectHandler onDisconnected) = 0;
    virtual bool write(const QByteArray& bytes, bool closeAfterWrite = false) = 0;
    virtual void close() = 0;
    virtual bool isConnected() const = 0;
    virtual qint64 bytesToWrite() const = 0;
    virtual qint64 maxPendingReadBytes() const = 0;
    virtual QString peerHost() const = 0;
};
} // namespace jam2::application

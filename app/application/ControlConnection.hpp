#pragma once
#include <QByteArray>
#include <QObject>
#include <QString>
#include <functional>
#include <algorithm>
#include <memory>

namespace jam2::application {
// Ordered reliable bytes for the existing authenticated control/asset protocol.
// Implementations provide TCP I/O or a logical stream of the punched UDP link.
enum class ControlTransport { Tcp, ReliableUdp };

// TCP keeps its existing absolute five-second bound. Multiplexed UDP
// expires only after ten seconds without new ordered message bytes.
struct ControlFrameDeadline {
    qint64 began = -1, progress = -1;
    void clear() { began = progress = -1; }
    void received(qint64 now) { if (began < 0) began = now; progress = now; }
    int remaining(ControlTransport transport, qint64 now) const {
        if (began < 0) return transport == ControlTransport::Tcp ? 5000 : 10000;
        const auto left = transport == ControlTransport::Tcp ? 5000 - (now - began)
            : qint64{10000} - (now - progress);
        return static_cast<int>((std::max)(qint64{0}, left));
    }
};
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
    virtual ControlTransport transport() const { return ControlTransport::Tcp; }
    int authenticationDeadlineMs() const { return transport() == ControlTransport::Tcp ? 5000 : 20000; }
};
} // namespace jam2::application

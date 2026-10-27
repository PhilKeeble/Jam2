#pragma once
#include <QByteArray>
#include <QObject>
#include <QString>
#include <functional>
#include <algorithm>
#include <memory>

namespace jam2::application {
// Ordered reliable bytes on one punched UDP link.
inline constexpr qint64 kControlStreamQueueBytes=256*1024;
// An incomplete record expires after ten seconds without ordered progress.
struct ControlFrameDeadline {
    qint64 began = -1, progress = -1;
    void clear() { began = progress = -1; }
    void received(qint64 now) { if (began < 0) began = now; progress = now; }
    int remaining(qint64 now) const {
        const auto left=progress<0 ? qint64{10000} : qint64{10000}-(now-progress);
        return static_cast<int>((std::max)(qint64{0},left));
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
    // Optional independent ordering for bounded self-contained replies.
    virtual bool hasIndependentControl() const { return false; }
    virtual bool writeIndependent(const QByteArray&) { return false; }
    virtual qint64 independentBytesToWrite() const { return 0; }
    virtual void startIndependent(QObject*, DataHandler) {}
    int authenticationDeadlineMs() const { return 20000; }
};
} // namespace jam2::application

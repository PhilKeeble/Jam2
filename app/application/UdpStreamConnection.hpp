#pragma once

#include "ControlConnection.hpp"
#include "reliable_udp_stream.hpp"

#include <QPointer>
#include <deque>
#include <mutex>
#include <optional>

namespace jam2::application {

enum class UdpMuxChannel : quint8 { Control = 1, Asset = 2 };
enum class UdpMuxKind : quint8 { Open = 1, OpenAck, Data, Ack, Close, CloseAck };
struct UdpMuxFrame {
    UdpMuxKind kind = UdpMuxKind::Data;
    UdpMuxChannel channel = UdpMuxChannel::Control;
    quint64 session = 0, introduction = 0, stream = 0, sequence = 0;
    QByteArray payload;
};
QByteArray encodeUdpMux(const UdpMuxFrame& frame, const QByteArray& key);
std::optional<UdpMuxFrame> decodeUdpMux(const QByteArray& packet, quint64 session, const QByteArray& key);

// One reliable logical stream, never an additional physical UDP socket.
// Its network methods are called by the link I/O worker; ControlConnection
// callbacks are queued to the existing control owner's QObject thread.
class UdpStreamConnection final : public ControlConnection,
                                  public std::enable_shared_from_this<UdpStreamConnection> {
public:
    using Emit = std::function<bool(const QByteArray&)>;
    UdpStreamConnection(quint64 session, quint64 introduction, quint64 stream,
                       UdpMuxChannel channel, QByteArray key, QString peerHost, bool incoming);
    ~UdpStreamConnection() override;
    void start(QObject* context, DataHandler onData, DisconnectHandler onDisconnected) override;
    bool write(const QByteArray& bytes, bool closeAfterWrite = false) override;
    void close() override;
    bool isConnected() const override;
    qint64 bytesToWrite() const override;
    qint64 maxPendingReadBytes() const override;
    QString peerHost() const override { return peerHost_; }
    quint64 streamId() const noexcept { return stream_; }
    UdpMuxChannel channel() const noexcept { return channel_; }
    bool terminal() const;
    QString failure() const;
    jam2::ReliableUdpStream::Stats stats() const;
    quint64 inputDeferrals() const;
    void setWakeHandler(std::function<void()> wake);
    bool needsPolling() const;
    std::size_t poll(quint64 nowUs, std::size_t packetBudget, const Emit& transmit);
    void receive(const UdpMuxFrame& frame, quint64 nowUs, const Emit& transmit);

private:
    QByteArray packet(UdpMuxKind kind, quint64 sequence = 0, QByteArray payload = {}) const;
    void scheduleDelivery();
    void finish(const QString& detail);
    void requestClose();
    quint64 session_, introduction_, stream_;
    UdpMuxChannel channel_;
    QByteArray key_;
    QString peerHost_;
    mutable std::mutex mutex_;
    jam2::ReliableUdpStream sender_, receiver_;
    QPointer<QObject> context_;
    DataHandler onData_;
    DisconnectHandler onDisconnected_;
    std::deque<QByteArray> input_;
    qint64 buffered_ = 0, highWater_ = 0;
    quint64 startedUs_ = 0, nextOpenUs_ = 0, closeStartedUs_ = 0;
    quint64 inputDeferrals_ = 0;
    bool incoming_, connected_, closing_ = false, closed_ = false, draining_ = false;
    bool deliveryScheduled_ = false;
    QString failure_;
    std::function<void()> wake_;
};
} // namespace jam2::application

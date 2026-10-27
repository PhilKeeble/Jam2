#pragma once

#include "ControlConnection.hpp"
#include "reliable_udp_stream.hpp"

#include <QPointer>
#include <deque>
#include <mutex>
#include <optional>

namespace jam2::application {

enum class UdpStreamRetirement { Replaced, PeerLeft };
enum class UdpMuxChannel : quint8 { Control = 1, Asset = 2, IndependentControl = 3 };
inline constexpr quint8 kUdpMuxVersion = 3;
enum class UdpMuxKind : quint8 { Open = 1, OpenAck, Data, Ack, Close, CloseAck, PeerLeave, AckWindow, AckPair, DataAck, IntentAck };
struct UdpMuxFrameView {
    UdpMuxKind kind = UdpMuxKind::Data;
    UdpMuxChannel channel = UdpMuxChannel::Control;
    quint64 session = 0, introduction = 0, stream = 0, sequence = 0;
    std::span<const std::uint8_t> payload;
};
struct UdpMuxFrame {
    UdpMuxKind kind = UdpMuxKind::Data;
    UdpMuxChannel channel = UdpMuxChannel::Control;
    quint64 session = 0, introduction = 0, stream = 0, sequence = 0;
    QByteArray payload;
};
QByteArray encodeUdpMux(const UdpMuxFrame& frame, const QByteArray& key);
std::optional<UdpMuxFrame> decodeUdpMux(const QByteArray& packet, quint64 session, const QByteArray& key);
QByteArray encodeUdpMuxView(const UdpMuxFrameView& frame, const QByteArray& key);
// Borrows the authenticated payload until the caller releases packet storage.
std::optional<UdpMuxFrameView> decodeUdpMuxView(std::span<const std::uint8_t> packet, quint64 session, const QByteArray& key);
bool isCompactUdpMux(std::span<const std::uint8_t> packet) noexcept;
quint16 compactUdpMuxHandle(std::span<const std::uint8_t> packet) noexcept;
QByteArray compactUdpMuxKey(const QByteArray& key,quint64 session,quint64 introduction,quint64 stream,bool initiator);
QByteArray encodeCompactUdpMux(const UdpMuxFrameView& frame,quint16 handle,const QByteArray& key);
std::optional<UdpMuxFrameView> decodeCompactUdpMux(std::span<const std::uint8_t> packet,quint16 handle,
    quint64 session,quint64 introduction,quint64 stream,const QByteArray& key);

// One reliable logical stream, never an additional physical UDP socket.
// Its network methods are called by the link I/O worker; ControlConnection
// callbacks are queued to the existing control owner's QObject thread.
class UdpStreamConnection final : public ControlConnection,
                                  public std::enable_shared_from_this<UdpStreamConnection> {
public:
    using Emit = std::function<bool(const QByteArray&)>;
    struct Options {
        bool directInOrder = true, aggregateAcks = true, packUnsent = true, fastRecovery = true;
        quint64 fastReorderUs = 10000;
        bool combineAcks = true, compactFrames = true;
    };
    struct Stats : jam2::ReliableUdpStream::Stats {
        quint64 ackDatagramsSent = 0, acksCoalesced = 0, ackTransmitDeferrals = 0, invalidAckWindows = 0;
        quint64 ackPairs = 0, piggybackedAcks = 0, compactPackets = 0, compactBytesSaved = 0;
    };
    UdpStreamConnection(quint64 session, quint64 introduction, quint64 stream,
                       UdpMuxChannel channel, QByteArray key, QString peerHost, bool incoming);
    UdpStreamConnection(quint64 session, quint64 introduction, quint64 stream, UdpMuxChannel channel,
        QByteArray key, QString peerHost, bool incoming, Options options,quint16 handle=0);
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
    bool reusable() const;
    void retire(UdpStreamRetirement reason = UdpStreamRetirement::Replaced);
    ControlTransport transport() const override { return ControlTransport::ReliableUdp; }
    bool hasIndependentControl() const override { return channel_ == UdpMuxChannel::Control; }
    bool writeIndependent(const QByteArray& bytes) override;
    qint64 independentBytesToWrite() const override;
    void startIndependent(QObject* context, DataHandler onData) override;
    QString failure() const;
    Stats stats() const;
    quint64 inputDeferrals() const;
    void setWakeHandler(std::function<void()> wake);
    bool needsPolling() const;
    bool hasSendCredit() const;
    std::size_t poll(quint64 nowUs, std::size_t packetBudget, const Emit& transmit);
    void receive(const UdpMuxFrame& frame, quint64 nowUs, const Emit& transmit);
    void receiveView(const UdpMuxFrameView& frame, quint64 nowUs, const Emit& transmit);
    bool receiveDatagram(std::span<const std::uint8_t> packet,quint64 nowUs,const Emit& transmit);
    quint16 compactHandle() const noexcept { return compactHandle_; }

private:
    QByteArray packet(UdpMuxKind kind, quint64 sequence = 0, QByteArray payload = {}, bool independent = false) const;
    void flushAcknowledgements(const Emit& transmit);
    bool emitFrame(UdpMuxFrameView frame,const Emit& transmit);
    bool emitData(quint64 sequence,std::span<const std::uint8_t> bytes,bool independent,const Emit& transmit);
    void scheduleDelivery();
    void finish(const QString& detail);
    void requestClose();
    quint64 session_, introduction_, stream_;
    UdpMuxChannel channel_;
    QByteArray key_;
    QString peerHost_;
    mutable std::mutex mutex_;
    jam2::ReliableUdpStream sender_, receiver_;
    jam2::ReliableUdpStream independentSender_, independentReceiver_;
    Options options_;
    quint64 pendingAcks_ = 0, pendingIndependentAcks_ = 0;
    quint64 ackDatagramsSent_ = 0, acksCoalesced_ = 0, ackTransmitDeferrals_ = 0, invalidAckWindows_ = 0;
    quint16 compactHandle_=0;
    bool compactConfirmed_=false;
    QByteArray compactSendKey_,compactReceiveKey_;
    quint64 ackPairs_=0,piggybackedAcks_=0,compactPackets_=0,compactBytesSaved_=0;
    QPointer<QObject> context_;
    DataHandler onData_;
    DataHandler onIndependent_;
    DisconnectHandler onDisconnected_;
    std::deque<QByteArray> input_;
    std::deque<QByteArray> independentInput_;
    qint64 independentBuffered_ = 0;
    qint64 buffered_ = 0, highWater_ = 0;
    quint64 startedUs_ = 0, nextOpenUs_ = 0, closeStartedUs_ = 0;
    quint64 inputDeferrals_ = 0;
    bool incoming_, connected_, closing_ = false, closed_ = false, draining_ = false;
    bool deliveryScheduled_ = false;
    QString failure_;
    std::function<void()> wake_;
};
} // namespace jam2::application

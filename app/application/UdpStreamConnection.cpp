#include "UdpStreamConnection.hpp"
#include "ControlProtocol.hpp"

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QtEndian>
#include <algorithm>

namespace jam2::application {
namespace {
constexpr qsizetype kPrefixBytes = 42, kTagBytes = 16;
void put64(QByteArray& bytes, qsizetype offset, quint64 value)
{
    qToBigEndian(value, reinterpret_cast<uchar*>(bytes.data() + offset));
}
quint64 get64(const QByteArray& bytes, qsizetype offset)
{
    return qFromBigEndian<quint64>(reinterpret_cast<const uchar*>(bytes.constData() + offset));
}
}

QByteArray encodeUdpMux(const UdpMuxFrame& frame, const QByteArray& key)
{
    if (key.size() != 16 || frame.stream == 0 || frame.introduction == 0 ||
        frame.payload.size() > static_cast<qsizetype>(ReliableUdpStream::kFragmentBytes)) return {};
    QByteArray result(kPrefixBytes, '\0');
    result.replace(0, 4, "J2MX");
    result[4] = 1;
    result[5] = static_cast<char>(frame.kind);
    result[6] = static_cast<char>(frame.channel);
    put64(result, 8, frame.session);
    put64(result, 16, frame.introduction);
    put64(result, 24, frame.stream);
    put64(result, 32, frame.sequence);
    qToBigEndian(static_cast<quint16>(frame.payload.size()), reinterpret_cast<uchar*>(result.data() + 40));
    result += frame.payload;
    result += QMessageAuthenticationCode::hash(result, key, QCryptographicHash::Sha256).left(kTagBytes);
    return result;
}

std::optional<UdpMuxFrame> decodeUdpMux(const QByteArray& bytes, quint64 session, const QByteArray& key)
{
    if (key.size() != 16 || bytes.size() < kPrefixBytes + kTagBytes ||
        bytes.size() > kPrefixBytes + kTagBytes + static_cast<qsizetype>(ReliableUdpStream::kFragmentBytes) ||
        bytes.first(4) != QByteArrayLiteral("J2MX") || bytes[4] != 1 || bytes[7] != 0 ||
        get64(bytes, 8) != session || get64(bytes, 16) == 0 || get64(bytes, 24) == 0) return {};
    const auto length = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(bytes.constData() + 40));
    if (bytes.size() != kPrefixBytes + length + kTagBytes) return {};
    const auto kind = static_cast<UdpMuxKind>(static_cast<quint8>(bytes[5]));
    const auto channel = static_cast<UdpMuxChannel>(static_cast<quint8>(bytes[6]));
    if (kind < UdpMuxKind::Open || kind > UdpMuxKind::PeerLeave ||
        (channel != UdpMuxChannel::Control && channel != UdpMuxChannel::Asset)) return {};
    const auto tag = QMessageAuthenticationCode::hash(
        bytes.first(bytes.size() - kTagBytes), key, QCryptographicHash::Sha256).left(kTagBytes);
    if (!control_protocol::constantTimeEqual(tag, bytes.last(kTagBytes))) return {};
    if (kind == UdpMuxKind::PeerLeave && channel != UdpMuxChannel::Control) return {};
    const auto sequence = get64(bytes, 32);
    if ((kind == UdpMuxKind::Data && (sequence == 0 || length == 0)) ||
        (kind == UdpMuxKind::Ack && (sequence == 0 || length != 0)) ||
        (kind != UdpMuxKind::Data && kind != UdpMuxKind::Ack && (length != 0 || sequence != 0))) return {};
    return UdpMuxFrame{kind, channel, session, get64(bytes, 16), get64(bytes, 24),
        sequence, bytes.mid(kPrefixBytes, length)};
}

UdpStreamConnection::UdpStreamConnection(
    quint64 session, quint64 introduction, quint64 stream, UdpMuxChannel channel,
    QByteArray key, QString peerHost, bool incoming)
    : session_(session), introduction_(introduction), stream_(stream), channel_(channel),
      key_(std::move(key)), peerHost_(std::move(peerHost)), incoming_(incoming), connected_(incoming)
{
}
UdpStreamConnection::~UdpStreamConnection() = default;

QByteArray UdpStreamConnection::packet(UdpMuxKind kind, quint64 sequence, QByteArray payload) const
{
    return encodeUdpMux({kind, channel_, session_, introduction_, stream_, sequence, std::move(payload)}, key_);
}

void UdpStreamConnection::scheduleDelivery()
{
    if (deliveryScheduled_ || !context_ || !onData_ || input_.empty() || closed_) return;
    deliveryScheduled_ = true;
    const auto weak = weak_from_this();
    QMetaObject::invokeMethod(context_, [weak] {
        const auto self = weak.lock();
        if (!self) return;
        std::deque<QByteArray> pending;
        DataHandler callback;
        {
            std::lock_guard lock(self->mutex_);
            self->deliveryScheduled_ = false;
            if (self->closed_) return;
            pending.swap(self->input_);
            self->buffered_ = 0;
            callback = self->onData_;
        }
        // No transport mutex is held while the existing control parser runs.
        for (const auto& bytes : pending) if (callback) callback(bytes);
    }, Qt::QueuedConnection);
}

void UdpStreamConnection::start(QObject* owner, DataHandler onData, DisconnectHandler onDisconnected)
{
    std::lock_guard lock(mutex_);
    context_ = owner;
    onData_ = std::move(onData);
    onDisconnected_ = std::move(onDisconnected);
    scheduleDelivery();
}

bool UdpStreamConnection::write(const QByteArray& bytes, bool closeAfterWrite)
{
    std::lock_guard lock(mutex_);
    if (!connected_ || closing_ || closed_ || draining_) return false;
    if (!sender_.enqueue({reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                          static_cast<std::size_t>(bytes.size())})) return false;
    draining_ = closeAfterWrite;
    if (wake_) wake_();
    return true;
}

void UdpStreamConnection::requestClose()
{
    closing_ = true;
    connected_ = false;
}
void UdpStreamConnection::close()
{
    std::lock_guard lock(mutex_);
    requestClose();
    if (wake_) wake_();
}
bool UdpStreamConnection::isConnected() const
{
    std::lock_guard lock(mutex_); return connected_ && !closed_ && !closing_;
}
qint64 UdpStreamConnection::bytesToWrite() const
{
    std::lock_guard lock(mutex_); return static_cast<qint64>(sender_.queuedBytes());
}
qint64 UdpStreamConnection::maxPendingReadBytes() const
{
    std::lock_guard lock(mutex_); return highWater_;
}
bool UdpStreamConnection::terminal() const
{
    std::lock_guard lock(mutex_); return closed_;
}
void UdpStreamConnection::retire(UdpStreamRetirement reason)
{
    std::lock_guard lock(mutex_);
    finish(reason == UdpStreamRetirement::PeerLeft ? QStringLiteral("Peer left UDP link")
        : QStringLiteral("UDP logical stream replaced"));
}
bool UdpStreamConnection::reusable() const
{
    std::lock_guard lock(mutex_); return !closed_ && !closing_ && !draining_;
}
QString UdpStreamConnection::failure() const
{
    std::lock_guard lock(mutex_); return failure_;
}
ReliableUdpStream::Stats UdpStreamConnection::stats() const
{
    std::lock_guard lock(mutex_);
    auto snapshot = sender_.stats();
    snapshot.received = receiver_.stats().received;
    snapshot.duplicates = receiver_.stats().duplicates;
    snapshot.reordered = receiver_.stats().reordered;
    snapshot.rejected += receiver_.stats().rejected;
    snapshot.reorder_high_water_bytes = receiver_.stats().reorder_high_water_bytes;
    return snapshot;
}

quint64 UdpStreamConnection::inputDeferrals() const
{
    std::lock_guard lock(mutex_); return inputDeferrals_;
}

void UdpStreamConnection::setWakeHandler(std::function<void()> wake)
{
    std::lock_guard lock(mutex_); wake_ = std::move(wake);
}
bool UdpStreamConnection::needsPolling() const
{
    std::lock_guard lock(mutex_);
    return !closed_ && (!connected_ || closing_ || !sender_.empty());
}

void UdpStreamConnection::finish(const QString& detail)
{
    if (closed_) return;
    closed_ = true;
    connected_ = false;
    input_.clear(); buffered_ = 0;
    const auto callback = onDisconnected_;
    const auto context = context_;
    if (context && callback) {
        QMetaObject::invokeMethod(context, [callback, detail] { callback(detail); }, Qt::QueuedConnection);
    }
}

std::size_t UdpStreamConnection::poll(quint64 nowUs, std::size_t packetBudget, const Emit& transmit)
{
    std::lock_guard lock(mutex_);
    if (closed_ || packetBudget == 0) return 0;
    if (startedUs_ == 0) startedUs_ = nowUs;
    if (draining_ && sender_.empty()) requestClose();
    if (closing_) {
        if (closeStartedUs_ == 0) closeStartedUs_ = nowUs;
        if (nowUs - closeStartedUs_ >= 2000000) { finish(QStringLiteral("UDP logical stream closed")); return 0; }
        if (nowUs >= nextOpenUs_ && transmit(packet(UdpMuxKind::Close))) {
            nextOpenUs_ = nowUs + 250000; return 1;
        }
        return 0;
    }
    if (!connected_) {
        if (nowUs - startedUs_ >= 8000000) {
            failure_ = QStringLiteral("UDP logical stream open timeout"); finish(failure_); return 0;
        }
        if (nowUs >= nextOpenUs_ && transmit(packet(UdpMuxKind::Open))) {
            nextOpenUs_ = nowUs + 250000; return 1;
        }
        return 0;
    }
    const auto count = sender_.poll(nowUs, packetBudget, [&](auto sequence, auto bytes, bool) {
        return transmit(packet(UdpMuxKind::Data, sequence,
            QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()))));
    });
    if (sender_.failed()) {
        failure_ = QStringLiteral("UDP logical stream acknowledgement timeout"); finish(failure_);
    }
    return count;
}

void UdpStreamConnection::receive(const UdpMuxFrame& frame, quint64 nowUs, const Emit& transmit)
{
    std::lock_guard lock(mutex_);
    if (frame.session != session_ || frame.introduction != introduction_ ||
        frame.stream != stream_ || frame.channel != channel_) return;
    if (frame.kind == UdpMuxKind::Close) {
        transmit(packet(UdpMuxKind::CloseAck)); finish(QStringLiteral("Peer closed UDP logical stream")); return;
    }
    if (frame.kind == UdpMuxKind::CloseAck && closing_) { finish(QStringLiteral("UDP logical stream closed")); return; }
    if (closed_ || closing_) return;
    if (frame.kind == UdpMuxKind::Open) {
        if (incoming_) transmit(packet(UdpMuxKind::OpenAck));
        return;
    }
    if (frame.kind == UdpMuxKind::OpenAck) { if (!incoming_) connected_ = true; return; }
    if (frame.kind == UdpMuxKind::Ack) { sender_.acknowledge(frame.sequence, nowUs); return; }
    if (frame.kind != UdpMuxKind::Data) return;
    // Valid data can overtake a lost OpenAck on the already identified stream.
    connected_ = true;
    if (buffered_ + static_cast<qint64>(receiver_.reorderedBytes()) + frame.payload.size() >
        static_cast<qint64>(ReliableUdpStream::kQueueBytes)) {
        ++inputDeferrals_; // Withhold the ACK until the bounded reader has capacity.
        return;
    }
    receiver_.receive(frame.sequence,
        {reinterpret_cast<const std::uint8_t*>(frame.payload.constData()), static_cast<std::size_t>(frame.payload.size())},
        [&](auto bytes) {
            input_.emplace_back(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
            buffered_ += static_cast<qint64>(bytes.size());
            highWater_ = (std::max)(highWater_, buffered_);
        },
        [&](auto sequence) { transmit(packet(UdpMuxKind::Ack, sequence)); });
    scheduleDelivery();
}

} // namespace jam2::application

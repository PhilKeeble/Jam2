#include "UdpStreamConnection.hpp"
#include "ControlProtocol.hpp"

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <QtEndian>
#include <algorithm>
#include <array>

namespace jam2::application {
namespace {
constexpr qsizetype kPrefixBytes = 42, kTagBytes = 16;
void put64(QByteArray& bytes, qsizetype offset, quint64 value)
{
    qToBigEndian(value, reinterpret_cast<uchar*>(bytes.data() + offset));
}
std::span<const std::uint8_t> view(const QByteArray& bytes)
{ return {reinterpret_cast<const std::uint8_t*>(bytes.constData()), static_cast<std::size_t>(bytes.size())}; }
bool validMuxPayload(UdpMuxKind kind,UdpMuxChannel channel,quint64 sequence,std::span<const std::uint8_t> payload)
{
    const auto length=payload.size();
    const auto read=[&](std::size_t at) { return qFromBigEndian<quint64>(payload.data()+at); };
    if (kind<UdpMuxKind::Open || kind>UdpMuxKind::IntentAck ||
        (channel!=UdpMuxChannel::Control && channel!=UdpMuxChannel::Asset && channel!=UdpMuxChannel::IndependentControl)) return false;
    if (channel==UdpMuxChannel::IndependentControl && kind!=UdpMuxKind::Data && kind!=UdpMuxKind::DataAck && kind!=UdpMuxKind::Ack && kind!=UdpMuxKind::AckWindow) return false;
    if (kind==UdpMuxKind::Data) return sequence && length;
    if (kind==UdpMuxKind::DataAck) {
        if (!sequence || length<10 || payload[0]>1) return false;
        const auto prefix=payload[0] ? 17u : 9u;
        return length>prefix && (read(1) || (payload[0] && read(9)));
    }
    if (kind==UdpMuxKind::Ack) return sequence && !length;
    if (kind==UdpMuxKind::AckWindow) return (length==0 || length==8) && (sequence || (length==8 && read(0)));
    if (kind==UdpMuxKind::AckPair) return channel==UdpMuxChannel::Control && length==24 && (sequence || read(0)) && (read(8) || read(16));
    if (kind==UdpMuxKind::IntentAck) return channel==UdpMuxChannel::Control && !sequence && length==28;
    if (kind==UdpMuxKind::Open || kind==UdpMuxKind::OpenAck) return !sequence && (length==0 || (length==2 && qFromBigEndian<quint16>(payload.data())!=0));
    return !sequence && !length && (kind!=UdpMuxKind::PeerLeave || channel==UdpMuxChannel::Control);
}
}

bool isCompactUdpMux(std::span<const std::uint8_t> packet) noexcept
{ return packet.size()>=6 && std::equal(packet.begin(),packet.begin()+4,"J2MX") && (packet[5]&0x80); }
quint16 compactUdpMuxHandle(std::span<const std::uint8_t> packet) noexcept
{ return packet.size()>=16 && isCompactUdpMux(packet) ? qFromBigEndian<quint16>(packet.data()+6) : 0; }
QByteArray compactUdpMuxKey(const QByteArray& key,quint64 session,quint64 introduction,quint64 stream,bool initiator)
{
    QByteArray context(25,'\0'); put64(context,0,session); put64(context,8,introduction); put64(context,16,stream); context[24]=initiator ? 1 : 2;
    context.prepend("jam2-mux-compact-v3");
    return QMessageAuthenticationCode::hash(context,key,QCryptographicHash::Sha256);
}
QByteArray encodeCompactUdpMux(const UdpMuxFrameView& frame,quint16 handle,const QByteArray& key)
{
    if (!handle || key.size()!=32 || frame.payload.size()>ReliableUdpStream::kFragmentBytes ||
        (frame.kind!=UdpMuxKind::Data && frame.kind!=UdpMuxKind::DataAck && frame.kind!=UdpMuxKind::Ack && frame.kind!=UdpMuxKind::AckWindow && frame.kind!=UdpMuxKind::AckPair)) return {};
    QByteArray packet; packet.reserve(32+frame.payload.size()); packet.resize(16); packet.fill('\0');
    packet.replace(0,4,"J2MX"); packet[4]=kUdpMuxVersion;
    packet[5]=static_cast<char>(0x80 | static_cast<quint8>(frame.kind) | (static_cast<quint8>(frame.channel)<<4));
    qToBigEndian(handle,reinterpret_cast<uchar*>(packet.data()+6)); put64(packet,8,frame.sequence);
    packet.append(reinterpret_cast<const char*>(frame.payload.data()),frame.payload.size());
    packet+=QMessageAuthenticationCode::hash(packet,key,QCryptographicHash::Sha256).first(16); return packet;
}
std::optional<UdpMuxFrameView> decodeCompactUdpMux(std::span<const std::uint8_t> packet,quint16 handle,
    quint64 session,quint64 introduction,quint64 stream,const QByteArray& key)
{
    if (key.size()!=32 || packet.size()<32 || packet.size()>32+ReliableUdpStream::kFragmentBytes || !handle ||
        !isCompactUdpMux(packet) || packet[4]!=kUdpMuxVersion || (packet[5]&0x40) || compactUdpMuxHandle(packet)!=handle) return {};
    const auto tag=QMessageAuthenticationCode::hash(QByteArrayView(reinterpret_cast<const char*>(packet.data()),packet.size()-16),key,QCryptographicHash::Sha256).first(16);
    if (!control_protocol::constantTimeEqual(tag,QByteArray::fromRawData(reinterpret_cast<const char*>(packet.data()+packet.size()-16),16))) return {};
    const auto kind=static_cast<UdpMuxKind>(packet[5]&15);
    const auto channel=static_cast<UdpMuxChannel>((packet[5]>>4)&3);
    if (kind!=UdpMuxKind::Data && kind!=UdpMuxKind::DataAck && kind!=UdpMuxKind::Ack && kind!=UdpMuxKind::AckWindow && kind!=UdpMuxKind::AckPair) return {};
    const auto seq=qFromBigEndian<quint64>(packet.data()+8); const auto payload=packet.subspan(16,packet.size()-32);
    if (!validMuxPayload(kind,channel,seq,payload)) return {};
    return UdpMuxFrameView{kind,channel,session,introduction,stream,seq,payload};
}

QByteArray encodeUdpMux(const UdpMuxFrame& frame, const QByteArray& key)
{
    return encodeUdpMuxView({frame.kind,frame.channel,frame.session,frame.introduction,frame.stream,frame.sequence,view(frame.payload)},key);
}

QByteArray encodeUdpMuxView(const UdpMuxFrameView& frame, const QByteArray& key)
{
    if (key.size() != 16 || frame.stream == 0 || frame.introduction == 0 ||
        frame.payload.size() > ReliableUdpStream::kFragmentBytes) return {};
    QByteArray result;
    result.reserve(kPrefixBytes + static_cast<qsizetype>(frame.payload.size()) + kTagBytes);
    result.resize(kPrefixBytes); result.fill('\0');
    result.replace(0, 4, "J2MX");
    result[4] = kUdpMuxVersion;
    result[5] = static_cast<char>(frame.kind);
    result[6] = static_cast<char>(frame.channel);
    put64(result, 8, frame.session);
    put64(result, 16, frame.introduction);
    put64(result, 24, frame.stream);
    put64(result, 32, frame.sequence);
    qToBigEndian(static_cast<quint16>(frame.payload.size()), reinterpret_cast<uchar*>(result.data() + 40));
    result.append(reinterpret_cast<const char*>(frame.payload.data()),static_cast<qsizetype>(frame.payload.size()));
    result += QMessageAuthenticationCode::hash(result, key, QCryptographicHash::Sha256).left(kTagBytes);
    return result;
}

std::optional<UdpMuxFrame> decodeUdpMux(const QByteArray& bytes, quint64 session, const QByteArray& key)
{
    const auto frame = decodeUdpMuxView(view(bytes),session,key);
    if (!frame) return {};
    return UdpMuxFrame{frame->kind,frame->channel,frame->session,frame->introduction,frame->stream,frame->sequence,
        QByteArray(reinterpret_cast<const char*>(frame->payload.data()),static_cast<qsizetype>(frame->payload.size()))};
}

std::optional<UdpMuxFrameView> decodeUdpMuxView(std::span<const std::uint8_t> packet, quint64 session, const QByteArray& key)
{
    const QByteArrayView bytes(reinterpret_cast<const char*>(packet.data()),static_cast<qsizetype>(packet.size()));
    const auto read64 = [&](qsizetype offset) { return qFromBigEndian<quint64>(reinterpret_cast<const uchar*>(bytes.data()+offset)); };
    if (key.size() != 16 || bytes.size() < kPrefixBytes + kTagBytes ||
        bytes.size() > kPrefixBytes + kTagBytes + static_cast<qsizetype>(ReliableUdpStream::kFragmentBytes) ||
        bytes.first(4) != QByteArrayView("J2MX") || bytes[4] != kUdpMuxVersion || bytes[7] != 0 ||
        read64(8) != session || read64(16) == 0 || read64(24) == 0) return {};
    const auto length = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(bytes.data() + 40));
    if (bytes.size() != kPrefixBytes + length + kTagBytes) return {};
    const auto kind = static_cast<UdpMuxKind>(static_cast<quint8>(bytes[5]));
    const auto channel = static_cast<UdpMuxChannel>(static_cast<quint8>(bytes[6]));
    if (kind < UdpMuxKind::Open || kind > UdpMuxKind::IntentAck ||
        (channel != UdpMuxChannel::Control && channel != UdpMuxChannel::Asset && channel != UdpMuxChannel::IndependentControl)) return {};
    auto tag = QMessageAuthenticationCode::hash(bytes.first(bytes.size() - kTagBytes), key, QCryptographicHash::Sha256);
    tag.truncate(kTagBytes);
    if (!control_protocol::constantTimeEqual(tag, QByteArray::fromRawData(bytes.data()+bytes.size()-kTagBytes,kTagBytes))) return {};
    if (kind == UdpMuxKind::PeerLeave && channel != UdpMuxChannel::Control) return {};
    const auto sequence = read64(32);
    if (!validMuxPayload(kind,channel,sequence,packet.subspan(kPrefixBytes,length))) return {};
    return UdpMuxFrameView{kind, channel, session, read64(16), read64(24),sequence,packet.subspan(kPrefixBytes,length)};
}

UdpStreamConnection::UdpStreamConnection(
    quint64 session, quint64 introduction, quint64 stream, UdpMuxChannel channel,
    QByteArray key, QString peerHost, bool incoming)
    : UdpStreamConnection(session,introduction,stream,channel,std::move(key),std::move(peerHost),incoming,Options{}) {}
UdpStreamConnection::UdpStreamConnection(quint64 session, quint64 introduction, quint64 stream, UdpMuxChannel channel,
    QByteArray key, QString peerHost, bool incoming, Options options,quint16 handle)
    : session_(session), introduction_(introduction), stream_(stream), channel_(channel),
      key_(std::move(key)), peerHost_(std::move(peerHost)),
      sender_(jam2::ReliableUdpStream::Options{true,options.packUnsent,options.fastRecovery,options.fastReorderUs}),
      receiver_(jam2::ReliableUdpStream::Options{options.directInOrder}),
      independentSender_(jam2::ReliableUdpStream::Options{true,options.packUnsent,options.fastRecovery,options.fastReorderUs}),
      independentReceiver_(jam2::ReliableUdpStream::Options{options.directInOrder}), options_(options),incoming_(incoming), connected_(incoming)
{
    compactHandle_=options_.compactFrames ? handle : 0;
    compactConfirmed_=incoming && compactHandle_;
    if (compactHandle_) {
        compactSendKey_=compactUdpMuxKey(key_,session_,introduction_,stream_,!incoming);
        compactReceiveKey_=compactUdpMuxKey(key_,session_,introduction_,stream_,incoming);
    }
}
UdpStreamConnection::~UdpStreamConnection() = default;

QByteArray UdpStreamConnection::packet(UdpMuxKind kind, quint64 sequence, QByteArray payload, bool independent) const
{
    if ((kind==UdpMuxKind::Open || kind==UdpMuxKind::OpenAck) && compactHandle_) {
        payload.resize(2); qToBigEndian(compactHandle_,reinterpret_cast<uchar*>(payload.data()));
    }
    return encodeUdpMux({kind, independent ? UdpMuxChannel::IndependentControl : channel_, session_, introduction_, stream_, sequence, std::move(payload)}, key_);
}

void UdpStreamConnection::scheduleDelivery()
{
    if (deliveryScheduled_ || !context_ || closed_ ||
        ((!onData_ || input_.empty()) && (!onIndependent_ || independentInput_.empty()))) return;
    deliveryScheduled_ = true;
    const auto weak = weak_from_this();
    QMetaObject::invokeMethod(context_, [weak] {
        const auto self = weak.lock();
        if (!self) return;
        std::deque<QByteArray> pending;
        DataHandler callback;
        std::deque<QByteArray> independent;
        DataHandler independentCallback;
        {
            std::lock_guard lock(self->mutex_);
            self->deliveryScheduled_ = false;
            if (self->closed_) return;
            pending.swap(self->input_);
            self->buffered_ = 0;
            callback = self->onData_;
            if (self->onIndependent_) {
                independent.swap(self->independentInput_); self->independentBuffered_ = 0;
                independentCallback = self->onIndependent_;
            }
        }
        // No transport mutex is held while the existing control parser runs.
        for (const auto& bytes : pending) if (callback) callback(bytes);
        for (const auto& bytes : independent) if (independentCallback) independentCallback(bytes);
    }, Qt::QueuedConnection);
}
void UdpStreamConnection::startIndependent(QObject* owner, DataHandler onData)
{
    std::lock_guard lock(mutex_);
    if (owner != context_ || !hasIndependentControl()) return;
    onIndependent_ = std::move(onData); scheduleDelivery();
}
bool UdpStreamConnection::writeIndependent(const QByteArray& bytes)
{
    std::lock_guard lock(mutex_);
    if (!hasIndependentControl() || !connected_ || closing_ || closed_ || draining_ ||
        independentSender_.queuedBytes() + bytes.size() > 16 * 1024) return false;
    if (!independentSender_.enqueue({reinterpret_cast<const std::uint8_t*>(bytes.constData()), static_cast<std::size_t>(bytes.size())})) return false;
    if (wake_) wake_(); return true;
}
qint64 UdpStreamConnection::independentBytesToWrite() const
{
    std::lock_guard lock(mutex_); return independentSender_.queuedBytes();
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
    std::lock_guard lock(mutex_); return static_cast<qint64>(sender_.queuedBytes() + independentSender_.queuedBytes());
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
UdpStreamConnection::Stats UdpStreamConnection::stats() const
{
    std::lock_guard lock(mutex_);
    Stats snapshot;
    static_cast<ReliableUdpStream::Stats&>(snapshot) = sender_.stats();
    snapshot.received = receiver_.stats().received;
    snapshot.duplicates = receiver_.stats().duplicates;
    snapshot.reordered = receiver_.stats().reordered;
    snapshot.rejected += receiver_.stats().rejected;
    snapshot.reorder_high_water_bytes = receiver_.stats().reorder_high_water_bytes;
    const auto& tx = independentSender_.stats(); const auto& rx = independentReceiver_.stats();
    snapshot.sent += tx.sent; snapshot.retransmitted += tx.retransmitted; snapshot.acknowledged += tx.acknowledged;
    snapshot.received += rx.received; snapshot.duplicates += rx.duplicates; snapshot.reordered += rx.reordered;
    snapshot.rejected += tx.rejected + rx.rejected; snapshot.timeouts += tx.timeouts;
    snapshot.in_order_direct = receiver_.stats().in_order_direct + rx.in_order_direct;
    snapshot.reorder_insertions = receiver_.stats().reorder_insertions + rx.reorder_insertions;
    snapshot.tail_bytes_packed += tx.tail_bytes_packed; snapshot.fragments_avoided += tx.fragments_avoided;
    snapshot.fast_retransmitted+=tx.fast_retransmitted; snapshot.sack_gap_reports+=tx.sack_gap_reports;
    snapshot.ackPairs=ackPairs_; snapshot.piggybackedAcks=piggybackedAcks_;
    snapshot.compactPackets=compactPackets_; snapshot.compactBytesSaved=compactBytesSaved_;
    snapshot.ackDatagramsSent = ackDatagramsSent_; snapshot.acksCoalesced = acksCoalesced_;
    snapshot.ackTransmitDeferrals = ackTransmitDeferrals_; snapshot.invalidAckWindows = invalidAckWindows_;
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
    return !closed_ && (!connected_ || closing_ || !sender_.empty() || !independentSender_.empty() || pendingAcks_ || pendingIndependentAcks_);
}
bool UdpStreamConnection::hasSendCredit() const
{
    std::lock_guard lock(mutex_);
    return connected_ && !closed_ && !closing_ && (sender_.hasSendCredit() || independentSender_.hasSendCredit());
}

void UdpStreamConnection::finish(const QString& detail)
{
    if (closed_) return;
    closed_ = true;
    connected_ = false;
    input_.clear(); buffered_ = 0;
    independentInput_.clear(); independentBuffered_ = 0;
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
    if (draining_ && sender_.empty() && independentSender_.empty()) requestClose();
    if (closing_) {
        flushAcknowledgements(transmit);
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
    const auto urgentBudget = packetBudget > 1 && !sender_.empty() ? std::size_t{1} : std::min<std::size_t>(packetBudget, 2);
    const auto urgent = independentSender_.poll(nowUs, urgentBudget, [&](auto sequence, auto bytes, bool) {
        return emitData(sequence,bytes,true,transmit);
    });
    const auto count = sender_.poll(nowUs, packetBudget - urgent, [&](auto sequence, auto bytes, bool) {
        return emitData(sequence,bytes,false,transmit);
    });
    flushAcknowledgements(transmit);
    if (sender_.failed() || independentSender_.failed()) {
        failure_ = QStringLiteral("UDP logical stream acknowledgement timeout"); finish(failure_);
    }
    return count + urgent;
}

void UdpStreamConnection::receive(const UdpMuxFrame& frame, quint64 nowUs, const Emit& transmit)
{
    receiveView({frame.kind,frame.channel,frame.session,frame.introduction,frame.stream,frame.sequence,view(frame.payload)},nowUs,transmit);
}
void UdpStreamConnection::receiveView(const UdpMuxFrameView& frame, quint64 nowUs, const Emit& transmit)
{
    std::lock_guard lock(mutex_);
    if (frame.session != session_ || frame.introduction != introduction_ ||
        frame.stream != stream_ || (frame.channel != channel_ && !(hasIndependentControl() && frame.channel == UdpMuxChannel::IndependentControl))) return;
    const bool independent = frame.channel == UdpMuxChannel::IndependentControl;
    if (!validMuxPayload(frame.kind,frame.channel,frame.sequence,frame.payload)) return;
    if (frame.kind == UdpMuxKind::Close) {
        transmit(packet(UdpMuxKind::CloseAck)); finish(QStringLiteral("Peer closed UDP logical stream")); return;
    }
    if (frame.kind == UdpMuxKind::CloseAck && closing_) { finish(QStringLiteral("UDP logical stream closed")); return; }
    if (closed_ || closing_) return;
    if (frame.kind == UdpMuxKind::Open) {
        const auto handle=frame.payload.size()==2 ? qFromBigEndian<quint16>(frame.payload.data()) : 0;
        if (handle!=compactHandle_ && options_.compactFrames) return;
        if (incoming_) transmit(packet(UdpMuxKind::OpenAck));
        return;
    }
    if (frame.kind == UdpMuxKind::OpenAck) {
        const auto handle=frame.payload.size()==2 ? qFromBigEndian<quint16>(frame.payload.data()) : 0;
        if (handle!=compactHandle_ && handle!=0) return;
        if (!incoming_) { connected_=true; compactHandle_=handle; compactConfirmed_=compactHandle_!=0; } return;
    }
    auto& sender = independent ? independentSender_ : sender_;
    auto& receiver = independent ? independentReceiver_ : receiver_;
    auto& buffered = independent ? independentBuffered_ : buffered_;
    auto& input = independent ? independentInput_ : input_;
    if (frame.kind == UdpMuxKind::Ack) { sender.acknowledge(frame.sequence, nowUs); return; }
    if (frame.kind == UdpMuxKind::AckWindow) {
        if ((frame.payload.size()!=0 && frame.payload.size()!=8) ||
            !sender.acknowledgeWindow({frame.sequence,frame.payload.empty() ? 0 :
                qFromBigEndian<quint64>(frame.payload.data())},nowUs)) ++invalidAckWindows_;
        return;
    }
    if (frame.kind==UdpMuxKind::AckPair && hasIndependentControl()) {
        const jam2::ReliableUdpStream::AckWindow primary{frame.sequence,qFromBigEndian<quint64>(frame.payload.data())};
        const jam2::ReliableUdpStream::AckWindow urgent{qFromBigEndian<quint64>(frame.payload.data()+8),qFromBigEndian<quint64>(frame.payload.data()+16)};
        if (!sender_.validAckWindow(primary) || !independentSender_.validAckWindow(urgent)) { ++invalidAckWindows_; return; }
        sender_.acknowledgeWindow(primary,nowUs); independentSender_.acknowledgeWindow(urgent,nowUs); return;
    }
    auto data=frame.payload;
    if (frame.kind==UdpMuxKind::DataAck) {
        const jam2::ReliableUdpStream::AckWindow window{qFromBigEndian<quint64>(data.data()+1),data[0] ? qFromBigEndian<quint64>(data.data()+9) : 0};
        if (!sender.acknowledgeWindow(window,nowUs)) { ++invalidAckWindows_; return; }
        data=data.subspan(data[0] ? 17 : 9);
    } else if (frame.kind != UdpMuxKind::Data) return;
    // Valid data can overtake a lost OpenAck on the already identified stream.
    connected_ = true;
    if (compactHandle_) compactConfirmed_=true;
    if (buffered + static_cast<qint64>(receiver.reorderedBytes()) + static_cast<qint64>(data.size()) >
        (independent ? 16 * 1024 : static_cast<qint64>(ReliableUdpStream::kQueueBytes))) {
        ++inputDeferrals_; // Withhold the ACK until the bounded reader has capacity.
        return;
    }
    receiver.receive(frame.sequence,data,
        [&](auto bytes) {
            input.emplace_back(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size()));
            buffered += static_cast<qint64>(bytes.size());
            highWater_ = (std::max)(highWater_, buffered_ + independentBuffered_);
        },
        [&](auto sequence) {
            if (options_.aggregateAcks) ++(independent ? pendingIndependentAcks_ : pendingAcks_);
            else if (transmit(packet(UdpMuxKind::Ack, sequence, {}, independent))) ++ackDatagramsSent_;
        });
    scheduleDelivery();
}

bool UdpStreamConnection::receiveDatagram(std::span<const std::uint8_t> packet,quint64 nowUs,const Emit& transmit)
{
    const auto frame=decodeCompactUdpMux(packet,compactHandle_,session_,introduction_,stream_,compactReceiveKey_);
    if (!frame || (frame->channel!=channel_ && !(hasIndependentControl() && frame->channel==UdpMuxChannel::IndependentControl))) return false;
    receiveView(*frame,nowUs,transmit); return true;
}
bool UdpStreamConnection::emitFrame(UdpMuxFrameView frame,const Emit& transmit)
{
    const bool compact=compactHandle_ && compactConfirmed_;
    const auto encoded=compact ? encodeCompactUdpMux(frame,compactHandle_,compactSendKey_) : encodeUdpMuxView(frame,key_);
    if (encoded.isEmpty() || !transmit(encoded)) return false;
    if (compact) { ++compactPackets_; compactBytesSaved_+=26; }
    return true;
}
bool UdpStreamConnection::emitData(quint64 sequence,std::span<const std::uint8_t> data,bool independent,const Emit& transmit)
{
    auto& pending=independent ? pendingIndependentAcks_ : pendingAcks_;
    const auto channel=independent ? UdpMuxChannel::IndependentControl : channel_;
    if (options_.combineAcks && pending) {
        const auto ack=(independent ? independentReceiver_ : receiver_).acknowledgementWindow();
        const std::size_t prefix=ack.selective ? 17 : 9;
        if (data.size()+prefix<=ReliableUdpStream::kFragmentBytes) {
            std::array<std::uint8_t,ReliableUdpStream::kFragmentBytes> body{};
            body[0]=ack.selective ? 1 : 0; qToBigEndian(ack.contiguous,body.data()+1);
            if (ack.selective) qToBigEndian(ack.selective,body.data()+9);
            std::copy(data.begin(),data.end(),body.begin()+prefix);
            if (!emitFrame({UdpMuxKind::DataAck,channel,session_,introduction_,stream_,sequence,std::span(body).first(prefix+data.size())},transmit)) return false;
            ++piggybackedAcks_; acksCoalesced_+=pending-1; pending=0; return true;
        }
    }
    return emitFrame({UdpMuxKind::Data,channel,session_,introduction_,stream_,sequence,data},transmit);
}

void UdpStreamConnection::flushAcknowledgements(const Emit& transmit)
{
    if (options_.combineAcks && pendingAcks_ && pendingIndependentAcks_ && hasIndependentControl()) {
        const auto primary=receiver_.acknowledgementWindow(),urgent=independentReceiver_.acknowledgementWindow();
        std::array<std::uint8_t,24> payload{}; qToBigEndian(primary.selective,payload.data());
        qToBigEndian(urgent.contiguous,payload.data()+8); qToBigEndian(urgent.selective,payload.data()+16);
        if (!emitFrame({UdpMuxKind::AckPair,channel_,session_,introduction_,stream_,primary.contiguous,payload},transmit)) { ++ackTransmitDeferrals_; return; }
        ++ackPairs_; ++ackDatagramsSent_; acksCoalesced_+=pendingAcks_+pendingIndependentAcks_-1; pendingAcks_=pendingIndependentAcks_=0;
    }
    for (const bool independent : {false,true}) {
        auto& count = independent ? pendingIndependentAcks_ : pendingAcks_;
        if (!count) continue;
        const auto window = (independent ? independentReceiver_ : receiver_).acknowledgementWindow();
        std::array<std::uint8_t,8> bitmap{};
        if (window.selective) qToBigEndian(window.selective,bitmap.data());
        const auto payload = window.selective ? std::span<const std::uint8_t>(bitmap) : std::span<const std::uint8_t>{};
        if (!emitFrame({UdpMuxKind::AckWindow,independent ? UdpMuxChannel::IndependentControl : channel_,
                session_,introduction_,stream_,window.contiguous,payload},transmit)) { ++ackTransmitDeferrals_; continue; }
        ++ackDatagramsSent_; acksCoalesced_ += count-1; count = 0;
    }
}

} // namespace jam2::application

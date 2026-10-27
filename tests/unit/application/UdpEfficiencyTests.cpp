#include "UdpStreamConnection.hpp"
#include <QCoreApplication>
#include <QtEndian>
#include <array>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using namespace jam2::application;
void check(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
std::span<const std::uint8_t> view(const QByteArray& bytes)
{ return {reinterpret_cast<const std::uint8_t*>(bytes.constData()),static_cast<std::size_t>(bytes.size())}; }
}
void udpEfficiencyFixtures()
{
    using namespace jam2::application;
    const QByteArray key(16,'k'); const auto forward=compactUdpMuxKey(key,1,2,3,true),reverse=compactUdpMuxKey(key,1,2,3,false);
    const QByteArray text("abc");
    const auto encoded=encodeCompactUdpMux({UdpMuxKind::Data,UdpMuxChannel::Control,1,2,3,1,view(text)},7,forward);
    check(encoded.size()==35,"compact data header is not 16 bytes plus 16-byte tag");
    check(decodeCompactUdpMux(view(encoded),7,1,2,3,forward).has_value(),"compact authenticated packet rejected");
    check(!decodeCompactUdpMux(view(encoded),8,1,2,3,forward) && !decodeCompactUdpMux(view(encoded),7,1,2,3,reverse) &&
        !decodeCompactUdpMux(view(encoded),7,1,2,4,compactUdpMuxKey(key,1,2,4,true)),"handle, direction or retired stream binding bypassed");
    for (qsizetype size=0;size<encoded.size();++size)
        check(!decodeCompactUdpMux(view(encoded.first(size)),7,1,2,3,forward),"truncated compact packet accepted");
    auto changed=encoded; changed[16]^=1;
    check(!decodeCompactUdpMux(view(changed),7,1,2,3,forward),"compact payload tampering accepted");
    auto a=std::make_shared<UdpStreamConnection>(1,2,3,UdpMuxChannel::Control,key,"local",false,UdpStreamConnection::Options{},7);
    auto b=std::make_shared<UdpStreamConnection>(1,2,3,UdpMuxChannel::Control,key,"local",true,UdpStreamConnection::Options{},7);
    std::vector<QByteArray> ab,ba;
    const auto toB=[&](const QByteArray& bytes) { ab.push_back(bytes); return true; };
    const auto toA=[&](const QByteArray& bytes) { ba.push_back(bytes); return true; };
    a->poll(1,2,toB); check(ab.size()==1,"compact Open absent");
    const auto open=decodeUdpMux(ab[0],1,key); check(open && open->payload.size()==2,"Open did not establish compact handle");
    b->receive(*open,2,toA); check(ba.size()==1,"OpenAck absent");
    const auto opened=decodeUdpMux(ba[0],1,key); check(opened.has_value(),"OpenAck malformed"); a->receive(*opened,3,toB);
    check(a->isConnected(),"compact stream did not confirm mapping"); ab.clear(); ba.clear();
    QObject context; QByteArray receivedA,receivedB;
    a->start(&context,[&](const QByteArray& bytes) { receivedA+=bytes; },{});
    b->start(&context,[&](const QByteArray& bytes) { receivedB+=bytes; },{});
    check(a->write("first") && b->write("reply") && a->poll(10,2,toB)==1 && ab.size()==1,"queue reciprocal data");
    check(b->receiveDatagram(view(ab[0]),11,toA),"compact data decode");
    check(b->poll(12,2,toA)==1 && ba.size()==1,"ready reverse data did not absorb ACK");
    const auto piggy=decodeCompactUdpMux(view(ba[0]),7,1,2,3,reverse);
    check(piggy && piggy->kind==UdpMuxKind::DataAck && b->stats().piggybackedAcks==1,"piggyback format/counter");
    check(a->receiveDatagram(view(ba[0]),13,toB) && a->bytesToWrite()==0,"piggyback ACK lost sender credit");
    ab.clear(); check(a->poll(14,2,toB)==0 && ab.size()==1 && b->receiveDatagram(view(ab[0]),15,toA) && b->bytesToWrite()==0,
        "sparse ACK waited for another packet");
    QCoreApplication::processEvents();
    check(receivedA=="reply" && receivedB=="first","piggyback bytes escaped or corrupted asynchronous ownership");
    a->startIndependent(&context,[](const QByteArray&) {}); b->startIndependent(&context,[](const QByteArray&) {});
    ab.clear(); ba.clear(); check(a->write("p") && a->writeIndependent("i") && a->poll(20,2,toB)==2,"two ordering domains setup");
    for (const auto& packet : ab) check(b->receiveDatagram(view(packet),21,toA),"ordering domain data rejected");
    b->poll(22,2,[](const QByteArray&) { return false; }); check(b->stats().ackTransmitDeferrals>0,"blocked ACK pair was not retained");
    b->poll(23,2,toA); check(ba.size()==1,"ordering-domain ACKs were not combined");
    const auto pair=decodeCompactUdpMux(view(ba[0]),7,1,2,3,reverse);
    check(pair && pair->kind==UdpMuxKind::AckPair && pair->payload.size()==24 && b->stats().ackPairs==1,"ACK pair layout/counter");
    auto payload=QByteArray(reinterpret_cast<const char*>(pair->payload.data()),pair->payload.size());
    qToBigEndian(quint64{99999},reinterpret_cast<uchar*>(payload.data()+8));
    const auto invalid=encodeCompactUdpMux({UdpMuxKind::AckPair,UdpMuxChannel::Control,1,2,3,pair->sequence,view(payload)},7,reverse);
    const auto queued=a->bytesToWrite();
    check(a->receiveDatagram(view(invalid),24,toB) && a->bytesToWrite()==queued && a->stats().invalidAckWindows==1,
        "invalid combined window partially freed another ordering domain");
    check(a->receiveDatagram(view(ba[0]),25,toB) && a->bytesToWrite()==0 && a->independentBytesToWrite()==0,"valid pair did not drain both domains");
    check(a->stats().compactBytesSaved>0 && b->stats().compactBytesSaved>0,"compact savings not observable");
    a->retire(); b->retire();
    UdpStreamConnection::Options full; full.compactFrames=false;
    a=std::make_shared<UdpStreamConnection>(1,2,4,UdpMuxChannel::Control,key,"local",false,UdpStreamConnection::Options{},8);
    b=std::make_shared<UdpStreamConnection>(1,2,4,UdpMuxChannel::Control,key,"local",true,full,8);
    ab.clear(); ba.clear(); a->poll(30,1,toB);
    auto opening=decodeUdpMux(ab.front(),1,key); check(opening.has_value(),"diagnostic full Open invalid"); b->receive(*opening,31,toA);
    auto fallback=decodeUdpMux(ba.front(),1,key); check(fallback && fallback->payload.isEmpty(),"diagnostic full-only peer did not omit its handle");
    a->receive(*fallback,32,toB); ab.clear();
    check(!a->isConnected() && a->compactHandle()!=0,"compact stream silently downgraded to unbound full framing");
    a->retire(); b->retire();
    std::cout << "udp_efficiency compact_envelope_bytes=32 saved_per_datagram=26 piggyback=checked ack_pair=atomic sparse_ack=immediate\n";
}

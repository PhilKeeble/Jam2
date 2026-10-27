#include "UdpStreamConnection.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QtEndian>
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

namespace {
using namespace jam2::application;
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
struct Variant { const char* name; bool direct, aggregate, views; bool pack=false, compact=false, combine=false, fast=false; };
constexpr std::array<Variant,10> variants{{
    {"reference",false,false,false}, {"in-order",true,false,false},
    {"views",false,false,true}, {"ack-aggregation",false,true,false}, {"combined",true,true,true}, {"tail-packing",true,true,true,true},
    {"compact",true,true,true,true,true}, {"ack-combining",true,true,true,true,false,true},
    {"selective-repair",true,true,true,true,false,false,true}, {"all-transport",true,true,true,true,true,true,true}}};
struct Result { qint64 ns; quint64 bytes, data, acks, retransmissions, direct, insertions, packed, avoided, completed, fast; };

Result run(Variant variant,bool impaired,qsizetype writeBytes)
{
    const qsizetype total=writeBytes==73 ? 64*1024 : 2*1024*1024;
    QByteArray source(total,'\0'), received;
    for (qsizetype index=0;index<source.size();++index) source[index]=static_cast<char>(index%251);
    received.reserve(total);
    const QByteArray key(16,'k');
    UdpStreamConnection::Options options{variant.direct,variant.aggregate,variant.pack};
    options.fastRecovery=variant.fast; options.combineAcks=variant.combine; options.compactFrames=variant.compact;
    auto sender=std::make_shared<UdpStreamConnection>(1,2,3,UdpMuxChannel::Asset,key,"local",false,options,variant.compact ? 7 : 0);
    auto receiver=std::make_shared<UdpStreamConnection>(1,2,3,UdpMuxChannel::Asset,key,"local",true,options,variant.compact ? 7 : 0);
    QObject context;
    receiver->start(&context,[&](const QByteArray& bytes) { received+=bytes; },{});
    quint64 wireBytes=0,dataPackets=0,ackPackets=0;
    std::vector<QByteArray> data,acks;
    std::map<quint64,unsigned> transmissions;
    const auto toReceiver=[&](const QByteArray& bytes) { data.push_back(bytes); wireBytes+=bytes.size()+28; ++dataPackets; return true; };
    const auto toSender=[&](const QByteArray& bytes) { acks.push_back(bytes); wireBytes+=bytes.size()+28; ++ackPackets; return true; };
    auto decode=[&](const QByteArray& bytes,auto& target,quint64 now,const auto& transmit) {
        const auto packet=std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.constData()),static_cast<std::size_t>(bytes.size()));
        if (isCompactUdpMux(packet)) require(target->receiveDatagram(packet,now,transmit),"benchmark compact decoder rejected packet");
        else if (variant.views) {
            const auto frame=decodeUdpMuxView({reinterpret_cast<const std::uint8_t*>(bytes.constData()),static_cast<std::size_t>(bytes.size())},1,key);
            require(frame.has_value(),"benchmark view rejected authenticated packet");
            target->receiveView(*frame,now,transmit);
        } else {
            const auto frame=decodeUdpMux(bytes,1,key);
            require(frame.has_value(),"benchmark owning decoder rejected authenticated packet");
            target->receive(*frame,now,transmit);
        }
    };
    sender->poll(1,1,toReceiver);
    for (const auto& bytes : data) decode(bytes,receiver,2,toSender);
    data.clear(); for (const auto& bytes : acks) decode(bytes,sender,3,toReceiver); acks.clear();
    require(sender->isConnected(),"benchmark stream handshake"); wireBytes=dataPackets=ackPackets=0;
    qsizetype queued=0; unsigned turn=0; quint64 completed=0;
    QElapsedTimer elapsed; elapsed.start();
    for (quint64 now=1;now<60000000 && (received.size()!=total || sender->bytesToWrite()!=0);now+=1000,++turn) {
        for (int write=0;write<(writeBytes==73 ? 32 : 1) && queued<total && sender->bytesToWrite()<=128*1024;++write) {
            const auto chunk=source.mid(queued,writeBytes);
            require(sender->write(chunk),"benchmark sender rejected bounded work"); queued+=chunk.size();
        }
        sender->poll(now,32,toReceiver);
        if (impaired && turn%7==0) std::reverse(data.begin(),data.end());
        for (const auto& bytes : data) {
            // The impairment boundary peeks only the routing header. The
            // actual production receiver below authenticates every delivery.
            require(bytes.size()>=32,"benchmark transmit truncated");
            const auto sequence=qFromBigEndian<quint64>(reinterpret_cast<const uchar*>(bytes.constData()+((static_cast<unsigned char>(bytes[5])&0x80) ? 8 : 32)));
            const auto count=impaired ? ++transmissions[sequence] : 0;
            if (impaired && count==1 && sequence%17==0) continue;
            decode(bytes,receiver,now,toSender);
            if (impaired && count==1 && sequence%19==0) decode(bytes,receiver,now,toSender);
        }
        data.clear();
        receiver->poll(now,32,toSender); // Flush ACKs at this batch boundary, with no timer delay.
        // Drop the same ACK turns, rather than favoring the smaller packet count.
        if (!impaired || turn%23!=0) for (const auto& bytes : acks) decode(bytes,sender,now,toReceiver);
        acks.clear();
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        require(sender->failure().isEmpty(),"benchmark reliable stream timed out");
        completed=now;
    }
    const auto ns=elapsed.nsecsElapsed();
    require(received==source && sender->bytesToWrite()==0,"benchmark lost, duplicated or reordered application bytes");
    const auto tx=sender->stats(),rx=receiver->stats();
    require(impaired ? tx.retransmitted>0 : tx.retransmitted==0,"benchmark impairment coverage invalid");
    require(!variant.direct || rx.in_order_direct>0,"benchmark skipped in-order fast path");
    sender->retire(); receiver->retire();
    if (variant.pack && writeBytes==73) require(tx.fragments_avoided>0,"packing workload skipped queued tail");
    return {ns,wireBytes,dataPackets,ackPackets,tx.retransmitted,rx.in_order_direct,rx.reorder_insertions,tx.tail_bytes_packed,tx.fragments_avoided,completed,tx.fast_retransmitted};
}
struct DuplexResult { qint64 ns; quint64 bytes,packets,pairs,piggybacks; };
DuplexResult duplex(Variant variant)
{
    const QByteArray key(16,'k'),primary(128,'p'),independent(64,'i');
    UdpStreamConnection::Options options; options.compactFrames=variant.compact; options.combineAcks=variant.combine; options.fastRecovery=variant.fast;
    auto a=std::make_shared<UdpStreamConnection>(1,2,3,UdpMuxChannel::Control,key,"local",false,options,variant.compact ? 7 : 0);
    auto b=std::make_shared<UdpStreamConnection>(1,2,3,UdpMuxChannel::Control,key,"local",true,options,variant.compact ? 7 : 0);
    std::vector<QByteArray> ab,ba; quint64 bytes=0,packets=0,delivered=0; QObject context;
    const auto toB=[&](const QByteArray& packet) { ab.push_back(packet); bytes+=packet.size()+28; ++packets; return true; };
    const auto toA=[&](const QByteArray& packet) { ba.push_back(packet); bytes+=packet.size()+28; ++packets; return true; };
    const auto deliver=[&](auto& pending,auto& target,quint64 now,const auto& transmit) {
        for (const auto& packet : pending) {
            const auto view=std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(packet.constData()),packet.size());
            if (isCompactUdpMux(view)) require(target->receiveDatagram(view,now,transmit),"duplex compact decode");
            else { const auto frame=decodeUdpMuxView(view,1,key); require(frame.has_value(),"duplex full decode"); target->receiveView(*frame,now,transmit); }
        }
        pending.clear();
    };
    a->poll(1,2,toB); deliver(ab,b,2,toA); deliver(ba,a,3,toB); bytes=packets=0;
    const auto received=[&](const QByteArray& data) { delivered+=data.size(); };
    a->start(&context,received,{}); b->start(&context,received,{}); a->startIndependent(&context,received); b->startIndependent(&context,received);
    QElapsedTimer timer; timer.start();
    for (quint64 round=0;round<1000;++round) {
        require(a->write(primary) && b->write(primary) && a->writeIndependent(independent) && b->writeIndependent(independent),"duplex enqueue");
        const auto now=10+round*1000;
        a->poll(now,4,toB); deliver(ab,b,now+1,toA);
        b->poll(now+2,4,toA); deliver(ba,a,now+3,toB);
        a->poll(now+4,4,toB); deliver(ab,b,now+5,toA);
        QCoreApplication::processEvents();
        require(a->bytesToWrite()==0 && b->bytesToWrite()==0 && a->independentBytesToWrite()==0 && b->independentBytesToWrite()==0,"duplex ACKs did not finish in same turn");
    }
    const auto elapsed=timer.nsecsElapsed(); const auto as=a->stats(),bs=b->stats();
    require(delivered==384000,"duplex payload coverage");
    if (variant.combine) require(as.ackPairs+bs.ackPairs==1000 && as.piggybackedAcks+bs.piggybackedAcks==2000 && packets==5000,"duplex combination did not reduce datagrams");
    else require(packets==8000,"duplex reference packet count");
    a->retire(); b->retire(); return {elapsed,bytes,packets,as.ackPairs+bs.ackPairs,as.piggybackedAcks+bs.piggybackedAcks};
}
}
int main(int argc,char** argv)
{
    QCoreApplication app(argc,argv);
    try {
        for (const auto writeBytes : {qsizetype{65536},qsizetype{24614},qsizetype{73}}) for (const bool impaired : {false,true}) {
            std::array<std::vector<Result>,variants.size()> results;
            const unsigned begin=writeBytes==65536 ? 0 : 4, count=static_cast<unsigned>(variants.size())-begin;
            for (unsigned round=0;round<7;++round) for (unsigned turn=0;turn<count;++turn) {
                const auto index=begin+(round+turn)%count; results[index].push_back(run(variants[index],impaired,writeBytes));
            }
            for (unsigned index=begin;index<variants.size();++index) {
                auto& values=results[index];
                std::sort(values.begin(),values.end(),[](const auto& a,const auto& b) { return a.ns<b.ns; });
                const auto& median=values[values.size()/2];
                std::cout << "workload=" << (impaired ? "loss-dup-reorder" : "ordered") << " variant=" << variants[index].name
                    << " samples=" << values.size() << " write_bytes=" << writeBytes << " payload_bytes=" << (writeBytes==73 ? 64*1024 : 2*1024*1024) << " median_us=" << median.ns/1000.0
                    << " p95_us=" << values.back().ns/1000.0 << " modeled_ipv4_bytes=" << median.bytes
                    << " data_packets=" << median.data << " ack_packets=" << median.acks << " retransmissions=" << median.retransmissions
                    << " in_order_direct=" << median.direct << " reorder_insertions=" << median.insertions
                    << " tail_bytes_packed=" << median.packed << " fragments_avoided=" << median.avoided
                    << " simulated_completion_us=" << median.completed << " fast_retransmissions=" << median.fast << '\n';
            }
        }
        std::array<std::vector<DuplexResult>,4> duplexResults; const std::array<unsigned,4> indexes{5,6,7,9};
        for (unsigned round=0;round<7;++round) for (unsigned order=0;order<4;++order) {
            const auto index=(round+order)%4; duplexResults[index].push_back(duplex(variants[indexes[index]]));
        }
        for (unsigned i=0;i<4;++i) {
            auto& samples=duplexResults[i]; std::sort(samples.begin(),samples.end(),[](auto a,auto b) { return a.ns<b.ns; }); const auto median=samples[3];
            std::cout << "workload=duplex-control-readiness variant=" << variants[indexes[i]].name << " samples=7 exchanges=1000 median_us=" << median.ns/1000.0
                << " modeled_ipv4_bytes=" << median.bytes << " datagrams=" << median.packets << " ack_pairs=" << median.pairs << " piggybacked_acks=" << median.piggybacks << '\n';
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

#include "UdpCandidateNetwork.hpp"
#include "UdpMultiplexTransport.hpp"
#include "ControlClient.hpp"
#include "ControlServer.hpp"
#include "PeerName.hpp"
#include "PeerBootstrap.hpp"
#include "protocol.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QThread>
#include <QUdpSocket>
#include <QtEndian>
#include <array>
#include <iostream>
#include <memory>
#include <set>

using namespace jam2::application;
namespace {
void require(bool okay,const char* why) { if (!okay) throw std::runtime_error(why); }
QString token(int i) { return QString(32,QChar('1'+i)); }
quint64 id(int i) { return *jam2::control_protocol::peerIdFromToken(token(i)); }
QString nonce() { return QString::fromLatin1(jam2::control_protocol::randomNonce().toHex()); }
}
int main(int argc,char** argv)
{
    QCoreApplication app(argc,argv);
    try {
        const bool stun=app.arguments().contains("--stun");
        QUdpSocket discovery;
        int stunRequests=0;
        if (stun) {
            require(discovery.bind(QHostAddress::LocalHost,0),"bind fake STUN discovery");
            QObject::connect(&discovery,&QUdpSocket::readyRead,[&] {
                while (discovery.hasPendingDatagrams()) {
                    QByteArray request(512,Qt::Uninitialized); QHostAddress source; quint16 port;
                    const auto count=discovery.readDatagram(request.data(),request.size(),&source,&port);
                    require(count==20,"STUN request must be a binding request"); ++stunRequests;
                    QByteArray response(32,'\0');
                    qToBigEndian<quint16>(0x0101,response.data()); qToBigEndian<quint16>(12,response.data()+2);
                    qToBigEndian<quint32>(0x2112a442,response.data()+4);
                    std::copy_n(request.data()+8,12,response.data()+8);
                    qToBigEndian<quint16>(0x0020,response.data()+20); qToBigEndian<quint16>(8,response.data()+22);
                    response[25]=1; qToBigEndian<quint16>(port^0x2112,response.data()+26);
                    qToBigEndian<quint32>(source.toIPv4Address()^0x2112a442,response.data()+28);
                    require(discovery.writeDatagram(response,source,port)==response.size(),"reply to STUN discovery");
                }
            });
        }
        const QString session="0123456789abcdef",key="0123456789abcdef0123456789abcdef";
        const auto keyBytes=QByteArray::fromHex(key.toLatin1());
        std::array<std::uint8_t,16> nativeKey{}; std::copy(keyBytes.begin(),keyBytes.end(),nativeKey.begin());
        std::array<std::unique_ptr<UdpMultiplexTransport>,4> peers;
        std::array<std::unique_ptr<ControlClient>,4> clients;
        std::array<bool,4> attached{};
        std::array<int,4> repaired{};
        ControlServer host;
        require(host.listenViaTransport(session,key),"configure creator typed control");
        std::set<QString> authenticated;
        int failures=0;
        bool holdAdmission=true;
        for (int i=0;i<4;++i) {
            peers[i]=std::make_unique<UdpMultiplexTransport>(); clients[i]=std::make_unique<ControlClient>();
            clients[i]->setPeerName(*PeerName::fromInput(i==0 ? "Alice" : i==1 ? "Alex" : i==2 ? "Élodie" : "Alex"));
            peers[i]->onPairFailure=[&](const QString&,const QString&,const QString& reason) { ++failures; std::cerr << reason.toStdString() << '\n'; };
            peers[i]->onLinkReady=[&,i](const QString& peer) { if (i && peer==token(0)) peers[i]->openStream(peer,UdpMuxChannel::Control); };
            peers[i]->onStreamReady=[&,i](const QString& peer,UdpMuxChannel channel,ControlConnection::Pointer connection,bool incoming) {
                require(channel==UdpMuxChannel::Control,"unexpected logical channel");
                if (i==0 && incoming) { host.acceptTransport(connection); return; }
                require(i && !incoming && peer==token(0),"unexpected direct logical stream");
                const auto observed=peers[i]->observedEndpoint(peer); require(bool(observed),"direct observed endpoint missing");
                clients[i]->connectViaTransport(connection,session,key,token(i),QString::fromStdString(jam2::endpoint_to_string(*observed)));
            };
            peers[i]->onLinkStalled=[&,i](const QString& peer) {
                if (i && peer!=token(0)) { ++repaired[i]; clients[i]->send({{"type","session.udp.recover"},{"peer",peer}}); }
            };
        }
        host.onAuthenticated=[&](const QString& peer,const ControlServer::AuthenticatedPeer& message) {
            require(message.name.text()==(peer==token(2) ? QString::fromUtf8("Élodie") : QStringLiteral("Alex")),"join name not authenticated");
            if (holdAdmission || !authenticated.insert(peer).second) return;
            peers[0]->admitPeer(peer);
            for (const auto& other : authenticated) if (other!=peer) peers[0]->introduce(peer,other,nonce());
        };
        host.onMessage=[&](const QString& peer,const QJsonObject& message) {
            if (message.value("type")=="session.udp.recover") peers[0]->introduce(peer,message.value("peer").toString(),nonce());
        };
        UdpMultiplexTransport::Config creator;
        creator.creator=true; creator.strategy=stun ? jam2::ConnectionStrategy::Stun : jam2::ConnectionStrategy::Direct;
        creator.stunServer={"127.0.0.1",discovery.localPort()}; creator.bindHost="127.0.0.1";
        creator.socketBuffers={32768,65536};
        creator.pendingAdmissionTimeoutMs=1000;
        creator.localToken=token(0); creator.sessionHex=session; creator.keyHex=key; creator.linkRepairSilenceMs=1000;
        QString error;
        { UdpMultiplexTransport invalid; auto bad=creator; bad.socketBuffers.sendBytes=0;
          require(!invalid.start(bad,error) && !invalid.snapshot().running,"invalid socket buffer configuration retained resources"); }
        require(peers[0]->start(creator,error),error.toUtf8().constData());
        QElapsedTimer ready; ready.start();
        while (!peers[0]->bootstrapEndpoint() && ready.elapsed()<3000) { QCoreApplication::processEvents(); QThread::msleep(1); }
        const auto actual=peers[0]->bootstrapEndpoint(); require(bool(actual),"forwarded creator port not ready");
        // macOS only binds assigned loopback addresses. Model peers without
        // a shared LAN explicitly, keeping every public path inside the proxy
        // on all platforms instead of relying on unassigned 127.0.0.x aliases.
        CandidateNetwork network(actual->port, CandidateNetwork::IsolatedHosts{0x0123456789abcdefULL, keyBytes});
        network.impairMultiplex=false;
        std::set<std::pair<int,int>> audioPaths;
        auto pump=[&] {
            network.pump(); QCoreApplication::processEvents(QEventLoop::AllEvents,5);
            for (int i=0;i<4;++i) if (attached[i]) {
                std::array<std::uint8_t,jam2::NetworkDatagramTransport::kReceiveBufferBytes> buffer;
                for (int work=0;work<32;++work) {
                    const auto packet=peers[i]->receiveFor(buffer,0,nullptr); if (!packet) break;
                    const auto parsed=jam2::protocol::parse_packet(std::span(buffer).first(packet->size),nativeKey,0x0123456789abcdefULL,jam2::NetworkAudioFormat::Pcm24Mono);
                    require(bool(parsed),"native bytes altered by direct multiplex");
                    if (parsed.header.type==jam2::protocol::PacketType::Audio) {
                        const int sender=static_cast<int>(parsed.header.timing_value)-1;
                        require(sender>=0 && sender<4 && buffer[jam2::protocol::kHeaderSize]==sender+1,"native audio payload altered");
                        audioPaths.insert({sender,i});
                    }
                }
            }
        };
        auto until=[&](auto predicate,const char* why,int timeout=15000) {
            QElapsedTimer timer; timer.start();
            do { pump(); if (predicate()) return; QThread::msleep(1); } while (timer.elapsed()<timeout);
            for (int i=0;i<4;++i) for (const auto& link : peers[i]->snapshot().links)
                std::cerr << i << ' ' << link.peer.toStdString() << " phase=" << static_cast<int>(link.phase) << " attempt=" << link.attempt << " hello=" << link.receivedHello << " ack=" << link.receivedAck << '\n';
            throw std::runtime_error(why);
        };
        QUdpSocket pending;
        require(pending.bind(QHostAddress::LocalHost,0),"bind unauthenticated attempt source");
        for (int attempt=0;attempt<32;++attempt) {
            PeerBootstrapMessage message; message.kind=PeerBootstrapKind::Join;
            message.session=0x0123456789abcdefULL;
            message.sender=QStringLiteral("%1cccccccccccccccc").arg(100+attempt,16,16,QChar('0'));
            message.request=nonce(); const auto packet=encodePeerBootstrap(message,keyBytes);
            require(pending.writeDatagram(packet,QHostAddress::LocalHost,actual->port)==packet.size(),"send bounded pending probe");
        }
        until([&] { const auto state=peers[0]->snapshot();
            return state.links.size()==8 && state.bootstrapRejected>=24;
        },"pending bootstrap attempts exceeded their limit");
        until([&] { return peers[0]->snapshot().links.empty(); },
            "failed unauthenticated attempts were retained",7000);
        auto held=creator; held.creator=false; held.localToken=token(1); held.server={"127.0.0.1",network.port()};
        require(peers[1]->start(held,error),error.toUtf8().constData());
        until([&] { return clients[1]->isConnected(); },"pending admission did not reach control authentication");
        until([&] { return peers[0]->snapshot().pendingAdmissionTimeouts==1; },
            "unadmitted active physical link never expired");
        clients[1]->close(); peers[1]->stop(); holdAdmission=false;
        require(failures==8,"unproved joins did not report exactly eight terminal proof failures");
        failures=0;
        for (int i=1;i<4;++i) {
            auto join=creator; join.creator=false; join.localToken=token(i); join.server={"127.0.0.1",network.port()};
            require(peers[i]->start(join,error),error.toUtf8().constData());
            until([&] { return authenticated.contains(token(i)); },"direct authenticated join failed");
            if (i==1) for (int j=0;j<2;++j) { peers[j]->receiverAttached(); attached[j]=true; }
            else { peers[i]->receiverAttached(); attached[i]=true; }
        }
        until([&] {
            for (int i=0;i<4;++i) for (int j=0;j<4;++j) if (i!=j && !peers[i]->peerEndpoint(id(j))) return false;
            return true;
        },"direct four-peer mesh failed to converge after socket rotation");
        require(network.isolatedCandidates>0, "isolated public candidate path was not exercised");
        require(network.dropped>0 && failures==0,"first failed socket attempts were not recovered");
        std::array<std::map<QString,std::string>,4> original;
        for (int i=0;i<4;++i) {
            const auto state=peers[i]->snapshot();
            require(state.signalSendBufferBytes>=32768 && state.signalReceiveBufferBytes>=65536,"bootstrap socket ignored explicit buffer tuning");
            require(state.physicalSocketCount==static_cast<quint64>(i==0 ? 1 : 3),"shared creator socket was duplicated");
            require(!state.usesRendezvous,"direct transport used an external rendezvous");
            for (const auto& link : state.links) {
                require(link.sendBufferBytes>=32768 && link.receiveBufferBytes>=65536,"rotatable peer socket ignored explicit buffer tuning");
                require(link.route == QStringLiteral("public"), "isolated peers bypassed the impaired public path through LAN");
                original[i][link.peer]=jam2::endpoint_to_string(link.local);
                if (i && link.peer!=token(0)) require(link.attempt>=2,"failed peer socket pair was not rotated");
            }
        }
        auto sendAudio=[&] {
            for (int i=0;i<4;++i) {
                std::array<std::uint8_t,192> payload{}; payload[0]=static_cast<std::uint8_t>(i+1);
                const auto packet=jam2::protocol::encode_packet({jam2::protocol::PacketType::Audio,0x0123456789abcdefULL,1,static_cast<quint64>(i+1)},payload,nativeKey,jam2::NetworkAudioFormat::Pcm24Mono);
                for (int j=0;j<4;++j) if (i!=j) peers[i]->sendToPeer(id(j),{},packet);
            }
        };
        sendAudio(); until([&] { return audioPaths.size()==12; },"not all twelve native audio paths delivered");
        network.blockCurrentPair(token(1),token(2)); audioPaths.clear();
        QElapsedTimer traffic; traffic.start(); qint64 nextAudio=0;
        until([&] {
            if (traffic.elapsed()>=nextAudio) { sendAudio(); nextAudio=traffic.elapsed()+50; }
            const auto one=peers[1]->snapshot(),two=peers[2]->snapshot();
            const auto changed=[&](const auto& state,const QString& partner,int index) {
                for (const auto& link : state.links) if (link.peer==partner)
                    return link.phase==UdpMultiplexTransport::LinkPhase::Active && jam2::endpoint_to_string(link.local)!=original[index].at(partner);
                return false;
            };
            return changed(one,token(2),1) && changed(two,token(1),2);
        },"only-failed-pair direct recovery did not rotate sockets",20000);
        for (int i=0;i<4;++i) for (const auto& link : peers[i]->snapshot().links) {
            require(link.sendBufferBytes>=32768 && link.receiveBufferBytes>=65536,
                "socket rotation lost the requested OS buffer tuning");
            if ((i==1 && link.peer==token(2)) || (i==2 && link.peer==token(1))) continue;
            require(jam2::endpoint_to_string(link.local)==original[i].at(link.peer),"failed B-C recovery rotated a healthy socket");
        }
        require(stunRequests==(stun ? 1 : 0),"STUN must only discover the creator endpoint once");
        const auto finalEndpoint=peers[0]->bootstrapEndpoint();
        require(finalEndpoint && finalEndpoint->host==actual->host && finalEndpoint->port==actual->port,"creator forwarded port changed");
        audioPaths.clear(); sendAudio(); until([&] { return audioPaths.size()==12; },"native audio paths not restored after recovery");
        for (int i=0;i<4;++i) if (attached[i]) peers[i]->receiverDetached();
        for (int i=3;i>=0;--i) peers[i]->stop();
        std::cout << "four-peer direct multiplex: named authentication, no rendezvous, twelve native paths and isolated socket rotation passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}

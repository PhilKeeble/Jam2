#include "RendezvousWire.hpp"
#include "ControlProtocol.hpp"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QUdpSocket>
#include <array>
#include <deque>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace rv = jam2::application::rendezvous_wire;
namespace {
void require(bool okay,const char* detail) { if (!okay) throw std::runtime_error(detail); }
struct Peer {
    QUdpSocket socket;
    QString token=jam2::control_protocol::randomPeerToken();
    std::deque<rv::Message> pending;
};
class Session {
public:
    QHostAddress server;
    quint16 port;
    QString id=QString::fromLatin1(jam2::control_protocol::randomNonce().first(8).toHex());
    QString key=QString::fromLatin1(jam2::control_protocol::randomNonce().toHex());
    std::array<Peer,4> peers;
    Session(const QString& host,quint16 port):server(host),port(port) {
        require(!server.isNull() && port!=0,"invalid server test endpoint");
        for (auto& peer:peers) require(peer.socket.bind(QHostAddress::LocalHost,0),"bind protocol test socket");
    }
    rv::Message message(int index,rv::Kind kind) const {
        rv::Message m; m.kind=kind; m.session=id; m.key=key; m.peer=peers[index].token; return m;
    }
    void send(int index,const rv::Message& message) {
        const auto bytes=rv::encode(message); require(!bytes.isEmpty(),"protocol fixture encoding failed");
        require(peers[index].socket.writeDatagram(bytes,server,port)==bytes.size(),"send binary rendezvous packet");
    }
    rv::Message receive(int index,rv::Kind kind,const QString& nonce={},std::function<void()> retry={}) {
        auto& peer=peers[index]; QElapsedTimer timeout; timeout.start(); qint64 nextRetry=250;
        while (timeout.elapsed()<5000) {
            if (retry && timeout.elapsed()>=nextRetry) { retry(); nextRetry=timeout.elapsed()+250; }
            for (auto it=peer.pending.begin();it!=peer.pending.end();++it) if (it->kind==kind && (nonce.isEmpty() || it->nonce==nonce)) {
                const auto result=*it; peer.pending.erase(it); return result;
            }
            while (peer.socket.hasPendingDatagrams()) {
                QByteArray bytes(static_cast<qsizetype>(peer.socket.pendingDatagramSize()),'\0');
                require(peer.socket.readDatagram(bytes.data(),bytes.size())==bytes.size(),"read rendezvous reply");
                const auto m=rv::decode(bytes);
                require(m && !rv::request(m->kind) && m->session==id && m->to==peer.token,"server emitted invalid or misbound binary reply");
                if (m->kind==rv::Kind::Connected || m->kind==rv::Kind::Failed) {
                    auto ack=message(index,rv::Kind::PairAck); ack.nonce=m->nonce;
                    ack.attempt=m->attempt; ack.introduction=m->introduction; send(index,ack);
                }
                peer.pending.push_back(*m);
            }
            QCoreApplication::processEvents(); QThread::msleep(1);
        }
        std::cerr << "timeout peer=" << index << " kind=" << static_cast<int>(kind) << '\n';
        throw std::runtime_error("binary server protocol did not complete");
    }
};
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        require(argc==3,"requires server host and UDP port");
        bool valid=false; const auto port=QString::fromLocal8Bit(argv[2]).toUShort(&valid); require(valid,"invalid port");
        Session session(QString::fromLocal8Bit(argv[1]),port);
        auto create=session.message(0,rv::Kind::Create); create.directoryRevision=1; create.maximumPeers=3;
        session.send(0,create);
        const auto created=session.receive(0,rv::Kind::Created);
        require(created.creator==session.peers[0].token && created.directoryRevision==1,"registration lost creator/revision");
        // The actual UDP receive loop must reject the retired JSON format and
        // unknown binary versions without entering product dispatch.
        auto bad=rv::encode(session.message(3,rv::Kind::Join)); bad[4]=0;
        require(session.peers[3].socket.writeDatagram(bad,session.server,port)==bad.size(),"send old binary version");
        const QByteArray json=QByteArrayLiteral("{\"v\":1,\"type\":\"jam_join\"}");
        require(session.peers[3].socket.writeDatagram(json,session.server,port)==json.size(),"send retired JSON format");
        require(!session.peers[3].socket.waitForReadyRead(150),"retired/invalid format received a reply");
        auto wrong=session.message(3,rv::Kind::Join); wrong.key=QString(32,'0'); session.send(3,wrong);
        require(session.receive(3,rv::Kind::Error).error==rv::Error::Credentials,"wrong session key was not rejected by typed error");
        for (int i=1;i<4;++i) {
            session.send(i,session.message(i,rv::Kind::Join));
            require(session.receive(i,rv::Kind::Wait).creator==session.peers[0].token,"join omitted creator identity");
            require(session.receive(0,rv::Kind::JoinRequest).peer==session.peers[i].token,"join request changed peer identity");
            auto admit=session.message(0,rv::Kind::Admit); admit.to=session.peers[i].token; admit.memberSequence=1;
            session.send(0,admit);
            require(session.receive(0,rv::Kind::AdmitAck).peer==session.peers[i].token,"membership ack changed identity");
        }
        int pairs=0;
        for (int a=0;a<4;++a) for (int b=a+1;b<4;++b) {
            auto pair=session.message(0,rv::Kind::Pair); pair.a=session.peers[a].token; pair.b=session.peers[b].token;
            pair.nonce=jam2::control_protocol::randomPeerToken(); session.send(0,pair);
            const auto prepareA=session.receive(a,rv::Kind::Prepare,pair.nonce),prepareB=session.receive(b,rv::Kind::Prepare,pair.nonce);
            require(prepareA.introduction==prepareB.introduction && prepareA.attempt==1 && prepareA.peer==pair.b && prepareB.peer==pair.a,"pair preparation binding differs");
            for (const int i:{a,b}) {
                auto candidate=session.message(i,rv::Kind::Candidate); candidate.nonce=pair.nonce;
                candidate.attempt=1; candidate.introduction=prepareA.introduction;
                session.send(i,candidate);
            }
            for (const int i:{a,b}) {
                const auto offer=session.receive(i,rv::Kind::Offer,pair.nonce);
                require(offer.endpoint.valid() && offer.observed.valid() && !offer.lan,"public candidate endpoints lost");
                auto ready=session.message(i,rv::Kind::Ready); ready.nonce=pair.nonce; ready.attempt=1; ready.introduction=prepareA.introduction;
                session.send(i,ready);
            }
            for (const int i:{a,b}) {
                session.receive(i,rv::Kind::Go,pair.nonce);
                auto outcome=session.message(i,rv::Kind::Outcome); outcome.nonce=pair.nonce; outcome.attempt=1; outcome.introduction=prepareA.introduction; outcome.success=true;
                session.send(i,outcome);
            }
            for (const int i:{a,b}) session.receive(i,rv::Kind::Connected,pair.nonce);
            if (a!=0) session.receive(0,rv::Kind::Connected,pair.nonce);
            for (const int i: {0,a,b}) {
                auto ack=session.message(i,rv::Kind::PairAck); ack.nonce=pair.nonce; ack.attempt=1; ack.introduction=prepareA.introduction;
                session.send(i,ack);
            }
            ++pairs;
        }
        require(pairs==6,"did not exercise exactly four peers and all six introductions");
        auto pause=session.message(0,rv::Kind::Pause); pause.directoryRevision=2;
        session.send(0,pause); require(session.receive(0,rv::Kind::Paused,{},[&] { session.send(0,pause); }).directoryRevision==2,"directory pause lost revision");
        auto leave=session.message(0,rv::Kind::Leave); leave.directoryRevision=3;
        session.send(0,leave); session.receive(0,rv::Kind::Paused,{},[&] { session.send(0,leave); });
        std::cout << "four-peer binary rendezvous over UDP: six introductions, key rejection, obsolete-format rejection and shutdown passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

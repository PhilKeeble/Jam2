#include "ControlProtocol.hpp"
#include "ControlClient.hpp"
#include "ControlServer.hpp"
#include "RendezvousWire.hpp"
#include "ControlByteLink.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QElapsedTimer>
#include <QThread>
#include <array>
#include <iostream>
#include <stdexcept>

using namespace jam2::control_protocol;
namespace rv = jam2::application::rendezvous_wire;
namespace {
void require(bool value, const char* detail) { if (!value) throw std::runtime_error(detail); }
template<class Predicate> void until(Predicate predicate) {
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < 3000) { QCoreApplication::processEvents(); QThread::msleep(1); }
    require(predicate(), "authentication boundary timed out");
}
const QString session = QStringLiteral("0123456789abcdef");
const QString key = QString(32, '9');
const QString peer = QString(32, '1');
void handshakeCodec() {
    Proof proof{QByteArray(16,'c'), peer, QByteArray(16,'p'), HandshakeChannel::Control,
        rv::Endpoint::from(QStringLiteral("127.0.0.1"),50000), *jam2::application::PeerName::fromInput("Alex")};
    Proof asset = proof; asset.channel = HandshakeChannel::Asset; asset.endpoint = {}; asset.name = {};
    const std::array<Handshake,4> messages{Challenge{session,QByteArray(16,'s')}, proof, Accepted{QByteArray(16,'p')}, Rejected{HandshakeRejection::InvalidProof}};
    const std::array<qsizetype,4> sizes{30,66,22,7};
    for (std::size_t i=0;i<messages.size();++i) {
        auto frame = encodeHandshake(messages[i]);
        require(frame.size()==sizes[i], "binary handshake byte cost changed");
        QByteArray body; QString error;
        require(takeFrame(frame,body,error,kMaximumHandshakeBodyBytes)==TakeFrameResult::Ready && frame.isEmpty(), "binary handshake framing failed");
        Handshake decoded;
        require(decodeHandshake(body,decoded,error) && encodeHandshake(decoded)==encodeHandshake(messages[i]), "typed handshake lost fields");
        for (qsizetype end=0;end<body.size();++end)
            require(!decodeHandshake(body.first(end),decoded,error), "truncated handshake accepted");
        require(!decodeHandshake(body+'x',decoded,error), "trailing handshake data accepted");
        body[0]=4; require(!decodeHandshake(body,decoded,error), "old JSON-era handshake version accepted");
    }
    require(encodeHandshake(asset).size()==55, "asset proof repeats control-only metadata");
    auto bad=encodeHandshake(proof).mid(4); Handshake decoded; QString error;
    bad[50]=3; require(!decodeHandshake(bad,decoded,error), "unknown handshake channel accepted");
    bad=encodeHandshake(proof).mid(4); bad[57]=33;
    require(!decodeHandshake(bad,decoded,error), "oversized peer name accepted");
    bad=encodeHandshake(proof).mid(4); bad[58]='1';
    require(!decodeHandshake(bad,decoded,error), "invalid peer name accepted");
    bad=encodeHandshake(proof).mid(4); bad[1]=99;
    require(!decodeHandshake(bad,decoded,error), "unknown handshake operation accepted");
    require(!decodeHandshake(QByteArrayLiteral("{\"type\":\"hello.challenge\"}"),decoded,error), "JSON handshake fallback remains");
    proof.proof.clear(); require(encodeHandshake(proof).isEmpty(), "short authentication proof encoded");
    asset.name=*jam2::application::PeerName::fromInput("Alex");
    require(encodeHandshake(asset).isEmpty(), "asset proof accepted an unsolicited name");
    QByteArray prefix=QByteArray::fromHex("00000100"), body;
    require(takeFrame(prefix,body,error,kMaximumHandshakeBodyBytes)==TakeFrameResult::Invalid, "unauthenticated oversized length waits for body");
    require(!rv::Endpoint::fromText("127.0.0.1:65536").valid() && !rv::Endpoint::fromText("256.0.0.1:2").valid(), "invalid compact endpoint accepted");
}
rv::Message fixture(rv::Kind kind) {
    rv::Message m; m.kind=kind; m.session=session; m.key=key; m.peer=peer;
    m.to=QString(32,'2'); m.creator=QString(32,'3'); m.nonce=QString(32,'5'); m.a=QString(32,'6'); m.b=QString(32,'7');
    m.directoryRevision=0x0102030405060708ULL; m.memberSequence=0x1112131415161718ULL; m.introduction=0x2122232425262728ULL;
    m.maximumPeers=4; m.memberCount=3; m.attempt=2;
    m.endpoint=rv::Endpoint::from("203.0.113.5",40404); m.observed=rv::Endpoint::from("198.51.100.7",50000); m.local=rv::Endpoint::from("192.168.1.8",51000);
    m.lan=m.success=true; m.error=rv::Error::Unavailable; return m;
}
void rendezvousCodec(const QString& path) {
    QFile file(path); require(file.open(QIODevice::ReadOnly), "missing shared Go/C++ golden vectors");
    int count=0;
    while (!file.atEnd()) {
        const auto row=file.readLine().trimmed().split('\t'); require(row.size()==2,"invalid golden vector");
        const auto bytes=QByteArray::fromHex(row[1]); const auto decoded=rv::decode(bytes);
        require(decoded.has_value(), "rendezvous golden vector rejected");
        require(rv::encode(fixture(decoded->kind))==bytes, "C++ rendezvous bytes disagree with Go/shared specification");
        require(decoded->session==session && (rv::request(decoded->kind) ? decoded->key==key && decoded->peer==peer : decoded->to==QString(32,'2')), "rendezvous identity decoding failed");
        for (qsizetype end=0;end<bytes.size();++end) require(!rv::decode(bytes.first(end)), "truncated rendezvous datagram accepted");
        require(!rv::decode(bytes+'x'), "trailing rendezvous data accepted");
        auto wrong=bytes; wrong[4]=1; require(!rv::decode(wrong), "old rendezvous version accepted");
        wrong=bytes; wrong[5]=31; require(!rv::decode(wrong), "unknown rendezvous kind accepted");
        ++count;
    }
    require(count==29,"golden vectors omit a rendezvous message kind");
    auto m=fixture(rv::Kind::Create); m.nonce.clear();
    require(rv::encode(m).size()==63 && rv::decode(rv::encode(m))->nonce.isEmpty(), "ordinary registration repeats an absent nonce");
    m.maximumPeers=UINT32_MAX; require(rv::encode(m).isEmpty(), "negative/overflowed peer limit encoded");
    m=fixture(rv::Kind::Candidate); m.local={};
    require(rv::decode(rv::encode(m))->local.empty(), "absent LAN candidate was lost");
    m.local={1,0}; require(rv::encode(m).isEmpty(), "partial LAN endpoint encoded");
    m=fixture(rv::Kind::Outcome); auto bytes=rv::encode(m); bytes[bytes.size()-1]=2;
    require(!rv::decode(bytes), "noncanonical outcome boolean accepted");
    bytes=rv::encode(m); bytes[62]=0; require(!rv::decode(bytes), "zero introduction attempt accepted");
    m=fixture(rv::Kind::Offer); bytes=rv::encode(m); bytes[bytes.size()-1]=2;
    require(!rv::decode(bytes), "noncanonical LAN-route boolean accepted");
    m=fixture(rv::Kind::Error); bytes=rv::encode(m); bytes[30]=0;
    require(!rv::decode(bytes), "unknown error code accepted");
    for (quint8 rejected=0; rejected<=14; ++rejected) {
        m=fixture(rv::Kind::Error); m.error=rv::Error::Capacity; m.rejectedKind=rejected;
        if (rejected>=7) m.reference=QString(32,'5');
        bytes=rv::encode(m); auto decoded=rv::decode(bytes);
        require(bytes.size()==(rejected>=7 ? 48 : 32) && decoded &&
            decoded->rejectedKind==rejected && decoded->reference==m.reference,
            "typed error correlation failed");
        require(rv::errorText(rv::Error::Capacity).contains("STUN / Direct") &&
            rv::errorText(rv::Error::InvalidSchema).contains("update Jam2"),
            "actionable signaling diagnostic missing");
        bytes[31]=15; require(!rv::decode(bytes), "invalid rejected kind accepted");
    }
    require(!rv::decode(QByteArrayLiteral("{\"v\":1,\"type\":\"jam_join\"}")), "rendezvous JSON fallback remains");
}
// Feed actual ControlServer instances through byte links. Changed fields must
// fail the transcript proof, and an old proof must fail a fresh challenge.
void authenticationBinding() {
    Proof stale;
    for (int scenario=0;scenario<6;++scenario) {
        ControlServer server; auto [incoming,outgoing]=jam2::test::endpoints();
        outgoing->paused=true;
        require(server.listenViaTransport(session,key), "configure binary authentication test");
        server.acceptTransport(incoming);
        require(!outgoing->input.empty(), "server omitted binary challenge");
        auto frame=outgoing->input.front(); QByteArray body; QString error; Handshake hello;
        require(takeFrame(frame,body,error)==TakeFrameResult::Ready && decodeHandshake(body,hello,error), "server challenge is not binary");
        const auto challenge=std::get<Challenge>(hello);
        Proof proof{QByteArray(16,'n'),peer,{},HandshakeChannel::Control,rv::Endpoint::from("127.0.0.1",50000),*jam2::application::PeerName::fromInput("Alex")};
        const auto transcript=makeTranscript(session,challenge.nonce,proof.nonce,peer,proof.endpoint.text(),"control",proof.name.text());
        proof.proof=keyedValue(QByteArray::fromHex(key.toLatin1()),"jam2-control-client-proof",transcript).first(16);
        if (scenario==0) stale=proof;
        if (scenario==1) proof=stale;
        if (scenario==2) proof.endpoint.port++;
        if (scenario==3) proof.name=*jam2::application::PeerName::fromInput("Bob");
        if (scenario==4) proof.peer=QString(32,'2');
        if (scenario==5) { proof.channel=HandshakeChannel::Asset; proof.endpoint={}; proof.name={}; }
        require(outgoing->write(encodeHandshake(proof)), "inject binary authentication proof");
        until([&] { return server.hasPeer() || server.stats().authenticationRejects>0; });
        if (scenario==0) require(server.hasPeer(), "valid binary proof rejected");
        else require(!server.hasPeer() && server.stats().authenticationRejects==1, "changed/replayed binary proof authenticated");
        server.close(); require(server.stats().activeConnections==0, "authentication teardown leaked a connection");
    }
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        require(argc==2,"requires rendezvous golden vectors");
        handshakeCodec(); rendezvousCodec(QString::fromLocal8Bit(argv[1])); authenticationBinding();
        std::cout << "binary bootstrap: exact byte vectors, bounds, transcript binding and cleanup passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

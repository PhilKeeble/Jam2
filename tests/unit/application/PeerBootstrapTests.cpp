#include "PeerBootstrap.hpp"
#include "PeerIntroductionCoordinator.hpp"
#include "UdpStreamConnection.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

using namespace jam2::application;
namespace {
void require(bool okay,const char* why) { if (!okay) throw std::runtime_error(why); }
QString id(int i) { return QString(32,QChar('1'+i)); }
PeerBootstrapEndpoint endpoint(int i) { return {0x7f000001,static_cast<quint16>(41000+i)}; }
void wire()
{
    const QByteArray key(16,'K');
    for (int value=1;value<=static_cast<int>(PeerBootstrapKind::Rejected);++value) {
        PeerBootstrapMessage message;
        message.kind=static_cast<PeerBootstrapKind>(value); message.session=123;
        message.sender=id(0); message.partner=id(1); message.request=id(3); message.introduction=100;
        message.attempt=1; message.endpoint=endpoint(1); message.observed=endpoint(0); message.local=endpoint(0); message.success=true;
        auto bytes=encodePeerBootstrap(message,key);
        require(!bytes.isEmpty() && bytes.size()<=kPeerBootstrapMaximumBytes,"bootstrap encoding");
        auto span=[&] { return std::span(reinterpret_cast<const std::uint8_t*>(bytes.constData()),static_cast<std::size_t>(bytes.size())); };
        const auto decoded=decodePeerBootstrap(span(),123,key);
        require(decoded && decoded->kind==message.kind && decoded->sender==message.sender && decoded->request==message.request,"bootstrap roundtrip");
        require(!decodePeerBootstrap(span(),124,key),"wrong session accepted");
        bytes.back()^=1; require(!decodePeerBootstrap(span(),123,key),"wrong MAC accepted"); bytes.back()^=1;
        bytes[4]=static_cast<char>(kUdpMuxVersion-1); require(!decodePeerBootstrap(span(),123,key),"older bootstrap version accepted");
        bytes[4]=kUdpMuxVersion; bytes.chop(1); require(!decodePeerBootstrap(span(),123,key),"truncated bootstrap accepted");
    }
    PeerBootstrapMessage invalid; invalid.sender=id(0); invalid.request=id(1);
    require(encodePeerBootstrap(invalid,key).isEmpty(),"zero session accepted");
    invalid.session=123; invalid.request=QString(32,'0');
    require(encodePeerBootstrap(invalid,key).isEmpty(),"zero request nonce accepted");
}
void fourPeerIntroductions()
{
    PeerIntroductionCoordinator host(123,id(0));
    for (int i=1;i<4;++i) host.admit(id(i),endpoint(i));
    require(host.introduce(id(1),id(2),id(4),0) && host.introduce(id(1),id(3),id(5),0) &&
        host.introduce(id(2),id(3),id(6),0),"four-peer mesh introductions");
    require(!host.introduce(id(1),id(2),id(7),0),"duplicate concurrent pair accepted");
    std::array<quint64,3> firstIntroductions{};
    int complete=0;
    for (quint64 now=0;now<9000000;now+=10000) {
        for (const auto& transmission : host.poll(now)) {
            auto response=transmission.message;
            const int pair=response.request==id(4) ? 0 : response.request==id(5) ? 1 : 2;
            if (!firstIntroductions[pair]) firstIntroductions[pair]=response.introduction;
            if (pair==0 && response.attempt==1) continue; // only B-C loses its first socket pair
            if (pair==0) require(response.attempt==2 && response.introduction!=firstIntroductions[0],"socket retry reused attempt identity");
            response.sender=transmission.to==endpoint(1) ? id(1) : transmission.to==endpoint(2) ? id(2) : id(3);
            if (response.kind==PeerBootstrapKind::Prepare) { response.kind=PeerBootstrapKind::Candidate; response.local=transmission.to; }
            else if (response.kind==PeerBootstrapKind::Offer) response.kind=PeerBootstrapKind::Ready;
            else if (response.kind==PeerBootstrapKind::Go) { response.kind=PeerBootstrapKind::Outcome; response.success=true; }
            else if (response.kind==PeerBootstrapKind::Complete) { response.kind=PeerBootstrapKind::CompleteAck; ++complete; }
            host.receive(response,transmission.to,now);
        }
    }
    require(complete==6 && host.pending()==0 && host.takeFailures().empty(),"four-peer introductions did not converge independently");
    host.introduce(id(1),id(2),id(7),10000000); host.remove(id(2));
    require(host.pending()==0,"peer leave retained pending introduction");
    host.admit(id(2),endpoint(2)); host.introduce(id(1),id(2),id(8),0);
    int maxAttempt=0;
    for (quint64 now=0;now<100000000 && host.pending();now+=10000) {
        for (const auto& transmission : host.poll(now)) {
            auto response=transmission.message; maxAttempt=std::max(maxAttempt,static_cast<int>(response.attempt));
            response.sender=transmission.to==endpoint(1) ? id(1) : id(2);
            if (response.kind==PeerBootstrapKind::Prepare) { response.kind=PeerBootstrapKind::Candidate; response.local=transmission.to; }
            else if (response.kind==PeerBootstrapKind::Offer) response.kind=PeerBootstrapKind::Ready;
            else if (response.kind==PeerBootstrapKind::Go) { response.kind=PeerBootstrapKind::Outcome; response.success=true; }
            else continue; // Completion receipts are lost on every attempt.
            host.receive(response,transmission.to,now);
        }
    }
    require(maxAttempt==10 && host.pending()==0 && host.takeFailures().size()==1,
        "lost completion acknowledgements were silently accepted or retried without a bound");
}
}
int main()
{
    try { wire(); fourPeerIntroductions(); std::cout << "peer bootstrap wire and four-peer introduction checks passed\n"; return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

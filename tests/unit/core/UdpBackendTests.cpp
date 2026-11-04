#include "udp_socket.hpp"
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <system_error>
namespace {
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
void exchange(jam2::UdpBackend backend,jam2::UdpServiceClass service) {
    std::cout << "backend=" << static_cast<int>(backend) << " service=" << static_cast<int>(service) << std::endl;
    jam2::UdpSocket sender, receiver(backend,service);
    sender.bind({"127.0.0.1",0});
    receiver.bind({"127.0.0.1",0}); receiver.set_receive_timestamps(true);
    require(receiver.service_class()==service,"OS service class must match explicit request");
    receiver.activate_peer(jam2::resolve_udp_endpoint(sender.local_endpoint()));
    const auto target=jam2::resolve_udp_endpoint(receiver.local_endpoint());
    if (backend!=jam2::UdpBackend::Bsd) {
        auto other=jam2::resolve_udp_endpoint(sender.local_endpoint()); other.port=other.port==65535 ? 1 : other.port+1;
        const std::array<std::uint8_t,1> byte{};
        require(receiver.send_to(other,byte).outcome==jam2::UdpSendOutcome::Fatal,
            "a connected diagnostic flow must reject a different destination explicitly");
    }
    require(target.port!=0,"backend must retain a concrete bound UDP port");
    std::array<std::uint8_t,100> payload{}, buffer{}; payload[0]=42;
    for (int i=0;i<16;++i) require(sender.send_to(target,payload).outcome==jam2::UdpSendOutcome::Sent,"send burst to backend");
    for (int i=0;i<16;++i) {
        const std::array<const jam2::UdpSocket*,1> sockets{&receiver};
        jam2::UdpSocket::waitForDatagrams(sockets,1000000);
        const auto packet=receiver.recv_from_for(buffer,0);
        require(packet && packet->size==payload.size() && buffer[0]==42,"backend must deliver the real ordered burst");
        if (backend!=jam2::UdpBackend::NetworkFramework) require(packet->kernel_arrival_us>0,"BSD must expose the enabled kernel stamp");
        require(!packet->kernel_arrival_us || packet->observed_us>=packet->kernel_arrival_us,"available receive metadata must be ordered");
        require(receiver.send_to(packet->endpoint,payload).outcome==jam2::UdpSendOutcome::Sent,"reply through retained UDP flow");
        const auto reply=sender.recv_from_for(buffer,1000000);
        require(reply && reply->endpoint==target,"backend reply must preserve the punched source port");
    }
    jam2::UdpSocket moved(std::move(receiver));
    require(moved.local_endpoint().port==target.port,"backend move must preserve endpoint ownership");
    require(sender.send_to(target,payload).outcome==jam2::UdpSendOutcome::Sent,"send after backend move");
    require(moved.recv_from_for(buffer,1000000).has_value(),"moved backend must retain callbacks and waits");
    if (backend==jam2::UdpBackend::NetworkFramework) {
        bool rejected=false; try { moved.set_recv_buffer_size(1024); } catch (const std::exception&) { rejected=true; }
        require(rejected && moved.recv_buffer_size()==0,"framework buffer limits must be reported unavailable");
    }
    // Outgoing-first flow, matching rendezvous/candidate bootstrap, must also
    // retain its port and receive the BSD peer's reply.
    jam2::UdpSocket outbound(backend,service); outbound.bind({"127.0.0.1",0});
    jam2::UdpSocket echo; echo.bind({"127.0.0.1",0});
    outbound.activate_peer(jam2::resolve_udp_endpoint(echo.local_endpoint()));
    require(outbound.send_to(jam2::resolve_udp_endpoint(echo.local_endpoint()),payload).outcome==jam2::UdpSendOutcome::Sent,"create outgoing-first flow");
    const auto request=echo.recv_from_for(buffer,1000000);
    require(request.has_value(),"BSD echo must receive outgoing-first flow");
    require(request->endpoint.port==outbound.local_endpoint().port,"outgoing flow must bind the listener's port");
    require(echo.send_to(request->endpoint,payload).outcome==jam2::UdpSendOutcome::Sent,"reply to outgoing-first flow");
    require(outbound.recv_from_for(buffer,1000000).has_value(),"outgoing-first flow must receive its response");
}
}
int main() {
    try {
        jam2::NetworkRuntime network;
        for (bool invalidBackend : {false,true}) {
            bool rejected=false;
            try { jam2::UdpSocket invalid(invalidBackend ? static_cast<jam2::UdpBackend>(99) : jam2::UdpBackend::Bsd,
                invalidBackend ? jam2::UdpServiceClass::BestEffort : static_cast<jam2::UdpServiceClass>(99)); }
            catch (const std::invalid_argument&) { rejected=true; }
            require(rejected,"invalid typed transport configuration must fail before resource allocation");
        }
#if defined(__APPLE__)
        for (auto backend : {jam2::UdpBackend::Bsd,jam2::UdpBackend::NetworkFramework,jam2::UdpBackend::ConnectedBsd})
            for (auto service : {jam2::UdpServiceClass::BestEffort,jam2::UdpServiceClass::InteractiveVoice}) exchange(backend,service);
#else
        bool rejected=false;
        try { jam2::UdpSocket unsupported(jam2::UdpBackend::NetworkFramework); } catch (const std::exception&) { rejected=true; }
        require(rejected,"unsupported backend must fail explicitly");
#endif
        return 0;
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}

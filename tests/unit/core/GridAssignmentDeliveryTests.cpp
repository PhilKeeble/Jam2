#include "grid_assignment_delivery.hpp"
#include "protocol.hpp"
#include "session_authority.hpp"
#include <array>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void deliveryLifecycle()
{
    jam2::GridAssignmentDelivery d;
    require(!d.due(0) && !d.pending(), "empty delivery is due");
    d.begin(0, 7); require(!d.pending(), "zero revision accepted");
    d.begin(1, 0); require(!d.pending(), "zero token accepted");
    d.begin(1, 7); require(d.due(0), "new delivery delayed");
    require(!d.acknowledge(1, 7), "unsent delivery acknowledged");
    d.attempted(0);
    require(!d.due(99999) && d.due(100000), "initial retry interval");
    d.attempted(100000);
    require(!d.due(299999) && d.due(300000), "retry backoff");
    require(!d.acknowledge(2, 7) && !d.acknowledge(1, 8), "unrelated ACK accepted");
    require(d.acknowledge(1, 7) && d.applied() && !d.due(3600000000ULL), "ACK did not stop repeats");
    require(d.acknowledge(1, 7), "duplicate valid ACK not idempotent");
    d.begin(2, 8); d.attempted(0);
    require(!d.acknowledge(1, 7) && d.pending(), "stale revision ACK completed latest");
    d.reset(); d.begin(2, 9); d.attempted(0);
    require(!d.acknowledge(2, 8) && d.pending(), "pre-reconnect ACK completed fresh delivery");
    for (std::uint64_t t=100000; t<=3000000; t+=100000) if (d.due(t)) d.attempted(t);
    require(d.attempts()==6 && !d.due(3499999) && d.due(3500000), "maximum retry interval not bounded to 1s");
}
void authenticatedWire()
{
    using namespace jam2::protocol;
    const std::array<std::uint8_t,16> key{1,2,3};
    std::array<std::uint8_t,kGridAssignmentPayloadSize> assignment{};
    assignment[20]=0x92; assignment[56]=7;
    auto packet=encode_packet({PacketType::GridAssignment,42,1,1},assignment,key,jam2::NetworkAudioFormat::Pcm24Mono);
    require(packet.size()==100 && bool(parse_packet(packet,key,42,jam2::NetworkAudioFormat::Pcm24Mono)), "fixed assignment wire shape");
    packet.back()^=1;
    require(parse_packet(packet,key,42,jam2::NetworkAudioFormat::Pcm24Mono).error==ParseError::AuthenticationFailed, "token not authenticated");
    packet.pop_back();
    require(!parse_packet(packet,key,42,jam2::NetworkAudioFormat::Pcm24Mono), "truncated assignment accepted");
    const std::array<std::uint8_t,kGridAssignmentAppliedPayloadSize> revision{1};
    packet=encode_packet({PacketType::GridAssignmentApplied,42,2,7},revision,key,jam2::NetworkAudioFormat::Pcm24Mono);
    require(packet.size()==44 && bool(parse_packet(packet,key,42,jam2::NetworkAudioFormat::Pcm24Mono)), "fixed applied ACK wire shape");
    bool rejected=false;
    try { (void)encode_packet({PacketType::GridAssignmentApplied,42,3,7},assignment,key,jam2::NetworkAudioFormat::Pcm24Mono); }
    catch (const std::exception&) { rejected=true; }
    require(rejected, "invalid ACK payload size encoded");
    packet=encode_packet({PacketType::GridAssignmentRequest,42,3,999},{},key,jam2::NetworkAudioFormat::Pcm24Mono);
    require(packet.size()==36 && bool(parse_packet(packet,key,42,jam2::NetworkAudioFormat::Pcm24Mono)), "fixed recovery request shape");
    packet[20]^=1;
    require(parse_packet(packet,key,42,jam2::NetworkAudioFormat::Pcm24Mono).error==ParseError::AuthenticationFailed, "recovery nonce not authenticated");
    const std::array<std::uint8_t,kMetronomeStatePayloadSize> legacy{};
    packet=encode_packet({PacketType::MetronomeState,42,4,1},legacy,key,jam2::NetworkAudioFormat::Pcm24Mono);
    require(packet.size()==92 && bool(parse_packet(packet,key,42,jam2::NetworkAudioFormat::Pcm24Mono)), "legacy wire shape changed");
}
void fourPeerLossAndRecovery()
{
    std::array<jam2::SessionAuthority,4> peers{
        jam2::SessionAuthority{1,1,1},jam2::SessionAuthority{2,1,1},jam2::SessionAuthority{3,1,1},jam2::SessionAuthority{4,1,1}};
    std::array<jam2::GridAssignmentDelivery,3> delivery;
    std::array<int,3> applications{}, packets{};
    require(bool(peers[0].orderGridProposal({1,1,jam2::GridRunState::Running,0,true,false})), "initial ordering");
    const auto old=peers[0].grid();
    for (int i=0;i<3;++i) delivery[i].begin(old.revision,100+i);
    for (std::uint64_t t=0;t<4000000;t+=10000) {
        for (int i=0;i<3;++i) {
            auto& d=delivery[i]; if (!d.due(t)) continue;
            d.attempted(t); ++packets[i];
            if (i==0 && packets[i]==1) continue; // First assignment lost.
            if (i==2 && t<1000000) continue; // Application deliberately isolated.
            const auto result=peers[i+1].acceptGridAssignment(1,old);
            if (result==jam2::AuthorityUpdateResult::Accepted) ++applications[i];
            require(result==jam2::AuthorityUpdateResult::Accepted || result==jam2::AuthorityUpdateResult::Duplicate, "assignment rejected");
            if (i==1 && packets[i]<=2) continue; // Two applied receipts lost.
            require(d.acknowledge(old.revision,100+i), "application ACK rejected");
        }
        if (t==500000) require(delivery[0].applied() && delivery[1].applied() && delivery[2].pending(), "isolated peer blocked healthy peers");
    }
    for (int i=0;i<3;++i) require(delivery[i].applied() && applications[i]==1 && !delivery[i].due(3600000000ULL), "loss recovery reapplied reset or resumed steady broadcasts");
    require(bool(peers[0].orderGridProposal({1,2,jam2::GridRunState::Running,0,true,false})), "changed revision");
    for (int i=0;i<3;++i) {
        auto& d=delivery[i]; d.begin(2,200+i);
        require(d.due(4000001), "new revision waited for retry deadline"); d.attempted(4000001);
        require(!d.acknowledge(1,100+i), "old ACK cleared new revision");
        require(peers[i+1].acceptGridAssignment(1,peers[0].grid())==jam2::AuthorityUpdateResult::Accepted, "new assignment not applied");
        require(peers[i+1].acceptGridAssignment(1,old)==jam2::AuthorityUpdateResult::StaleRevision, "reordering rolled grid back");
        require(d.acknowledge(2,200+i), "latest ACK");
    }
    // Reconnect needs the same revision again and a fresh application token.
    delivery[2].reset(); delivery[2].begin(2,999); delivery[2].attempted(4000002);
    require(!delivery[2].acknowledge(2,202), "old connection ACK accepted");
    peers[3]=jam2::SessionAuthority{4,1,1};
    require(peers[3].acceptGridAssignment(1,peers[0].grid())==jam2::AuthorityUpdateResult::Accepted && delivery[2].acknowledge(2,999), "restarted receiver did not acquire unchanged revision");
    std::cout << "four_peer_grid_assignment initial_packets=" << packets[0]+packets[1]+packets[2]
        << " applications=3 idle_hour_packets=0 reconnect_same_revision=applied\n";
}
}
int main()
{
    try { deliveryLifecycle(); authenticatedWire(); fourPeerLossAndRecovery(); return 0; }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

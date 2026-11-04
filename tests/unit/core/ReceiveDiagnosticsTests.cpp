#include "receive_diagnostics.hpp"
#include "udp_socket.hpp"
#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using Phase = jam2::ReceiveServicePhase;
using Trace = jam2::ReceiveServiceDiagnostics;
jam2::ReceiveClockSample at(std::uint64_t wall, std::uint64_t cpu) { return {wall,cpu,true}; }
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
void beforeRead()
{
    Trace trace;
    trace.phase(Phase::SocketWait, at(100,100));
    trace.phase(Phase::SocketRead, at(4100,110));
    trace.phase(Phase::Classification, at(4110,120));
    trace.observe(7,1000,4100,4110,4120,at(4200,125),{2500,2501,1});
    const auto snapshot = trace.snapshot(); const auto& e = snapshot.events[0];
    require(snapshot.count == 1 && e.peer_id == 7 && e.before_read_us == 3100 &&
        e.receive_call_us == 10 && e.after_read_us == 10 && e.after_observation_us == 80,
        "before-read, receive-call, post-read and handoff delays must remain separate");
    require(e.covered_us == 3200 && e.phase_wall_us[static_cast<std::size_t>(Phase::SocketWait)] == 3100 &&
        e.dominant_phase == Phase::SocketWait && e.dominant_span_wall_us == 4000 &&
        e.dominant_span_cpu_us == 10 && e.dominant_cpu_valid && e.dominant_overlap_us == 3100,
        "event must identify the overlapping wait and report full-span CPU without inventing partial CPU timing");
    require(e.would_block_after_kernel_us == 1500 && e.readiness.datagrams_since_would_block == 1,
        "a timed EAGAIN after the kernel stamp must remain visible as evidence the packet was not yet readable");
    trace.observe(7,1100,4200,4210,4220,at(4300,130));
    const auto coalesced = trace.snapshot();
    require(coalesced.slow_packets == 2 && coalesced.coalesced_packets == 1 && coalesced.count == 1,
        "one delayed burst must not allocate an event for every old packet");
    trace.observe(7,1100,4800,4810,4820,at(4900,135));
    const auto worst=trace.snapshot();
    require(worst.count == 1 && worst.events[0].id == 1 && worst.events[0].trace_us == 4900 &&
        worst.coalesced_packets == 2, "coalescing must retain the worst packet in an episode, not hide later larger delays");
}
void duringRead()
{
    Trace trace;
    trace.phase(Phase::SocketRead, at(1000,10));
    trace.phase(Phase::Classification, at(5000,20));
    trace.observe(8,2000,1000,5000,5010,at(5020,25),{},{1200,true});
    const auto e = trace.snapshot().events[0];
    require(e.read_cpu.valid && e.read_cpu.us==1200,"per-call CPU must retain its own interval separately from enclosing phase CPU");
    require(e.before_read_us == 0 && e.receive_call_us == 4000 && e.after_read_us == 10 &&
        e.covered_us == 3020 && e.dominant_phase == Phase::SocketRead,
        "a packet arriving during a delayed receive call must not acquire fictitious pre-read residence");
}
void slowServiceWithoutPackets()
{
    Trace trace;
    trace.phase(Phase::SocketRead, at(100,10));
    trace.phase(Phase::SocketWait, at(6100,30));
    trace.phase(Phase::SocketRead, at(16100,40));
    trace.phase(Phase::Classification, at(16110,50));
    auto snapshot = trace.snapshot();
    const auto& read = snapshot.worst_phase_spans[static_cast<std::size_t>(Phase::SocketRead)];
    const auto& wait = snapshot.worst_phase_spans[static_cast<std::size_t>(Phase::SocketWait)];
    require(snapshot.count == 0 && snapshot.slow_packets == 0 && read.begin_us == 100 && read.end_us == 6100 &&
        read.cpu_valid && read.cpu_us == 20 && wait.begin_us == 6100 && wait.end_us == 16100 && wait.cpu_us == 10,
        "slow empty reads and waits must remain visible without a delayed packet or an observe call");
    trace.phase(Phase::SocketRead, at(16200,60));
    trace.phase(Phase::Classification, at(18200,70));
    snapshot = trace.snapshot();
    require(snapshot.worst_phase_spans[static_cast<std::size_t>(Phase::SocketRead)].end_us == 6100,
        "later shorter phases must not erase the startup stall");
    require(snapshot.worst_phase_spans[static_cast<std::size_t>(Phase::Classification)].end_us == 0,
        "short normal work must not publish a slow-service record");
    trace.phase(Phase::Telemetry, {18201,0,false});
    trace.phase(Phase::SocketRead, at(21201,100));
    const auto unknownCpu = trace.snapshot().worst_phase_spans[static_cast<std::size_t>(Phase::Telemetry)];
    require(!unknownCpu.cpu_valid && unknownCpu.cpu_us == 0 && unknownCpu.end_us - unknownCpu.begin_us == 3000,
        "missing CPU evidence must retain wall duration without inventing CPU use");
}
void afterRead()
{
    Trace trace;
    trace.phase(Phase::SocketRead, at(1000,10));
    trace.phase(Phase::Classification, at(1010,15));
    trace.observe(9,990,1000,1010,5000,at(5010,20));
    const auto e = trace.snapshot().events[0];
    require(e.before_read_us == 10 && e.receive_call_us == 10 && e.after_read_us == 3990 &&
        e.dominant_phase == Phase::Classification && e.covered_us == 4010,
        "post-read descheduling must not be attributed to time waiting in the socket");
}
void busyWorkAndLimits()
{
    Trace trace;
    trace.phase(Phase::CapturePack, at(1000,10));
    trace.phase(Phase::AudioSend, at(4500,3510));
    trace.phase(Phase::SocketRead, at(4600,3520));
    trace.phase(Phase::Classification, at(4610,3525));
    trace.observe(1,2000,4600,4610,4615,at(4620,3530));
    auto s = trace.snapshot();
    require(s.events[0].dominant_phase == Phase::CapturePack && s.events[0].dominant_span_cpu_us == 3500,
        "actual worker CPU work must be distinguishable from a long low-CPU span");
    trace.observe(1,0,1,2,3,at(4700,3531));
    require(trace.snapshot().invalid_samples == 1, "invalid timing endpoints must be reported and excluded");
    Trace ring;
    for (std::uint64_t i=0;i<40;++i) {
        const auto base = 10000+i*10000;
        ring.phase(Phase::SocketWait, at(base,i*20));
        ring.phase(Phase::SocketRead, at(base+2900,i*20+5));
        ring.phase(Phase::Classification, at(base+2910,i*20+10));
        ring.observe(1,base+500,base+2900,base+2910,base+2920,at(base+2930,i*20+15));
    }
    s=ring.snapshot();
    require(s.count == Trace::kEventCount && s.overwritten_events == 8 &&
        s.events[0].id == 9 && s.events[s.count-1].id == 40,
        "bounded event storage must retain ordered newest evidence and expose overwritten history");
    Trace history;
    for (std::uint64_t i=0;i<Trace::kHistorySize+20;++i) history.phase(Phase::ControlSend,at(100+i*10,i));
    const auto end=100+(Trace::kHistorySize+20)*10;
    history.observe(1,1,end-10,end-5,end,at(end+10,Trace::kHistorySize+30));
    const auto limited=history.snapshot().events[0];
    require(limited.covered_us < limited.trace_us-limited.kernel_us,
        "history truncation must remain visible rather than inventing phase coverage");
}
void realClockAndSocket()
{
    const auto first=jam2::receive_clock_sample();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto second=jam2::receive_clock_sample();
    require(first.cpu_valid && second.cpu_valid && second.cpu_us >= first.cpu_us && second.wall_us >= first.wall_us,
        "real per-thread CPU and receive clocks must be available and monotonic");
#if defined(__APPLE__)
    jam2::NetworkRuntime network;
    jam2::UdpSocket sender,receiver; receiver.bind({"127.0.0.1",0}); receiver.set_receive_timestamps(true);
    Trace trace; trace.phase(Phase::Maintenance,jam2::receive_clock_sample());
    std::array<std::uint8_t,16> bytes{},buffer{};
    require(sender.send_to(jam2::resolve_udp_endpoint(receiver.local_endpoint()),bytes).outcome == jam2::UdpSendOutcome::Sent,"send real delayed-service probe");
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    trace.phase(Phase::SocketRead,jam2::receive_clock_sample());
    const auto packet=receiver.recv_from_for(buffer,100000); require(packet.has_value(),"receive real delayed-service probe");
    trace.phase(Phase::Classification,jam2::receive_clock_sample());
    trace.observe(1,packet->kernel_arrival_us,packet->read_begin_us,packet->read_end_us,packet->observed_us,jam2::receive_clock_sample());
    const auto s=trace.snapshot();
    require(s.count == 1 && s.events[0].before_read_us >= 8000 && s.events[0].dominant_phase == Phase::Maintenance,
        "production socket timestamps must attribute injected local service delay to its actual worker phase");
#endif
}
}
int main()
{
    try { beforeRead();duringRead();slowServiceWithoutPackets();afterRead();busyWorkAndLimits();realClockAndSocket();return 0; }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}

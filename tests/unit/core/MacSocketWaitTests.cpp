#include "macos_socket_wait.hpp"
#include "udp_socket.hpp"
#include "udp_wait.hpp"
#include <unistd.h>
#include <fcntl.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Pipe {
    int readFd=-1, writeFd=-1;
    Pipe() { int pair[2]; require(pipe(pair)==0,"create test pipe"); readFd=pair[0]; writeFd=pair[1]; }
    ~Pipe() { if (readFd>=0) close(readFd); if (writeFd>=0) close(writeFd); }
    Pipe(const Pipe&)=delete;
    Pipe& operator=(const Pipe&)=delete;
    void put() { const char byte=1; require(write(writeFd,&byte,1)==1,"publish real descriptor readiness"); }
    void take() { char byte=0; require(read(readFd,&byte,1)==1 && byte==1,"wait must leave data for its owner"); }
};
double elapsedUs(Clock::time_point start) { return std::chrono::duration<double,std::micro>(Clock::now()-start).count(); }

void readinessAndDeadlines(jam2::UdpWaitMode mode) {
    jam2::UdpWaitDiagnostics metrics;
    jam2::UdpWaitScope scope(mode,&metrics);
    jam2::MacSocketWaiter waiter;
    Pipe pipe;
    const std::array<int,2> descriptors{pipe.readFd,pipe.readFd};
    auto start=Clock::now();
    auto result=waiter.waitForRead(descriptors,2000);
    require(result.deadline && !result.readiness && elapsedUs(start)>=1500,"empty descriptors wait for their deadline");
    pipe.put();
    result=waiter.waitForRead(descriptors,1000000);
    require(result.readiness && waiter.readable(pipe.readFd),"already-ready input wins over a long timer");
    pipe.take();
    // The old long deadline must not leak into or shorten the next wait.
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    start=Clock::now(); result=waiter.waitForRead(descriptors,3000);
    require(result.deadline && !result.readiness && elapsedUs(start)>=2500,"early readiness leaves no stale timer event");
    pipe.put(); result=waiter.waitForRead(descriptors,0);
    require(result.readiness,"zero budget can observe already-ready input"); pipe.take();
    result=waiter.waitForRead(descriptors,0);
    require(result.deadline && !result.readiness,"zero budget never invents a readiness event");
    pipe.put(); result=waiter.waitForRead(descriptors,std::numeric_limits<std::uint64_t>::max());
    require(result.readiness,"large bounded deadline still returns immediately for input"); pipe.take();
    const auto snapshot=metrics.snapshot();
    require(snapshot.calls==6 && snapshot.deadline_wakes==3 && snapshot.readiness_wakes==3 && snapshot.errors==0,
        "production wait counters retain exact completed outcomes");
}

void changingDescriptorsAndReusedNumbers() {
    jam2::MacSocketWaiter waiter;
    Pipe old, replacement;
    const std::array<int,1> watched{old.readFd};
    old.put(); require(waiter.waitForRead(watched,10000).readiness,"register the first socket owner");
    const int number=old.readFd; require(close(old.readFd)==0,"close registered descriptor");old.readFd=-1;
    require(dup2(replacement.readFd,number)==number,"reuse the same numeric descriptor for a new owner");old.readFd=number;
    auto result=waiter.waitForRead(watched,2000);
    require(result.deadline && !result.readiness,"closed owner's queued event cannot wake its replacement");
    replacement.put(); result=waiter.waitForRead(watched,10000);
    require(result.readiness && waiter.readable(number),"new owner of the same descriptor registers correctly");replacement.take();

    std::vector<std::unique_ptr<Pipe>> pipes;
    std::vector<int> descriptors;
    for (int i=0;i<24;++i) { pipes.push_back(std::make_unique<Pipe>());descriptors.push_back(pipes.back()->readFd);pipes.back()->put(); }
    result=waiter.waitForRead(descriptors,10000);
    require(result.readiness,"larger experimental meshes have no fixed wait-array limit");
    for (int fd:descriptors) require(waiter.readable(fd),"all ready descriptors survive event-buffer growth");
    pipes.front()->take();
    result=waiter.waitForRead(std::span(descriptors).first(1),2000);
    require(result.deadline && !result.readiness,"removed route interests cannot repeatedly wake the next route set");
}

void errorAndScopeCleanup() {
    jam2::UdpWaitDiagnostics metrics;
    jam2::UdpWaitScope outer(jam2::UdpWaitMode::Legacy);
    {
        jam2::UdpWaitScope scope(jam2::UdpWaitMode::PlatformDefault,&metrics);
        jam2::MacSocketWaiter waiter;
        const std::array<int,1> bad{-1};bool failed=false;
        try { waiter.waitForRead(bad,100); } catch (const std::invalid_argument&) { failed=true; }
        require(failed && metrics.snapshot().errors==1,"invalid descriptors are rejected and counted");
        Pipe pipe; const std::array<int,1> valid{pipe.readFd};pipe.put();
        require(waiter.waitForRead(valid,10000).readiness,"invalid wait does not poison later valid input");pipe.take();
        int closed=dup(pipe.readFd); require(closed>=0,"duplicate error fixture");require(close(closed)==0,"close error fixture");
        failed=false;
        try { const std::array<int,1> gone{closed};waiter.waitForRead(gone,100); } catch (const std::exception&) { failed=true; }
        require(failed && metrics.snapshot().errors==2,"kernel registration errors are explicit and counted");
        pipe.put();require(waiter.waitForRead(valid,10000).readiness,"registration errors leave the waiter reusable");pipe.take();
    }
    require(jam2::current_udp_wait_mode()==jam2::UdpWaitMode::Legacy && jam2::current_udp_wait_diagnostics()==nullptr,
        "nested comparison scopes restore their caller's mode and diagnostics");
    bool failed=false;
    try { jam2::UdpWaitScope bad(static_cast<jam2::UdpWaitMode>(99)); } catch (const std::invalid_argument&) { failed=true; }
    require(failed && jam2::current_udp_wait_mode()==jam2::UdpWaitMode::Legacy,"invalid mode cannot alter the active scope");
}

void wakeAndUdpOwners(jam2::UdpWaitMode mode) {
    jam2::UdpWaitScope scope(mode);
    jam2::RealtimeWakeSignal stop, capture;
    jam2::UdpSocket receiver,sender; receiver.bind({"127.0.0.1",0});
    std::array<std::uint8_t,8> bytes{};
    require(sender.send_to(jam2::resolve_udp_endpoint(receiver.local_endpoint()),bytes).outcome==jam2::UdpSendOutcome::Sent,"send actual UDP input");
    const std::array<const jam2::UdpSocket*,1> sockets{&receiver};
    // send() completion need not mean that the receiving socket is readable.
    // Establish that precondition before publishing the simultaneous wake.
    jam2::UdpSocket::waitForDatagrams(sockets,1000000);
    capture.requestWake(); capture.signal(jam2::monotonic_us());
    jam2::UdpSocket::waitForDatagrams(sockets,1000000,&capture);
    require(receiver.recv_from_for(bytes,0).has_value(),"readiness wait leaves UDP bytes for production receive");
    require(capture.consumptionCount()==1,"simultaneous capture readiness is consumed once");
    stop.requestWake(); capture.requestWake(); capture.signal(jam2::monotonic_us());
    stop.waitFor(1000000,&capture);
    require(capture.consumptionCount()==2,"additional wake can release an empty receive queue");
    std::atomic<bool> waiting=false; bool returned=false;
    std::exception_ptr failure;
    std::thread reader([&] {
        try { jam2::UdpWaitScope threadScope(mode);stop.requestWake();waiting.store(true);stop.waitFor(1000000);returned=true; }
        catch (...) { failure=std::current_exception(); }
    });
    while (!waiting.load()) std::this_thread::yield();
    stop.signal(jam2::monotonic_us()); reader.join();
    if (failure) std::rethrow_exception(failure);
    require(returned && stop.consumptionCount()==1,"stop/wake publication safely cancels a long wait");
    const auto start=Clock::now(); stop.waitFor(2000);
    require(elapsedUs(start)>=1500,"consumed stop notification cannot become a stale future wake");
}

int openDescriptors() {
    int count=0;for (int fd=0;fd<256;++fd) if (fcntl(fd,F_GETFD)>=0) ++count;return count;
}
void resourceCleanup() {
    const int before=openDescriptors();
    for (int i=0;i<10;++i) { jam2::MacSocketWaiter waiter;require(waiter.waitForRead({},100).deadline,"empty descriptor set supports critical deadlines"); }
    require(openDescriptors()==before,"owned kqueues close on destruction");
    std::exception_ptr failure;
    std::thread reader([&] { try { jam2::RealtimeWakeSignal wake;wake.waitFor(100); } catch (...) { failure=std::current_exception(); } });
    reader.join(); if (failure) std::rethrow_exception(failure);
    require(openDescriptors()==before,"thread-owned wait queue closes when the reader exits");
}

void measure() {
    std::cout<<"method,repetition,interval_us,p50_us,p99_us,max_us,late_over_1ms\n";
    for (int interval:{100,250,1000}) for (int repetition=0;repetition<2;++repetition) {
        const std::array modes=repetition==0
            ? std::array{jam2::UdpWaitMode::Legacy,jam2::UdpWaitMode::PlatformDefault}
            : std::array{jam2::UdpWaitMode::PlatformDefault,jam2::UdpWaitMode::Legacy};
        for (auto mode:modes) {
            jam2::UdpWaitDiagnostics metrics;jam2::UdpWaitScope scope(mode,&metrics);
            jam2::MacSocketWaiter waiter;jam2::UdpSocket receiver;receiver.bind({"127.0.0.1",0});
            const std::array<const jam2::UdpSocket*,1> sockets{&receiver};std::vector<double> samples;samples.reserve(1000);
            for (int i=-100;i<1000;++i) {
                const auto start=Clock::now();const auto result=waiter.waitUdp(sockets,interval);
                const double elapsed=elapsedUs(start);
                require(result.deadline && !result.readiness,"timing probe exercises the production timeout path");
                require(elapsed>=interval*.9,"critical timer must not return substantially before its deadline");
                if (i>=0) samples.push_back(elapsed);
            }
            std::sort(samples.begin(),samples.end());
            require(metrics.snapshot().calls==1100 && metrics.snapshot().errors==0,"every measured production wait is accounted for");
            std::cout<<jam2::udp_wait_mode_text(mode)<<','<<repetition<<','<<interval<<','<<samples[500]<<','<<samples[990]<<','<<samples.back()<<','<<metrics.snapshot().late_over_1ms<<'\n';
        }
    }
}
}
int main(int argc,char** argv) {
    try {
        jam2::NetworkRuntime runtime;
        if (argc==2 && std::string_view(argv[1])=="--measure") {measure();return 0;}
        require(argc==1,"unknown wait test argument");
        readinessAndDeadlines(jam2::UdpWaitMode::PlatformDefault);
        readinessAndDeadlines(jam2::UdpWaitMode::Legacy);
        changingDescriptorsAndReusedNumbers();errorAndScopeCleanup();
        wakeAndUdpOwners(jam2::UdpWaitMode::PlatformDefault);wakeAndUdpOwners(jam2::UdpWaitMode::Legacy);
        resourceCleanup();return 0;
    } catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}

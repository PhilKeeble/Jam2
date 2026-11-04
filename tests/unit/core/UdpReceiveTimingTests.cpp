#include "udp_socket.hpp"

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
}
int main()
{
    try {
        jam2::NetworkRuntime network;
        jam2::UdpSocket sender, receiver;
        receiver.bind({"127.0.0.1", 0});
        const auto destination = jam2::resolve_udp_endpoint(receiver.local_endpoint());
        std::array<std::uint8_t, 16> payload{}, buffer{};
        auto send = [&] { require(sender.send_to(destination, payload).outcome == jam2::UdpSendOutcome::Sent, "send timing probe"); };
        send();
        auto plain = receiver.recv_from_for(buffer, 100000);
        require(plain && !plain->kernel_arrival_us && !plain->socket_residence_us, "disabled timing must remain absent");
        jam2::RealtimeWakeSignal polling(jam2::RealtimeWakeSignal::NotificationMode::PollOnly);
        polling.requestWake(); polling.signal(123); polling.signal(124);
        require(polling.signalCount() == 1 && polling.lastSignalTimeUs() == 123,
            "poll-only notifications must retain coalesced atomic state and original ready timing");
        const auto waitStart = jam2::monotonic_us();
        require(!receiver.recv_from_for(buffer, 3000, &polling), "poll-only state cannot fabricate a UDP packet");
        require(jam2::monotonic_us() - waitStart >= 2000, "poll-only notification must not signal the native wait handle");
        polling.cancelWakeRequest();
        require(polling.consumptionCount() == 1, "network consumer must clear poll-only pending state");
        polling.requestWake(); polling.signal(125);
        require(polling.signalCount() == 2 && polling.lastSignalTimeUs() == 125,
            "poll-only notifications must rearm after consumption");
        polling.cancelWakeRequest();
#if defined(__APPLE__)
        receiver.set_receive_timestamps(true);
        send();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto queued = receiver.recv_from_for(buffer, 100000);
        require(queued && queued->size == payload.size() && queued->kernel_arrival_us > 0,
            "kernel receive timestamp must accompany the real datagram");
        require(queued->socket_residence_us >= 8000, "queued residence must include deliberate application service delay");
        require(queued->read_begin_us >= queued->kernel_arrival_us + 8000 &&
            queued->read_end_us >= queued->read_begin_us && queued->observed_us >= queued->read_end_us &&
            queued->socket_residence_us == queued->observed_us - queued->kernel_arrival_us,
            "kernel-to-app delay must expose ordered read entry, return and observation timestamps");
        const auto first = queued->kernel_arrival_us;
        require(queued->read_cpu.valid,"receive timing must expose the actual calling thread CPU interval");
        jam2::UdpSocket moved(std::move(receiver));
        send();
        auto next = moved.recv_from_for(buffer, 100000);
        require(next && next->kernel_arrival_us >= first + 8000,
            "kernel gaps use the monotonic arrival domain and survive socket moves");
        jam2::UdpSocket assigned; assigned = std::move(moved);
        send();
        auto afterAssignment = assigned.recv_from_for(buffer, 100000);
        require(afterAssignment && afterAssignment->kernel_arrival_us >= next->kernel_arrival_us,
            "move assignment retains the enabled diagnostic state");
        assigned.set_receive_timestamps(false);
        send();
        auto disabled = assigned.recv_from_for(buffer, 100000);
        require(disabled && !disabled->kernel_arrival_us && !disabled->socket_residence_us,
            "disabling timing must stop ancillary diagnostics");
        assigned.set_receive_timestamps(true);
        jam2::UdpSocket::ReceiveAttemptTiming empty;
        require(!assigned.recv_from_for(buffer,0,nullptr,&empty) && empty.would_block &&
            empty.begin_us > 0 && empty.end_us >= empty.begin_us,
            "an empty nonblocking read must expose timed EAGAIN evidence independently of packet arrival");
        send();
        auto truncated = assigned.recv_from_for(std::span(buffer).first(1), 100000);
        require(!truncated, "timing metadata must not make truncated packets deliverable");
#else
        bool rejected = false;
        try { receiver.set_receive_timestamps(true); } catch (const std::exception&) { rejected = true; }
        require(rejected, "unsupported platforms must report unavailable kernel timing");
        receiver.set_receive_timestamps(false);
#endif
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

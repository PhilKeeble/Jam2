#include "ReceivedTransportRecovery.hpp"
#include "session_authority.hpp"
#include "transport_timing.hpp"
#include <iostream>

int main()
{
    using Recovery = jam2::cli::ReceivedTransportRecovery;
    using Action = jam2::EngineTransportAction;
    int failures = 0;
    const auto expect = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    };
    Recovery recovery;
    jam2::SessionAuthority authority(4, 1, 1);
    const auto receive = [&](std::uint64_t peer, std::uint32_t event, Action action) {
        if (!authority.acceptTransportEvent(peer, event, 0, false)) return false;
        recovery.retain({{action, event, 0, 24000, 24000}, peer, 1000, 1000});
        return true;
    };
    expect(receive(1, 1, Action::TrackRestart), "authenticated restart accepted");
    expect(recovery.takeDue(0) != nullptr && recovery.takeDue(1) == nullptr,
        "recovery retries are explicitly bounded to one attempt per 20 ms");
    expect(recovery.takeDue(2000000) != nullptr,
        "the receiver retains a deferred intent beyond the sender's one-second window");
    expect(receive(2, 1, Action::TrackStop), "Stop from a different peer replaces deferred restart");
    expect(!receive(1, 1, Action::TrackRestart) && recovery.pending()->transport.action == Action::TrackStop,
        "retransmitted old restart cannot undo a newer Stop");
    expect(receive(1, 2, Action::TrackRestart) && !receive(1, 1, Action::TrackStop),
        "a new restart replaces Stop while reordered older events are rejected");
    recovery.clear();
    expect(!recovery.pending() && !recovery.takeDue(3000000),
        "local Stop or session/source removal clears pending recovery");
    expect(receive(3, 1, Action::TrackRestart) && recovery.takeDue(3000001),
        "a fresh command can recover immediately after cancellation");
    recovery.clear();
    expect(!receive(3, 1, Action::TrackRestart) && !recovery.pending(),
        "completed commands never execute twice");
    return failures == 0 ? 0 : 1;
}

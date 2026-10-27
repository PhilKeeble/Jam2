#include "metronome.hpp"
#include "session_authority.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
constexpr std::uint64_t second=1000000, duration=3600*second, eventAt=second+10000;
enum class Case { Stable, LateJoin, Revision, LostRevision, LostJoin, Handoff, Reorder };
struct Scenario { const char* name; Case kind; };
struct Policy { const char* name; std::uint64_t interval; bool disabled=false,initialOnly=false; };
struct Packet { std::uint64_t arrival; int target; bool assignment; jam2::GridAuthorityState state; };
struct Peer {
    jam2::SessionAuthority owner;
    jam2::metronome::AuthorityBeaconSchedule beacons{second};
    bool active=true;
    std::uint64_t firstReady=0,latestReady=0,beaconRevision=0,lastAssignment=0,rejectedBeacons=0,staleAssignments=0;
    explicit Peer(std::uint64_t id) : owner(id,1,1) {}
};
void require(bool yes,const char* reason) { if (!yes) throw std::runtime_error(reason); }
void run(Scenario scenario,Policy policy)
{
    std::array<Peer,4> peers{Peer(1),Peer(2),Peer(3),Peer(4)};
    const bool joining=scenario.kind==Case::LateJoin || scenario.kind==Case::LostJoin;
    const bool revising=scenario.kind==Case::Revision || scenario.kind==Case::LostRevision || scenario.kind==Case::Handoff || scenario.kind==Case::Reorder;
    peers[3].active=!joining;
    auto ordered=peers[0].owner.orderGridProposal({1,1,jam2::GridRunState::Running,0,true,false});
    require(ordered.has_value() && peers[0].owner.activateLocalGrid(1000,480000),"initial coordinator grid");
    auto current=peers[0].owner.grid(); const auto old=current;
    std::uint64_t nextAssignment=0,assignmentBatches=0,assignmentDatagrams=0,beaconDatagrams=0,dropped=0;
    bool lost=false; std::vector<Packet> pending;
    for (std::uint64_t time=0;time<=duration;time+=10000) {
        if (time==eventAt) {
            if (joining) peers[3].active=true;
            if (revising) {
                const bool handoff=scenario.kind==Case::Handoff;
                ordered=peers[0].owner.orderGridProposal({handoff ? 2ULL : 1ULL,2,jam2::GridRunState::Running,
                    static_cast<std::uint8_t>(handoff ? 1 : 0),!handoff,handoff});
                require(ordered.has_value(),"coordinator revision/handoff proposal");
                if (!handoff) require(peers[0].owner.activateLocalGrid(100000,528480),"new epoch activation");
                current=peers[0].owner.grid();
                nextAssignment=0; // Existing runtime immediately publishes a newly ordered revision.
                for (int i=1;i<4;++i) peers[i].latestReady=0;
            }
            if (scenario.kind==Case::Reorder && !policy.disabled) for (int i=1;i<4;++i) pending.push_back({time+200000,i,true,old});
        }
        if (time<duration && !policy.disabled && !(policy.initialOnly && assignmentBatches) && time>=nextAssignment) {
            ++assignmentBatches; nextAssignment=time+policy.interval;
            auto assignment=current; assignment.authority_packet_frame=0; // Assignments carry an ID, not a current clock timestamp.
            for (int i=1;i<4;++i) if (peers[i].active) {
                ++assignmentDatagrams;
                if (i==3 && !lost && time>=eventAt && (scenario.kind==Case::LostRevision || scenario.kind==Case::LostJoin)) {
                    lost=true; ++dropped; continue;
                }
                pending.push_back({time+2000,i,true,assignment});
            }
        }
        // Deliver assignments first at this virtual service boundary. Acceptance,
        // duplicates, stale revisions and publisher authorization are production code.
        for (auto it=pending.begin();it!=pending.end();) {
            if (it->arrival>time) { ++it; continue; }
            auto& peer=peers[it->target];
            if (it->assignment) {
                const auto result=peer.owner.acceptGridAssignment(1,it->state);
                if (result==jam2::AuthorityUpdateResult::Accepted) { peer.beaconRevision=0; peer.lastAssignment=it->arrival; }
                else if (result==jam2::AuthorityUpdateResult::StaleRevision) ++peer.staleAssignments;
            } else {
                if (peer.owner.acceptGridAuthorityState(it->state.authority_peer_id,it->state)==jam2::AuthorityUpdateResult::Accepted)
                    peer.beaconRevision=it->state.revision;
                else ++peer.rejectedBeacons;
            }
            it=pending.erase(it);
        }
        const auto publisher=static_cast<int>(current.authority_peer_id-1);
        auto& source=peers[publisher];
        // A newly assigned publisher cannot publish that grid before learning its assignment.
        if (time<duration && source.owner.grid().revision==current.revision && source.owner.localIsGridAuthority()) {
            const auto active=static_cast<std::uint64_t>(std::count_if(peers.begin(),peers.end(),[](const auto& peer) { return peer.active; })-1);
            const jam2::metronome::AuthorityBeaconState stamp{current.revision,current.authority_epoch_frame,active};
            if (source.beacons.due(time,stamp)) {
                auto beacon=current; beacon.authority_packet_frame=480000+time*48000/second;
                for (int i=0;i<4;++i) if (i!=publisher && peers[i].active) { pending.push_back({time+2000,i,false,beacon}); ++beaconDatagrams; }
                source.beacons.commitAttempt(time,stamp);
            }
        }
        for (int i=1;i<4;++i) {
            auto& peer=peers[i];
            const bool ready=peer.active && peer.owner.grid().revision==current.revision &&
                peer.owner.grid().authority_peer_id==current.authority_peer_id &&
                (peer.owner.localIsGridAuthority() || peer.beaconRevision==current.revision);
            if (ready) { if (!peer.firstReady) peer.firstReady=time; if (!peer.latestReady) peer.latestReady=time; }
        }
    }
    require(policy.disabled ? assignmentDatagrams==0 : assignmentDatagrams>0,"assignment policy not exercised");
    if (scenario.kind==Case::Stable && !policy.disabled && !policy.initialOnly)
        require(assignmentBatches==duration/policy.interval,"stable assignment cadence/count mismatch");
    for (int i=1;i<4;++i) {
        const auto& peer=peers[i];
        const bool expected=!policy.disabled && !(policy.initialOnly && (revising || (joining && i==3)));
        const bool converged=peer.owner.grid().revision==current.revision && peer.owner.grid().authority_peer_id==current.authority_peer_id &&
            (peer.owner.localIsGridAuthority() || peer.beaconRevision==current.revision);
        require(converged==expected,"assignment change altered final convergence unexpectedly");
        if (policy.disabled) require(peer.owner.grid().revision==0 && peer.rejectedBeacons>0,"beacons initialized an unassigned follower");
        if (scenario.kind==Case::Reorder && expected) require(peer.staleAssignments>0,"reordered old assignment rolled back current grid");
        const auto origin=(revising || (joining && i==3)) ? eventAt : 0;
        if (converged && i==3 && scenario.kind==Case::LostRevision && !policy.initialOnly)
            require(peer.lastAssignment-origin==policy.interval+2000,"lost revision retry did not use configured repeat interval");
        std::cout << "assignment_sim scenario=" << scenario.name << " policy=" << policy.name << " peers=4 peer=" << i
            << " virtual_seconds=3600 converged=" << converged << " revision=" << peer.owner.grid().revision
            << " expected_revision=" << current.revision << " authority=" << peer.owner.grid().authority_peer_id
            << " assignment_wait_us=" << (converged ? static_cast<std::int64_t>(peer.lastAssignment>=origin ? peer.lastAssignment-origin : 0) : -1)
            << " ready_wait_us=" << (converged ? static_cast<std::int64_t>(peer.latestReady-origin) : -1)
            << " accepted_assignments=" << peer.owner.stats().grid_assignments_accepted
            << " duplicates=" << peer.owner.stats().grid_assignments_duplicate << " rejected_beacons=" << peer.rejectedBeacons
            << " stale_assignments=" << peer.staleAssignments << " assignment_batches=" << assignmentBatches
            << " assignment_datagrams=" << assignmentDatagrams << " assignment_ipv4_bytes=" << assignmentDatagrams*120
            << " dropped=" << dropped << " beacon_datagrams=" << beaconDatagrams << '\n';
    }
}
}
int main()
{
    try {
        constexpr std::array<Policy,6> policies{{{"100ms",100000},{"1s",second},{"60s",60*second},{"600s",600*second},
            {"off-from-start",100000,true},{"initial-only",100000,false,true}}};
        constexpr std::array<Scenario,7> scenarios{{{"stable",Case::Stable},{"late-join",Case::LateJoin},
            {"revision",Case::Revision},{"lost-revision",Case::LostRevision},{"lost-join",Case::LostJoin},
            {"authority-handoff",Case::Handoff},{"reordered-old",Case::Reorder}}};
        for (const auto scenario : scenarios) for (const auto policy : policies) run(scenario,policy);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

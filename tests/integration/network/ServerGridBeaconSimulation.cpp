#include "metronome.hpp"
#include "session_authority.hpp"
#include "transport_timing.hpp"
#include "RuntimeContracts.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
constexpr std::uint64_t sampleRate=48000, second=1000000, duration=3600*second;
enum class Delivery { Periodic, Every100ms, Every250ms, Every1000ms, EveryMinute, BootstrapOnly, Disabled };
enum class Network { Clean, Jitter, Asymmetric, CallbackBlocks, Outage, ClockChange, LateJoin, EpochReset, ResetLoss };
struct Scenario { const char* name; Network network; bool drift; };
struct Packet { std::uint64_t arrival; jam2::GridAuthorityState state; };
struct Peer {
    jam2::SessionAuthority authority;
    std::deque<Packet> packets;
    bool mapped=false;
    std::uint64_t epoch=0,firstMapped=0,delivered=0,filtered=0,lost=0;
    std::uint64_t invalidatedAt=0,maxMappingWait=0,mappings=0,unmappedUs=0,stale=0;
    std::int64_t base=0,offset=0,target=0;
    std::array<double,3> checkpoints{};
    std::vector<double> errors;
    double maxError=0;
    double maxFirstTenMinutes=0;
    double currentError=0;
    explicit Peer(std::uint64_t id) : authority(id,1,1) { checkpoints.fill(std::numeric_limits<double>::quiet_NaN()); }
};
void require(bool value,const char* reason) { if (!value) throw std::runtime_error(reason); }
std::uint64_t clockFrame(int peer,std::uint64_t time,Scenario scenario,bool observed)
{
    constexpr std::array<int,4> ppm{0,50,-50,100};
    const auto drift=scenario.drift ? ppm[peer] : 0;
    long double driftTime=static_cast<long double>(time);
    if (scenario.network==Network::ClockChange && time>duration/2)
        driftTime=static_cast<long double>(duration)-time; // Same ppm, opposite sign after 30 min.
    auto frames=static_cast<std::uint64_t>(480000+peer*8192+
        std::floor(static_cast<long double>(sampleRate)*time/second+
            static_cast<long double>(sampleRate)*drift*driftTime/1000000000000.0L));
    if (observed && scenario.network==Network::CallbackBlocks) frames=frames/128*128;
    return frames;
}
const char* name(Delivery mode)
{
    switch (mode) {
    case Delivery::Periodic: return "20ms";
    case Delivery::Every100ms: return "100ms";
    case Delivery::Every250ms: return "250ms";
    case Delivery::Every1000ms: return "1000ms";
    case Delivery::EveryMinute: return "60000ms";
    case Delivery::BootstrapOnly: return "bootstrap-then-off";
    case Delivery::Disabled: return "off-from-start";
    }
    return "invalid";
}

void fixtures()
{
    jam2::metronome::AuthorityBeaconSchedule cadence(1000000);
    jam2::metronome::AuthorityBeaconState state{1,1000,3};
    require(cadence.intervalUs()==1000000 && cadence.due(0,state),"initial authority observation deferred");
    require(cadence.phaseFreshnessUs()==2500000 && cadence.phaseFresh(600001,1000) &&
        cadence.phaseFresh(2501000,1000) && !cadence.phaseFresh(2501001,1000) && !cadence.phaseFresh(999,1000) &&
        !cadence.phaseFresh(1000,0),"one-second authority phase validity has a periodic stale gap or accepts invalid time");
    cadence.commitAttempt(0,state);
    require(!cadence.due(999999,state) && cadence.due(1000000,state),"steady beacon interval is not one second");
    ++state.revision; require(cadence.due(100,state),"revision update waited for periodic beacon"); cadence.commitAttempt(100,state);
    ++state.epoch; require(cadence.due(101,state),"new epoch waited for periodic beacon"); cadence.commitAttempt(101,state);
    ++state.active_peers; require(cadence.due(102,state),"new active peer waited for periodic beacon"); cadence.commitAttempt(102,state);
    require(!cadence.due(103,state) && cadence.due(1000102,state),"event refresh created a periodic send burst");
    cadence.commitAttempt(UINT64_MAX-10,state);
    require(!cadence.due(UINT64_MAX-1,state) && cadence.due(UINT64_MAX,state),"beacon deadline wrapped");
    using jam2::metronome::grid_phase_error;
    require(grid_phase_error({100,0,90,0,1000})==10,"positive phase error");
    require(grid_phase_error({995,0,5,0,1000})==-10,"bar wrap phase error");
    require(grid_phase_error({100,200,90,200,1000})==10,"future epoch phase error");
    require(!grid_phase_error({0,0,0,0,0}) && !grid_phase_error({0,0,0,0,UINT64_MAX}),"invalid bar interval accepted");
    const auto mapping=jam2::metronome::map_authority_clock(1000,5000,7000);
    require(mapping.valid && mapping.render_offset_frames==-2000,"initial clock mapping");
}

void simulate(Scenario scenario,Delivery mode)
{
    std::array<Peer,4> peers{Peer(1),Peer(2),Peer(3),Peer(4)};
    auto assignment=peers[0].authority.orderGridProposal({1,1,jam2::GridRunState::Running,0,true,false});
    require(assignment.has_value(),"four-peer authority setup");
    auto epoch=clockFrame(0,200000,scenario,false);
    require(peers[0].authority.activateLocalGrid(epoch,clockFrame(0,0,scenario,true)),"authority grid activation");
    assignment=peers[0].authority.grid();
    for (int i=1;i<4;++i) require(peers[i].authority.acceptGridAssignment(1,*assignment)==jam2::AuthorityUpdateResult::Accepted,"four-peer assignment");
    const Jam2MetronomeCompensationSettings settings; // Actual runtime defaults.
    constexpr auto tick=10000ULL,bar=96000ULL;
    const auto interval=mode==Delivery::Every100ms ? 100000ULL : mode==Delivery::Every250ms ? 250000ULL :
        mode==Delivery::Every1000ms ? 1000000ULL : mode==Delivery::EveryMinute ? 60000000ULL : 20000ULL;
    constexpr auto changeAt=second+10000; // Just after a scheduled 1-s/minute observation.
    std::array<bool,4> resetBeaconLost{};
    double maxPairwise=0,maxPairwiseFirstTenMinutes=0;
    std::uint64_t emitted=0;
    for (std::uint64_t time=0;time<=duration;time+=tick) {
        if ((scenario.network==Network::EpochReset || scenario.network==Network::ResetLoss) && time==changeAt) {
            assignment=peers[0].authority.orderGridProposal({1,2,jam2::GridRunState::Running,0,true,false});
            require(assignment.has_value(),"ordered epoch reset");
            epoch=clockFrame(0,time+200000,scenario,false);
            require(peers[0].authority.activateLocalGrid(epoch,clockFrame(0,time,scenario,true)),"reset authority activation");
            assignment=peers[0].authority.grid();
            for (int i=1;i<4;++i) {
                auto& peer=peers[i];
                require(peer.authority.acceptGridAssignment(1,*assignment)==jam2::AuthorityUpdateResult::Accepted,"reset assignment rejected");
                peer.mapped=false; peer.base=peer.offset=peer.target=0; peer.invalidatedAt=time;
            }
        }
        if (time%interval==0 && time<duration) {
            ++emitted;
            for (int i=1;i<4;++i) {
                auto& peer=peers[i];
                if (scenario.network==Network::LateJoin && i==3 && time<changeAt) continue;
                if (mode==Delivery::Disabled || (mode==Delivery::BootstrapOnly && peer.mapped)) { ++peer.filtered; continue; }
                const auto sequence=time/interval;
                if (scenario.network==Network::ResetLoss && time>=changeAt && !resetBeaconLost[i]) {
                    resetBeaconLost[i]=true; ++peer.lost; continue;
                }
                if (scenario.network==Network::Outage &&
                    ((sequence%17==16) || (time>=300*second && time<310*second))) { ++peer.lost; continue; }
                const std::uint64_t baseDelay=scenario.network==Network::Asymmetric ? 8000 : 2000;
                const std::uint64_t noise=scenario.network==Network::Jitter ? ((sequence*37+i*11)%6)*1000 : 0;
                // Delays are under one send interval, so this queue stays ordered.
                auto state=*assignment; state.authority_packet_frame=clockFrame(0,time,scenario,true);
                peer.packets.push_back({time+baseDelay+noise,state});
            }
        }
        for (int i=1;i<4;++i) {
            auto& peer=peers[i];
            while (!peer.packets.empty() && peer.packets.front().arrival<=time) {
                const auto packet=peer.packets.front(); peer.packets.pop_front();
                if (peer.authority.acceptGridAuthorityState(1,packet.state)!=jam2::AuthorityUpdateResult::Accepted) { ++peer.stale; continue; }
                ++peer.delivered;
                // Clean RTT=4 ms; asymmetric RTT=10 ms but forward path=8 ms.
                const auto oneWayFrames=sampleRate*(scenario.network==Network::Asymmetric ? 5000 : 2000)/second;
                const auto projected=packet.state.authority_packet_frame+oneWayFrames;
                const auto local=clockFrame(i,packet.arrival,scenario,true);
                if (!peer.mapped) {
                    const auto mapping=jam2::metronome::map_authority_clock(packet.state.authority_epoch_frame,projected,local);
                    require(mapping.valid,"initial authority mapping rejected");
                    peer.mapped=true; peer.epoch=mapping.epoch_sample_time; peer.base=peer.offset=peer.target=mapping.render_offset_frames;
                    if (!peer.mappings) peer.firstMapped=packet.arrival;
                    ++peer.mappings;
                    const auto invalidated=scenario.network==Network::LateJoin && i==3 ? changeAt : peer.invalidatedAt;
                    peer.maxMappingWait=(std::max)(peer.maxMappingWait,packet.arrival-invalidated);
                }
                const auto error=jam2::metronome::grid_phase_error({projected,packet.state.authority_epoch_frame,
                    jam2::transport_musical_frame_from_raw(local,peer.offset),peer.epoch,bar});
                require(error.has_value(),"production phase calculation rejected simulated observation");
                peer.target=peer.offset+*error;
            }
            if (!peer.mapped) {
                if (time && !(scenario.network==Network::LateJoin && i==3 && time<changeAt)) peer.unmappedUs+=tick;
                continue;
            }
            if (time) peer.offset=jam2_next_metronome_compensation_offset(peer.offset,peer.base,peer.target,
                tick/1000.0,sampleRate,settings);
            // Ground truth uses continuous independent device clocks, not the
            // production RTT-derived error estimate or callback snapshots.
            const auto phase=static_cast<std::int64_t>(clockFrame(i,time,scenario,false))+peer.offset-static_cast<std::int64_t>(peer.epoch);
            const auto truth=static_cast<std::int64_t>(clockFrame(0,time,scenario,false))-static_cast<std::int64_t>(epoch);
            const double errorMs=(phase-truth)*1000.0/sampleRate;
            peer.currentError=errorMs;
            if (time>=second && time<=600*second) peer.maxFirstTenMinutes=(std::max)(peer.maxFirstTenMinutes,std::abs(errorMs));
            if (time>=second) peer.maxError=(std::max)(peer.maxError,std::abs(errorMs));
            if (time>=second && time%second==0) {
                peer.errors.push_back(std::abs(errorMs));
            }
            if (time==60*second) peer.checkpoints[0]=errorMs;
            if (time==600*second) peer.checkpoints[1]=errorMs;
            if (time==duration) peer.checkpoints[2]=errorMs;
        }
        if (time>=second && std::all_of(peers.begin()+1,peers.end(),[](const auto& peer) { return peer.mapped; })) {
            double low=0,high=0;
            for (int i=1;i<4;++i) { low=(std::min)(low,peers[i].currentError); high=(std::max)(high,peers[i].currentError); }
            maxPairwise=(std::max)(maxPairwise,high-low);
            if (time<=600*second) maxPairwiseFirstTenMinutes=(std::max)(maxPairwiseFirstTenMinutes,high-low);
        }
    }
    for (int i=1;i<4;++i) {
        auto& peer=peers[i];
        require(peer.mapped==(mode!=Delivery::Disabled),"off-from-start unexpectedly initialized the grid");
        require(mode!=Delivery::BootstrapOnly || peer.delivered==peer.mappings,"bootstrap-only delivered continuing beacons");
        require(mode!=Delivery::Disabled || peer.delivered==0,"disabled delivered a beacon");
        if (scenario.network==Network::Clean && !scenario.drift && peer.mapped) require(peer.maxError<=1.0/sampleRate*1000,"equal clocks drifted in a clean path");
        if (scenario.network==Network::Clean && scenario.drift && mode==Delivery::BootstrapOnly) {
            constexpr std::array<int,4> ppm{0,50,-50,100};
            require(std::abs(peer.checkpoints[2]-ppm[i]*3.6)<=0.05,"free-running drift disagrees with injected ppm");
        }
        if (scenario.network==Network::Clean && scenario.drift && mode!=Delivery::BootstrapOnly && mode!=Delivery::Disabled && mode!=Delivery::EveryMinute)
            require(std::abs(peer.checkpoints[1])<3.0,"periodic synchronization did not bound drift over ten minutes");
        if (scenario.network==Network::Clean && scenario.drift && mode==Delivery::EveryMinute) {
            constexpr std::array<int,4> ppm{0,50,-50,100};
            require(std::abs(peer.checkpoints[0]-ppm[i]*0.06)<=0.05,"minute cadence did not coast until its second observation");
        }
        if (scenario.network==Network::LateJoin && i==3 && mode!=Delivery::Disabled)
            require(peer.maxMappingWait==(mode==Delivery::BootstrapOnly ? 10000 : interval-(changeAt%interval))+2000,"late join observation wait differs from cadence");
        if ((scenario.network==Network::EpochReset || scenario.network==Network::ResetLoss) && mode!=Delivery::Disabled) {
            const auto recoveryInterval=mode==Delivery::BootstrapOnly ? 20000ULL : interval;
            const auto lostDelay=scenario.network==Network::ResetLoss ? recoveryInterval : 0;
            require(peer.mappings==2 && peer.maxMappingWait==recoveryInterval-(changeAt%recoveryInterval)+lostDelay+2000,"epoch recovery wait did not account for next/lost beacon");
        }
        std::sort(peer.errors.begin(),peer.errors.end());
        const auto percentile=[&](double fraction) { return peer.errors.empty() ? std::numeric_limits<double>::quiet_NaN() : peer.errors[static_cast<std::size_t>((peer.errors.size()-1)*fraction)]; };
        std::cout << "grid_beacon_sim scenario=" << scenario.name << " delivery=" << name(mode) << " peers=4 peer=" << i
            << " virtual_seconds=" << duration/second << " mapped=" << peer.mapped << " first_mapping_us=" << peer.firstMapped
            << " mappings=" << peer.mappings << " max_mapping_wait_us=" << peer.maxMappingWait << " unmapped_us=" << peer.unmappedUs << " stale=" << peer.stale
            << " delivered=" << peer.delivered << " filtered=" << peer.filtered << " lost=" << peer.lost
            << " delivered_ipv4_bytes=" << peer.delivered*120 << " abs_p50_ms=" << percentile(0.5) << " abs_p95_ms=" << percentile(0.95)
            << " abs_max_ms=" << (peer.mapped ? peer.maxError : std::numeric_limits<double>::quiet_NaN()) << " error_60s_ms=" << peer.checkpoints[0] << " error_600s_ms=" << peer.checkpoints[1]
            << " max_first_600s_ms=" << (peer.mapped ? peer.maxFirstTenMinutes : std::numeric_limits<double>::quiet_NaN())
            << " error_3600s_ms=" << peer.checkpoints[2] << " offset_from_base_ms=" << (peer.offset-peer.base)*1000.0/sampleRate << '\n';
    }
    require(emitted==duration/interval,"simulation did not span one full hour at configured cadence");
    std::cout << "grid_beacon_group scenario=" << scenario.name << " delivery=" << name(mode) << " peers=4"
        << " max_pairwise_first_600s_ms=" << (mode==Delivery::Disabled ? std::numeric_limits<double>::quiet_NaN() : maxPairwiseFirstTenMinutes)
        << " max_pairwise_ms=" << (mode==Delivery::Disabled ? std::numeric_limits<double>::quiet_NaN() : maxPairwise) << '\n';
}
}
int main()
{
    try {
        fixtures();
        constexpr std::array<Scenario,10> scenarios{{
            {"equal-clean",Network::Clean,false},{"drift-clean",Network::Clean,true},
            {"equal-jitter",Network::Jitter,false},{"drift-asymmetric",Network::Asymmetric,true},
            {"drift-callback128",Network::CallbackBlocks,true},{"drift-loss-outage",Network::Outage,true},
            {"drift-sign-change",Network::ClockChange,true},{"late-join",Network::LateJoin,true},
            {"epoch-reset",Network::EpochReset,true},{"epoch-reset-first-lost",Network::ResetLoss,true}}};
        for (const auto scenario : scenarios) for (const auto mode : {Delivery::Periodic,Delivery::Every100ms,Delivery::Every250ms,
            Delivery::Every1000ms,Delivery::EveryMinute,Delivery::BootstrapOnly,Delivery::Disabled}) simulate(scenario,mode);
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

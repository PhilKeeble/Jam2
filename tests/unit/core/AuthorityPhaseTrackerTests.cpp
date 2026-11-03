#include "metronome.hpp"
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
namespace {
void require(bool condition,const char* why) { if (!condition) throw std::runtime_error(why); }
using jam2::metronome::AuthorityPhaseDecision;
void orderingAndReset() {
    jam2::metronome::AuthorityPhaseTracker tracker;
    const jam2::metronome::AuthorityPhaseTrackingConfig config;
    require(tracker.observe({100,1000,-900000},config)==AuthorityPhaseDecision::Accepted,"initial large clock offset rejected");
    require(tracker.observe({90,2000,-800000},config)==AuthorityPhaseDecision::Reordered,"older source frame changed clock");
    require(tracker.observe({110,500,-800000},config)==AuthorityPhaseDecision::Reordered,"backward receive timestamp accepted");
    require(tracker.observe({120,2000,-930000},config)==AuthorityPhaseDecision::Delayed,"late beacon changed established clock");
    require(tracker.observe({115,3000,-900000},config)==AuthorityPhaseDecision::Reordered,"late observation did not retain ordering boundary");
    require(tracker.observe({130,4000,-899000},config)==AuthorityPhaseDecision::Accepted,"better delivery baseline rejected");
    tracker.reset();
    require(tracker.observe({1,1,900000},config)==AuthorityPhaseDecision::Accepted,"new epoch or authority retained old clock baseline");
    require(tracker.observe({2,2,0},{0,240,1000})==AuthorityPhaseDecision::Invalid,"invalid sample rate accepted");
    tracker.reset();
    for (std::uint64_t second=0;second<100;++second) {
        const auto result=tracker.observe({second*48000+1,second*1000000+1,-std::int64_t(second*192)},config);
        if (second>2) require(result==AuthorityPhaseDecision::Delayed,"small queue-delay steps accumulated the tolerance band");
    }
}
void epochsAndHandover() {
    using jam2::metronome::grid_clock_offset;
    require(grid_clock_offset({10000,8000,20000,18000})==0,"different epoch origins changed musical phase");
    require(grid_clock_offset({10000,8000,43200,8000})==-33200,"long delivery delay wrapped into a forward correction");
    constexpr auto maximum=(std::numeric_limits<std::uint64_t>::max)();
    require(!grid_clock_offset({maximum,0,0,0}),"oversized signed clock offset accepted");
    constexpr auto signedMaximum=std::uint64_t((std::numeric_limits<std::int64_t>::max)());
    require(!grid_clock_offset({signedMaximum,0,0,signedMaximum}),"clock plus epoch offset overflow accepted");
    constexpr std::array<std::uint64_t,4> origins{10000,20000,30000,40000};
    for (std::size_t local=0;local<origins.size();++local) {
        const auto before=grid_clock_offset({origins[0]+96000,origins[0]+48000,origins[local]+96000,origins[0]+48000});
        const auto after=grid_clock_offset({origins[2]+96000,origins[2]+48000,origins[local]+96000,origins[0]+48000});
        require(before==after,"four-peer authority handover lost the preserved listener epoch");
    }
}
void fourPeerStress() {
    constexpr std::array<int,4> ppm{0,500,-500,17};
    std::array<jam2::metronome::AuthorityPhaseTracker,4> peers;
    const jam2::metronome::AuthorityPhaseTrackingConfig config;
    for (std::uint64_t second=0;second<3600;++second) for (std::size_t peer=0;peer<peers.size();++peer) {
        const auto trueOffset=std::int64_t(std::llround(double(ppm[peer])*second*48000/1000000));
        const bool delayed=second>10 && second%180>=19 && second%180<=24;
        const auto delay=delayed ? 200000+(second%6)*90000 : (second%3)*1000;
        const auto candidate=trueOffset-std::int64_t(delay*48000/1000000);
        const auto result=peers[peer].observe({second*48000+1,second*1000000+delay+1,candidate},config);
        if (delayed) require(result==AuthorityPhaseDecision::Delayed,"stress beacon produced a false large correction");
        else require(result==AuthorityPhaseDecision::Accepted,"normal drift or ordinary jitter rejected");
        require(std::abs(peers[peer].stats().accepted_offset_frames-trueOffset)<480,
            "four independent clocks lost synchronization during delayed observations");
    }
}
}
int main() {
    try { orderingAndReset();epochsAndHandover();fourPeerStress();std::cout<<"authority phase ordering, reset, epochs and four-peer delay/drift stress passed\n";return 0; }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}

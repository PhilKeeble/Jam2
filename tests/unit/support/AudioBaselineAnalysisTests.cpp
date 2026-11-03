#include "../../support/AudioBaselineAnalysis.hpp"
#include <iostream>
#include <numbers>
int main() {
    try {
        constexpr int rate=44100;
        std::vector<std::int64_t> a,b,c,d;
        for (int i=0;i<30;++i) {
            a.push_back(700+i*rate);b.push_back(700+i*rate+4200+132);
            c.push_back(200+i*rate);d.push_back(200+i*rate-4200+196);
        }
        const auto first=jam2::test::pulsePhaseFrames(a,b,rate),second=jam2::test::pulsePhaseFrames(c,d,rate);
        if (std::abs(jam2::test::pulseRoundTripMs(first,second,rate)-328000./rate)>1e-9)
            throw std::runtime_error("pulse measurement did not cancel independent clock origins");
        bool rejected=false;
        try { (void)jam2::test::pulsePhaseFrames({},b,rate); } catch (const std::runtime_error&) { rejected=true; }
        if (!rejected) throw std::runtime_error("missing signal accepted");
        std::vector<std::int16_t> tone(rate*10);
        for (std::size_t i=0;i<tone.size();++i) tone[i]=std::int16_t(std::sin(i*2*std::numbers::pi*440/rate)*4000);
        if (std::abs(jam2::test::crossingToneHz(tone,rate,0,tone.size())-440)>.001)
            throw std::runtime_error("tone measurement inaccurate");
        const auto curvature=jam2::test::secondDifferenceRms(tone,0,tone.size());
        const auto expected=4000/std::sqrt(2.)*(2-2*std::cos(2*std::numbers::pi*440/rate));
        if (std::abs(curvature-expected)>1)
            throw std::runtime_error("tone curvature measurement inaccurate");
        auto held=tone;
        for (std::size_t i=63;i<held.size();i+=64) held[i]=tone[i-1];
        if (jam2::test::secondDifferenceRms(held,0,held.size())<curvature*2)
            throw std::runtime_error("repeated held samples escaped waveform analysis");
        rejected=false;
        try { (void)jam2::test::secondDifferenceRms(tone,0,2); } catch (const std::runtime_error&) { rejected=true; }
        if (!rejected) throw std::runtime_error("invalid curvature range accepted");
        std::cout<<"recorded pulse timing and tone-frequency analysis passed\n";return 0;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}

#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace jam2::test {
// PCM curvature exposes short held/skipped samples that can remain below a
// large-step detector. Report in PCM16 units rather than a quality score.
inline double secondDifferenceRms(const std::vector<std::int16_t>& samples,
    std::int64_t begin, std::int64_t end) {
    if (begin<0 || end>std::int64_t(samples.size()) || end-begin<3)
        throw std::runtime_error("invalid waveform curvature range");
    long double squared=0;
    for (auto n=begin+1;n<end-1;++n) {
        const long double difference=int(samples[std::size_t(n+1)])-
            2*int(samples[std::size_t(n)])+int(samples[std::size_t(n-1)]);
        squared+=difference*difference;
    }
    return std::sqrt(double(squared/(end-begin-2)));
}
inline double pulsePhaseFrames(const std::vector<std::int64_t>& local,
    const std::vector<std::int64_t>& remote,std::int64_t period) {
    if (local.empty() || remote.empty() || period<=0) throw std::runtime_error("missing pulse references");
    std::vector<double> phase;
    for (const auto frame:local) {
        const auto after=std::lower_bound(remote.begin(),remote.end(),frame);
        auto nearest=after==remote.end() ? remote.back() : *after;
        if (after!=remote.begin() && std::abs(*(after-1)-frame)<std::abs(nearest-frame)) nearest=*(after-1);
        if (std::abs(nearest-frame)<period/2) phase.push_back(double(nearest-frame));
    }
    if (phase.size()<5) throw std::runtime_error("insufficient paired pulse observations");
    std::sort(phase.begin(),phase.end());
    return phase.size()%2 ? phase[phase.size()/2] : (phase[phase.size()/2-1]+phase[phase.size()/2])*.5;
}
// Opposite-direction phase offsets cancel the devices' different clock origins.
// This is a rendered round trip modulo one pulse period; use on clean paths with
// delay known to be below half that period. It excludes the analog converters.
inline double pulseRoundTripMs(double phaseA,double phaseB,int sampleRate) {
    if (sampleRate<=0) throw std::runtime_error("invalid pulse sample rate");
    const double sum=phaseA+phaseB;
    const double positive=sum-std::floor(sum/sampleRate)*sampleRate;
    if (positive>=sampleRate*.5) throw std::runtime_error("pulse round trip is ambiguous or exceeds half a second");
    return positive*1000/sampleRate;
}
inline double crossingToneHz(const std::vector<std::int16_t>& samples,int sampleRate,
    std::int64_t begin,std::int64_t end) {
    double first=0,last=0;std::uint64_t crossings=0;
    for (auto n=begin+1;n<end;++n) {
        const auto a=samples[std::size_t(n-1)],b=samples[std::size_t(n)];
        if (a<=0 && b>0) {
            const double frame=double(n-1)-double(a)/(double(b)-a);
            if (!crossings) first=frame;
            last=frame;++crossings;
        }
    }
    return crossings>1 && last>first ? (crossings-1)*sampleRate/(last-first) : 0;
}
}

#include "peer_stream.hpp"
#include "protocol.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>

namespace {
// Observe the production estimator and its actual source-resampler request.
// Queueing/quality consequences are measured separately by the device tests.
struct Sink final : jam2::PeerStreamPlayback {
    double ratio=1;
    std::size_t depthFrames() const noexcept override { return 0; }
    std::size_t pushFrames(std::span<const std::int32_t> frames) noexcept override { return frames.size(); }
    void requestDropFrames(std::size_t) noexcept override {}
    void setResamplerRatio(double value) noexcept override { ratio=value; }
};
int failures=0;
void expect(bool condition,const char* reason) {
    if(!condition) {std::cerr<<"FAIL: "<<reason<<'\n';++failures;}
}
template<int packet=128> void run(int stepUs,int clockPpm,bool jitter=false,bool accountOffsets=true) {
    constexpr int rate=48000;
    Sink sink;
    jam2::PeerStreamConfig config;
    config.sample_rate=rate;config.frames_per_packet=packet;
    config.audio_format=jam2::NetworkAudioFormat::Pcm16Mono;
    config.sample_time_playout=false;config.playback_max_frames=0;
    config.stats_warmup_us=0;config.drift_smoothing=.02;
    config.drift_deadband_ppm=0;config.drift_max_correction_ppm=500;
    config.drift_arrival_step_threshold_us=accountOffsets ? 2000 : 0;
    jam2::PeerStream stream(config,0,&sink);
    std::array<std::uint8_t,packet*2> bytes{};
    std::uint64_t previous=0;
    int nextCheckpoint=10;
    for(std::uint32_t sequence=0;sequence<rate*60/packet;++sequence) {
        const auto frame=std::uint64_t(sequence)*packet;
        const long double nominalUs=static_cast<long double>(frame)*1000000/rate/(1+clockPpm/1000000.L);
        const auto sendUs=std::uint64_t(std::llround(nominalUs));
        const bool held=sendUs>=3000000 && sendUs<40000000;
        // A removed delay drains a FIFO burst; receive timestamps remain
        // monotonic even when two contiguous packets become ready together.
        const auto jitterUs=jitter ? (sequence % 37U)*7U : 0U;
        const auto now=std::max(previous+1,std::uint64_t(10000+std::int64_t(sendUs)+(held ? stepUs : 0)+jitterUs));
        previous=now;
        const jam2::protocol::Header header{jam2::protocol::PacketType::Audio,1,sequence,frame,
            std::uint16_t(bytes.size()),0};
        if(stream.receiveAudio(header,bytes,now)!=jam2::PeerAudioResult::Accepted)
            throw std::runtime_error("delay-step fixture rejected contiguous audio");
        if(sendUs>=std::uint64_t(nextCheckpoint)*1000000) {
            const auto& stats=stream.stats();
            std::cout<<"METRIC drift_delay_step packet_frames="<<packet<<" step_us="<<stepUs<<" clock_ppm="<<clockPpm
                <<" jitter="<<jitter
                <<" elapsed_s="<<nextCheckpoint<<" offset_adjustments="<<stats.drift_arrival_offset_adjustments
                <<" offset_us="<<stats.drift_arrival_offset_us<<" raw_ppm="<<stats.raw_drift_ppm
                <<" smoothed_ppm="<<stats.drift_ppm<<" requested_ratio="<<sink.ratio
                <<" reordered_lost="<<stats.reordered_lost<<'\n';
            expect(stats.drift_valid && stats.reordered_lost==0,"fixture must retain valid drift observations and contiguous packets");
            // A constant added transit delay changes offset, not oscillator
            // frequency. After seven seconds at that delay, correction should
            // again describe the known source clock within 25 ppm.
            if (accountOffsets || stepUs==0 || nextCheckpoint>=40)
                expect(std::abs(stats.drift_ppm-clockPpm)<=25,
                    "settled constant delivery delay must not remain interpreted as source clock drift");
            else expect(std::abs(stats.drift_ppm-clockPpm)>25,
                "disabled offset accounting retains the measured lifetime-estimator negative control");
            expect(stats.drift_arrival_offset_adjustments ==
                (accountOffsets && stepUs!=0 ? (nextCheckpoint<40 ? 1U : 2U) : 0U),
                "offset counters distinguish real frequency, added/removed delay and disabled accounting");
            const auto expectedCorrection=std::clamp(accountOffsets ? double(clockPpm) : stats.drift_ppm,-500.0,500.0);
            expect(std::abs((sink.ratio-1)*1000000-expectedCorrection)<=25,
                "actual source correction must follow the known clock and its configured correction cap");
            nextCheckpoint=nextCheckpoint==10 ? 30 : 50;
            if(sendUs>=50000000) break;
        }
    }
}

void testResumeClearsArrivalOffset()
{
    constexpr std::uint64_t rate = 48000, packet = 128;
    Sink sink;
    jam2::PeerStreamConfig config;
    config.sample_rate = rate;
    config.frames_per_packet = packet;
    config.audio_format = jam2::NetworkAudioFormat::Pcm16Mono;
    config.sample_time_playout = false;
    config.stats_warmup_us = 0;
    jam2::PeerStream stream(config, 0, &sink);
    std::array<std::uint8_t, packet * 2> bytes{};
    std::uint32_t sequence = 0;
    const auto receive = [&](std::uint64_t now) {
        const jam2::protocol::Header header{jam2::protocol::PacketType::Audio, 1,
            sequence, std::uint64_t(sequence) * packet, std::uint16_t(bytes.size()), 0};
        expect(stream.receiveAudio(header, bytes, now) == jam2::PeerAudioResult::Accepted,
            "resumed offset fixture accepts contiguous authenticated audio");
        ++sequence;
    };
    std::uint64_t last = 0;
    while (std::uint64_t(sequence) * packet < rate * 10) {
        const auto elapsed = std::uint64_t(sequence) * packet * 1000000 / rate;
        last = 10000 + elapsed + (elapsed >= 3000000 ? 3000 : 0);
        receive(last);
    }
    expect(stream.stats().drift_arrival_offset_adjustments == 1 &&
            std::abs(stream.stats().drift_arrival_offset_us - 3000) <= 1,
        "fixture establishes a nonzero arrival-offset baseline before suspension");
    stream.advance(last + 3000000);
    expect(stream.stats().audio_suspended, "audio inactivity suspends the existing source");
    const auto resumedAt = last + 4000000;
    receive(resumedAt);
    expect(!stream.stats().audio_suspended && !stream.stats().drift_valid && sink.ratio == 1 &&
            stream.stats().drift_arrival_offset_us == 0 &&
            stream.stats().drift_arrival_offset_adjustments == 1,
        "resume clears the old offset and correction while preserving cumulative event counts");
    for (std::uint64_t i = 1; i <= 1000; ++i)
        receive(resumedAt + i * packet * 1000000 / rate);
    expect(stream.stats().drift_valid && std::abs(stream.stats().drift_ppm) <= 25 &&
            stream.stats().drift_arrival_offset_us == 0 &&
            stream.stats().drift_arrival_offset_adjustments == 1,
        "the resumed clock recalibrates without reusing pre-suspension offset observations");
}
}
int main() {
    try {
        run(0,0);run(0,200);run(0,-200);run(0,500);run(0,-500);run(0,1000);run(0,-1000);
        run(3000,0);run(-3000,0);
        run(3000,200);run(-3000,-200);run(3000,0,true);run(-3000,0,true);
        run<64>(0,0);run<64>(0,200);run<64>(0,-200);
        run<64>(3000,0);run<64>(-3000,0);
        run<64>(3000,200);run<64>(-3000,-200);run<64>(3000,0,true);
        run(3000,0,false,false);
        testResumeClearsArrivalOffset();
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
    return failures ? 1 : 0;
}

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "audio_device_processing.hpp"
#include "duplex_processing.hpp"
#include "wasapi_processing.hpp"
#include "engine.hpp"
#include "input_source.hpp"
#include "network_session.hpp"
#include "tuning_profile.hpp"
#include "../../support/ProfileComparison.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
int rate = 48000; // Selected once by the test case, before creating any audio/session objects.
constexpr std::size_t packetFrames = 64;
constexpr std::size_t callbackFrames = 32;
constexpr std::size_t participants = 4;
int failures = 0;

void expect(bool value, const char* message)
{
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

// Regression experiments exercise the interactions that previously lost
// capture time, stranded returning peers, and ignored the local device clock.
// Only packet delivery and device callback timing are injected. Acceptance,
// clocks, jitter, mixing, recovery, rings and resampling are production code.
class DeviceSink final : public jam2::PeerStreamPlayback {
public:
    explicit DeviceSink(const jam2::JoinProfile& profile)
        : ring(profile.playback_ring_frames), prefill(profile.playback_prefill_frames)
    {
        ring.set_diagnostics_enabled(true);
        control.playback_ratio_ramp_frames.store(rate / 4);
        control.playback_release_return_ramp_frames.store(
            rate * jam2::audio::kPlaybackReleaseReturnRampMs / 1000);
    }
    std::size_t depthFrames() const noexcept override { return ring.available_read(); }
    std::size_t callbackFrames() const noexcept override { return callbackQuantum; }
    std::uint64_t underrunFrames() const noexcept override { return ring.underrun_frames(); }
    jam2::audio::PlaybackRecoveryState playbackRecoveryState() const noexcept override
    {
        auto state = jam2::audio::device_processing::read_playback_recovery_state(control);
        state.padding_policy = paddingPolicy;
        return state;
    }
    std::size_t pushFrames(std::span<const std::int32_t> frames) noexcept override { return ring.push(frames); }
    void requestDropFrames(std::size_t frames) noexcept override { ring.request_drop_oldest(frames); }
    void setResamplerRatio(double ratio) noexcept override
    { control.playback_ratio_ppm.store(static_cast<int>(std::llround(ratio * 1000000.0))); }
    jam2::audio::DeviceClockSample deviceClockSample() const noexcept override
    { return jam2::audio::device_processing::read_device_clock(control); }
    void publishClock(std::uint64_t frame, std::uint64_t now)
    {
        jam2::audio::device_processing::publish_callback_begin(&control, generation);
        jam2::audio::device_processing::publish_device_clock(&control, {frame, now});
        jam2::audio::device_processing::publish_callback_end(&control, generation);
    }
    void callback()
    {
        if (!started && ring.available_read() < prefill) return;
        started = true;
        jam2::audio::device_processing::pop_resampled_playback(
            &ring, &control, resampler, scratch, std::span(output).first(callbackQuantum));
        ++callbacks;
        for (auto sample : std::span(output).first(callbackQuantum)) {
            nonzeroFrames += sample != 0 ? 1U : 0U;
            if (measureWaveform) {
                if (havePreviousSample) {
                    const auto step = static_cast<std::uint64_t>(std::abs(static_cast<std::int64_t>(sample) - previousSample));
                    maximumSampleStep = std::max(maximumSampleStep, step);
                    largeSampleSteps += step > sampleStepThreshold;
                }
                previousSample = sample; havePreviousSample = true;
                zeroRun = sample == 0 ? zeroRun + 1 : 0;
                maximumZeroRun = std::max(maximumZeroRun, zeroRun);
            }
        }
    }
    jam2::audio::MonoRingBuffer ring;
    jam2::audio::StreamControl control;
    jam2::audio::device_processing::PlaybackResamplerState resampler;
    std::array<std::int32_t, 520> scratch{};
    std::array<std::int32_t, 256> output{};
    std::size_t callbackQuantum = ::callbackFrames;
    jam2::audio::RecoveryPaddingPolicy paddingPolicy = jam2::audio::RecoveryPaddingPolicy::LearnedCushion;
    std::size_t prefill;
    bool started = false;
    std::uint64_t callbacks = 0, nonzeroFrames = 0;
    std::uint64_t generation = 0;
    bool measureWaveform = false, havePreviousSample = false;
    std::int32_t previousSample = 0;
    std::uint64_t sampleStepThreshold = 4200000, maximumSampleStep = 0, largeSampleSteps = 0;
    std::uint64_t zeroRun = 0, maximumZeroRun = 0;
};

jam2::PeerStreamConfig streamConfig(const jam2::JoinProfile& profile,int frames=int(packetFrames),
    jam2::NetworkAudioFormat format=jam2::NetworkAudioFormat::Pcm24Mono)
{
    jam2::PeerStreamConfig config;
    config.sample_rate = rate;
    config.frames_per_packet = frames;
    config.audio_format = format;
    config.playout_delay_frames = profile.playout_delay_frames;
    config.playback_max_frames = profile.playback_max_frames;
    config.playback_queue_capacity_frames = profile.playback_ring_frames;
    config.jitter_buffer_frames = profile.jitter_buffer_frames;
    config.jitter_buffer_max_frames = profile.jitter_buffer_max_frames;
    config.adaptive_playback_cushion = profile.adaptive_playback_cushion;
    config.adaptive_playback_target_frames = profile.adaptive_playback_target_frames;
    config.adaptive_playback_min_frames = profile.adaptive_playback_min_frames;
    config.adaptive_playback_max_frames = profile.adaptive_playback_max_frames;
    config.adaptive_playback_release_ppm = profile.adaptive_playback_release_ppm;
    config.drift_correction = profile.drift_correction;
    config.drift_smoothing = profile.drift_smoothing;
    config.drift_deadband_ppm = profile.drift_deadband_ppm;
    config.drift_max_correction_ppm = profile.drift_max_correction_ppm;
    config.collect_diagnostics = true;
    return config;
}

jam2::SessionInfo sessionInfo()
{
    jam2::SessionInfo info;
    info.session_id = 123456;
    info.key.fill(0x53);
    return info;
}

struct Receiver {
    DeviceSink sink;
    std::unique_ptr<jam2::NetworkSession> session;
    Receiver(std::size_t local, const jam2::JoinProfile& profile, std::int64_t gapMinimumUs=-1,int frames=int(packetFrames),
        jam2::NetworkAudioFormat format=jam2::NetworkAudioFormat::Pcm24Mono)
        : sink(profile)
    {
        std::vector<jam2::NetworkPeerDescriptor> peers;
        for (std::size_t peer = 0; peer < participants; ++peer) {
            if (peer == local) continue;
            peers.push_back({jam2::PeerId{peer + 1},
                jam2::resolve_udp_endpoint({"127.0.0.1", static_cast<std::uint16_t>(31000 + peer)}),
                jam2::PeerEndpointState::Active});
        }
        jam2::NetworkSessionContract contract;
        contract.audio_format = format;
        contract.sample_rate = rate;
        contract.frames_per_packet = frames;
        jam2::UdpSocket socket;
        socket.bind({"127.0.0.1", 0});
        auto config=streamConfig(profile,frames,format);
        if (gapMinimumUs>=0) config.adaptive_delivery_gap_minimum_us=std::uint64_t(gapMinimumUs);
        session = std::make_unique<jam2::NetworkSession>(std::move(socket), sessionInfo(), contract,
            jam2::SessionBootstrapRole::Creator, jam2::PeerId{local + 1}, peers,
            config, &sink);
    }
    std::uint64_t path() const
    {
        std::uint64_t largest = 0;
        for (std::size_t peer = 0; peer < participants; ++peer) {
            const jam2::PeerId id{peer + 1};
            if (session->peer(id) == nullptr) continue;
            largest = std::max(largest, session->peerMixStats(id)->queue_depth_frames +
                session->peerStream(id).stats().jitter_buffer_depth_frames);
        }
        return largest + sink.depthFrames();
    }
};

jam2::PeerAudioResult deliver(jam2::PeerStream& stream, std::uint32_t sequence,
    std::uint64_t frame, std::uint64_t now, std::span<const std::int32_t> samples,
    jam2::NetworkAudioFormat format=jam2::NetworkAudioFormat::Pcm24Mono)
{
    std::array<std::uint8_t, 256 * 3> storage{};
    if (samples.size()>256) throw std::runtime_error("experiment packet exceeds fixture capacity");
    auto payload=std::span(storage).first(samples.size()*jam2::protocol::audio_bytes_per_sample(format));
    if (!jam2::protocol::pack_audio_q31_into(format, samples, payload))
        throw std::runtime_error("failed to pack experiment audio");
    const auto info = sessionInfo();
    jam2::protocol::Header header{jam2::protocol::PacketType::Audio, info.session_id,
        sequence, frame, static_cast<std::uint16_t>(payload.size()), 0};
    std::array<std::uint8_t, jam2::protocol::kMaxDatagramSize> wire{};
    const auto size = jam2::protocol::encode_packet_into(header, payload, info.key,
        format, wire);
    const auto parsed = jam2::protocol::parse_packet(std::span(wire).first(size), info.key,
        info.session_id, format);
    if (!parsed) throw std::runtime_error("experiment packet failed authentication/validation");
    return stream.receiveAudio(parsed.header,
        std::span<const std::uint8_t>(wire).subspan(jam2::protocol::kHeaderSize, payload.size()), now);
}

class FrameMarker final : public jam2::audio::InputSourceRenderer {
public:
    bool render_mono(const jam2::audio::InputSourceRenderRequest& request,
        std::span<std::int32_t> output) noexcept override
    {
        for (std::size_t i = 0; i < output.size(); ++i)
            output[i] = static_cast<std::int32_t>((request.engine_frame + i + 1) * 256);
        return true;
    }
};

void captureOverflow()
{
    // First pin the exact discontinuity deterministically at the callback/ring
    // boundary, then check the public Engine capture API with real fake-device
    // callbacks. No producer/consumer index is modified by the fixture.
    jam2::audio::MonoRingBuffer ring(256, true);
    jam2::audio::StreamControl control;
    control.network_capture_requested_enabled.store(true);
    control.network_capture_generation_requested.store(1);
    jam2::NetworkPacketSchedule schedule(rate, static_cast<int>(packetFrames), 0);
    auto config = streamConfig(*jam2::find_join_profile("fast"));
    config.jitter_buffer_frames = config.jitter_buffer_max_frames = 0;
    config.adaptive_playback_cushion = false;
    config.drift_correction = false;
    DeviceSink sink(*jam2::find_join_profile("fast"));
    jam2::PeerStream stream(config, 0, &sink);
    std::array<std::int32_t, packetFrames> block{};
    auto publish = [&](std::uint64_t frame) {
        expect(jam2::audio::prepare_network_capture_callback(control, ring, frame), "capture is attached");
        for (std::size_t i = 0; i < block.size(); ++i) block[i] = static_cast<std::int32_t>((frame + i + 1) * 256);
        return jam2::audio::push_network_capture_callback(control, ring, block, frame * 1000000 / rate);
    };
    for (std::uint64_t frame = 0; frame < 512; frame += packetFrames) (void)publish(frame);
    expect(ring.stats().overruns == 256 && ring.available_read() == 256,
        "a stopped capture consumer retains 256 old frames and drops 256 new frames");
    for (int i = 0; i < 4; ++i) {
        std::uint64_t first = 0;
        expect(ring.pop_exact_timed(block, first) == packetFrames, "retained capture packet is complete");
        expect(schedule.setAudioSampleTime(first), "retained capture timestamp is monotonic");
        expect(static_cast<std::uint64_t>(block.front() / 256 - 1) == schedule.sampleTime(),
            "before the overflow gap source identity matches the transmitted timeline");
        expect(deliver(stream, schedule.audioSequence(), schedule.sampleTime(),
            10000 + static_cast<std::uint64_t>(i) * 1333, block) == jam2::PeerAudioResult::Accepted,
            "retained audio is accepted");
        schedule.commitAudioPacket();
    }
    std::uint64_t first = 0;
    expect(publish(512) == packetFrames && ring.pop_exact_timed(block, first) == packetFrames,
        "live capture resumes after storage becomes available");
    expect(schedule.setAudioSampleTime(first) && schedule.audioSequence() == 5,
        "capture loss advances audio time without inventing unsent packet sequences");
    const auto markerFrame = static_cast<std::uint64_t>(block.front() / 256 - 1);
    expect(deliver(stream, schedule.audioSequence(), schedule.sampleTime(), 16000, block) ==
        jam2::PeerAudioResult::Accepted, "post-overflow packet passes authentication and sample acceptance");
    expect(markerFrame == schedule.sampleTime() && stream.stats().missing_audio_frames_inserted == 256,
        "receiver observes the real capture gap without relabelling later audio");
    std::cout << "METRIC capture deterministic_lost_frames=" << ring.stats().overruns
        << " actual_source_frame=" << markerFrame << " wire_frame=" << schedule.sampleTime()
        << " receiver_missing_frames=" << stream.stats().missing_audio_frames_inserted << '\n';

    FrameMarker renderer;
    jam2::audio::InputSourceRouter router(packetFrames, 0);
    expect(router.configure(0, {jam2::audio::InputSourceKind::MidiInstrument,
        jam2::audio::kNoInputChannel, jam2::audio::kNoInputChannel, 1000000, true, &renderer}),
        "engine marker renderer configures");
    jam2::Engine engine;
    jam2::EngineConfig engineConfig;
    engineConfig.backend = jam2::EngineAudioBackend::Headless;
    engineConfig.audio_buffer_frames = static_cast<long>(packetFrames);
    engineConfig.capture_ring_frames = 256;
    engineConfig.input_source_router = &router;
    engine.start(engineConfig);
    const auto attachment = engine.attachNetworkCapture();
    const auto deadline = jam2::monotonic_us() + 5000000;
    while (engine.snapshot().capture_ring.overruns < 256 && jam2::monotonic_us() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto lost = engine.snapshot().capture_ring.overruns;
    bool firstMatches = false, mismatch = false, gap = false;
    std::uint64_t actual = 0, labelled = 0;
    std::uint64_t previousEnd = 0;
    while (!mismatch && !gap && jam2::monotonic_us() < deadline) {
        const auto captured = engine.popNetworkCapture(attachment, block);
        if (captured.frames == 0) { std::this_thread::yield(); continue; }
        actual = static_cast<std::uint64_t>(block.front() / 256 - 1);
        labelled = captured.first_frame;
        if (!firstMatches) firstMatches = actual == labelled;
        mismatch = actual != labelled;
        gap = previousEnd != 0 && labelled > previousEnd;
        previousEnd = labelled + captured.frames;
    }
    engine.detachNetworkCapture(attachment);
    engine.requestStop();
    engine.join();
    expect(lost >= 256 && firstMatches && gap && !mismatch,
        "Engine capture preserves both queued timestamps and the overflow discontinuity");
    std::cout << "METRIC capture engine_observed_overrun_frames=" << lost
        << " actual_source_frame=" << actual << " engine_label_frame=" << labelled
        << " timestamp_error_frames=" << (actual > labelled ? actual - labelled : 0) << '\n';
}

struct Observations {
    std::uint64_t underruns = 0, padding = 0, trims = 0, missing = 0, raises = 0;
    std::uint64_t target = 0, path = 0, recovery = 0, completions = 0;
    std::uint64_t recoveryPending = 0, recoveryRetired = 0, recoveryCancelled = 0;
};
Observations observe(const Receiver& receiver)
{
    const auto& stats = receiver.session->mixStats();
    return {receiver.sink.underrunFrames(), stats.adaptive_padding_frames,
        stats.live_tail_trimmed_frames, stats.missing_peer_frames, stats.adaptive_raise_events,
        stats.adaptive_target_frames, receiver.path(), stats.receive_recovery_events,
        stats.receive_recovery_completions, stats.recovery_pending_frames,
        stats.recovery_retired_frames, stats.recovery_cancelled_frames};
}

void interruptionCase(std::uint64_t outageUs, bool deactivate, const char* profileName)
{
    const auto& profile = *jam2::find_join_profile(profileName);
    std::array<std::unique_ptr<Receiver>, participants> receivers;
    for (std::size_t i = 0; i < participants; ++i) receivers[i] = std::make_unique<Receiver>(i, profile);
    const auto base = jam2::monotonic_us() + 1000;
    constexpr std::uint64_t outageBegin = 5000000;
    const auto outageEnd = outageBegin + outageUs;
    const auto end = outageEnd + 15000000;
    std::array<std::array<std::int32_t, packetFrames>, participants> audio{};
    for (std::size_t i = 0; i < participants; ++i) audio[i].fill(static_cast<std::int32_t>((i + 1) * 65536));
    std::array<Observations, participants> before{}, atInactive{}, atReturn{}, final{};
    std::array<std::uint64_t, participants> acceptedReturn{}, rejectedReturn{}, futureReturn{}, healthyAccepted{};
    std::array<std::uint64_t, participants> maxTarget{};
    std::array<std::uint64_t, participants> finalMatchingFrames{}, finalUnexpectedFrames{};
    std::array<std::uint64_t, participants> outageMatchingFrames{}, outageUnexpectedFrames{};
    bool began = false, inactivated = false, returned = false;
    for (std::uint64_t tick = 0;; ++tick) {
        const auto elapsed = tick * callbackFrames * 1000000 / rate;
        if (elapsed > end) break;
        const auto now = base + elapsed;
        if (!began && elapsed >= outageBegin) {
            began = true;
            for (std::size_t i = 0; i < participants; ++i) before[i] = observe(*receivers[i]);
        }
        // Inject the production runtime's resulting endpoint-state transition.
        // This tests NetworkSession/PeerStream/PeerMixer composition, not the
        // CLI's timer or Hello/HelloAck proof implementation.
        if (deactivate && !inactivated && elapsed >= outageBegin + 3000000 && elapsed < outageEnd) {
            for (std::size_t i = 0; i < participants; ++i) {
                for (std::size_t peer = 0; peer < participants; ++peer)
                    if (i != peer && (i == 3 || peer == 3))
                        expect(receivers[i]->session->setPeerEndpointState({peer + 1}, jam2::PeerEndpointState::Candidate),
                            "outage deactivates only disconnected mesh edges");
                atInactive[i] = observe(*receivers[i]);
            }
            inactivated = true;
        }
        if (!returned && elapsed >= outageEnd) {
            returned = true;
            for (std::size_t i = 0; i < participants; ++i) {
                atReturn[i] = observe(*receivers[i]);
                if (inactivated) for (std::size_t peer = 0; peer < participants; ++peer)
                    if (i != peer && (i == 3 || peer == 3))
                        expect(receivers[i]->session->setPeerEndpointState({peer + 1}, jam2::PeerEndpointState::Active),
                            "successful reproving reactivates existing stream state");
            }
        }
        for (auto& receiver : receivers) receiver->session->advance(now);
        if (tick % 2 == 0) {
            const auto sequence = static_cast<std::uint32_t>(tick / 2 + 1);
            const auto frame = tick * callbackFrames;
            for (std::size_t i = 0; i < participants; ++i) for (std::size_t peer = 0; peer < participants; ++peer) {
                if (i == peer) continue;
                const bool brokenEdge = i == 3 || peer == 3;
                if (brokenEdge && elapsed >= outageBegin && elapsed < outageEnd) continue;
                auto& receiver = *receivers[i];
                const auto result = deliver(receiver.session->peerStream({peer + 1}), sequence, frame, now, audio[peer]);
                if (elapsed >= outageEnd && brokenEdge) {
                    acceptedReturn[i] += result == jam2::PeerAudioResult::Accepted ? 1U : 0U;
                    rejectedReturn[i] += result != jam2::PeerAudioResult::Accepted ? 1U : 0U;
                    futureReturn[i] += result == jam2::PeerAudioResult::FutureSampleTime ? 1U : 0U;
                } else if (!brokenEdge && elapsed >= outageBegin) {
                    healthyAccepted[i] += result == jam2::PeerAudioResult::Accepted ? 1U : 0U;
                    expect(result == jam2::PeerAudioResult::Accepted, "healthy edges keep accepting audio during another peer's outage");
                }
            }
        }
        for (std::size_t i = 0; i < participants; ++i) {
            auto& receiver = *receivers[i];
            receiver.session->advance(now);
            receiver.session->finishReceiveBatch(false);
            if (elapsed >= outageBegin) maxTarget[i] = std::max(maxTarget[i], receiver.session->mixStats().adaptive_target_frames);
            receiver.sink.callback();
            if (outageUs >= 9000000 && elapsed >= outageEnd - 2000000 && elapsed < outageEnd) {
                std::int32_t survivingOutput = 0;
                for (std::size_t peer = 0; peer < participants; ++peer)
                    if (i != 3 && peer != 3 && peer != i) survivingOutput += audio[peer].front();
                for (const auto sample : receiver.sink.output) {
                    outageMatchingFrames[i] += sample == survivingOutput ? 1U : 0U;
                    outageUnexpectedFrames[i] += sample != survivingOutput ? 1U : 0U;
                }
            }
            if (elapsed >= end - 5000000) {
                std::int32_t expectedOutput = 0;
                for (std::size_t peer = 0; peer < participants; ++peer)
                    if (peer != i)
                        expectedOutput += audio[peer].front();
                for (const auto sample : receiver.sink.output) {
                    finalMatchingFrames[i] += sample == expectedOutput ? 1U : 0U;
                    finalUnexpectedFrames[i] += sample != expectedOutput ? 1U : 0U;
                }
            }
        }
    }
    for (std::size_t i = 0; i < participants; ++i) {
        final[i] = observe(*receivers[i]);
        expect(maxTarget[i] <= profile.adaptive_playback_max_frames, "outage adaptation remains bounded");
        expect(acceptedReturn[i] > 100 && futureReturn[i] == 0,
            "interrupted audio resumes on every edge, including gaps exceeding ten seconds");
        expect(finalMatchingFrames[i] > 200000 && finalUnexpectedFrames[i] == 0,
            "settled device output contains every resumed source and every healthy source");
        if (outageUs >= 9000000)
            expect(outageMatchingFrames[i] >= 95000 && outageUnexpectedFrames[i] == 0,
                "healthy sources continue reaching the device while the interrupted peer is suspended");
        if (inactivated) {
            expect(atReturn[i].missing == atInactive[i].missing && atReturn[i].target == profile.adaptive_playback_min_frames,
                "deactivation stops missing-contribution debt and lets shared adaptation recover before return");
            expect(final[i].recovery == final[i].completions,
                "deactivated missing source does not leave shared recovery stuck");
        } else if (outageUs > 3000000) {
            expect(atReturn[i].target == profile.adaptive_playback_min_frames &&
                final[i].target == profile.adaptive_playback_min_frames && final[i].recovery == final[i].completions,
                "audio-only inactivity lets shared adaptation recover while the endpoint stays active");
        }
        if (outageUs > 3000000) {
            const auto& resumed = receivers[i]->session->peerStream({i == 3 ? 1ULL : 4ULL}).stats();
            expect(resumed.audio_suspensions == 1 && !resumed.audio_suspended &&
                resumed.audio_timeline_rebases >= 1 && resumed.missing_audio_frames_retired > rate,
                "interrupted source suspends once and retires the missing timeline on resumption");
        }
        std::cout << "METRIC interruption profile=" << profileName << " seconds=" << outageUs / 1000000
            << " deactivate=" << deactivate << " receiver=" << i + 1
            << " healthy_accepted=" << healthyAccepted[i]
            << " return_accepted=" << acceptedReturn[i] << " return_rejected=" << rejectedReturn[i]
            << " return_future=" << futureReturn[i]
            << " baseline_target=" << before[i].target << " peak_target=" << maxTarget[i]
            << " target_at_return=" << atReturn[i].target << " final_target=" << final[i].target
            << " outage_missing=" << atReturn[i].missing - before[i].missing
            << " missing_after_deactivation=" << (inactivated ? atReturn[i].missing - atInactive[i].missing : 0)
            << " outage_underruns=" << atReturn[i].underruns - before[i].underruns
            << " after_return_underruns=" << final[i].underruns - atReturn[i].underruns
            << " outage_trimmed=" << atReturn[i].trims - before[i].trims
            << " final_matching_output_frames=" << finalMatchingFrames[i]
            << " final_unexpected_output_frames=" << finalUnexpectedFrames[i]
            << " final_path=" << final[i].path << " recovery_events=" << final[i].recovery
            << " recovery_completions=" << final[i].completions << '\n';
    }
}

struct ClockResult {
    std::uint64_t lateUnderruns = 0, lateTrims = 0, latePadding = 0;
    double drift = 0, sourceRatio = 1, averagePath = 0;
    std::uint64_t releaseFrames = 0;
    std::uint64_t recoveries = 0, completions = 0;
};

enum class StartupArrival { Clean, BeforeRecoveryDecision, AfterRecoveryDecision, AfterReceiveBatch };

const char* startupArrivalName(StartupArrival arrival)
{
    switch (arrival) {
    case StartupArrival::Clean: return "clean";
    case StartupArrival::BeforeRecoveryDecision: return "audio-before-decision";
    case StartupArrival::AfterRecoveryDecision: return "audio-after-decision";
    case StartupArrival::AfterReceiveBatch: return "audio-after-batch";
    }
    throw std::runtime_error("invalid startup arrival ordering");
}

struct StartupRecoveryResult {
    Observations final, settled;
    double averagePath = 0;
    std::uint64_t pathMinimum = 0, pathMaximum = 0, maximumOneSecondMinimum = 0;
    std::uint64_t firstUnderrunUs = 0, targetReturnedUs = 0, targetMaximum = 0;
    std::uint64_t acceleratedCallbacks = 0, nonUnityRequests = 0, workerRunwayMaximum = 0;
};

StartupRecoveryResult startupRecoveryCase(StartupArrival arrival, std::uint64_t workerPeriodUs,
    std::uint64_t callbackPhaseUs, jam2::NetworkAudioFormat format)
{
    // Exactly four participants. Only the arrival of packet 2 and its ordering
    // against advance() are injected. Callback reads, authentication, streams,
    // recovery padding, mixing and playback rate changes are production code.
    const auto profile = *jam2::find_join_profile("fast");
    Receiver receiver(0, profile, -1, int(packetFrames), format);
    const auto base = receiver.session->schedule().startTimeUs();
    constexpr std::uint64_t endUs = 60000000, settledBeginUs = 10000000;
    const auto timeAt = [](std::uint64_t frame) { return frame * 1000000 / rate; };
    std::array<std::int32_t, packetFrames> audio{};
    audio.fill(1048576);
    StartupRecoveryResult result;
    result.pathMinimum = (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t packet = 1, callback = 1, worker = workerPeriodUs, now = 0;
    std::uint64_t bucket = 0, bucketMinimum = result.pathMinimum, observedUs = 0;
    long double pathIntegral = 0;
    bool settled = false, targetWasRaised = false;
    const auto samplePath = [&] {
        if (now < settledBeginUs) return;
        const auto path = receiver.path();
        const auto nextBucket = now / 1000000;
        if (nextBucket != bucket) {
            if (bucket != 0) result.maximumOneSecondMinimum = std::max(result.maximumOneSecondMinimum, bucketMinimum);
            bucket = nextBucket;
            bucketMinimum = (std::numeric_limits<std::uint64_t>::max)();
        }
        bucketMinimum = std::min(bucketMinimum, path);
        result.pathMinimum = std::min(result.pathMinimum, path);
        result.pathMaximum = std::max(result.pathMaximum, path);
    };
    while (now < endUs) {
        const auto callbackUs = timeAt(callback * callbackFrames) + callbackPhaseUs;
        const auto next = std::min({callbackUs, worker, endUs});
        if (next > settledBeginUs) {
            const auto elapsed = next - std::max(now, settledBeginUs);
            pathIntegral += static_cast<long double>(receiver.path()) * elapsed;
            observedUs += elapsed;
        }
        now = next;
        if (!settled && now >= settledBeginUs) {
            result.settled = observe(receiver);
            settled = true;
        }
        if (now == callbackUs) {
            receiver.sink.publishClock(callback * callbackFrames, base + now);
            receiver.sink.callback();
            if (!result.firstUnderrunUs && receiver.sink.underrunFrames()) result.firstUnderrunUs = now;
            if (now >= settledBeginUs && receiver.sink.control.playback_ratio_applied_ppm.load() > 1000000)
                ++result.acceleratedCallbacks;
            ++callback;
            samplePath();
        }
        if (now == worker) {
            const auto deliverReady = [&] {
                while (timeAt(packet * packetFrames) <= now) {
                    // Hold one packet until a real 32-frame callback has read
                    // an empty ring. Later packet timestamps remain untouched.
                    if (packet == 2 && arrival != StartupArrival::Clean && !result.firstUnderrunUs) break;
                    for (std::size_t peer = 1; peer < participants; ++peer)
                        expect(deliver(receiver.session->peerStream({peer + 1}), std::uint32_t(packet),
                            (packet - 1) * packetFrames, base + now, audio, format) == jam2::PeerAudioResult::Accepted,
                            "startup recovery accepts every authenticated source packet");
                    ++packet;
                }
            };
            if (arrival == StartupArrival::BeforeRecoveryDecision) deliverReady();
            receiver.session->advance(base + now);
            samplePath();
            if (arrival != StartupArrival::BeforeRecoveryDecision && arrival != StartupArrival::AfterReceiveBatch) deliverReady();
            receiver.session->advance(base + now);
            receiver.session->finishReceiveBatch(false);
            if (arrival == StartupArrival::AfterReceiveBatch) {
                deliverReady();
                receiver.session->advance(base + now);
            }
            samplePath();
            const auto& mix = receiver.session->mixStats();
            if (mix.adaptive_target_frames > profile.adaptive_playback_min_frames) targetWasRaised = true;
            if (targetWasRaised && !result.targetReturnedUs && mix.adaptive_target_frames == profile.adaptive_playback_min_frames) {
                result.targetReturnedUs = now;
                std::cout << "METRIC startup-recovery stage=target-return ordering=" << startupArrivalName(arrival)
                    << " format=" << (format == jam2::NetworkAudioFormat::Pcm16Mono ? "pcm16" : "pcm24")
                    << " worker_us=" << workerPeriodUs << " callback_phase_us=" << callbackPhaseUs
                    << " elapsed_us=" << now << " target=" << mix.adaptive_target_frames
                    << " path=" << receiver.path() << " padding=" << mix.adaptive_padding_frames << '\n';
            }
            result.targetMaximum = std::max(result.targetMaximum, mix.adaptive_target_frames);
            result.workerRunwayMaximum = std::max(result.workerRunwayMaximum, mix.worker_runway_frames);
            if (now >= settledBeginUs && receiver.sink.control.playback_ratio_ppm.load() > 1000000)
                ++result.nonUnityRequests;
            worker += workerPeriodUs;
        }
    }
    result.final = observe(receiver);
    result.averagePath = double(pathIntegral / observedUs);
    expect(receiver.session->peerCount() == 3 && receiver.session->mixStats().contributing_peers == 3,
        "startup recovery keeps exactly four participants contributing");
    expect(result.final.target == profile.adaptive_playback_min_frames,
        "startup target returns to the configured minimum");
    expect(result.final.underruns == result.settled.underruns && result.final.padding == result.settled.padding &&
        result.final.trims == 0 && result.final.recovery == 0,
        "settled startup recovery adds no underruns, padding, trimming or hard recovery");
    const auto& mix = receiver.session->mixStats();
    expect(mix.output_drop_requested_frames == 0 && mix.output_dropped_frames == 0 && mix.capacity_dropped_frames == 0,
        "startup latency cannot be reduced by dropping queued audio");
    expect(mix.missing_peer_frames == 0 && mix.late_after_release_frames == 0,
        "startup latency cannot be reduced by advancing a missing-source timeline");
    expect(mix.adaptive_delivery_gap_required_max_frames == 0 && !mix.delivery_gap_guard_active,
        "startup recovery case isolates padding without learned delivery-gap protection");
    for (std::size_t peer = 1; peer < participants; ++peer) {
        expect(receiver.session->peerStream({peer + 1}).stats().sequence.lost == 0,
            "startup injection holds delivery without packet loss");
        expect(receiver.session->peerMixStats({peer + 1})->requested_drop_frames == 0,
            "startup latency cannot be reduced by discarding a peer's queued audio");
    }
    std::cout << "METRIC startup-recovery stage=settled ordering=" << startupArrivalName(arrival)
        << " format=" << (format == jam2::NetworkAudioFormat::Pcm16Mono ? "pcm16" : "pcm24")
        << " worker_us=" << workerPeriodUs << " callback_phase_us=" << callbackPhaseUs
        << " sample_rate=" << rate << " packet_frames=" << packetFrames << " callback_frames=" << callbackFrames
        << " first_underrun_us=" << result.firstUnderrunUs << " total_underruns=" << result.final.underruns
        << " padding=" << result.final.padding << " target_max=" << result.targetMaximum
        << " target=" << result.final.target << " target_returned_us=" << result.targetReturnedUs
        << " target_release_events=" << mix.adaptive_release_events
        << " settled_begin_us=" << settledBeginUs
        << " late_underruns=" << result.final.underruns - result.settled.underruns
        << " late_padding=" << result.final.padding - result.settled.padding << " trims=" << result.final.trims
        << " average_path=" << result.averagePath << " minimum_path=" << result.pathMinimum
        << " maximum_path=" << result.pathMaximum << " greatest_one_second_minimum=" << result.maximumOneSecondMinimum
        << " batch_tolerance=" << mix.live_tail_batch_tolerance_frames
        << " configured_release_ppm=" << profile.adaptive_playback_release_ppm
        << " worker_runway_max=" << result.workerRunwayMaximum << " acceleration_requests=" << result.nonUnityRequests
        << " accelerated_callbacks=" << result.acceleratedCallbacks
        << " recovery_pending=" << mix.recovery_pending_frames << " recovery_retired=" << mix.recovery_retired_frames << '\n';
    return result;
}

void startupRecoveryExperiments()
{
    rate = 44100;
    for (auto format : {jam2::NetworkAudioFormat::Pcm16Mono, jam2::NetworkAudioFormat::Pcm24Mono}) {
        for (const auto [worker, phase] : {std::pair{250ULL, 400ULL}, std::pair{250ULL, 650ULL}, std::pair{500ULL, 650ULL}}) {
            const auto clean = startupRecoveryCase(StartupArrival::Clean, worker, phase, format);
            const auto ready = startupRecoveryCase(StartupArrival::BeforeRecoveryDecision, worker, phase, format);
            const auto padded = startupRecoveryCase(StartupArrival::AfterRecoveryDecision, worker, phase, format);
            const auto deferred = startupRecoveryCase(StartupArrival::AfterReceiveBatch, worker, phase, format);
            expect(clean.final.underruns == 0 && clean.final.padding == 0, "matched startup control remains clean");
            expect(ready.final.underruns == callbackFrames && padded.final.underruns == callbackFrames,
                "arrival-order comparison injects exactly one identical callback underrun");
            expect(ready.final.padding == 0,
                "real queued audio avoids unnecessary recovery padding");
            expect(padded.final.padding == 0,
                "real audio in the same drained batch satisfies isolated callback recovery");
            expect(deferred.final.padding == packetFrames && deferred.final.underruns == callbackFrames,
                "an empty drained batch still inserts the genuinely required bounded recovery cushion");
            std::cout << "METRIC startup-recovery stage=comparison worker_us=" << worker << " callback_phase_us=" << phase
                << " format=" << (format == jam2::NetworkAudioFormat::Pcm16Mono ? "pcm16" : "pcm24")
                << " clean_average_path=" << clean.averagePath << " ready_average_path=" << ready.averagePath
                << " padded_average_path=" << padded.averagePath << " retained_extra_frames=" << padded.averagePath - ready.averagePath
                << " retained_extra_ms=" << (padded.averagePath - ready.averagePath) * 1000 / rate
                << " same_batch_padding_frames=" << padded.final.padding
                << " empty_batch_padding_frames=" << deferred.final.padding
                << " empty_batch_retained_extra_frames=" << deferred.averagePath - ready.averagePath << '\n';
            expect(padded.averagePath <= ready.averagePath + 1.0,
                "startup recovery padding must retire after clean delivery without leaving a packet of extra latency");
            expect(deferred.averagePath <= ready.averagePath + 1.0,
                "genuinely required recovery padding also retires after clean delivery");
        }
    }
}

struct RecoveryCycleResult {
    Observations quietBegin, final;
    long double pathIntegral = 0;
    std::uint64_t observedUs = 0, minimumPath = (std::numeric_limits<std::uint64_t>::max)(), maximumPath = 0;
    std::uint64_t acceleratedCallbacks = 0, nonUnityRequests = 0, workerGuardObservations = 0, gapGuardObservations = 0;
    std::uint64_t workerRunwayMaximum = 0, stoppingMaximum = 0, targetMaximum = 0;
    std::uint64_t minimumOutput = (std::numeric_limits<std::uint64_t>::max)(), maximumOutput = 0;
    std::uint64_t minimumWorkerPath = (std::numeric_limits<std::uint64_t>::max)(), workerOutputAtMinimum = 0;
    std::uint64_t acceptedPackets = 0, latePackets = 0, stalePackets = 0;
};

enum class RecoverySchedule { Repeated, Once };

std::vector<RecoveryCycleResult> recoverySequenceCase(std::uint64_t pauseUs, std::uint64_t workerPeriodUs,
    std::uint64_t callbackPhaseUs, jam2::NetworkAudioFormat format, RecoverySchedule schedule)
{
    // Sixteen seconds is an exact multiple of both 64-frame packets at
    // 44.1 kHz and the worker periods. Every pause therefore has identical
    // packet/callback phase. Delivery resumes in order without changing audio
    // timestamps; callbacks and the production recovery controller keep running.
    constexpr std::uint64_t cycleUs = 16000000, quietOffsetUs = 6000000;
    constexpr std::size_t cycles = 31; // clean reference, then thirty observation windows
    constexpr auto endUs = cycles * cycleUs;
    const auto profile = *jam2::find_join_profile("fast");
    Receiver receiver(0, profile, -1, int(packetFrames), format);
    const auto base = receiver.session->schedule().startTimeUs();
    const auto timeAt = [](std::uint64_t frame) { return frame * 1000000 / rate; };
    std::array<std::int32_t, packetFrames> audio{};
    audio.fill(1048576);
    std::vector<RecoveryCycleResult> results(cycles);
    std::array<bool, cycles> quietStarted{};
    std::array<bool, cycles> heldAudio{};
    std::uint64_t packet = 1, callback = 1, worker = workerPeriodUs, now = 0;
    std::uint64_t maximumTarget = 0;
    while (now < endUs) {
        const auto cycle = std::size_t(now / cycleUs);
        const auto callbackUs = timeAt(callback * callbackFrames) + callbackPhaseUs;
        const auto next = std::min({callbackUs, worker, (cycle + 1) * cycleUs});
        const auto quietBeginUs = cycle * cycleUs + quietOffsetUs;
        auto& result = results[cycle];
        if (next > quietBeginUs) {
            const auto elapsed = next - std::max(now, quietBeginUs);
            const auto path = receiver.path();
            result.pathIntegral += static_cast<long double>(path) * elapsed;
            result.observedUs += elapsed;
            result.minimumPath = std::min(result.minimumPath, path);
            result.maximumPath = std::max(result.maximumPath, path);
            result.minimumOutput = std::min<std::uint64_t>(result.minimumOutput, receiver.sink.depthFrames());
            result.maximumOutput = std::max<std::uint64_t>(result.maximumOutput, receiver.sink.depthFrames());
        }
        now = next;
        if (!quietStarted[cycle] && now >= quietBeginUs) {
            result.quietBegin = observe(receiver);
            quietStarted[cycle] = true;
        }
        if (now == callbackUs) {
            receiver.sink.publishClock(callback * callbackFrames, base + now);
            receiver.sink.callback();
            if (now >= quietBeginUs && receiver.sink.control.playback_ratio_applied_ppm.load() > 1000000)
                ++result.acceleratedCallbacks;
            ++callback;
        }
        if (now == worker) {
            // Observe an empty drained receive batch before admitting any
            // newly arrived audio, matching the existing startup ordering case.
            receiver.session->advance(base + now);
            receiver.session->finishReceiveBatch(false);
            const auto deliveryCycle = now / cycleUs;
            const bool scheduled = deliveryCycle != 0 && deliveryCycle < cycles &&
                (schedule == RecoverySchedule::Repeated || deliveryCycle == 1);
            const bool paused = pauseUs != 0 && scheduled && now % cycleUs < pauseUs;
            if (paused && timeAt(packet * packetFrames) <= now) heldAudio[std::size_t(deliveryCycle)] = true;
            if (!paused) {
                while (timeAt(packet * packetFrames) <= now) {
                    for (std::size_t peer = 1; peer < participants; ++peer) {
                        const auto received = deliver(receiver.session->peerStream({peer + 1}), std::uint32_t(packet),
                            (packet - 1) * packetFrames, base + now, audio, format);
                        result.acceptedPackets += received == jam2::PeerAudioResult::Accepted;
                        result.latePackets += received == jam2::PeerAudioResult::Late;
                        result.stalePackets += received == jam2::PeerAudioResult::StaleSampleTime;
                        const bool obsoleteDuringStress = schedule == RecoverySchedule::Once && pauseUs > 4000 &&
                            now >= cycleUs && now <= cycleUs + pauseUs + 50000 &&
                            (received == jam2::PeerAudioResult::Late || received == jam2::PeerAudioResult::StaleSampleTime);
                        expect(received == jam2::PeerAudioResult::Accepted || obsoleteDuringStress,
                            "recovery accepts valid audio or explicitly rejects obsolete packets during the injected stress");
                    }
                    ++packet;
                }
            }
            receiver.session->advance(base + now);
            receiver.session->finishReceiveBatch(false);
            maximumTarget = std::max(maximumTarget, receiver.session->mixStats().adaptive_target_frames);
            result.targetMaximum = std::max(result.targetMaximum, receiver.session->mixStats().adaptive_target_frames);
            if (now >= quietBeginUs) {
                const auto& mix = receiver.session->mixStats();
                result.nonUnityRequests += receiver.sink.control.playback_ratio_ppm.load() > 1000000;
                result.workerGuardObservations += mix.release_worker_guard_active;
                result.gapGuardObservations += mix.delivery_gap_guard_active;
                result.workerRunwayMaximum = std::max(result.workerRunwayMaximum, mix.worker_runway_frames);
                result.stoppingMaximum = std::max(result.stoppingMaximum, mix.playback_stopping_frames);
                const auto path = receiver.path();
                if (path < result.minimumWorkerPath) {
                    result.minimumWorkerPath = path;
                    result.workerOutputAtMinimum = receiver.sink.depthFrames();
                }
            }
            worker += workerPeriodUs;
        }
        if (now == (cycle + 1) * cycleUs) result.final = observe(receiver);
    }
    const auto average = [](const RecoveryCycleResult& result) {
        return double(result.pathIntegral / result.observedUs);
    };
    const auto firstRecoveryAverage = average(results[1]);
    for (std::size_t cycle = 0; cycle < cycles; ++cycle) {
        const auto& result = results[cycle];
        expect(result.observedUs == cycleUs - quietOffsetUs,
            "each repeated-recovery measurement covers the same complete quiet window");
        expect(result.final.underruns == result.quietBegin.underruns &&
            result.final.padding == result.quietBegin.padding && result.final.trims == result.quietBegin.trims &&
            result.final.missing == result.quietBegin.missing && result.final.recovery == result.quietBegin.recovery,
            "identical isolated pauses leave no continuing corrections during clean delivery");
        if (schedule == RecoverySchedule::Repeated || cycle >= 4 || !pauseUs)
            expect(result.final.target == profile.adaptive_playback_min_frames,
            "isolated-pause target returns to the configured minimum before the next event");
        if (cycle > 0 && pauseUs && (schedule == RecoverySchedule::Repeated || cycle == 1)) expect(heldAudio[cycle],
            "every repeated impairment actually delays due audio packets");
        if (cycle > 1 && schedule == RecoverySchedule::Repeated) {
            expect(average(result) <= firstRecoveryAverage + 1.0,
                "identical isolated pauses cannot ratchet settled buffered latency above the first recovery");
            expect(result.minimumPath <= results[1].minimumPath + 1 && result.maximumPath <= results[1].maximumPath + 1,
                "settled buffer extrema cannot grow while the average conceals extra buffering");
        }
        if (!pauseUs) expect(result.final.underruns == 0 && result.final.padding == 0 &&
            std::abs(average(result) - average(results[0])) <= 1.0,
            "matched clean control has no starvation, padding or latency growth");
        std::cout << "METRIC " << (schedule == RecoverySchedule::Repeated ? "repeated-recovery" : "large-recovery") << " format="
            << (format == jam2::NetworkAudioFormat::Pcm16Mono ? "pcm16" : "pcm24")
            << " worker_us=" << workerPeriodUs << " callback_phase_us=" << callbackPhaseUs
            << " pause_us=" << pauseUs << " cycle=" << cycle
            << " quiet_begin_us=" << cycle * cycleUs + quietOffsetUs << " quiet_duration_us=" << result.observedUs
            << " average_path=" << average(result) << " minimum_path=" << result.minimumPath
            << " maximum_path=" << result.maximumPath
            << " extra_over_clean_frames=" << average(result) - average(results[0])
            << " extra_over_first_recovery_frames=" << average(result) - firstRecoveryAverage
            << " target=" << result.final.target << " target_max=" << maximumTarget << " underruns=" << result.final.underruns
            << " padding=" << result.final.padding << " trimmed=" << result.final.trims
            << " missing=" << result.final.missing << " recoveries=" << result.final.recovery
            << " cycle_target_max=" << result.targetMaximum << " accelerated_callbacks=" << result.acceleratedCallbacks
            << " acceleration_requests=" << result.nonUnityRequests << " worker_guard_observations=" << result.workerGuardObservations
            << " gap_guard_observations=" << result.gapGuardObservations << " worker_runway_max=" << result.workerRunwayMaximum
            << " stopping_max=" << result.stoppingMaximum << " minimum_output=" << result.minimumOutput
            << " maximum_output=" << result.maximumOutput << " accepted_packets=" << result.acceptedPackets
            << " minimum_worker_path=" << result.minimumWorkerPath << " worker_output_at_minimum=" << result.workerOutputAtMinimum
            << " late_packets=" << result.latePackets << " stale_packets=" << result.stalePackets
            << " recovery_pending=" << result.final.recoveryPending << " recovery_retired=" << result.final.recoveryRetired
            << " recovery_cancelled=" << result.final.recoveryCancelled << '\n';
    }
    const auto& mix = receiver.session->mixStats();
    expect(mix.recovery_pending_frames <= profile.adaptive_playback_max_frames,
        "recovery consumption credit remains bounded by the configured adaptive maximum");
    if (!pauseUs) expect(mix.recovery_release_events == 0 && mix.recovery_retired_frames == 0 && mix.recovery_pending_frames == 0,
        "clean delivery creates no recovery debt or additional release commands");
    if (pauseUs) expect(results[1].final.underruns > 0 && results[1].final.padding > 0,
        "the first isolated pause actually exercises callback starvation and recovery padding");
    if (pauseUs >= 4000 && schedule == RecoverySchedule::Repeated) expect(results.back().final.padding > results[1].final.padding,
        "the longer repeated pause exercises further padding rather than only a retained startup cushion");
    expect(receiver.session->peerCount() == 3 && mix.contributing_peers == 3,
        "repeated recovery keeps exactly four participants contributing");
    expect(maximumTarget <= profile.adaptive_playback_max_frames,
        "repeated recovery never exceeds the configured adaptive target maximum");
    expect(mix.output_drop_requested_frames == 0 && mix.output_dropped_frames == 0 && mix.capacity_dropped_frames == 0,
        "recovery never overflows playback or source capacity");
    const bool losslessPause = schedule == RecoverySchedule::Repeated || pauseUs <= 4000;
    if (losslessPause) expect(mix.live_tail_trimmed_frames == 0 && mix.receive_recovery_events == 0 &&
        mix.missing_peer_frames == 0 && mix.late_after_release_frames == 0,
        "isolated-pause latency stays bounded without discarding audio or skipping source time");
    for (std::size_t peer = 1; peer < participants; ++peer) {
        expect(receiver.session->peerStream({peer + 1}).stats().sequence.lost == 0 &&
            (!losslessPause || receiver.session->peerMixStats({peer + 1})->requested_drop_frames == 0),
            "delivery holds cause no packet loss, with no source trimming in the lossless cases");
    }
    return results;
}

void repeatedRecoveryExperiments()
{
    rate = 44100;
    for (auto format : {jam2::NetworkAudioFormat::Pcm16Mono, jam2::NetworkAudioFormat::Pcm24Mono})
        for (const auto [worker, phase] : {std::pair{250ULL, 400ULL}, std::pair{250ULL, 650ULL}, std::pair{500ULL, 650ULL}})
            for (std::uint64_t pause : {0ULL, 1600ULL, 4000ULL})
                recoverySequenceCase(pause, worker, phase, format, RecoverySchedule::Repeated);
}

void largeRecoveryExperiments()
{
    rate = 44100;
    const auto average = [](const RecoveryCycleResult& result) { return double(result.pathIntegral / result.observedUs); };
    for (auto format : {jam2::NetworkAudioFormat::Pcm16Mono, jam2::NetworkAudioFormat::Pcm24Mono}) {
        for (const auto [worker, phase] : {std::pair{250ULL, 400ULL}, std::pair{250ULL, 650ULL}, std::pair{500ULL, 650ULL}}) {
            const auto clean = recoverySequenceCase(0, worker, phase, format, RecoverySchedule::Once);
            const auto smallRecovery = recoverySequenceCase(1600, worker, phase, format, RecoverySchedule::Once);
            for (std::uint64_t pause : {4000ULL, 8000ULL, 12000ULL, 20000ULL, 80000ULL}) {
                const auto large = recoverySequenceCase(pause, worker, phase, format, RecoverySchedule::Once);
                const auto extraOverClean = average(large.back()) - average(clean.back());
                const auto extraOverSmall = average(large.back()) - average(smallRecovery.back());
                std::cout << "METRIC large-recovery-comparison format="
                    << (format == jam2::NetworkAudioFormat::Pcm16Mono ? "pcm16" : "pcm24")
                    << " worker_us=" << worker << " callback_phase_us=" << phase << " pause_us=" << pause
                    << " clean_average_path=" << average(clean.back()) << " small_average_path=" << average(smallRecovery.back())
                    << " large_average_path=" << average(large.back()) << " retained_extra_frames=" << extraOverClean
                    << " retained_extra_ms=" << extraOverClean * 1000 / rate
                    << " extra_over_small_frames=" << extraOverSmall << '\n';
                // The user accepted the small residual provisionally. A larger
                // past disturbance must not establish a higher permanent floor.
                // Also check intermediate late windows so a transient final dip
                // cannot disguise retained latency. All are long past the 30 s hold.
                bool allLateWindowsRecovered = true;
                for (std::size_t cycle = 4; cycle < large.size(); ++cycle)
                    allLateWindowsRecovered &= average(large[cycle]) <= average(smallRecovery[cycle]) + 1.0;
                expect(allLateWindowsRecovered,
                    "a larger past pause must not retain more settled latency than the accepted small recovery");
            }
        }
    }
}

ClockResult clockCase(int senderPpm, int receiverPpm, bool adaptive, bool interruptPeer = false,
    std::uint64_t deliveryBatchUs = 0,int releasePpm = -1,std::int64_t gapMinimumUs=-1,int durationSeconds=180,
    std::uint64_t callbackPhaseUs=137,std::uint64_t workerPeriodUs=250,int frames=int(packetFrames),
    std::uint64_t deliveryJitterUs=0, std::uint64_t workerStallUs=0, bool stampArrival=false,
    const jam2::test::ProfileComparison* comparison=nullptr, std::uint64_t delayStepUs=0)
{
    auto profile = comparison ? comparison->local : *jam2::find_join_profile("fast");
    const auto format = comparison ? jam2::NetworkAudioFormat::Pcm16Mono : jam2::NetworkAudioFormat::Pcm24Mono;
    const auto callbackFrames = comparison ? static_cast<std::size_t>(profile.audio_buffer_size) : ::callbackFrames;
    profile.adaptive_playback_cushion = adaptive;
    if (releasePpm>=0) profile.adaptive_playback_release_ppm=releasePpm;
    // Fixed mode needs a useful runway for an isolated oscillator experiment.
    if (!adaptive) profile.playout_delay_frames = profile.playback_prefill_frames = 256;
    Receiver receiver(0, profile,gapMinimumUs,frames,format); // one receiver + three remote senders = four peers
    receiver.sink.callbackQuantum = callbackFrames;
    const auto base = receiver.session->schedule().startTimeUs();
    const std::uint64_t end = std::uint64_t(durationSeconds)*1000000;
    const std::uint64_t lateBegin = end-60000000;
    std::vector<std::int32_t> audio(std::size_t(frames),1048576);
    const auto timeAt = [](std::uint64_t frames, int ppm) {
        return static_cast<std::uint64_t>(static_cast<long double>(frames) * 1000000.0L /
            (rate * (1.0L + static_cast<long double>(ppm) / 1000000.0L)));
    };
    std::uint64_t packet = 1, callback = 1, worker = workerPeriodUs, now = 0;
    Observations lateStart;
    bool lateStarted = false;
    long double pathIntegral = 0;
    std::uint64_t observedUs = 0, maximumPath = 0, accepted = 0;
    std::uint64_t releaseFrames = 0;
    while (now < end) {
        const auto callbackUs = timeAt(callback * callbackFrames, receiverPpm) + callbackPhaseUs;
        const auto next = std::min({callbackUs, worker, end});
        if (next > lateBegin) {
            const auto duration = next - std::max(now, lateBegin);
            pathIntegral += static_cast<long double>(receiver.path()) * duration;
            observedUs += duration;
        }
        now = next;
        if (!lateStarted && now >= lateBegin) { lateStart = observe(receiver); lateStarted = true; }
        if (now == callbackUs) {
            receiver.sink.publishClock(callback * callbackFrames, base + callbackUs);
            receiver.sink.callback();
            if (now >= lateBegin && receiver.sink.control.playback_ratio_applied_ppm.load() > 1000000)
                releaseFrames += callbackFrames;
            ++callback;
        }
        if (now == worker) {
            // A local runtime service pause is distinct from delaying packets
            // on the network: callbacks keep consuming, but advance/dequeue
            // cannot execute. No product queue or controller is substituted.
            if (workerStallUs && now % 70000 < workerStallUs) {
                worker += workerPeriodUs;
                continue;
            }
            receiver.session->advance(base + now);
            const auto packetDue = [&](std::uint64_t number) {
                const auto random = std::uint32_t(number) * 1664525U + 1013904223U;
                const auto sent=timeAt(number * std::uint64_t(frames),senderPpm) +
                    (deliveryJitterUs ? 7500 + random % (deliveryJitterUs + 1) : 0);
                const auto delayed = sent + (sent >= 30000000 ? delayStepUs : 0);
                return deliveryBatchUs ? (delayed / deliveryBatchUs + 1) * deliveryBatchUs : delayed;
            };
            while (packetDue(packet) <= now) {
                for (std::size_t peer = 1; peer < participants; ++peer) {
                    if (interruptPeer && peer == 3 && now >= 5000000 && now < 25000000) continue;
                    const auto result = deliver(receiver.session->peerStream({peer + 1}),
                        static_cast<std::uint32_t>(packet), (packet - 1) * std::uint64_t(frames),
                        base + (stampArrival ? packetDue(packet) : now), audio, format);
                    expect(result == jam2::PeerAudioResult::Accepted ||
                        (interruptPeer && peer == 3 && now >= 25000000 && now < 25010000 &&
                         result == jam2::PeerAudioResult::ForwardGapRejected),
                        "clock experiment accepts continuous audio and the bounded sequence resync after interruption");
                    accepted += result == jam2::PeerAudioResult::Accepted ? 1U : 0U;
                }
                ++packet;
            }
            receiver.session->advance(base + now);
            receiver.session->finishReceiveBatch(false);
            worker += workerPeriodUs;
        }
        maximumPath = std::max(maximumPath, receiver.path());
    }
    const auto final = observe(receiver);
    const auto& stream = receiver.session->peerStream({2}).stats();
    if (!deliveryBatchUs && !interruptPeer && !deliveryJitterUs && !workerStallUs)
        expect(receiver.session->mixStats().adaptive_delivery_gap_required_max_frames==0 &&
            !receiver.session->mixStats().delivery_gap_guard_active,
            "unimpaired delivery retains the original controller and never selects gap protection");
    expect(stream.drift_valid && accepted > 100000, "clock fixture reaches calibrated sustained playback");
    expect(std::abs(stream.drift_ppm - senderPpm) < 10,
        "peer estimator retains the independently measured sender-to-monotonic rate");
    expect(receiver.session->deviceClockStats().valid &&
        std::abs(receiver.session->deviceClockStats().ppm - receiverPpm) < 10,
        "device estimator measures receiving clock independently of packet arrival");
    const auto expectedRatio = (1.0 + senderPpm / 1000000.0) / (1.0 + receiverPpm / 1000000.0);
    expect(std::abs(stream.resampler_ratio - expectedRatio) < 0.000010,
        "source resampling compensates sender relative to the actual receiving device");
    if (senderPpm == 0 && receiverPpm == -500)
        expect(stream.drift_correction_clamped_samples > 0 && stream.resampler_ratio <= 1.0005,
            "relative clock correction retains the explicit 500 ppm limit");
    if (interruptPeer) {
        const auto& resumed = receiver.session->peerStream({4}).stats();
        expect(resumed.audio_suspensions == 1 && resumed.audio_timeline_rebases == 1 &&
            !resumed.audio_suspended && resumed.drift_valid && receiver.session->peerMixStats({4})->contributing &&
            final.missing == lateStart.missing && std::abs(resumed.resampler_ratio - expectedRatio) < 0.000010,
            "returning source recalibrates against the continuing local device clock while healthy peers keep streaming");
    }
    ClockResult result{final.underruns - lateStart.underruns, final.trims - lateStart.trims,
        final.padding - lateStart.padding, stream.drift_ppm, stream.resampler_ratio,
        observedUs > 0 ? static_cast<double>(pathIntegral / observedUs) : 0, releaseFrames};
    result.recoveries=final.recovery;result.completions=final.completions;
    const auto& mix = receiver.session->mixStats();
    std::cout << "METRIC clock profile=" << profile.name
        << " format=" << (format == jam2::NetworkAudioFormat::Pcm16Mono ? "pcm16" : "pcm24")
        << " callback_frames=" << callbackFrames
        << " sender_ppm=" << senderPpm << " receiver_ppm=" << receiverPpm
        << " adaptive=" << adaptive << " duration_s=" << durationSeconds << " late_window_s=60"
        << " delivery_batch_us=" << deliveryBatchUs
        << " delivery_jitter_us=" << deliveryJitterUs << " delay_step_us=" << delayStepUs
        << " release_ppm=" << profile.adaptive_playback_release_ppm
        << " gap_minimum_us_override=" << gapMinimumUs
        << " callback_phase_us=" << callbackPhaseUs
        << " worker_period_us=" << workerPeriodUs
        << " worker_stall_us=" << workerStallUs << " arrival_timestamp_ablation=" << stampArrival
        << " observed_packet_gap_us=" << stream.audio_packet_gap_max_us
        << " learned_gap_frames=" << mix.adaptive_delivery_gap_required_max_frames
        << " packet_frames=" << frames
        << " sample_rate=" << rate
        << " interrupted_peer=" << interruptPeer
        << " estimated_drift_ppm=" << result.drift << " source_ratio=" << result.sourceRatio
        << " arrival_offset_adjustments=" << stream.drift_arrival_offset_adjustments
        << " arrival_offset_us=" << stream.drift_arrival_offset_us
        << " local_device_drift_ppm=" << receiver.session->deviceClockStats().ppm
        << " desired_relative_ratio=" << (1.0 + senderPpm / 1000000.0) / (1.0 + receiverPpm / 1000000.0)
        << " late_underruns=" << result.lateUnderruns << " late_trimmed=" << result.lateTrims
        << " late_padding=" << result.latePadding << " late_missing=" << final.missing - lateStart.missing
        << " late_raises=" << final.raises - lateStart.raises << " final_target=" << final.target
        << " late_accelerated_output_frames=" << result.releaseFrames
        << " late_average_path=" << result.averagePath << " maximum_path=" << maximumPath
        << " final_path=" << final.path << " recovery_events=" << final.recovery
        << " total_padding=" << mix.adaptive_padding_frames
        << " recovery_completions=" << final.completions << '\n';
    return result;
}

struct GapDeliveryScenario {
    std::string_view name;
    std::uint64_t pauseUs = 0;
    std::size_t affectedPeers = 3;
    std::uint64_t serializationUs = 0;
    std::size_t receiveBudget = 64;
    std::uint64_t repeatUs = 1000000;
    std::size_t maximumPauses = 100;
    bool irregular = false;
    bool dropHeld = false;
    bool localWorkerStall = false;
    bool expireBeforeReceive = true;
    std::uint64_t peerStaggerUs = 0;
    jam2::audio::RecoveryPaddingPolicy paddingPolicy = jam2::audio::RecoveryPaddingPolicy::LearnedCushion;
    std::uint64_t workerPeriodUs = 250;
    std::size_t pausesPerCluster = 0;
    std::uint64_t clusterQuietUs = 0;
};

template<class Stats>
std::uint64_t rebufferEvents(const Stats& stats)
{
    if constexpr (requires { stats.rebuffer_events; }) return stats.rebuffer_events;
    return 0;
}

struct RebufferTriggerStats { std::uint64_t required = 0, windowUs = 0; };
template<class Stats>
RebufferTriggerStats rebufferTrigger(const Stats& stats)
{
    if constexpr (requires { stats.rebuffer_required_episodes; stats.rebuffer_window_us; })
        return {stats.rebuffer_required_episodes, stats.rebuffer_window_us};
    return {};
}

template<class Stats>
std::uint64_t recoveryHoldUs(const Stats& stats)
{
    if constexpr (requires { stats.delivery_gap_hold_us; }) return stats.delivery_gap_hold_us;
    return 0;
}

void writeComparisonWave(const std::filesystem::path& path, std::span<const std::int16_t> samples)
{
    std::ofstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot create comparison WAV");
    const auto u16 = [&](std::uint16_t v) { file.put(static_cast<char>(v)); file.put(static_cast<char>(v >> 8)); };
    const auto u32 = [&](std::uint32_t v) { u16(static_cast<std::uint16_t>(v)); u16(static_cast<std::uint16_t>(v >> 16)); };
    file.write("RIFF",4); u32(36 + static_cast<std::uint32_t>(samples.size()*2)); file.write("WAVEfmt ",8);
    u32(16); u16(1); u16(1); u32(static_cast<std::uint32_t>(rate)); u32(static_cast<std::uint32_t>(rate*2));
    u16(2); u16(16); file.write("data",4); u32(static_cast<std::uint32_t>(samples.size()*2));
    for (const auto sample:samples) u16(static_cast<std::uint16_t>(sample));
    if (!file) throw std::runtime_error("comparison WAV write failed");
}

void gapDeliveryCase(const jam2::test::ProfileComparison& comparison, const GapDeliveryScenario& scenario,
    bool triggerStudy = false, bool holdStudy = false)
{
    constexpr std::uint64_t endUs = 120000000, impairmentEndUs = 40000000;
    struct Pause { std::uint64_t begin, end; };
    std::vector<Pause> pauses;
    constexpr std::array<std::uint64_t, 8> irregularPauses{20000, 40000, 80000, 120000, 35000, 100000, 65000, 145000};
    constexpr std::array<std::uint64_t, 4> irregularSpacing{1370000, 2630000, 730000, 4110000};
    if (scenario.pauseUs) {
        for (std::uint64_t start = 4000000; start < impairmentEndUs && pauses.size() < scenario.maximumPauses;) {
            const auto index = pauses.size();
            const auto duration = scenario.irregular ? irregularPauses[index % irregularPauses.size()] : scenario.pauseUs;
            pauses.push_back({start, start + duration});
            if (scenario.pausesPerCluster && pauses.size() % scenario.pausesPerCluster == 0)
                start += duration + scenario.clusterQuietUs;
            else start += scenario.irregular ? irregularSpacing[index % irregularSpacing.size()] : scenario.repeatUs;
        }
    }
    const std::uint64_t lastPauseEnd = pauses.empty() ? 0 : pauses.back().end +
        (scenario.affectedPeers ? (scenario.affectedPeers - 1) * scenario.peerStaggerUs : 0);
    // All packet generation is offline input injection. The receiver retains
    // production authentication, packet parsing, clocks, mixer and callbacks.
    struct Delivery { std::uint64_t due; std::uint32_t sequence; bool drop; };
    std::array<std::vector<Delivery>, 3> deliveries;
    for (std::size_t peer = 0; peer < deliveries.size(); ++peer) {
        std::uint64_t previousDue = 0;
        for (std::uint32_t number = 1;; ++number) {
            const auto nominal = std::uint64_t(number) * comparison.packet_frames * 1000000 / rate + 300 + peer * 137;
            if (nominal > endUs) break;
            auto due = nominal;
            bool held = false;
            if (!scenario.localWorkerStall && peer < scenario.affectedPeers) {
                const auto stagger = peer * scenario.peerStaggerUs;
                for (const auto& pause : pauses) {
                    if (nominal >= pause.begin + stagger && nominal < pause.end + stagger) {
                        due = pause.end + stagger;
                        held = true;
                        break;
                    }
                }
            }
            due = std::max(due, previousDue + scenario.serializationUs);
            previousDue = due;
            deliveries[peer].push_back({due, number, held && scenario.dropHeld});
        }
    }
    Receiver receiver(0, comparison.local, -1, comparison.packet_frames, jam2::NetworkAudioFormat::Pcm16Mono);
    const auto callbackQuantum = static_cast<std::size_t>(comparison.local.audio_buffer_size);
    receiver.sink.callbackQuantum = callbackQuantum;
    receiver.sink.paddingPolicy = scenario.paddingPolicy;
    const auto base = receiver.session->schedule().startTimeUs();
    std::vector<std::int32_t> audio(static_cast<std::size_t>(comparison.packet_frames));
    std::vector<std::int32_t> tone(static_cast<std::size_t>(rate));
    for (std::size_t i=0;i<tone.size();++i) tone[i]=static_cast<std::int32_t>(
        std::sin(2.0*3.14159265358979323846*220*i/rate)*(67108864.0/3));
    const char* waveDirectory = std::getenv("JAM2_RECOVERY_AUDIO_DIR");
    const bool saveWave = waveDirectory && comparison.local.name == "candidate" &&
        (scenario.name == "clean" || scenario.name == "isolated8-one" ||
         scenario.name == "isolated100-one" || scenario.name == "periodic100-one-atomic" ||
         scenario.name == "measured-shortage100-one" || (holdStudy &&
            (scenario.name == "burst5-quiet6s-one" || scenario.name == "burst5-quiet12s-one" ||
             scenario.name == "irregular-one")));
    std::vector<std::int16_t> recording;
    const std::uint64_t recordingEndUs = holdStudy ? 80000000 : triggerStudy ? 26000000 : 12000000;
    if (saveWave) recording.reserve(static_cast<std::size_t>(rate)*(holdStudy ? 78 : triggerStudy ? 24 : 10));
    std::array<std::size_t, 3> indices{};
    std::array<std::uint64_t, 3> lastArrival{}, lastApplication{};
    std::uint64_t now = 0, worker = scenario.workerPeriodUs, callback = 1, accepted = 0, dropped = 0;
    std::uint64_t maximumArrivalGap = 0, maximumApplicationGap = 0, maximumTarget = 0, maximumPath = 0;
    std::uint64_t firstRequirementUs = 0, firstRaiseUs = 0, firstHoldUs = 0, traceCount = 0, nextTraceUs = 4000000;
    std::uint64_t firstRebufferUs = 0;
    const bool traceHold = holdStudy && comparison.local.name == "candidate" &&
        scenario.name == "periodic100-one-atomic";
    std::uint64_t nextHoldTraceUs = 4000000;
    std::uint64_t previousRecoveries = 0, previousHoldEvents = 0, holdDeadlineUs = 0;
    std::uint64_t recoveriesInTwoSeconds = 0, maximumRecoveriesInTwoSeconds = 0, recoveryBin = 0;
    std::uint64_t firstHoldClearAfterPause = 0, holdExpirations = 0;
    bool previousHoldActive = false;
    std::array<long double, 4> decayIntegral{};
    std::array<std::uint64_t, 4> decayDuration{};
    constexpr std::array<std::uint64_t, 5> decayEdges{0,5000000,10000000,20000000,40000000};
    std::uint64_t traceTarget = 0, traceRequired = 0, traceDebt = 0, traceRaises = 0, traceRecoveries = 0;
    long double cleanIntegral = 0, impairedIntegral = 0, finalIntegral = 0;
    std::uint64_t cleanDuration = 0, impairedDuration = 0, finalDuration = 0;
    Observations settled, impairmentEnd;
    bool settledCaptured = false, impairmentEndCaptured = false;
    auto sample = [&](std::string_view stage) {
        const auto& mix = receiver.session->mixStats();
        if (traceHold && now >= nextHoldTraceUs && now <= 80000000) {
            std::cout << "HOLD_TRACE time_us=" << now << " hold_us=" << recoveryHoldUs(mix)
                      << " active=" << mix.adaptive_delivery_gap_hold_active
                      << " remaining_us=" << mix.adaptive_delivery_gap_hold_remaining_us
                      << " hold_events=" << mix.adaptive_delivery_gap_hold_events
                      << " target=" << mix.adaptive_target_frames << " required=" << mix.adaptive_delivery_gap_required_frames
                      << " path=" << receiver.path() << " recoveries=" << mix.receive_recovery_events
                      << " rebuffers=" << rebufferEvents(mix) << " underruns=" << receiver.sink.underrunFrames() << '\n';
            nextHoldTraceUs += 1000000;
        }
        if (now / 2000000 != recoveryBin) {
            maximumRecoveriesInTwoSeconds = std::max(maximumRecoveriesInTwoSeconds, recoveriesInTwoSeconds);
            recoveriesInTwoSeconds = 0;
            recoveryBin = now / 2000000;
        }
        recoveriesInTwoSeconds += mix.receive_recovery_events - previousRecoveries;
        previousRecoveries = mix.receive_recovery_events;
        if (mix.adaptive_delivery_gap_hold_events != previousHoldEvents)
            holdDeadlineUs = now + mix.adaptive_delivery_gap_hold_remaining_us;
        previousHoldEvents = mix.adaptive_delivery_gap_hold_events;
        if (previousHoldActive && !mix.adaptive_delivery_gap_hold_active) {
            ++holdExpirations;
            if (holdStudy && recoveryHoldUs(mix))
                expect(now + scenario.workerPeriodUs >= holdDeadlineUs,
                    "learned recovery hold cannot expire before its packet-timestamp deadline");
        }
        previousHoldActive = mix.adaptive_delivery_gap_hold_active;
        if (!firstHoldClearAfterPause && firstHoldUs && now >= lastPauseEnd && !mix.adaptive_delivery_gap_hold_active)
            firstHoldClearAfterPause = now;
        maximumTarget = std::max(maximumTarget, mix.adaptive_target_frames);
        if (!firstRequirementUs && mix.adaptive_delivery_gap_required_max_frames) firstRequirementUs = now;
        if (!firstRaiseUs && mix.adaptive_delivery_gap_raise_events) firstRaiseUs = now;
        if (!firstHoldUs && mix.adaptive_delivery_gap_hold_events) firstHoldUs = now;
        if (!firstRebufferUs && rebufferEvents(mix)) firstRebufferUs = now;
        if (comparison.local.name == "candidate" && scenario.name == "periodic100-all-atomic" &&
            now >= 4000000 && now <= 5200000 && traceCount < 100 &&
            (now >= nextTraceUs || mix.adaptive_target_frames < traceTarget || mix.adaptive_target_frames >= traceTarget + 1024 ||
                traceRequired != mix.adaptive_delivery_gap_required_max_frames ||
                traceDebt != mix.receive_recovery_debt_frames || traceRaises != mix.adaptive_delivery_gap_raise_events ||
                traceRecoveries != mix.receive_recovery_events)) {
            ++traceCount;
            std::cout << "GAP_TRACE stage=" << stage << " time_us=" << now
                      << " target=" << mix.adaptive_target_frames << " requirement=" << mix.adaptive_delivery_gap_required_max_frames
                      << " gap_raises=" << mix.adaptive_delivery_gap_raise_events << " debt=" << mix.receive_recovery_debt_frames
                      << " recoveries=" << mix.receive_recovery_events << " underruns=" << receiver.sink.underrunFrames()
                      << " hold_events=" << mix.adaptive_delivery_gap_hold_events
                      << " hold_active=" << mix.adaptive_delivery_gap_hold_active << " path=" << receiver.path() << '\n';
            nextTraceUs = now + 100000;
            traceTarget = mix.adaptive_target_frames; traceRequired = mix.adaptive_delivery_gap_required_max_frames;
            traceDebt = mix.receive_recovery_debt_frames; traceRaises = mix.adaptive_delivery_gap_raise_events;
            traceRecoveries = mix.receive_recovery_events;
        }
    };
    auto nextPeer = [&]() {
        std::size_t chosen = deliveries.size();
        for (std::size_t peer = 0; peer < deliveries.size(); ++peer) {
            if (indices[peer] < deliveries[peer].size() &&
                (chosen == deliveries.size() || deliveries[peer][indices[peer]].due < deliveries[chosen][indices[chosen]].due))
                chosen = peer;
        }
        return chosen;
    };
    while (now < endUs) {
        const auto callbackUs = callback * callbackQuantum * 1000000 / rate + 137;
        const auto next = std::min({callbackUs, worker, endUs});
        auto integrate = [&](std::uint64_t begin, std::uint64_t end, long double& integral, std::uint64_t& duration) {
            const auto left = std::max(now, begin), right = std::min(next, end);
            if (right > left) { integral += receiver.path() * static_cast<long double>(right - left); duration += right - left; }
        };
        integrate(2000000, 3500000, cleanIntegral, cleanDuration);
        integrate(5000000, impairmentEndUs, impairedIntegral, impairedDuration);
        integrate(endUs - 10000000, endUs, finalIntegral, finalDuration);
        if (holdStudy && lastPauseEnd)
            for (std::size_t i=0; i<decayIntegral.size(); ++i)
                integrate(lastPauseEnd+decayEdges[i], lastPauseEnd+decayEdges[i+1], decayIntegral[i], decayDuration[i]);
        now = next;
        if (!impairmentEndCaptured && now >= impairmentEndUs) { impairmentEnd = observe(receiver); impairmentEndCaptured = true; }
        if (!settledCaptured && now >= impairmentEndUs + 2000000) { settled = observe(receiver); settledCaptured = true; }
        if (now == callbackUs) {
            receiver.sink.measureWaveform = now >= 2000000;
            receiver.sink.publishClock(callback * callbackQuantum, base + now);
            receiver.sink.callback();
            if (saveWave && now >= 2000000 && now < recordingEndUs)
                for (const auto sample:std::span(receiver.sink.output).first(callbackQuantum))
                    recording.push_back(static_cast<std::int16_t>(sample >> 16));
            ++callback;
        }
        if (now == worker) {
            const bool blocked = scenario.localWorkerStall && std::any_of(pauses.begin(), pauses.end(),
                [&](const Pause& pause) { return now >= pause.begin && now < pause.end; });
            if (blocked) { worker += scenario.workerPeriodUs; continue; }
            if (scenario.expireBeforeReceive) receiver.session->advance(base + now);
            sample("pre-receive");
            std::size_t drained = 0;
            auto peer = nextPeer();
            while (peer < deliveries.size() && deliveries[peer][indices[peer]].due <= now && drained < scenario.receiveBudget) {
                const auto packet = deliveries[peer][indices[peer]++];
                if (packet.drop) { ++dropped; peer = nextPeer(); continue; }
                const auto sourceFrame = std::uint64_t(packet.sequence-1)*comparison.packet_frames;
                for (std::size_t i=0;i<audio.size();++i) audio[i]=tone[(sourceFrame+i)%tone.size()];
                expect(deliver(receiver.session->peerStream({peer + 2}), packet.sequence,
                        std::uint64_t(packet.sequence - 1) * comparison.packet_frames, base + now, audio,
                        jam2::NetworkAudioFormat::Pcm16Mono) == jam2::PeerAudioResult::Accepted,
                    "gap delivery investigation accepts every delivered authenticated packet");
                ++accepted; ++drained;
                if (lastArrival[peer]) maximumArrivalGap = std::max(maximumArrivalGap, packet.due - lastArrival[peer]);
                if (lastApplication[peer]) maximumApplicationGap = std::max(maximumApplicationGap, now - lastApplication[peer]);
                lastArrival[peer] = packet.due; lastApplication[peer] = now;
                peer = nextPeer();
            }
            sample("after-datagrams");
            const bool exhausted = peer < deliveries.size() && deliveries[peer][indices[peer]].due <= now;
            receiver.session->advance(base + now);
            sample("post-receive");
            receiver.session->finishReceiveBatch(exhausted);
            sample("finish-batch");
            // Saturated turns retry promptly; a periodic turn is an additional
            // controlled scheduling constraint, not packet serialization.
            worker += exhausted ? 1 : scenario.workerPeriodUs;
            maximumPath = std::max(maximumPath, receiver.path());
        }
    }
    std::uint64_t loss = 0;
    for (std::size_t peer = 0; peer < deliveries.size(); ++peer)
        loss += receiver.session->peerStream({peer + 2}).stats().sequence.lost;
    const auto final = observe(receiver);
    const auto& mix = receiver.session->mixStats();
    std::cout << "GAP_DELIVERY profile=" << comparison.local.name << " scenario=" << scenario.name
              << " format=pcm16 participants=4 pause_us=" << scenario.pauseUs << " affected_peers=" << scenario.affectedPeers
              << " serialization_us=" << scenario.serializationUs << " receive_budget=" << scenario.receiveBudget
              << " worker_period_us=" << scenario.workerPeriodUs
              << " pause_count=" << pauses.size() << " repeat_us=" << scenario.repeatUs << " irregular=" << scenario.irregular
              << " drop_held=" << scenario.dropHeld << " worker_stall=" << scenario.localWorkerStall
              << " pre_receive_expiry=" << scenario.expireBeforeReceive << " peer_stagger_us=" << scenario.peerStaggerUs
              << " padding_policy=" << (scenario.paddingPolicy == jam2::audio::RecoveryPaddingPolicy::MeasuredShortage
                    ? "measured-shortage" : "learned-cushion")
              << " clean_ms=" << static_cast<double>(cleanIntegral / cleanDuration) / 48
              << " impaired_ms=" << static_cast<double>(impairedIntegral / impairedDuration) / 48
              << " final_ms=" << static_cast<double>(finalIntegral / finalDuration) / 48
              << " max_path=" << maximumPath << " max_target=" << maximumTarget << " final_target=" << final.target
              << " underruns=" << final.underruns << " late_underruns=" << final.underruns - settled.underruns
              << " padding=" << final.padding << " late_padding=" << final.padding - settled.padding
              << " trimmed=" << final.trims << " late_trimmed=" << final.trims - settled.trims
              << " gap_required=" << mix.adaptive_delivery_gap_required_max_frames
              << " gap_raises=" << mix.adaptive_delivery_gap_raise_events << " recovery_events=" << final.recovery
              << " hold_events=" << mix.adaptive_delivery_gap_hold_events << " first_hold_us=" << firstHoldUs
              << " impairment_end_target=" << impairmentEnd.target << " impairment_end_path=" << impairmentEnd.path
              << " recovery_completions=" << final.completions << " first_requirement_us=" << firstRequirementUs
              << " first_gap_raise_us=" << firstRaiseUs << " arrival_gap_us=" << maximumArrivalGap
              << " application_gap_us=" << maximumApplicationGap << " delivered=" << accepted << " dropped=" << dropped
              << " sequence_loss=" << loss
              << " max_sample_step_q31=" << receiver.sink.maximumSampleStep
              << " sample_step_threshold_q31=" << receiver.sink.sampleStepThreshold
              << " large_sample_steps=" << receiver.sink.largeSampleSteps
              << " max_zero_run_frames=" << receiver.sink.maximumZeroRun
              << " rebuffer_events=" << rebufferEvents(mix)
              << " first_rebuffer_us=" << firstRebufferUs
              << " trigger_required=" << rebufferTrigger(mix).required
              << " trigger_window_us=" << rebufferTrigger(mix).windowUs
              << " recovery_hold_us=" << recoveryHoldUs(mix)
              << " last_pause_end_us=" << lastPauseEnd
              << " hold_expirations=" << holdExpirations
              << " first_hold_clear_after_pause_us=" << firstHoldClearAfterPause
              << " max_recoveries_per_2s=" << std::max(maximumRecoveriesInTwoSeconds,recoveriesInTwoSeconds);
    for (std::size_t i=0; i<decayIntegral.size(); ++i)
        std::cout << " post" << decayEdges[i]/1000000 << "_" << decayEdges[i+1]/1000000 << "_ms="
                  << (decayDuration[i] ? static_cast<double>(decayIntegral[i]/decayDuration[i])/48 : 0);
    std::cout << '\n';
    if (saveWave) {
        std::filesystem::create_directories(waveDirectory);
        writeComparisonWave(std::filesystem::path(waveDirectory)/(std::string(scenario.name)+".wav"),recording);
    }
    expect(accepted > 50000 && maximumTarget <= comparison.local.adaptive_playback_max_frames,
        "gap investigation sustains real production audio within configured target bounds");
    expect(scenario.dropHeld || loss == 0, "lossless delivery variants preserve every source sequence");
    const auto trigger = rebufferTrigger(mix);
    if (holdStudy && recoveryHoldUs(mix) && firstHoldUs && scenario.name == "isolated100-one") {
        const auto tolerance = 2ULL * comparison.packet_frames * 1000000 / rate + scenario.workerPeriodUs;
        expect(firstHoldClearAfterPause >= lastPauseEnd + recoveryHoldUs(mix) &&
                firstHoldClearAfterPause <= lastPauseEnd + recoveryHoldUs(mix) + tolerance,
            "an isolated learned gap expires at the configured hold duration after the final delayed arrival");
    }
    if (trigger.required >= 5 && (scenario.name == "periodic100-every3s-one" ||
            scenario.name == "periodic100-every4s-one" || scenario.name == "cluster4-one"))
        expect(rebufferEvents(mix) == 0, "refill qualification uses a true rolling window, not a chain of nearby events");
    if (trigger.required == 10 && (scenario.name == "cluster5-one" || scenario.name == "cluster9-one"))
        expect(rebufferEvents(mix) == 0, "short clusters below the threshold never request a refill");
    if (trigger.required && firstRebufferUs && scenario.name.starts_with("cluster") && !scenario.irregular)
        expect(firstRebufferUs >= 4000000 + (trigger.required - 1) * scenario.repeatUs + scenario.pauseUs,
            "refill cannot start before the required number of distinct interruptions");
    if (scenario.name == "clean" || scenario.name.starts_with("isolated") || scenario.name.starts_with("sparse4") ||
            scenario.name.starts_with("sparse8") || scenario.name.starts_with("sparse20") || scenario.name == "sparse100-one")
        expect(rebufferEvents(mix) == 0, "clean delivery and isolated or sparse small spikes never request a refill");
}

void gapDeliveryExperiments(bool triggerStudy = false, bool holdStudy = false)
{
    std::vector<GapDeliveryScenario> scenarios{{"clean"}, {"periodic20-all-atomic", 20000},
        {"periodic40-all-atomic", 40000}, {"periodic60-all-atomic", 60000}, {"periodic70-all-atomic", 70000},
        {"periodic100-all-atomic", 100000}, {"periodic100-one-atomic", 100000, 1},
        {"periodic100-one-serial50", 100000, 1, 50}, {"periodic100-all-serial50", 100000, 3, 50},
        {"periodic100-all-serial250", 100000, 3, 250}, {"periodic100-all-budget8", 100000, 3, 0, 8},
        {"periodic100-one-budget8", 100000, 1, 0, 8},
        {"isolated100-one", 100000, 1, 50, 64, 1000000, 1},
        {"sparse100-one", 100000, 1, 50, 64, 8000000},
        {"irregular-one", 100000, 1, 50, 64, 1000000, 100, true},
        {"irregular-all", 100000, 3, 50, 64, 1000000, 100, true},
        {"lost100-one", 100000, 1, 0, 64, 1000000, 100, false, true},
        {"worker-stall100", 100000, 3, 0, 64, 1000000, 100, false, false, true},
        {"post-receive-only-control", 100000, 3, 0, 64, 1000000, 100, false, false, false, false},
        {"periodic100-all-staggered", 100000, 3, 50, 64, 1000000, 100, false, false, false, true, 10000}};
    for (const std::size_t index : {5U, 6U}) {
        auto scenario = scenarios[index];
        scenario.name = index == 5 ? "measured-shortage100-all" : "measured-shortage100-one";
        scenario.paddingPolicy = jam2::audio::RecoveryPaddingPolicy::MeasuredShortage;
        scenarios.push_back(scenario);
        scenario = scenarios[index];
        scenario.name = index == 5 ? "periodic100-all-worker1ms" : "periodic100-one-worker1ms";
        scenario.workerPeriodUs = 1000;
        scenarios.push_back(scenario);
    }
    scenarios.push_back({"periodic4-all", 4000});
    scenarios.push_back({"periodic8-one", 8000, 1});
    for (const auto duration : {1600U, 4000U, 8000U, 12000U, 20000U}) {
        static constexpr std::array<std::string_view,5> labels{"isolated1_6-one","isolated4-one","isolated8-one","isolated12-one","isolated20-one"};
        const std::size_t index = duration == 1600 ? 0 : duration == 4000 ? 1 : duration == 8000 ? 2 : duration == 12000 ? 3 : 4;
        scenarios.push_back({labels[index],duration,1,50,64,1000000,1});
    }
    scenarios.push_back({"sparse4-one",4000,1,50,64,16000000});
    scenarios.push_back({"sparse8-one",8000,1,50,64,16000000});
    scenarios.push_back({"sparse20-one",20000,1,50,64,16000000});
    if (triggerStudy) {
        scenarios.push_back({"periodic100-every2s-one",100000,1,50,64,2000000});
        scenarios.push_back({"periodic100-every3s-one",100000,1,50,64,3000000});
        scenarios.push_back({"periodic100-every4s-one",100000,1,50,64,4000000});
        scenarios.push_back({"cluster4-one",100000,1,50,64,1000000,4});
        scenarios.push_back({"cluster5-one",100000,1,50,64,1000000,5});
        scenarios.push_back({"cluster9-one",100000,1,50,64,1000000,9});
        scenarios.push_back({"cluster10-one",100000,1,50,64,1000000,10});
    }
    if (holdStudy) {
        for (const auto entry : {std::pair{"burst5-quiet6s-one",6000000ULL},
                std::pair{"burst5-quiet12s-one",12000000ULL}, std::pair{"burst5-quiet25s-one",25000000ULL},
                std::pair{"burst10-quiet6s-one",6000000ULL}, std::pair{"burst10-quiet12s-one",12000000ULL}}) {
            GapDeliveryScenario scenario{entry.first,100000,1,50};
            scenario.pausesPerCluster = std::string_view(entry.first).starts_with("burst10") ? 10 : 5;
            scenario.clusterQuietUs = entry.second;
            scenarios.push_back(scenario);
        }
        scenarios.push_back({"periodic100-every6s-one",100000,1,50,64,6000000});
        scenarios.push_back({"periodic100-every12s-one",100000,1,50,64,12000000});
    }
    for (const auto name : {"fast", "candidate", "moderate", "safe", "fast128-control"}) {
        const auto comparison = jam2::test::comparisonProfile(name);
        for (const auto& scenario : scenarios) gapDeliveryCase(comparison, scenario, triggerStudy, holdStudy);
    }
}

void clockAliasExperiments()
{
    // Fixed 8 ms batches are exactly three 128-frame packets at 48 kHz.
    // Vary the period, timestamp scatter and sample rate without altering the
    // estimator or its existing accuracy assertions.
    for (const auto name : {"candidate", "moderate", "safe"}) {
        const auto comparison = jam2::test::comparisonProfile(name);
        for (const std::uint64_t batch : {7900U, 8000U, 8100U})
            (void)clockCase(17, 0, true, false, batch, -1, -1, 180, 137, 250,
                comparison.packet_frames, 0, 0, false, &comparison);
    }
    const auto comparison = jam2::test::comparisonProfile("candidate");
    for (const std::uint64_t scatter : {300U, 3600U})
        (void)clockCase(17, 0, true, false, 8000, -1, -1, 180, 137, 250,
            comparison.packet_frames, scatter, 0, false, &comparison);
    for (const auto step : {3000U,12000U,40000U})
        (void)clockCase(17,0,true,false,0,-1,-1,180,137,250,comparison.packet_frames,0,0,false,&comparison,step);
    rate = 44100;
    (void)clockCase(17, 0, true, false, 8000, -1, -1, 180, 137, 250,
        comparison.packet_frames, 0, 0, false, &comparison);
}

void coalescingExperiments()
{
    // One local device plus three authenticated remote streams: exactly four
    // peers. The callbacks, rings, clocks, jitter, resamplers, mixer and recovery
    // are production components; only arrival-window timing is injected.
    for (const std::uint64_t batch : {0ULL,4000ULL,8000ULL}) {
        const auto result=clockCase(0,0,true,false,batch);
        expect(result.lateUnderruns==0 && result.lateTrims==0 && result.latePadding==0,
            "bounded delivery windows settle without recurring underruns, trimming or padding in the final minute");
        expect(result.recoveries==result.completions,
            "continuous bounded delivery does not leave recovery active");
        if (batch==0) expect(result.averagePath<=50.6,
            "clean deterministic latency must not exceed the unchanged-code baseline of 50.56 frames");
    }
    const auto releaseDisabled=clockCase(0,0,true,false,8000,0);
    expect(releaseDisabled.lateUnderruns==0 && releaseDisabled.latePadding==0,
        "release-disabled diagnostic isolates recurring starvation from packet coalescing alone");
    const auto cleanSkew=clockCase(17,0,true);
    expect(cleanSkew.lateUnderruns==0 && cleanSkew.lateTrims==0 && cleanSkew.latePadding==0 && cleanSkew.averagePath<=52.6,
        "clean independent clocks retain the unchanged-code latency baseline of 52.512 frames");
    for (const std::uint64_t batch : {4000ULL,8000ULL}) {
        const auto skewed=clockCase(17,0,true,false,batch);
        expect(skewed.lateUnderruns==0 && skewed.lateTrims==0 && skewed.latePadding==0,
            "small independent clock skew and bounded delivery windows settle without recurring corrections");
        const auto noRelease=clockCase(17,0,true,false,batch,0);
        // This ablation intentionally disables retirement of surplus. The
        // source estimator is allowed 10 ppm error above, so bounded trimming
        // is expected here; the default-release case above still requires zero.
        const auto estimatorErrorBudget=std::uint64_t(std::ceil(0.000010*rate*60*(participants-1)));
        expect(noRelease.lateUnderruns==0 && noRelease.lateTrims<=estimatorErrorBudget && noRelease.latePadding==0,
            "release-disabled skew control remains bounded by the source estimator tolerance without starvation");
    }
    const auto longRun=clockCase(17,0,true,false,8000,-1,-1,1800);
    expect(longRun.lateUnderruns==0 && longRun.lateTrims==0 && longRun.latePadding==0,
        "batching and drift stay stable over thirty simulated minutes rather than postponing corrections");
}

void clockExperiments()
{
    const auto resumed = clockCase(200, 100, true, true);
    expect(resumed.lateUnderruns == 0 && resumed.lateTrims == 0 && resumed.latePadding == 0,
        "combined clock skew and a twenty-second peer outage settle without recurring queue corrections");
    for (bool adaptive : {false, true}) {
        const auto nominal = clockCase(0, 0, adaptive);
        const auto sender = clockCase(200, 0, adaptive); // working sender correction control
        expect(nominal.lateUnderruns == 0 && nominal.lateTrims == 0 && nominal.latePadding == 0 &&
            sender.lateUnderruns == 0 && sender.lateTrims == 0 && sender.latePadding == 0,
            "nominal and sender-only skew controls have clean settled playback");
        for (int ppm : {100, 500}) {
            const auto slow = clockCase(0, -ppm, adaptive);
            const auto fast = clockCase(0, ppm, adaptive);
            // -500 ppm needs +500.25 ppm relative correction, just beyond the
            // configured +500 cap: at most one residual frame per source/minute.
            const std::uint64_t allowedClampedTrims = !adaptive && ppm == 500 ? participants - 1 : 0;
            expect(fast.lateUnderruns == 0 && fast.lateTrims == 0 && fast.latePadding == 0 &&
                slow.lateUnderruns == 0 && slow.lateTrims <= allowedClampedTrims && slow.latePadding == 0,
                "receiver skew settles without starvation or padding, retaining the configured correction cap");
        }
        const auto equal = clockCase(200, 200, adaptive);
        expect(equal.lateUnderruns == 0 && equal.lateTrims == 0 && equal.latePadding == 0 &&
            std::abs(equal.sourceRatio - 1.0) < 0.000010,
            "equal sender and receiver clock skew requires no sustained peer rate correction");
    }
}
// Full production capture bridge -> duplex graph -> authenticated peer streams
// -> mixer -> duplex render, with exactly four independent endpoint pairs.
class WasapiSink final : public jam2::PeerStreamPlayback {
public:
    jam2::audio::MonoRingBuffer captured{8192, true}, pitch{8192}, rendered{8192};
    jam2::audio::StreamControl control;
    jam2::audio::device_processing::DuplexProcessingState processing;
    jam2::audio::wasapi::CaptureBuffer bridge;
    std::array<std::int32_t, 480> input{};
    std::size_t period;
    std::uint64_t signalFrames = 0;
    explicit WasapiSink(std::size_t frames, std::size_t prefill = 960) : period(frames) {
        control.network_capture_requested_enabled.store(true);
        control.network_capture_generation_requested.store(1);
        control.network_playback_enabled.store(true);
        control.playback_ratio_ramp_frames.store(rate / 4);
        processing.capture = &captured; processing.pitch = &pitch;
        processing.playback = &rendered; processing.control = &control;
        processing.playback_prefill_frames = prefill;
        jam2::audio::device_processing::prepare_duplex_processing(processing, 480, 1, rate);
        bridge.configure(1, rate, {});
    }
    std::size_t depthFrames() const noexcept override { return rendered.available_read(); }
    std::size_t callbackFrames() const noexcept override { return period; }
    std::uint64_t underrunFrames() const noexcept override { return rendered.underrun_frames(); }
    jam2::audio::PlaybackRecoveryState playbackRecoveryState() const noexcept override {
        auto state = jam2::audio::device_processing::read_playback_recovery_state(control);
        state.padding_policy = jam2::audio::RecoveryPaddingPolicy::MeasuredShortage;
        return state;
    }
    jam2::audio::DeviceClockSample deviceClockSample() const noexcept override {
        return jam2::audio::device_processing::read_device_clock(control);
    }
    std::size_t pushFrames(std::span<const std::int32_t> frames) noexcept override { return rendered.push(frames); }
    void requestDropFrames(std::size_t frames) noexcept override { rendered.request_drop_oldest(frames); }
    void setResamplerRatio(double ratio) noexcept override { control.playback_ratio_ppm.store(static_cast<int>(std::llround(ratio * 1000000))); }
    void callback(std::uint64_t now) {
        const std::array<std::int32_t*, 1> writable{input.data()};
        const std::array<const std::int32_t*, 1> readable{input.data()};
        const jam2::audio::DeviceClockSample clock{processing.engine_frame_counter, now};
        for (std::size_t offset = 0; offset < period;) {
            const auto frames = std::min<std::size_t>(128, period - offset);
            bridge.pull(writable, frames);
            jam2::audio::device_processing::process_duplex_block(processing, readable, frames,
                true, clock);
            for (std::size_t i = 0; i < frames; ++i) signalFrames += processing.playback_scratch[i] != 0;
            offset += frames;
        }
    }
};

void wasapiMesh(bool fast = false)
{
    const std::array<std::size_t, 4> periods = fast
        ? std::array<std::size_t, 4>{336, 480, 128, 480}
        : std::array<std::size_t, 4>{128, 192, 480, 96};
    constexpr std::array<double, 4> inputPpm{-350, 100, 450, -100}, outputPpm{100, -100, 200, 0};
    std::array<std::unique_ptr<WasapiSink>, 4> sinks;
    std::array<std::unique_ptr<jam2::NetworkSession>, 4> sessions;
    std::array<double, 4> captureDue{}, renderDue{};
    std::array<std::uint32_t, 4> sequence{};
    auto config = streamConfig(*jam2::find_join_profile(fast ? "fast" : "moderate"));
    config.playback_queue_capacity_frames = 8192;
    if (!fast) {
        config.playout_delay_frames = 960;
        config.playback_max_frames = 4096;
        config.jitter_buffer_frames = 960;
        config.jitter_buffer_max_frames = 2048;
        config.adaptive_playback_cushion = false;
    }
    for (std::size_t i = 0; i < participants; ++i) {
        sinks[i] = std::make_unique<WasapiSink>(periods[i], fast ? 64 : 960);
        std::vector<jam2::NetworkPeerDescriptor> peers;
        for (std::size_t other = 0; other < participants; ++other) if (other != i)
            peers.push_back({{other + 1}, jam2::resolve_udp_endpoint({"127.0.0.1", static_cast<std::uint16_t>(32100 + other)}),
                jam2::PeerEndpointState::Active});
        jam2::UdpSocket socket; socket.bind({"127.0.0.1", 0});
        jam2::NetworkSessionContract contract; contract.frames_per_packet = packetFrames;
        sessions[i] = std::make_unique<jam2::NetworkSession>(std::move(socket), sessionInfo(), contract,
            i == 0 ? jam2::SessionBootstrapRole::Creator : jam2::SessionBootstrapRole::Joiner,
            jam2::PeerId{i + 1}, peers, config, sinks[i].get());
    }
    struct Pending {
        std::size_t sender;
        std::uint32_t sequence;
        std::uint64_t frame, due;
        std::array<std::int32_t, packetFrames> samples;
    };
    std::vector<Pending> pending;
    const auto base = jam2::monotonic_us() + 1000;
    std::uint64_t accepted = 0, duplicates = 0;
    std::array<std::uint64_t, 4> observedUnderruns{};
    for (std::uint64_t tick = 0; tick < 30000; ++tick) {
        const auto now = base + tick * 32 * 1000000 / rate;
        for (std::size_t i = 0; i < participants; ++i) {
            auto& sink = *sinks[i];
            captureDue[i] += 32 * (1. + inputPpm[i] / 1000000.);
            if (captureDue[i] >= periods[i]) {
                captureDue[i] -= periods[i];
                std::array<float, 480> source{}; source.fill(static_cast<float>(i + 1) * .02f);
                sink.bridge.push({reinterpret_cast<const std::uint8_t*>(source.data()), periods[i] * sizeof(float)},
                    periods[i], {jam2::audio::wasapi::SampleEncoding::Float32, 1, 32, 0}, false, false);
            }
            renderDue[i] += 32 * (1. + outputPpm[i] / 1000000.);
            if (renderDue[i] >= periods[i]) { renderDue[i] -= periods[i]; sink.callback(now); }
            while (sink.captured.available_read() >= packetFrames) {
                Pending packet{i, ++sequence[i], 0, now + 2000, {}};
                expect(sink.captured.pop_exact_timed(packet.samples, packet.frame) == packetFrames,
                    "WASAPI render-paced capture must packetize contiguous device-time audio");
                if (packet.sequence % 43 == 0) continue; // Deterministic loss.
                packet.due += packet.sequence % 7 == 0 ? 4000 : packet.sequence % 3 * 333; // Jitter/reordering.
                pending.push_back(packet);
                if (packet.sequence % 19 == 0) { packet.due += 333; pending.push_back(packet); }
            }
        }
        for (auto it = pending.begin(); it != pending.end();) {
            if (it->due > now) { ++it; continue; }
            for (std::size_t receiver = 0; receiver < participants; ++receiver) if (receiver != it->sender) {
                const auto result = deliver(sessions[receiver]->peerStream({it->sender + 1}),
                    it->sequence, it->frame, now, it->samples);
                accepted += result == jam2::PeerAudioResult::Accepted;
                duplicates += result == jam2::PeerAudioResult::Duplicate;
            }
            it = pending.erase(it);
        }
        for (std::size_t i = 0; i < sessions.size(); ++i) {
            auto& session = sessions[i];
            const auto paddingBefore = session->mixStats().adaptive_padding_frames;
            session->advance(now);
            session->finishReceiveBatch(false);
            const auto underruns = sinks[i]->underrunFrames();
            if (fast && tick > 15000) {
                expect(session->mixStats().adaptive_padding_frames - paddingBefore <=
                        underruns - observedUnderruns[i],
                    "WASAPI fast-profile recovery must not magnify native-batch shortages into cushion-sized silence");
            }
            observedUnderruns[i] = underruns;
        }
    }
    expect(accepted > 10000 && duplicates > 0, "four WASAPI-shaped peers must exchange authenticated audio under impairment");
    for (std::size_t i = 0; i < participants; ++i) {
        expect(sessions[i]->peerCount() == 3 && sinks[i]->signalFrames > rate * 10,
            "every WASAPI-shaped listener must retain its three peers and render received audio");
        expect(sinks[i]->processing.engine_frame_counter > rate * 19 &&
            sinks[i]->bridge.status().capture_overrun_frames == 0,
            "independent capture/render periods must preserve bounded capture and the advancing output timeline");
        expect(sinks[i]->captured.stats().overruns == 0 && sinks[i]->rendered.stats().overruns == 0,
            "WASAPI-shaped callbacks and network packet sizes must compose without ring overflows");
    }
}

} // namespace

int main(int argc, char** argv)
{
    try {
        jam2::NetworkRuntime runtime;
        if (argc != 2) throw std::runtime_error("expected capture, interruption, or clock");
        const std::string_view experiment = argv[1];
        if (experiment == "gap-hold-timers") gapDeliveryExperiments(true,true);
        else if (experiment == "gap-trigger-thresholds") gapDeliveryExperiments(true);
        else if (experiment == "gap-delivery-investigation") gapDeliveryExperiments();
        else if (experiment == "clock-alias-investigation") clockAliasExperiments();
        else if (experiment.starts_with("compare-profile-")) {
            const auto comparison = jam2::test::comparisonProfile(experiment.substr(16));
            struct Condition { int skew; std::uint64_t batch, jitter, stall; };
            for (const auto c : {Condition{0,0,0,0}, {17,0,0,0}, {17,4000,0,0},
                    {17,8000,0,0}, {17,0,3600,0}, {17,0,0,6000}}) {
                const auto r = clockCase(c.skew, 0, true, false, c.batch, -1, -1,
                    180, 137, 250, comparison.packet_frames, c.jitter, c.stall, false, &comparison);
                expect(r.lateUnderruns == 0 && r.lateTrims == 0 && r.latePadding == 0,
                    "comparison final minute has no underruns, source trimming or recovery padding");
            }
        }
        else if (experiment == "wasapi") wasapiMesh();
        else if (experiment == "wasapi-fast") wasapiMesh(true);
        else if (experiment == "capture") captureOverflow();
        else if (experiment == "interruption") {
            for (const char* profile : {"fast", "moderate"}) {
                interruptionCase(0, false, profile);
                interruptionCase(2000000, false, profile);
                interruptionCase(9000000, true, profile);
                interruptionCase(20000000, true, profile);
                interruptionCase(20000000, false, profile);
            }
        } else if (experiment == "clock") clockExperiments();
        else if (experiment == "coalescing") coalescingExperiments();
        else if (experiment == "ingress-timing") {
            for (std::uint64_t jitter : {0ULL, 3600ULL}) for (std::uint64_t stall : {0ULL, 3000ULL, 6000ULL}) {
                const auto actual = clockCase(0, 0, true, false, 0, -1, -1, 180, 137, 250, 64, jitter, stall);
                expect(actual.lateUnderruns == 0 && actual.lateTrims == 0 && actual.latePadding == 0,
                    "actual delivery timestamps must let the production pipeline settle under the known local stalls");
                // Diagnostic negative control only. Replacing delivery time
                // with arrival time must not be assumed to fix a blocked
                // runtime; its actual callback underruns/queues are retained.
                if (stall) {
                    const auto arrival = clockCase(0, 0, true, false, 0, -1, -1, 180, 137, 250, 64, jitter, stall, true);
                    if (stall == 6000) expect(arrival.lateUnderruns > 0 && arrival.latePadding > 0,
                        "arrival-time substitution must retain the measured starvation negative control for a blocked runtime");
                }
            }
        }
        else if (experiment == "gap-learning") {
            for (int skew:{0,17}) for (std::uint64_t batch:{0ULL,4000ULL,8000ULL}) {
                const auto r=clockCase(skew,0,true,false,batch,-1,0);
                expect(r.lateUnderruns==0 && r.lateTrims==0 && r.latePadding==0,
                    "short-gap evidence prevents repeated corrections without clock-skew trimming");
            }
        }
        else if (experiment == "callback-phases") {
            for (int skew:{0,17}) for (std::uint64_t phase:{0ULL,75ULL,250ULL,400ULL,650ULL}) {
                for (std::uint64_t batch:{0ULL,4000ULL,8000ULL}) {
                    const auto result=clockCase(skew,0,true,false,batch,-1,-1,180,phase);
                    expect(result.lateUnderruns==0 && result.lateTrims==0 && result.latePadding==0,
                        "callback-phase changes do not leave recurring default-mode audio corrections");
                    if (!batch) {
                        const auto legacy=clockCase(skew,0,true,false,0,-1,50000,180,phase);
                        expect(result.averagePath==legacy.averagePath,
                            "every clean callback phase retains the original latency exactly");
                    }
                }
            }
        }
        else if (experiment == "callback-runway") {
            for (int frames:{64,128}) for (int skew:{0,17}) for (std::uint64_t worker:{500ULL,1000ULL,1500ULL})
                for (std::uint64_t phase:{0ULL,137ULL,500ULL}) {
                    const auto result=clockCase(skew,0,true,false,0,-1,-1,180,phase,worker,frames);
                    expect(result.lateUnderruns==0 && result.lateTrims==0 && result.latePadding==0,
                        "ordinary worker scheduling retains interpolation runway across callback phases");
                }
        }
        else if (experiment == "asio-cadence") {
            rate = 44100;
            for (int frames : {64, 128}) for (int skew : {0, 17})
                for (std::uint64_t worker : {750ULL, 1000ULL, 1500ULL})
                    for (std::uint64_t phase : {0ULL, 137ULL, 500ULL}) {
                        const auto result = clockCase(skew, 0, true, false, 0, -1, -1, 180, phase, worker, frames);
                        expect(result.lateUnderruns == 0 && result.lateTrims == 0 && result.latePadding == 0,
                            "44.1 kHz ASIO cadence settles across packet sizes, callback phases and worker periods");
                    }
        }
        else if (experiment == "bounded-jitter") {
            rate = 44100;
            for (int frames : {64, 128}) for (int skew : {0, 17})
                for (std::uint64_t worker : {250ULL, 1000ULL, 1500ULL}) {
                    const auto result = clockCase(skew, 0, true, false, 0, -1, -1, 180, 137, worker, frames, 3600);
                    expect(result.lateUnderruns == 0 && result.lateTrims == 0 && result.latePadding == 0,
                        "bounded WAN arrival jitter settles without recurring corrections at both packet sizes");
                }
        }
        else if (experiment == "startup-recovery") startupRecoveryExperiments();
        else if (experiment == "repeated-recovery") repeatedRecoveryExperiments();
        else if (experiment == "large-recovery") largeRecoveryExperiments();
        else throw std::runtime_error("unknown experiment");
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        ++failures;
    } catch (...) {
        std::cerr << "FAIL: unknown experiment exception\n";
        ++failures;
    }
    std::cout << "Audio-flow regression failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}

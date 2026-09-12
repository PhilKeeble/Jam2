#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "audio_device_processing.hpp"
#include "engine.hpp"
#include "input_source.hpp"
#include "network_session.hpp"
#include "tuning_profile.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace {
constexpr int rate = 48000;
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
    std::size_t callbackFrames() const noexcept override { return ::callbackFrames; }
    std::uint64_t underrunFrames() const noexcept override { return ring.underrun_frames(); }
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
            &ring, &control, resampler, scratch, output);
        ++callbacks;
        for (auto sample : output) nonzeroFrames += sample != 0 ? 1U : 0U;
    }
    jam2::audio::MonoRingBuffer ring;
    jam2::audio::StreamControl control;
    jam2::audio::device_processing::PlaybackResamplerState resampler;
    std::array<std::int32_t, 70> scratch{};
    std::array<std::int32_t, ::callbackFrames> output{};
    std::size_t prefill;
    bool started = false;
    std::uint64_t callbacks = 0, nonzeroFrames = 0;
    std::uint64_t generation = 0;
};

jam2::PeerStreamConfig streamConfig(const jam2::JoinProfile& profile)
{
    jam2::PeerStreamConfig config;
    config.sample_rate = rate;
    config.frames_per_packet = static_cast<int>(packetFrames);
    config.audio_format = jam2::NetworkAudioFormat::Pcm24Mono;
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
    Receiver(std::size_t local, const jam2::JoinProfile& profile)
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
        contract.frames_per_packet = static_cast<int>(packetFrames);
        jam2::UdpSocket socket;
        socket.bind({"127.0.0.1", 0});
        session = std::make_unique<jam2::NetworkSession>(std::move(socket), sessionInfo(), contract,
            jam2::SessionBootstrapRole::Creator, jam2::PeerId{local + 1}, peers,
            streamConfig(profile), &sink);
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
    std::uint64_t frame, std::uint64_t now, std::span<const std::int32_t> samples)
{
    std::array<std::uint8_t, packetFrames * 3> payload{};
    if (!jam2::protocol::pack_audio_q31_into(jam2::NetworkAudioFormat::Pcm24Mono, samples, payload))
        throw std::runtime_error("failed to pack experiment audio");
    const auto info = sessionInfo();
    jam2::protocol::Header header{jam2::protocol::PacketType::Audio, info.session_id,
        sequence, frame, static_cast<std::uint16_t>(payload.size()), 0};
    std::array<std::uint8_t, jam2::protocol::kMaxDatagramSize> wire{};
    const auto size = jam2::protocol::encode_packet_into(header, payload, info.key,
        jam2::NetworkAudioFormat::Pcm24Mono, wire);
    const auto parsed = jam2::protocol::parse_packet(std::span(wire).first(size), info.key,
        info.session_id, jam2::NetworkAudioFormat::Pcm24Mono);
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
};
Observations observe(const Receiver& receiver)
{
    const auto& stats = receiver.session->mixStats();
    return {receiver.sink.underrunFrames(), stats.adaptive_padding_frames,
        stats.live_tail_trimmed_frames, stats.missing_peer_frames, stats.adaptive_raise_events,
        stats.adaptive_target_frames, receiver.path(), stats.receive_recovery_events,
        stats.receive_recovery_completions};
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
};
ClockResult clockCase(int senderPpm, int receiverPpm, bool adaptive, bool interruptPeer = false)
{
    auto profile = *jam2::find_join_profile("fast");
    profile.adaptive_playback_cushion = adaptive;
    // Fixed mode needs a useful runway for an isolated oscillator experiment.
    if (!adaptive) profile.playout_delay_frames = profile.playback_prefill_frames = 256;
    Receiver receiver(0, profile); // one receiver + three remote senders = four peers
    const auto base = receiver.session->schedule().startTimeUs();
    constexpr std::uint64_t end = 180000000;
    constexpr std::uint64_t lateBegin = 120000000;
    std::array<std::int32_t, packetFrames> audio{};
    audio.fill(1048576);
    const auto timeAt = [](std::uint64_t frames, int ppm) {
        return static_cast<std::uint64_t>(static_cast<long double>(frames) * 1000000.0L /
            (rate * (1.0L + static_cast<long double>(ppm) / 1000000.0L)));
    };
    std::uint64_t packet = 1, callback = 1, worker = 250, now = 0;
    Observations lateStart;
    bool lateStarted = false;
    long double pathIntegral = 0;
    std::uint64_t observedUs = 0, maximumPath = 0, accepted = 0;
    std::uint64_t releaseFrames = 0;
    while (now < end) {
        const auto callbackUs = timeAt(callback * callbackFrames, receiverPpm) + 137;
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
            receiver.session->advance(base + now);
            while (timeAt(packet * packetFrames, senderPpm) <= now) {
                for (std::size_t peer = 1; peer < participants; ++peer) {
                    if (interruptPeer && peer == 3 && now >= 5000000 && now < 25000000) continue;
                    const auto result = deliver(receiver.session->peerStream({peer + 1}),
                        static_cast<std::uint32_t>(packet), (packet - 1) * packetFrames, base + now, audio);
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
            worker += 250;
        }
        maximumPath = std::max(maximumPath, receiver.path());
    }
    const auto final = observe(receiver);
    const auto& stream = receiver.session->peerStream({2}).stats();
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
    std::cout << "METRIC clock sender_ppm=" << senderPpm << " receiver_ppm=" << receiverPpm
        << " adaptive=" << adaptive << " duration_s=180 late_window_s=60"
        << " interrupted_peer=" << interruptPeer
        << " estimated_drift_ppm=" << result.drift << " source_ratio=" << result.sourceRatio
        << " local_device_drift_ppm=" << receiver.session->deviceClockStats().ppm
        << " desired_relative_ratio=" << (1.0 + senderPpm / 1000000.0) / (1.0 + receiverPpm / 1000000.0)
        << " late_underruns=" << result.lateUnderruns << " late_trimmed=" << result.lateTrims
        << " late_padding=" << result.latePadding << " late_missing=" << final.missing - lateStart.missing
        << " late_raises=" << final.raises - lateStart.raises << " final_target=" << final.target
        << " late_accelerated_output_frames=" << result.releaseFrames
        << " late_average_path=" << result.averagePath << " maximum_path=" << maximumPath
        << " final_path=" << final.path << " recovery_events=" << final.recovery
        << " recovery_completions=" << final.completions << '\n';
    return result;
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
} // namespace

int main(int argc, char** argv)
{
    try {
        jam2::NetworkRuntime runtime;
        if (argc != 2) throw std::runtime_error("expected capture, interruption, or clock");
        const std::string_view experiment = argv[1];
        if (experiment == "capture") captureOverflow();
        else if (experiment == "interruption") {
            for (const char* profile : {"fast", "moderate"}) {
                interruptionCase(0, false, profile);
                interruptionCase(2000000, false, profile);
                interruptionCase(9000000, true, profile);
                interruptionCase(20000000, true, profile);
                interruptionCase(20000000, false, profile);
            }
        } else if (experiment == "clock") clockExperiments();
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

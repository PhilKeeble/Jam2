#include "audio_device.hpp"
#include "audio_ring.hpp"
#include "common.hpp"
#include "midi.hpp"
#include "network_session.hpp"
#include "peer_mixer.hpp"
#include "peer_stream.hpp"
#include "prepared_track_source.hpp"
#include "protocol.hpp"
#include "session_authority.hpp"
#include "stun.hpp"
#include "track_take_recorder.hpp"
#include "tuning_profile.hpp"
#include "udp_socket.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

template <typename Callable>
void expectThrows(Callable&& callable, const char* message)
{
    try {
        callable();
    } catch (const std::exception&) {
        return;
    }
    expect(false, message);
}

class BufferSink final : public jam2::PeerStreamPlayback {
public:
    std::size_t depthFrames() const noexcept override { return depth; }

    std::size_t pushFrames(std::span<const std::int32_t> frames) noexcept override
    {
        depth += frames.size();
        pushed += frames.size();
        return frames.size();
    }

    void requestDropFrames(std::size_t frames) noexcept override
    {
        const std::size_t dropped = (std::min)(depth, frames);
        depth -= dropped;
        requestedDrops += dropped;
    }

    void setResamplerRatio(double value) noexcept override { ratio = value; }
    std::uint64_t underrunFrames() const noexcept override { return underruns; }

    std::size_t depth = 0;
    std::size_t pushed = 0;
    std::size_t requestedDrops = 0;
    std::uint64_t underruns = 0;
    double ratio = 1.0;
};

class CollectingSink final : public jam2::PeerStreamPlayback {
public:
    std::size_t depthFrames() const noexcept override { return depth; }

    std::size_t pushFrames(std::span<const std::int32_t> frames) noexcept override
    {
        depth += frames.size();
        samples.insert(samples.end(), frames.begin(), frames.end());
        return frames.size();
    }

    void requestDropFrames(std::size_t frames) noexcept override
    {
        depth -= (std::min)(depth, frames);
    }

    void setResamplerRatio(double) noexcept override {}

    std::size_t depth = 0;
    std::vector<std::int32_t> samples;
};

void putU16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value)
{
    bytes[offset] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    bytes[offset + 1] = static_cast<std::uint8_t>(value & 0xffU);
}

void putU32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
    bytes[offset] = static_cast<std::uint8_t>((value >> 24U) & 0xffU);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 16U) & 0xffU);
    bytes[offset + 2] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
    bytes[offset + 3] = static_cast<std::uint8_t>(value & 0xffU);
}

std::vector<std::uint8_t> stunResponse(
    std::span<const std::uint8_t, 12> transaction,
    std::uint32_t address,
    std::uint16_t port)
{
    constexpr std::uint32_t magicCookie = 0x2112a442U;
    std::vector<std::uint8_t> response(32, 0);
    putU16(response, 0, 0x0101U);
    putU16(response, 2, 12U);
    putU32(response, 4, magicCookie);
    std::copy(transaction.begin(), transaction.end(), response.begin() + 8);
    putU16(response, 20, 0x0020U);
    putU16(response, 22, 8U);
    response[25] = 0x01U;
    putU16(response, 26, static_cast<std::uint16_t>(port ^ (magicCookie >> 16U)));
    putU32(response, 28, address ^ magicCookie);
    return response;
}

jam2::PeerStreamConfig peerConfig()
{
    jam2::PeerStreamConfig config;
    config.audio_format = jam2::NetworkAudioFormat::Pcm16Mono;
    config.sample_rate = 48000;
    config.frames_per_packet = 64;
    config.playback_queue_capacity_frames = 512;
    config.playback_max_frames = 1024;
    config.collect_diagnostics = true;
    return config;
}

void testRingMidiAndDownmixBoundaries()
{
    jam2::audio::MonoRingBuffer ring(4);
    expect(ring.available_write() == 4, "empty ring reports its writable capacity");
    const std::array<std::int32_t, 2> frames{10, 20};
    expect(ring.push(frames) == frames.size() && ring.available_write() == 2,
        "ring writable capacity follows pushes");
    ring.set_diagnostics_enabled(true);
    std::array<std::int32_t, 3> output{};
    expect(ring.pop(output) == 2 && output[2] == 0,
        "ring zero-fills a bounded underrun");
    ring.reset();
    const auto resetStats = ring.stats();
    expect(ring.available_read() == 0 && ring.available_write() == 4 &&
            resetStats.underruns == 0 && resetStats.overruns == 0,
        "ring reset restores capacity and clears diagnostics");

    jam2::audio::MonoRingBuffer exactRing(4);
    const std::array<std::int32_t, 2> exactFirst{31, 32};
    expect(exactRing.push(exactFirst) == exactFirst.size(),
        "exact ring fixture queues its partial packet");
    std::array<std::int32_t, 3> exactOutput{7, 8, 9};
    expect(exactRing.pop_exact(exactOutput) == 0 &&
            exactRing.available_read() == exactFirst.size() &&
            exactOutput == std::array<std::int32_t, 3>{7, 8, 9} &&
            exactRing.stats().underruns == 0,
        "exact ring pop preserves partial capture data and caller output");
    const std::array<std::int32_t, 1> exactLast{33};
    expect(exactRing.push(exactLast) == exactLast.size() &&
            exactRing.pop_exact(exactOutput) == exactOutput.size() &&
            exactOutput == std::array<std::int32_t, 3>{31, 32, 33} &&
            exactRing.available_read() == 0,
        "exact ring pop consumes one complete fixed packet in order");

    jam2::midi::EventQueue queue;
    expect(queue.push({1, 0, 0x90, 60, 100, 3}), "MIDI clear fixture enqueues");
    queue.clear();
    jam2::midi::Event event;
    expect(queue.depth() == 0 && !queue.pop(event),
        "MIDI clear atomically discards pending events");

    jam2::audio::SmoothedMonoDownmix downmix;
    downmix.configure(2, 48000.0, 64);
    const std::array<double, 2> peaks{0.5, 0.25};
    for (int block = 0; block < 8; ++block) downmix.beginBlock(peaks);
    expect(downmix.channelCount() == 2 && downmix.effectiveWeight() > 0.0 &&
            downmix.maxGainChangePerBlock() >= 0.0,
        "smoothed downmix publishes configured channel diagnostics");
    expect(jam2::audio::block_downmix_channel_active(0.5, 0.5) &&
            jam2::audio::block_downmix_active_channels(peaks) == 2,
        "offline downmix identifies channels relative to the loudest input");
    expect(jam2::audio::downmix_unit_to_ppm(-1.0) == 0 &&
            jam2::audio::downmix_unit_to_ppm(5.0) == 4000000,
        "downmix diagnostic conversion clamps its public range");
    jam2::audio::StreamControl control;
    jam2::audio::publish_downmix_diagnostics(control, downmix);
    expect(control.input_downmix_selected_channels.load() == 2 &&
            control.input_downmix_transition_count.load() >= 2,
        "downmix diagnostics publish to the callback control surface");
}

void testPreparedAndRecorderBoundaries()
{
    jam2::audio::PreparedTrackSource source(32);
    const int loading = source.claimLoadingSlot();
    expect(loading >= 0 && source.loadingData(loading) != nullptr,
        "prepared source exposes a claimed loading slot");
    source.abandonLoadingSlot(loading);
    const int ready = source.claimLoadingSlot();
    expect(ready == loading && source.publishReady(ready, 16, 48000),
        "abandoned loading slot is immediately reusable");
    source.abandonReadySlot(ready);
    expect(source.claimLoadingSlot() == ready,
        "abandoned ready slot is immediately reusable");
    source.abandonLoadingSlot(-1);
    source.abandonReadySlot(99);

    const std::filesystem::path takePath =
        std::filesystem::temp_directory_path() /
        ("jam2-track-take-boundary-" + std::to_string(jam2::monotonic_us()) + ".wav");
    {
        jam2::audio::TrackTakeRecorder recorder(128);
        const auto stats = recorder.stats();
        expect(!recorder.armed() && !stats.armed && !stats.recording && !stats.finalized &&
                stats.queue_capacity_frames == 4096,
            "track-take recorder stats expose a clean state and minimum queue bound");
        std::string error;
        expect(recorder.arm("boundary-take", takePath, 48000, error) && recorder.armed(),
            "armed track-take recorder publishes callback-side activity");
        recorder.cancel();
        expect(!recorder.armed(),
            "canceled track-take recorder clears callback-side activity");
    }
    std::error_code removeError;
    (void)std::filesystem::remove(takePath, removeError);
}

void testProtocolPeerAndMixerBoundaries()
{
    jam2::NetworkPacketSchedule packetSchedule(48000, 64, 1000);
    expect(jam2::capture_audio_pacing(32, 64) ==
                jam2::NetworkAudioPacing::CaptureClock &&
            jam2::capture_audio_pacing(64, 64) ==
                jam2::NetworkAudioPacing::CaptureClock &&
            jam2::capture_audio_pacing(128, 64) ==
                jam2::NetworkAudioPacing::CaptureSynchronized &&
            jam2::capture_audio_pacing(0, 64) ==
                jam2::NetworkAudioPacing::CaptureSynchronized,
        "capture-clock pacing is limited to valid callbacks no larger than one packet");
    jam2::NetworkCapturePacketPacer captureClockPacer(
        jam2::NetworkAudioPacing::CaptureClock, 64);
    expect(captureClockPacer.captureClockActive() &&
            captureClockPacer.captureReady(64) &&
            !captureClockPacer.catchupReady(64),
        "one freshly published callback-sized packet may lead its wall deadline");
    captureClockPacer.observePacketSent(64);
    expect(captureClockPacer.packetRequiresSpacing() &&
            !captureClockPacer.captureReady(64) &&
            !captureClockPacer.catchupReady(64),
        "one packet left from an accumulated callback remains wall-clock spaced");
    expect(captureClockPacer.catchupReady(128),
        "capture backlog catches up only while more than one packet remains");
    captureClockPacer.observePacketSent(0);
    expect(!captureClockPacer.packetRequiresSpacing() &&
            captureClockPacer.captureReady(64),
        "draining capture to the live edge re-enables device-clock dispatch");
    jam2::NetworkCapturePacketPacer largeCallbackPacer(
        jam2::NetworkAudioPacing::CaptureSynchronized, 64);
    expect(!largeCallbackPacer.captureClockActive() &&
            !largeCallbackPacer.captureReady(64) &&
            largeCallbackPacer.catchupReady(128),
        "large callbacks retain scheduled anti-burst pacing and bounded catch-up");
    expect(!packetSchedule.audioSendReady(
               999, jam2::NetworkAudioPacing::Scheduled) &&
            !packetSchedule.audioSendReady(
               999, jam2::NetworkAudioPacing::CaptureSynchronized) &&
            packetSchedule.audioSendReady(
               999, jam2::NetworkAudioPacing::CaptureSynchronized, true) &&
            packetSchedule.audioSendReady(
               999, jam2::NetworkAudioPacing::CaptureClock, true) &&
            packetSchedule.audioSendReady(
               1000, jam2::NetworkAudioPacing::Scheduled),
        "capture synchronization overrides only an explicit stale device deadline");
    expect(packetSchedule.audioSendWaitBudgetUs(0, 1000) == 1000 &&
            packetSchedule.audioSendWaitBudgetUs(500, 1000) == 500 &&
            packetSchedule.audioSendWaitBudgetUs(1000, 1000) == 0,
        "network receive wait is bounded by the next audio-send deadline");
    packetSchedule.commitAudioPacket();
    expect(packetSchedule.audioSendWaitBudgetUs(1500, 1000) == 833,
        "fractional packet schedule exposes its exact remaining receive wait");
    packetSchedule.resynchronizeAudioSend(5000);
    expect(packetSchedule.nextAudioSendUs() == 5000,
        "capture synchronization rebases the wall deadline without changing packet ownership");

    for (int value = static_cast<int>(jam2::protocol::ParseError::None);
         value <= static_cast<int>(jam2::protocol::ParseError::AuthenticationFailed);
         ++value) {
        expect(jam2::protocol::parse_error_text(
                   static_cast<jam2::protocol::ParseError>(value)) != nullptr,
            "every packet parse error has diagnostic text");
    }
    const std::uint64_t peerIdentity = 0xfedcba9876543210ULL;
    const auto encodedPeerIdentity = jam2::protocol::encode_peer_identity(peerIdentity);
    expect(jam2::protocol::decode_peer_identity(encodedPeerIdentity) == peerIdentity &&
            !jam2::protocol::decode_peer_identity(
                std::span<const std::uint8_t>(encodedPeerIdentity).first(7)) &&
            !jam2::protocol::decode_peer_identity(
                jam2::protocol::encode_peer_identity(0)),
        "UDP proof identity has a fixed nonzero eight-byte representation");
    jam2::protocol::ReplayWindow replay;
    expect(replay.observe(10) == jam2::protocol::ReplayResult::New,
        "replay window accepts its first sequence");
    replay.reset();
    expect(replay.observe(10) == jam2::protocol::ReplayResult::New,
        "replay reset forgets the previous window");

    BufferSink firstSink;
    BufferSink secondSink;
    const jam2::PeerStreamConfig streamConfig = peerConfig();
    expect(firstSink.acceptsFrames(),
        "default peer playback sink accepts frames");
    jam2::PeerStream first(streamConfig, jam2::monotonic_us(), &firstSink);
    jam2::PeerStream second(streamConfig, jam2::monotonic_us(), &secondSink);
    first = std::move(second);
    expect(first.config().frames_per_packet == 64 &&
            first.effectivePlayoutDelayFrames() == 0,
        "peer-stream move assignment retains immutable tuning");

    jam2::PeerMixerConfig mixerConfig;
    mixerConfig.sample_rate = 48000;
    mixerConfig.frames_per_block = 64;
    mixerConfig.output_max_frames = 1024;
    jam2::PeerMixer firstMixer(mixerConfig, &firstSink);
    jam2::PeerMixer secondMixer(mixerConfig, &secondSink);
    jam2::PeerStreamPlayback* slot = firstMixer.addPeer(44, 256);
    expect(slot != nullptr, "peer mixer creates a bounded peer slot");
    expect(firstMixer.setPeerActive(44, true),
        "peer mixer explicitly activates a new slot before contribution");
    const std::array<std::int32_t, 4> samples{1, 2, 3, 4};
    expect(slot->pushFrames(samples) > 0,
        "peer mixer slot accepts bounded source frames");
    expect(firstMixer.peerStats(44)->unity_resampler_fast_frames == samples.size(),
        "aligned unity peer audio bypasses scalar interpolation exactly");
    slot->setResamplerRatio(1.001);
    expect(slot->pushFrames(samples) > 0 &&
            firstMixer.peerStats(44)->unity_resampler_fast_frames == samples.size(),
        "non-unity peer audio retains the continuity-preserving resampler");
    slot->setResamplerRatio(1.0);
    expect(slot->pushFrames(samples) > 0 &&
            firstMixer.peerStats(44)->unity_resampler_fast_frames == samples.size(),
        "fractional resampler state is not discarded when correction returns to unity");
    slot->requestDropFrames(2);
    expect(firstMixer.setPeerGain(44, 750000) && firstMixer.setPeerMuted(44, true),
        "peer mixer applies per-peer gain and mute controls");
    const auto* peerStats = firstMixer.peerStats(44);
    expect(peerStats != nullptr && peerStats->gain_ppm == 750000 && peerStats->muted,
        "peer mixer diagnostics reflect gain and mute controls");
    secondMixer = std::move(firstMixer);
    expect(secondMixer.peerStats(44) != nullptr,
        "peer-mixer move assignment preserves owned slots");

    BufferSink recoverySink;
    mixerConfig.adaptive_playback_cushion = true;
    mixerConfig.adaptive_target_frames = 128;
    mixerConfig.adaptive_min_frames = 128;
    mixerConfig.adaptive_max_frames = 256;
    jam2::PeerMixer recoveryMixer(mixerConfig, &recoverySink);
    auto* recoverySlot = recoveryMixer.addPeer(55, 256);
    expect(recoveryMixer.setPeerActive(55, true),
        "idle-gated mixer activates its recovery fixture");
    const std::array<std::int32_t, 64> recoveryFrames{};
    expect(recoverySlot->pushFrames(recoveryFrames) == recoveryFrames.size(),
        "idle-gated mixer queues its first complete block");
    recoveryMixer.advance(1000);
    const std::size_t pushedBeforeIdle = recoverySink.pushed;
    recoveryMixer.advance(2000);
    expect(recoverySink.pushed == pushedBeforeIdle,
        "idle mixer advancement does not manufacture output without new work");
    recoverySink.depth = 0;
    recoverySink.underruns = 64;
    recoveryMixer.advance(10000);
    expect(recoverySink.pushed > pushedBeforeIdle &&
            recoveryMixer.stats().deadline_slots > 0 &&
            recoveryMixer.stats().adaptive_raise_events > 0,
        "idle mixer gate still observes asynchronous output underruns and advances recovery");
}

void testPeerStreamDriftCalibrationRejectsDequeuedBurst()
{
    BufferSink sink;
    jam2::PeerStreamConfig config = peerConfig();
    config.sample_time_playout = false;
    config.playback_max_frames = 0;
    config.stats_warmup_us = 3000000;
    config.drift_smoothing = 1.0;
    config.drift_deadband_ppm = 25;
    config.drift_max_correction_ppm = 500;
    jam2::PeerStream stream(config, 0, &sink);

    std::array<std::uint8_t, 128> payload{};
    auto receive = [&](std::uint32_t sequence, std::uint64_t receiveTimeUs) {
        const jam2::protocol::Header header{
            jam2::protocol::PacketType::Audio,
            1,
            sequence,
            static_cast<std::uint64_t>(sequence) * 64ULL,
            static_cast<std::uint16_t>(payload.size()),
            0,
        };
        expect(stream.receiveAudio(header, payload, receiveTimeUs) ==
                jam2::PeerAudioResult::Accepted,
            "drift calibration accepts each contiguous audio packet");
    };
    auto nominalReceiveTime = [](std::uint32_t sequence) {
        return 1000ULL + static_cast<std::uint64_t>(sequence) * 64ULL *
            1000000ULL / 48000ULL;
    };

    for (std::uint32_t sequence = 0; sequence < 2250; ++sequence) {
        receive(sequence, nominalReceiveTime(sequence));
    }
    // The first post-warmup packet waited 90 ms in the socket queue. The rest
    // of the queued packets are then timestamped within microseconds while the
    // receiver drains them, matching the runtime's bounded UDP receive loop.
    for (std::uint32_t sequence = 2250; sequence < 2318; ++sequence) {
        receive(sequence, 3090000ULL + static_cast<std::uint64_t>(sequence - 2250));
    }
    for (std::uint32_t sequence = 2318; sequence < 6000; ++sequence) {
        receive(sequence, nominalReceiveTime(sequence));
    }

    const auto& stats = stream.stats();
    expect(stats.drift_valid,
        "drift calibration becomes valid after the dequeued burst");
    expect(std::abs(stats.raw_drift_ppm) < 100.0,
        "dequeued startup burst does not become permanent remote clock drift");
    expect(stats.resampler_ratio > 0.9999 && stats.resampler_ratio < 1.0001,
        "dequeued startup burst does not pin peer resampling at its correction limit");
    expect(!stats.drift_baseline_calibrating &&
            stats.drift_baseline_calibration_packets > 68 &&
            stats.drift_baseline_delay_improvement_us > 80000,
        "drift diagnostics expose the bounded baseline calibration and burst delay");

    BufferSink driftingSink;
    config.stats_warmup_us = 0;
    jam2::PeerStream drifting(config, 0, &driftingSink);
    for (std::uint32_t sequence = 0; sequence < 4000; ++sequence) {
        const long double remoteTimeUs =
            static_cast<long double>(sequence) * 64.0L * 1000000.0L / 48000.0L;
        const std::uint64_t receiveTimeUs = 1000ULL +
            static_cast<std::uint64_t>(std::llround(remoteTimeUs / 1.0002L));
        const jam2::protocol::Header header{
            jam2::protocol::PacketType::Audio,
            1,
            sequence,
            static_cast<std::uint64_t>(sequence) * 64ULL,
            static_cast<std::uint16_t>(payload.size()),
            0,
        };
        expect(drifting.receiveAudio(header, payload, receiveTimeUs) ==
                jam2::PeerAudioResult::Accepted,
            "drift calibration accepts genuine clock-drift packets");
    }
    const auto& driftingStats = drifting.stats();
    expect(driftingStats.raw_drift_ppm > 150.0 &&
            driftingStats.raw_drift_ppm < 250.0,
        "baseline calibration preserves genuine positive remote clock drift");
    expect(driftingStats.resampler_ratio > 1.00015 &&
            driftingStats.resampler_ratio < 1.00025,
        "genuine clock drift still drives proportional peer resampling");

    BufferSink recurringBurstSink;
    config.stats_warmup_us = 0;
    config.drift_smoothing = 0.02;
    config.collect_diagnostics = true;
    jam2::PeerStream recurringBursts(config, 0, &recurringBurstSink);
    for (std::uint32_t sequence = 0; sequence < 12000; ++sequence) {
        const std::uint64_t nominal = nominalReceiveTime(sequence);
        const std::uint32_t position = sequence % 750U;
        const std::uint64_t secondStart = nominalReceiveTime(sequence - position);
        const std::uint64_t receiveTimeUs = position < 68U
            ? secondStart + 90000ULL + static_cast<std::uint64_t>(position)
            : nominal;
        const jam2::protocol::Header header{
            jam2::protocol::PacketType::Audio,
            1,
            sequence,
            static_cast<std::uint64_t>(sequence) * 64ULL,
            static_cast<std::uint16_t>(payload.size()),
            0,
        };
        expect(recurringBursts.receiveAudio(header, payload, receiveTimeUs) ==
                jam2::PeerAudioResult::Accepted,
            "drift estimator accepts recurring lossless dequeue bursts");
    }
    const auto& recurringStats = recurringBursts.stats();
    const double recurringAverageRatio = recurringStats.resampler_ratio_samples > 0
        ? recurringStats.resampler_ratio_sum /
            static_cast<double>(recurringStats.resampler_ratio_samples)
        : 0.0;
    expect(std::abs(recurringStats.raw_drift_ppm) < 25.0 &&
            std::abs(recurringStats.drift_ppm) < 25.0,
        "recurring dequeue bursts do not masquerade as clock drift");
    expect(recurringStats.drift_correction_clamped_samples == 0 &&
            recurringAverageRatio > 0.999975 && recurringAverageRatio < 1.000025,
        "recurring dequeue bursts neither clamp nor bias peer resampling");
    expect(recurringStats.resampler_ratio_change_max_ppm_per_second <= 101.0,
        "peer drift correction obeys its bounded ratio slew");
}

void testJitterBufferTracksOccupancyAcrossClockDrift()
{
    constexpr std::uint32_t packetCount = 60000;
    std::array<std::uint8_t, 128> payload{};
    for (const long double clockScale : {1.0005L, 0.9995L}) {
        BufferSink sink;
        jam2::PeerStreamConfig config = peerConfig();
        config.sample_time_playout = false;
        config.playback_max_frames = 0;
        config.jitter_buffer_frames = 512;
        config.jitter_buffer_max_frames = 1024;
        config.stats_warmup_us = 0;
        jam2::PeerStream stream(config, 0, &sink);

        std::uint64_t lastReceiveTime = 0;
        for (std::uint32_t sequence = 0; sequence < packetCount; ++sequence) {
            const long double remoteTimeUs =
                static_cast<long double>(sequence) * 64.0L * 1000000.0L / 48000.0L;
            lastReceiveTime = 1000ULL + static_cast<std::uint64_t>(
                std::llround(remoteTimeUs / clockScale));
            const jam2::protocol::Header header{
                jam2::protocol::PacketType::Audio,
                1,
                sequence,
                static_cast<std::uint64_t>(sequence) * 64ULL,
                static_cast<std::uint16_t>(payload.size()),
                0,
            };
            expect(stream.receiveAudio(header, payload, lastReceiveTime) ==
                    jam2::PeerAudioResult::Accepted,
                "drifting jitter stream accepts ordered audio");
            stream.advance(lastReceiveTime);
        }
        stream.advance(lastReceiveTime + 20000ULL);

        const auto& stats = stream.stats();
        expect(stats.jitter_buffer_forced_releases == 0 &&
                stats.jitter_capacity_drops == 0,
            "clock drift does not drive the jitter queue into forced release or capacity pressure");
        expect(stats.jitter_buffer_depth_max_frames <= 512 &&
                stats.jitter_pending_high_water <= 8,
            "drift-safe jitter scheduling stays within one configured target window");
        expect(stats.jitter_buffer_target_releases > packetCount - 16 &&
                stats.jitter_buffer_released_packets == packetCount,
            "drift-safe jitter scheduling releases the complete ordered stream");
    }
}

void testJitterBufferPreservesOrderAndRebasesDiscontinuities()
{
    auto payloadForMarker = [](std::uint16_t marker) {
        std::array<std::uint8_t, 128> payload{};
        for (std::size_t offset = 0; offset < payload.size(); offset += 2) {
            payload[offset] = static_cast<std::uint8_t>(marker & 0xffU);
            payload[offset + 1] = static_cast<std::uint8_t>(marker >> 8U);
        }
        return payload;
    };
    auto receive = [](jam2::PeerStream& stream,
                      std::uint32_t sequence,
                      std::uint64_t receiveTimeUs,
                      std::span<const std::uint8_t> payload) {
        const jam2::protocol::Header header{
            jam2::protocol::PacketType::Audio,
            1,
            sequence,
            static_cast<std::uint64_t>(sequence) * 64ULL,
            static_cast<std::uint16_t>(payload.size()),
            0,
        };
        return stream.receiveAudio(header, payload, receiveTimeUs);
    };

    jam2::PeerStreamConfig config = peerConfig();
    config.sample_time_playout = false;
    config.playback_max_frames = 0;
    config.jitter_buffer_frames = 256;
    config.jitter_buffer_max_frames = 512;
    config.stats_warmup_us = 0;

    CollectingSink orderedSink;
    jam2::PeerStream ordered(config, 0, &orderedSink);
    const auto first = payloadForMarker(1);
    const auto second = payloadForMarker(2);
    const auto third = payloadForMarker(3);
    const auto fourth = payloadForMarker(4);
    expect(receive(ordered, 0, 1000, first) == jam2::PeerAudioResult::Accepted &&
            receive(ordered, 2, 2000, third) == jam2::PeerAudioResult::Accepted &&
            receive(ordered, 1, 3000, second) == jam2::PeerAudioResult::Accepted &&
            receive(ordered, 3, 4000, fourth) == jam2::PeerAudioResult::Accepted,
        "jitter buffer accepts bounded packet reordering");
    ordered.advance(10000);

    const auto& orderedStats = ordered.stats();
    expect(orderedSink.samples.size() == 256 &&
            orderedSink.samples[0] == 65536 &&
            orderedSink.samples[64] == 131072 &&
            orderedSink.samples[128] == 196608 &&
            orderedSink.samples[192] == 262144,
        "jitter buffer releases reordered packets in sequence order");
    expect(orderedStats.jitter_buffer_target_releases == 1 &&
            orderedStats.jitter_buffer_timeout_releases == 3 &&
            orderedStats.jitter_buffer_queued_packets == 4 &&
            orderedStats.jitter_buffer_released_packets == 4 &&
            orderedStats.reordered_recovered == 1,
        "bounded packet handoff preserves reordered target-depth and residence-time releases");

    CollectingSink discontinuitySink;
    config.jitter_buffer_frames = 512;
    config.jitter_buffer_max_frames = 1024;
    jam2::PeerStream discontinuity(config, 0, &discontinuitySink);
    expect(receive(discontinuity, 0, 1000, first) == jam2::PeerAudioResult::Accepted &&
            receive(discontinuity, 1, 2000, second) == jam2::PeerAudioResult::Accepted,
        "jitter discontinuity fixture queues its original stream");
    expect(receive(discontinuity, 1000, 3000, third) ==
                jam2::PeerAudioResult::ForwardGapRejected &&
            receive(discontinuity, 1001, 4000, third) ==
                jam2::PeerAudioResult::ForwardGapRejected &&
            receive(discontinuity, 1002, 5000, fourth) ==
                jam2::PeerAudioResult::Accepted,
        "jitter buffer confirms a large forward discontinuity before rebasing");
    discontinuity.advance(20000);

    const auto& discontinuityStats = discontinuity.stats();
    expect(discontinuityStats.forward_gap_resyncs == 1 &&
            discontinuityStats.jitter_buffer_rebases == 1 &&
            discontinuityStats.jitter_buffer_dropped_packets == 2 &&
            discontinuityStats.jitter_buffer_dropped_frames == 128,
        "jitter rebase discards only packets from the obsolete sequence timeline");
    expect(discontinuitySink.samples.size() == 64 &&
            discontinuitySink.samples.front() == 262144,
        "jitter rebase releases only the confirmed replacement timeline");
}

void testUdpStunAndSessionBoundaries()
{
    jam2::NetworkRuntime networkRuntime;

    jam2::UdpSocket movedFrom;
    jam2::UdpSocket movedTo;
    movedTo = std::move(movedFrom);
    movedTo.bind({"127.0.0.1", 0});
    movedTo.set_send_buffer_size(65536);
    movedTo.set_recv_buffer_size(65536);
    expect(movedTo.send_buffer_size() > 0 && movedTo.recv_buffer_size() > 0,
        "UDP socket applies and reports positive buffer sizes");
    expectThrows([&] { movedTo.set_send_buffer_size(0); },
        "UDP socket rejects a zero send buffer");
    expectThrows([&] { movedTo.set_recv_buffer_size(-1); },
        "UDP socket rejects a negative receive buffer");
    const std::array<std::uint8_t, 1> invalidPacket{0};
    const auto invalidSend = movedFrom.send_to({}, invalidPacket);
    expect(invalidSend.outcome == jam2::UdpSendOutcome::Fatal,
        "moved-from UDP send reports a classified fatal socket error");

    jam2::RealtimeWakeSignal captureReadyWake;
    jam2::audio::MonoRingBuffer captureReadyRing(8);
    jam2::audio::StreamControl captureReadyControl;
    captureReadyControl.network_capture_wake_signal.store(
        &captureReadyWake, std::memory_order_relaxed);
    captureReadyControl.network_capture_wake_frames.store(
        4, std::memory_order_relaxed);
    captureReadyControl.network_capture_requested_enabled.store(
        true, std::memory_order_relaxed);
    captureReadyControl.network_capture_generation_requested.store(
        1, std::memory_order_relaxed);
    expect(jam2::audio::prepare_network_capture_callback(
               captureReadyControl, captureReadyRing, 0),
        "capture-ready fixture enables its callback attachment");
    const std::array<std::int32_t, 2> firstCaptureFrames{1, 2};
    const std::array<std::int32_t, 2> secondCaptureFrames{3, 4};
    const std::array<std::int32_t, 4> dormantCaptureFrames{1, 2, 3, 4};
    (void)jam2::audio::push_network_capture_callback(
        captureReadyControl, captureReadyRing, dormantCaptureFrames, 50);
    expect(captureReadyWake.signalCount() == 0,
        "complete capture packet stays dormant when the network worker has not requested a wake");
    (void)captureReadyRing.discard_all();
    captureReadyWake.requestWake();
    (void)jam2::audio::push_network_capture_callback(
        captureReadyControl, captureReadyRing, firstCaptureFrames, 100);
    expect(captureReadyWake.signalCount() == 0,
        "capture callback does not wake before a complete packet is ready");
    (void)jam2::audio::push_network_capture_callback(
        captureReadyControl, captureReadyRing, secondCaptureFrames, 200);
    const auto wakeWaitStarted = std::chrono::steady_clock::now();
    std::array<std::uint8_t, 8> wakeBuffer{};
    const auto wakeDatagram = movedTo.recv_from_for(
        wakeBuffer, 250000, &captureReadyWake);
    const auto wakeWaitElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - wakeWaitStarted);
    expect(!wakeDatagram && wakeWaitElapsed < std::chrono::milliseconds(50) &&
            captureReadyWake.signalCount() == 1 &&
            captureReadyWake.consumptionCount() == 1 &&
            captureReadyWake.lastSignalTimeUs() == 200,
        "complete capture packet wakes an otherwise idle UDP receive immediately");

    jam2::UdpSocket simultaneousWakeSender;
    simultaneousWakeSender.bind({"127.0.0.1", 0});
    const auto simultaneousWakeTarget =
        jam2::resolve_udp_endpoint(movedTo.local_endpoint());
    const std::array<std::uint8_t, 1> simultaneousWakePacket{9};
    expect(simultaneousWakeSender.send_to(
               simultaneousWakeTarget, simultaneousWakePacket).outcome ==
               jam2::UdpSendOutcome::Sent,
        "simultaneous UDP/capture-wake fixture queues its datagram");
    captureReadyWake.signal(300);
    const auto simultaneousWakeDatagram = movedTo.recv_from_for(
        wakeBuffer, 250000, &captureReadyWake);
    expect(simultaneousWakeDatagram && simultaneousWakeDatagram->size == 1 &&
            wakeBuffer[0] == 9 && captureReadyWake.signalCount() == 2 &&
            captureReadyWake.consumptionCount() == 2,
        "UDP readiness consumes a simultaneous capture notification without losing the datagram");
    captureReadyWake.cancelWakeRequest();

    jam2::UdpSocket duplicateBind;
    expectThrows([&] { duplicateBind.bind(movedTo.local_endpoint()); },
        "UDP bind collision reports its platform socket error");

    jam2::UdpSocket burstSender;
    burstSender.bind({"127.0.0.1", 0});
    const auto burstTarget = jam2::resolve_udp_endpoint(movedTo.local_endpoint());
    const std::array<std::uint8_t, 1> burstOne{1};
    const std::array<std::uint8_t, 1> burstTwo{2};
    const std::array<std::uint8_t, 1> burstThree{3};
    expect(burstSender.send_to(burstTarget, burstOne).outcome ==
                jam2::UdpSendOutcome::Sent &&
            burstSender.send_to(burstTarget, burstTwo).outcome ==
                jam2::UdpSendOutcome::Sent &&
            burstSender.send_to(burstTarget, burstThree).outcome ==
                jam2::UdpSendOutcome::Sent,
        "nonblocking UDP fixture queues a three-datagram receive burst");
    std::array<std::uint8_t, 8> burstBuffer{};
    const auto burstFirst = movedTo.recv_from_for(burstBuffer, 500000);
    const bool firstMatches = burstFirst && burstFirst->size == 1 &&
        burstBuffer[0] == 1;
    const auto burstSecond = movedTo.recv_from_for(burstBuffer, 0);
    const bool secondMatches = burstSecond && burstSecond->size == 1 &&
        burstBuffer[0] == 2;
    const auto burstThird = movedTo.recv_from_for(burstBuffer, 0);
    const bool thirdMatches = burstThird && burstThird->size == 1 &&
        burstBuffer[0] == 3;
    const auto burstEmpty = movedTo.recv_from_for(burstBuffer, 0);
    expect(firstMatches && secondMatches && thirdMatches && !burstEmpty,
        "zero-timeout UDP drains queued datagrams in order and stops at would-block");

    const auto request = jam2::stun::make_binding_request();
    expect(request.bytes.size() == 20,
        "STUN binding request uses the fixed RFC header shape");
    const auto directResponse = stunResponse(
        request.transaction_id, 0xcb007109U, 54321);
    const auto directEndpoint = jam2::stun::parse_binding_response(
        directResponse, request.transaction_id);
    expect(directEndpoint.host == "203.0.113.9" && directEndpoint.port == 54321,
        "STUN parser decodes XOR-mapped IPv4 and port");
    expectThrows([&] {
        std::array<std::uint8_t, 4> shortPacket{};
        (void)jam2::stun::parse_binding_response(shortPacket, request.transaction_id);
    }, "STUN parser rejects a short response");

    jam2::UdpSocket stunServer;
    stunServer.bind({"127.0.0.1", 0});
    std::exception_ptr serverFailure;
    std::thread serverThread([&] {
        try {
            std::array<std::uint8_t, 128> buffer{};
            const auto received = stunServer.recv_from(buffer, 1000);
            if (!received || received->size != 20) {
                throw std::runtime_error("fake STUN server did not receive a fixed request");
            }
            std::array<std::uint8_t, 12> transaction{};
            std::copy_n(buffer.begin() + 8, transaction.size(), transaction.begin());
            const auto response = stunResponse(transaction, 0xc6336407U, 45678);
            const auto sent = stunServer.send_to(received->endpoint, response);
            if (sent.outcome != jam2::UdpSendOutcome::Sent) {
                throw std::runtime_error("fake STUN server could not send its response");
            }
        } catch (...) {
            serverFailure = std::current_exception();
        }
    });
    jam2::UdpSocket stunClient;
    stunClient.bind({"127.0.0.1", 0});
    jam2::Endpoint discovered;
    try {
        discovered = jam2::stun::discover_public_endpoint(
            stunClient, stunServer.local_endpoint(), 500, 1);
    } catch (...) {
        serverThread.join();
        throw;
    }
    serverThread.join();
    if (serverFailure) std::rethrow_exception(serverFailure);
    expect(discovered.host == "198.51.100.7" && discovered.port == 45678,
        "STUN discovery accepts a matching local server response");
    expectThrows([&] {
        (void)jam2::stun::discover_public_endpoint(
            stunClient, stunServer.local_endpoint(), 0, 1);
    }, "STUN discovery rejects an invalid timeout");

    BufferSink outputSink;
    jam2::UdpSocket receiverOne;
    receiverOne.bind({"127.0.0.1", 0});
    jam2::UdpSocket sender;
    sender.bind({"127.0.0.1", 0});
    jam2::SessionInfo sessionInfo;
    sessionInfo.session_id = 0x1122334455667788ULL;
    sessionInfo.key.fill(0x5aU);
    jam2::NetworkSessionContract contract;
    contract.audio_format = jam2::NetworkAudioFormat::Pcm16Mono;
    contract.frames_per_packet = 64;
    const jam2::NetworkPeerDescriptor remote{
        jam2::PeerId{2},
        jam2::resolve_udp_endpoint(receiverOne.local_endpoint()),
        jam2::PeerEndpointState::Active,
    };
    jam2::NetworkSession networkSession(
        std::move(sender), sessionInfo, contract,
        jam2::SessionBootstrapRole::Creator, jam2::PeerId{1}, remote,
        peerConfig(), &outputSink);
    expect(networkSession.sessionId() == sessionInfo.session_id &&
            networkSession.activePeerCount() == 1 &&
            networkSession.remotePeer().peer_id == jam2::PeerId{2} &&
            networkSession.recognizesEndpoint(remote.endpoint),
        "network session exposes its fixed identity and active endpoint");
    expect(networkSession.setPeerGain({2}, 700000) &&
            networkSession.setPeerMuted({2}, true),
        "network session applies peer gain and mute through its mixer");
    auto snapshot = networkSession.snapshot();
    expect(snapshot.peers.size() == 1 && snapshot.peers[0].has_mix_stats &&
            snapshot.peers[0].mix.gain_ppm == 700000 &&
            snapshot.peers[0].mix.muted,
        "network session snapshot publishes exact peer mixer state");
    const auto accessById = networkSession.accessPeer(jam2::PeerId{2});
    const auto accessByEndpoint = networkSession.accessPeer(remote.endpoint);
    expect(accessById && accessByEndpoint &&
            accessById.descriptor == accessByEndpoint.descriptor &&
            accessById.stream == accessByEndpoint.stream &&
            accessById.descriptor->peer_id == jam2::PeerId{2},
        "network session resolves one typed peer view by identity or endpoint");

    const std::size_t packetSize = networkSession.send(
        jam2::protocol::PacketType::Ping, 1, 99, {});
    std::array<std::uint8_t, jam2::protocol::kMaxDatagramSize> packet{};
    const auto receivedOne = receiverOne.recv_from(packet, 500);
    expect(packetSize == jam2::protocol::kHeaderSize && receivedOne &&
            receivedOne->size == packetSize &&
            networkSession.parse(std::span<const std::uint8_t>(
                packet.data(), receivedOne ? receivedOne->size : 0)),
        "network session sends and authenticates a fixed-shape ping");
    const std::array<std::uint8_t, 128> audioPayload{};
    const std::size_t audioPacketSize = networkSession.send(
        jam2::protocol::PacketType::Audio, 2, 0, audioPayload);
    const auto receivedAudio = receiverOne.recv_from(packet, 500);
    const auto* categorizedSend = networkSession.peerSendStats({2});
    expect(receivedAudio && receivedAudio->size == audioPacketSize &&
            categorizedSend != nullptr && categorizedSend->sent_packets == 2 &&
            categorizedSend->audio_sent_packets == 1 &&
            categorizedSend->audio_sent_bytes == audioPacketSize,
        "network send ownership counts audio without per-packet runtime peer rescans");

    jam2::UdpSocket receiverTwo;
    receiverTwo.bind({"127.0.0.1", 0});
    const auto replacementEndpoint = jam2::resolve_udp_endpoint(
        receiverTwo.local_endpoint());
    expect(networkSession.updatePeerEndpoint(
               {2}, replacementEndpoint, jam2::PeerEndpointState::Probing) &&
            networkSession.activePeerCount() == 0 &&
            networkSession.recognizesEndpoint(replacementEndpoint) &&
            !networkSession.acceptsEndpoint(replacementEndpoint) &&
            networkSession.peerMixStats({2}) != nullptr &&
            networkSession.peerMixStats({2})->gain_ppm == 700000 &&
            networkSession.peerMixStats({2})->muted,
        "network endpoint replacement resets the stream without losing peer mix controls");
    expect(networkSession.setPeerEndpointState({2}, jam2::PeerEndpointState::Active) &&
            networkSession.activePeerCount() == 1,
        "network endpoint promotion restores active fan-out");
    const std::size_t replacementPacketSize = networkSession.send(
        jam2::protocol::PacketType::Ping, 2, 100, {});
    const auto receivedTwo = receiverTwo.recv_from(packet, 500);
    expect(replacementPacketSize == jam2::protocol::kHeaderSize && receivedTwo &&
            receivedTwo->size == replacementPacketSize &&
            !receiverOne.recv_from(packet, 0),
        "network endpoint replacement sends only to the promoted endpoint");

    jam2::UdpSocket receiverThree;
    receiverThree.bind({"127.0.0.1", 0});
    const auto reboundEndpoint = jam2::resolve_udp_endpoint(
        receiverThree.local_endpoint());
    const auto* streamBeforeRebind = &networkSession.peerStream({2});
    expect(networkSession.rebindPeerEndpoint(
               {2}, reboundEndpoint, jam2::PeerEndpointState::Probing) &&
            &networkSession.peerStream({2}) == streamBeforeRebind &&
            networkSession.recognizesEndpoint(reboundEndpoint) &&
            !networkSession.recognizesEndpoint(replacementEndpoint) &&
            networkSession.peerMixStats({2}) != nullptr &&
            networkSession.peerMixStats({2})->gain_ppm == 700000 &&
            networkSession.peerMixStats({2})->muted,
        "authenticated endpoint rebinding preserves the peer stream and mix controls");
    expect(networkSession.setPeerEndpointState({2}, jam2::PeerEndpointState::Active),
        "authenticated endpoint rebinding can promote the preserved stream");
    const std::size_t reboundPacketSize = networkSession.send(
        jam2::protocol::PacketType::Ping, 3, 101, {});
    const auto receivedThree = receiverThree.recv_from(packet, 500);
    expect(reboundPacketSize == jam2::protocol::kHeaderSize && receivedThree &&
            receivedThree->size == reboundPacketSize &&
            !receiverTwo.recv_from(packet, 0),
        "authenticated endpoint rebinding sends only to the newly observed endpoint");

    jam2::UdpSocket placeholderSocket;
    placeholderSocket.bind({"127.0.0.1", 0});
    const jam2::NetworkPeerDescriptor placeholderPeer{
        jam2::PeerId{10}, remote.endpoint, jam2::PeerEndpointState::Active};
    jam2::NetworkSession owner(
        std::move(placeholderSocket), sessionInfo, contract,
        jam2::SessionBootstrapRole::Joiner, jam2::PeerId{9}, placeholderPeer,
        peerConfig(), &outputSink);
    owner = std::move(networkSession);
    expect(owner.sessionId() == sessionInfo.session_id && owner.peerCount() == 1,
        "network-session move assignment preserves the owned session");
    owner.close();
    expect(owner.bootstrapState() == jam2::SessionBootstrapState::Closed &&
            !owner.recognizesEndpoint(reboundEndpoint),
        "network session close rejects every former endpoint");
    expectThrows([&] {
        (void)owner.send(jam2::protocol::PacketType::Ping, 2, 100, {});
    }, "closed network session rejects sends");
}

void testSmallDiagnosticBoundaries()
{
    jam2::SessionAuthority authority(7, 7, 9);
    expect(authority.localPeerId() == 7,
        "session authority exposes its local peer identity");
    expect(!jam2::tuning_profile_names().empty(),
        "tuning profile diagnostics enumerate maintained profiles");
    const jam2::JoinProfile* fast = jam2::find_join_profile("fast");
    const jam2::CreateProfile* fast_create = jam2::find_create_profile("fast");
    expect(fast != nullptr && fast_create != nullptr && fast_create->local == fast &&
            fast_create->sample_rate == 48000 && fast_create->frame_size == 64 &&
            fast->audio_buffer_size == 32 && fast->playback_prefill_frames == 64 &&
            fast->playout_delay_frames == 64 && fast->jitter_buffer_frames == 64 &&
            fast->jitter_buffer_max_frames == 512 &&
            fast->adaptive_playback_target_frames == 64 &&
            fast->adaptive_playback_min_frames == 64 &&
            fast->adaptive_playback_max_frames == 512,
        "fast create and join profiles preserve the measured 64-frame floor and 512-frame recovery bounds");
    const jam2::JoinProfile* moderate = jam2::find_join_profile("moderate");
    const jam2::CreateProfile* moderate_create = jam2::find_create_profile("moderate");
    expect(moderate != nullptr && moderate_create != nullptr &&
            moderate_create->local == moderate &&
            moderate_create->sample_rate == 48000 && moderate_create->frame_size == 128 &&
            moderate->audio_buffer_size == 64 &&
            moderate->playback_prefill_frames == 256 &&
            moderate->playout_delay_frames == 512 &&
            moderate->jitter_buffer_frames == 1536 &&
            moderate->jitter_buffer_max_frames == 3072 &&
            moderate->adaptive_playback_target_frames == 512 &&
            moderate->adaptive_playback_min_frames == 512 &&
            moderate->adaptive_playback_max_frames == 4096,
        "moderate create and join profiles preserve the measured 256-frame prefill and 1536-frame jitter target");
}

void testQ31NetworkAudioConversion()
{
    constexpr auto minimum = (std::numeric_limits<std::int32_t>::min)();
    constexpr auto maximum = (std::numeric_limits<std::int32_t>::max)();
    const std::array<std::int32_t, 9> q31{
        minimum, -65537, -65536, -1, 0, 1, 65535, 65536, maximum,
    };
    std::array<std::int32_t, q31.size()> signed24{};
    for (std::size_t i = 0; i < q31.size(); ++i) {
        signed24[i] = q31[i] / 256;
    }
    for (const auto format : {
             jam2::NetworkAudioFormat::Pcm16Mono,
             jam2::NetworkAudioFormat::Pcm24Mono}) {
        const std::size_t bytes = jam2::protocol::audio_payload_size(format, q31.size());
        std::vector<std::uint8_t> legacy(bytes);
        std::vector<std::uint8_t> direct(bytes);
        expect(jam2::protocol::pack_audio_into(format, signed24, legacy) &&
                jam2::protocol::pack_audio_q31_into(format, q31, direct) &&
                direct == legacy,
            "direct Q31 network packing preserves the exact PCM wire bytes");

        std::array<std::int32_t, q31.size()> legacyDecoded{};
        std::array<std::int32_t, q31.size()> directDecoded{};
        expect(jam2::protocol::unpack_audio_into(format, legacy, legacyDecoded) &&
                jam2::protocol::unpack_audio_q31_into(format, legacy, directDecoded),
            "direct Q31 network unpacking accepts the maintained PCM formats");
        for (std::int32_t& sample : legacyDecoded) {
            sample *= 256;
        }
        expect(directDecoded == legacyDecoded,
            "direct Q31 network unpacking preserves the exact mixer samples");
    }

    std::array<std::uint8_t, 1> invalidBytes{};
    std::array<std::int32_t, 1> invalidOutput{};
    expect(!jam2::protocol::pack_audio_q31_into(
               jam2::NetworkAudioFormat::Pcm24Mono, q31, invalidBytes) &&
            !jam2::protocol::unpack_audio_q31_into(
                jam2::NetworkAudioFormat::Pcm24Mono, invalidBytes, invalidOutput),
        "direct Q31 conversion rejects mismatched fixed-shape buffers");
}

} // namespace

int main()
{
    try {
        testRingMidiAndDownmixBoundaries();
        testPreparedAndRecorderBoundaries();
        testProtocolPeerAndMixerBoundaries();
        testPeerStreamDriftCalibrationRejectsDequeuedBurst();
        testJitterBufferTracksOccupancyAcrossClockDrift();
        testJitterBufferPreservesOrderAndRebasesDiscontinuities();
        testUdpStunAndSessionBoundaries();
        testQ31NetworkAudioConversion();
        testSmallDiagnosticBoundaries();
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "FAIL: unexpected core-boundary exception: "
                  << exception.what() << '\n';
    }

    if (failures != 0) {
        std::cerr << failures << " core-boundary checks failed\n";
        return 1;
    }
    std::cout << "core boundary checks passed\n";
    return 0;
}

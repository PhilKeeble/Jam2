#include "duplex_processing.hpp"
#include "peer_mixer.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>

namespace {
void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

class DeviceSink final : public jam2::PeerStreamPlayback {
public:
    jam2::audio::MonoRingBuffer capture{8192}, pitch{8192}, ring{8192};
    jam2::audio::StreamControl control;
    jam2::audio::device_processing::DuplexProcessingState processing;
    std::size_t period;
    bool consumeRecovery = false;
    bool boundedRecovery;

    explicit DeviceSink(std::size_t frames, bool bounded) : period(frames), boundedRecovery(bounded)
    {
        ring.set_diagnostics_enabled(true);
        control.network_playback_enabled.store(true);
        processing.capture = &capture;
        processing.pitch = &pitch;
        processing.playback = &ring;
        processing.control = &control;
        jam2::audio::device_processing::prepare_duplex_processing(processing, 128, 0, 48000);
    }
    std::size_t depthFrames() const noexcept override { return ring.available_read(); }
    std::size_t callbackFrames() const noexcept override { return period; }
    std::uint64_t underrunFrames() const noexcept override { return ring.underrun_frames(); }
    std::size_t pushFrames(std::span<const std::int32_t> frames) noexcept override
    {
        const auto pushed = ring.push(frames);
        // Deterministic callback/producer interleaving: a device may consume
        // between successive recovery writes while the network thread refills.
        if (consumeRecovery) render(pushed);
        return pushed;
    }
    void requestDropFrames(std::size_t frames) noexcept override { ring.request_drop_oldest(frames); }
    void setResamplerRatio(double ratio) noexcept override
    { control.playback_ratio_ppm.store(static_cast<int>(ratio * 1000000)); }
    jam2::audio::PlaybackRecoveryState playbackRecoveryState() const noexcept override
    {
        auto state = jam2::audio::device_processing::read_playback_recovery_state(control);
        state.padding_policy = boundedRecovery ? jam2::audio::RecoveryPaddingPolicy::MeasuredShortage
            : jam2::audio::RecoveryPaddingPolicy::LearnedCushion;
        return state;
    }

    void render(std::size_t frames)
    {
        // The actual WASAPI backend consumes a native period in <=128-frame
        // duplex blocks. ASIO-sized periods exercise the same mixer boundary.
        while (frames != 0) {
            const auto count = std::min<std::size_t>(128, frames);
            jam2::audio::device_processing::process_duplex_block(processing, {}, count, true, {});
            frames -= count;
        }
    }
};

struct Fixture {
    DeviceSink sink;
    jam2::PeerMixer mixer;
    std::array<jam2::PeerStreamPlayback*, 3> peers{};
    std::array<std::int32_t, 64> pcm{};

    static jam2::PeerMixerConfig config()
    {
        jam2::PeerMixerConfig value;
        value.frames_per_block = 64;
        value.deadline_frames = 1024;
        value.output_max_frames = 1024;
        value.adaptive_playback_cushion = true;
        value.adaptive_min_frames = 64;
        value.adaptive_target_frames = value.adaptive_max_frames = 512;
        value.adaptive_release_ppm = 0;
        return value;
    }
    explicit Fixture(std::size_t period, bool bounded = true) : sink(period, bounded), mixer(config(), &sink)
    {
        pcm.fill(1000000);
        // Exactly four participants: this device and three real mixer sources.
        for (std::size_t i = 0; i < peers.size(); ++i) {
            peers[i] = mixer.addPeer(i + 1, 4096);
            require(peers[i] && mixer.setPeerActive(i + 1, true), "activate three remote contributors");
            peers[i]->pushFrames(pcm);
        }
        mixer.advance(1000000);
        mixer.finishReceiveBatch(false);
        require(sink.depthFrames() == 512 && mixer.stats().adaptive_padding_frames == 448,
            "initial prefill retains the configured target independently of later recovery");
    }
    void shortage(std::size_t missing)
    {
        const auto available = sink.depthFrames();
        require(available >= sink.period - missing, "fixture has a complete native render runway");
        sink.render(available - (sink.period - missing));
        sink.render(sink.period);
        require(sink.underrunFrames() == missing && sink.depthFrames() == 0,
            "production duplex consumption produces exactly the requested shortage");
        mixer.advance(1000100);
    }
};

void measuredRecovery(std::size_t period, std::size_t missing, bool interleave = false, bool bounded = true)
{
    Fixture f(period, bounded);
    const auto before = f.mixer.stats().adaptive_padding_frames;
    f.shortage(missing);
    f.sink.consumeRecovery = interleave;
    f.mixer.finishReceiveBatch(false);
    const auto padding = f.mixer.stats().adaptive_padding_frames - before;
    std::cout << "METRIC recovery_padding period=" << period << " missing=" << missing
              << " interleave=" << interleave << " padding=" << padding << '\n';
    require(padding == (bounded ? std::min<std::size_t>(448, missing) : 448),
        "WASAPI recovery is measured while the default ASIO recovery retains its existing cushion");
    require(f.mixer.stats().adaptive_target_frames == 512 &&
            f.mixer.stats().live_tail_trimmed_frames == 0 && f.mixer.stats().missing_peer_frames == 0,
        "measured recovery preserves the real-audio target without trimming or inventing missing peers");
    f.mixer.finishReceiveBatch(false);
    require(f.mixer.stats().adaptive_padding_frames == before + padding,
        "finishing the same batch twice cannot duplicate recovery padding");
}

void delayedRealAudioAndCancellation()
{
    for (bool removePeers : {false, true}) {
        Fixture f(480);
        const auto before = f.mixer.stats().adaptive_padding_frames;
        f.shortage(2);
        f.mixer.finishReceiveBatch(true);
        require(f.mixer.stats().adaptive_padding_frames == before,
            "an exhausted receive batch must defer silence while real audio is still pending");
        for (std::size_t i = 0; i < f.peers.size(); ++i) {
            if (removePeers) require(f.mixer.removePeer(i + 1), "remove all departing contributors");
            else require(f.peers[i]->pushFrames(f.pcm) == 64, "accept returning real peer audio");
        }
        f.mixer.advance(1000200);
        f.mixer.finishReceiveBatch(false);
        require(f.mixer.stats().adaptive_padding_frames == before,
            "real audio or the last peer leaving must cancel deferred recovery silence");
        require(f.sink.depthFrames() == (removePeers ? 0U : 64U),
            "returning real audio is mixed once; departing peers leave no orphan recovery output");
    }
}
} // namespace

int main()
{
    try {
        for (auto period : {32U, 128U, 336U, 480U})
            for (auto missing : {1U, 2U, 16U}) measuredRecovery(period, missing);
        measuredRecovery(480, 480);
        measuredRecovery(336, 2, true);
        measuredRecovery(480, 480, true);
        measuredRecovery(32, 1, false, false);
        measuredRecovery(128, 2, false, false);
        delayedRealAudioAndCancellation();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}

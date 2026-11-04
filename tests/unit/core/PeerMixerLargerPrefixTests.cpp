#include "peer_mixer.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <span>

namespace {
struct Sink final : jam2::PeerStreamPlayback {
    std::array<std::int32_t, 1024> frames{};
    std::size_t depth = 0, drops = 0;
    std::size_t depthFrames() const noexcept override { return depth; }
    std::size_t callbackFrames() const noexcept override { return 64; }
    std::size_t pushFrames(std::span<const std::int32_t> input) noexcept override {
        const auto count = std::min(input.size(), frames.size() - depth);
        std::copy_n(input.begin(), count, frames.begin() + depth);
        depth += count;
        return count;
    }
    void requestDropFrames(std::size_t count) noexcept override { drops += count; }
    void setResamplerRatio(double) noexcept override {}
};
int failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

void prefix(std::size_t count, std::size_t preceding) {
    Sink sink;
    jam2::PeerMixerConfig config;
    config.frames_per_block = 128;
    config.output_max_frames = config.adaptive_max_frames = 512;
    config.adaptive_playback_cushion = true;
    config.adaptive_target_frames = config.adaptive_min_frames = 512;
    jam2::PeerMixer mixer(config, &sink);
    std::array<std::int32_t, 129> audio{};
    std::array<jam2::PeerStreamPlayback*, 3> peers{};
    // Exactly four participants: the local output and three contributing peers.
    for (std::uint64_t id = 1; id <= 3; ++id) {
        auto* peer = mixer.addPeer(id, 1024);
        expect(peer && mixer.setPeerActive(id, true), "activate three remote peers");
        if (!peer) return;
        peers[id - 1] = peer;
        audio.fill(static_cast<std::int32_t>(id));
        expect(peer->pushFrames(std::span(audio).first(count)) == count, "accept real source prefix");
    }
    sink.depth = preceding;
    std::fill_n(sink.frames.begin(), preceding, 9);
    mixer.advance(0);
    const auto available = std::min(count, 512 - preceding);
    std::cout << "METRIC larger_prefix source_frames=" << count << " preceding=" << preceding
              << " output=" << sink.depth << " partial=" << mixer.stats().partial_release_frames << '\n';
    expect(sink.depth == preceding + available, "larger minimum releases the real common prefix within its output cap");
    expect(std::all_of(sink.frames.begin(), sink.frames.begin() + preceding,
        [](auto value) { return value == 9; }), "preserve preceding real output");
    expect(std::all_of(sink.frames.begin() + preceding, sink.frames.begin() + sink.depth,
        [](auto value) { return value == 6; }), "mix each real source exactly once in order");
    expect(sink.depth <= 512 && sink.drops == 0 && mixer.stats().adaptive_padding_frames == 0 &&
        mixer.stats().missing_peer_frames == 0 && mixer.stats().receive_recovery_debt_frames == 0,
        "prefix transfer creates no padding, drops, missing-source audio or recovery debt");
    if (count == 127 && preceding == 384) {
        for (std::size_t i = 0; i < peers.size(); ++i) {
            const std::int32_t last = static_cast<std::int32_t>(i + 1);
            expect(peers[i]->pushFrames(std::span(&last, 1)) == 1, "accept later final source frame");
        }
        mixer.advance(1);
        expect(sink.depth == 512 && sink.frames[511] == 6 &&
            mixer.stats().late_after_release_frames == 0 &&
            mixer.stats().missing_peer_frames == 0,
            "the later 128th frame completes the same logical block without loss or padding");
    }
}

void establishedContributorMustHaveAudio()
{
    Sink sink;
    jam2::PeerMixerConfig config;
    config.frames_per_block = 128;
    config.output_max_frames = config.adaptive_max_frames = 512;
    config.adaptive_playback_cushion = true;
    config.adaptive_target_frames = config.adaptive_min_frames = 512;
    jam2::PeerMixer mixer(config, &sink);
    std::array<jam2::PeerStreamPlayback*, 3> peers{};
    std::array<std::int32_t, 128> audio{};
    for (std::size_t i = 0; i < peers.size(); ++i) {
        peers[i] = mixer.addPeer(i + 1, 1024);
        expect(peers[i] && mixer.setPeerActive(i + 1, true), "activate three established contributors");
        if (!peers[i]) return;
        audio.fill(static_cast<std::int32_t>(i + 1));
        peers[i]->pushFrames(audio);
    }
    mixer.advance(0);
    sink.depth = 384;
    for (std::size_t i = 0; i < 2; ++i) {
        audio.fill(static_cast<std::int32_t>(i + 1));
        peers[i]->pushFrames(std::span(audio).first(127));
    }
    mixer.advance(1);
    expect(sink.depth == 384 && mixer.stats().partial_release_frames == 0 &&
            mixer.stats().missing_peer_frames == 0,
        "an empty established contributor prevents early partial release at the larger minimum");
    audio.fill(3);
    peers[2]->pushFrames(std::span(audio).first(127));
    mixer.advance(2);
    expect(sink.depth == 511 && mixer.stats().partial_release_frames == 127 &&
            sink.drops == 0 && mixer.stats().adaptive_padding_frames == 384 &&
            std::all_of(sink.frames.begin() + 384, sink.frames.begin() + 511,
                [](auto value) { return value == 6; }),
        "the common real prefix releases when the final contributor catches up without further padding");
}
}

int main() {
    // Existing runway satisfies initial prefill, isolating prefix release.
    for (auto count : {127U, 128U, 129U}) for (auto preceding : {384U, 450U, 512U})
        prefix(count, preceding);
    establishedContributorMustHaveAudio();
    return failures ? 1 : 0;
}

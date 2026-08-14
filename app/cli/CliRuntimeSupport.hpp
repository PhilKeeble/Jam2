#pragma once

#include "engine.hpp"
#include "peer_stream.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>

namespace jam2::cli {

inline std::string os_error_text(unsigned long code)
{
    if (code == 0) {
        return {};
    }
    return "error " + std::to_string(code);
}

// Adapts the CLI network worker's non-owning Engine lifetime to the peer
// stream playback contract. The network worker detaches this sink before its
// engine is stopped, so late flushes remain bounded no-ops.
class CliPeerStreamPlayback final : public jam2::PeerStreamPlayback {
public:
    explicit CliPeerStreamPlayback(jam2::Engine* engine) noexcept : engine_(engine) {}

    bool acceptsFrames() const noexcept override { return engine_ != nullptr; }

    std::size_t depthFrames() const noexcept override
    {
        return engine_ != nullptr
            ? engine_->networkPlaybackDepth()
            : (std::numeric_limits<std::size_t>::max)() / 2U;
    }

    std::size_t pushFrames(std::span<const std::int32_t> frames) noexcept override
    {
        return engine_ != nullptr ? engine_->pushNetworkPlayback(frames) : 0;
    }

    void requestDropFrames(std::size_t frames) noexcept override
    {
        if (engine_ != nullptr) engine_->requestNetworkPlaybackDrop(frames);
    }

    void setResamplerRatio(double ratio) noexcept override
    {
        if (engine_ != nullptr) engine_->setNetworkPlaybackRatio(ratio);
    }

    void detach() noexcept { engine_ = nullptr; }

private:
    jam2::Engine* engine_ = nullptr;
};

} // namespace jam2::cli

#pragma once

#include "audio_ring.hpp"
#include "engine.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace jam2 {

class PitchAnalyzer final {
public:
    PitchAnalyzer(
        audio::MonoRingBuffer& input,
        int sampleRate,
        const std::atomic<std::uint64_t>& engineFrame);
    ~PitchAnalyzer();

    PitchAnalyzer(const PitchAnalyzer&) = delete;
    PitchAnalyzer& operator=(const PitchAnalyzer&) = delete;

    bool start(std::string& error) noexcept;
    void stop() noexcept;
    EnginePitchSnapshot snapshot() const noexcept;

private:
    class Detector;

    void run() noexcept;
    void publishInvalidIfStale(std::uint64_t nowUs) noexcept;

    audio::MonoRingBuffer& input_;
    int sampleRate_ = 48000;
    const std::atomic<std::uint64_t>& engineFrame_;
    std::unique_ptr<Detector> detector_;
    std::thread worker_;
    std::atomic<bool> stopRequested_{false};
    mutable std::mutex snapshotMutex_;
    EnginePitchSnapshot snapshot_;
};

} // namespace jam2

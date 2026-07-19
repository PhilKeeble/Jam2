#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace jam2::audio {

// Control-thread loaded, callback-thread consumed mono PCM source.  The
// fixed slots and POD command queue keep loading and file work out of audio
// callbacks.
class PreparedTrackSource {
public:
    static constexpr std::size_t kSlots = 4;
    static constexpr std::size_t kCommandCapacity = 64;
    enum class SlotState : std::uint8_t { Free, Loading, Ready, Active, Retired, Reclaiming };
    enum class CommandType : std::uint8_t { Swap, Play, Stop, Seek, NudgeSource, SetLoop, SetLevel };
    struct Command { CommandType type{}; std::uint32_t slot = 0; std::uint64_t targetFrame = 0; std::uint64_t sourceFrame = 0; std::uint64_t loopEndFrame = 0; std::int32_t levelPpm = 1000000; std::uint64_t generation = 0; };
    explicit PreparedTrackSource(std::size_t maxFrames);
    int claimLoadingSlot();
    bool publishReady(int slot, std::uint64_t frames, int sampleRate);
    void abandonLoadingSlot(int slot) noexcept;
    void abandonReadySlot(int slot) noexcept;
    std::int16_t* loadingData(int slot);
    bool enqueue(const Command& command);
    bool enqueueBatch(std::span<const Command> commands);
    void cancelScheduled() noexcept;
    int mix(std::int32_t* output, std::size_t frames, std::uint64_t callbackFrame) noexcept;
    std::uint64_t sourceFrame() const noexcept { return sourceFrame_.load(std::memory_order_relaxed); }
    std::uint64_t underruns() const noexcept { return underruns_.load(std::memory_order_relaxed); }
    std::uint64_t scheduledStartFrame() const noexcept { return scheduledStartFrame_.load(std::memory_order_relaxed); }
    std::uint64_t actualStartFrame() const noexcept { return actualStartFrame_.load(std::memory_order_relaxed); }
    bool playing() const noexcept { return playingAtomic_.load(std::memory_order_relaxed); }
private:
    struct Slot { std::vector<std::int16_t> samples; std::atomic<SlotState> state{SlotState::Free}; std::uint64_t frames = 0; int sampleRate = 0; };
    std::array<Slot, kSlots> slots_;
    std::array<Command, kCommandCapacity> queue_{};
    std::mutex producerMutex_;
    std::atomic<std::uint32_t> write_{0}, read_{0};
    std::atomic<std::uint64_t> generation_{1};
    int active_ = -1; bool playing_ = false; std::uint64_t sourcePos_ = 0, loopStart_ = 0, loopEnd_ = 0; std::int32_t levelPpm_ = 1000000;
    std::atomic<std::uint64_t> sourceFrame_{0}, underruns_{0}, scheduledStartFrame_{0}, actualStartFrame_{0};
    std::atomic<bool> playingAtomic_{false};
};

} // namespace jam2::audio

#include "prepared_track_source.hpp"
#include <algorithm>
#include <limits>
namespace jam2::audio {
PreparedTrackSource::PreparedTrackSource(std::size_t maxFrames) { for (auto& s : slots_) s.samples.resize(maxFrames); }
int PreparedTrackSource::claimLoadingSlot() { for (int i=0;i<int(kSlots);++i) { for (SlotState expected : {SlotState::Free, SlotState::Retired}) { if (slots_[i].state.compare_exchange_strong(expected,SlotState::Loading)) return i; } } return -1; }
std::int16_t* PreparedTrackSource::loadingData(int slot) { return slot>=0&&slot<int(kSlots)&&slots_[slot].state.load()==SlotState::Loading ? slots_[slot].samples.data() : nullptr; }
bool PreparedTrackSource::publishReady(int slot,std::uint64_t frames,int rate) { if(slot<0||slot>=int(kSlots)||frames>slots_[slot].samples.size()||rate<=0) return false; auto& s=slots_[slot]; if(s.state.load()!=SlotState::Loading) return false; s.frames=frames;s.sampleRate=rate;s.state.store(SlotState::Ready,std::memory_order_release);return true; }
bool PreparedTrackSource::enqueue(const Command& command)
{
    std::lock_guard<std::mutex> lock(producerMutex_);
    const std::uint32_t write = write_.load(std::memory_order_relaxed);
    const std::uint32_t next = (write + 1U) % static_cast<std::uint32_t>(kCommandCapacity);
    if (next == read_.load(std::memory_order_acquire)) {
        return false;
    }
    queue_[write] = command;
    write_.store(next, std::memory_order_release);
    return true;
}
void PreparedTrackSource::mix(
    std::int32_t* output,
    std::size_t frames,
    std::uint64_t callbackFrame) noexcept
{
    std::size_t cursor = 0;
    while (cursor < frames) {
        const std::uint64_t currentFrame = callbackFrame + static_cast<std::uint64_t>(cursor);
        for (;;) {
            const std::uint32_t read = read_.load(std::memory_order_relaxed);
            if (read == write_.load(std::memory_order_acquire)) {
                break;
            }
            const Command command = queue_[read];
            if (command.targetFrame > currentFrame) {
                break;
            }
            read_.store(
                (read + 1U) % static_cast<std::uint32_t>(kCommandCapacity),
                std::memory_order_release);
            if (command.type == CommandType::Swap &&
                command.slot < kSlots &&
                slots_[command.slot].state.load(std::memory_order_acquire) == SlotState::Ready) {
                scheduledStartFrame_.store(command.targetFrame, std::memory_order_relaxed);
                if (active_ >= 0) {
                    slots_[active_].state.store(SlotState::Retired, std::memory_order_release);
                }
                active_ = static_cast<int>(command.slot);
                slots_[active_].state.store(SlotState::Active, std::memory_order_release);
                sourcePos_ = command.sourceFrame;
                actualStartFrame_.store(currentFrame, std::memory_order_relaxed);
            } else if (command.type == CommandType::Play) {
                scheduledStartFrame_.store(command.targetFrame, std::memory_order_relaxed);
                playing_ = true;
                actualStartFrame_.store(currentFrame, std::memory_order_relaxed);
            } else if (command.type == CommandType::Stop) {
                playing_ = false;
            } else if (command.type == CommandType::Seek) {
                sourcePos_ = command.sourceFrame;
            } else if (command.type == CommandType::NudgeSource && active_ >= 0) {
                const std::int64_t delta = command.levelPpm;
                const std::uint64_t slotFrames = slots_[active_].frames;
                if (loopEnd_ > loopStart_) {
                    const std::int64_t loopFrames = static_cast<std::int64_t>(loopEnd_ - loopStart_);
                    std::int64_t relative = sourcePos_ >= loopStart_
                        ? static_cast<std::int64_t>(sourcePos_ - loopStart_)
                        : -static_cast<std::int64_t>(loopStart_ - sourcePos_);
                    relative += delta;
                    relative %= loopFrames;
                    if (relative < 0) {
                        relative += loopFrames;
                    }
                    sourcePos_ = loopStart_ + static_cast<std::uint64_t>(relative);
                } else if (delta < 0) {
                    const std::uint64_t magnitude = static_cast<std::uint64_t>(-delta);
                    sourcePos_ = sourcePos_ > magnitude ? sourcePos_ - magnitude : 0ULL;
                } else {
                    sourcePos_ = std::min(slotFrames, sourcePos_ + static_cast<std::uint64_t>(delta));
                }
            } else if (command.type == CommandType::SetLoop) {
                loopStart_ = command.sourceFrame;
                loopEnd_ = command.loopEndFrame;
            } else if (command.type == CommandType::SetLevel) {
                levelPpm_ = command.levelPpm;
            }
        }

        std::size_t segmentEnd = frames;
        const std::uint32_t read = read_.load(std::memory_order_relaxed);
        if (read != write_.load(std::memory_order_acquire)) {
            const std::uint64_t target = queue_[read].targetFrame;
            if (target > currentFrame && target < callbackFrame + static_cast<std::uint64_t>(frames)) {
                segmentEnd = static_cast<std::size_t>(target - callbackFrame);
            }
        }

        if (playing_ && active_ >= 0) {
            Slot& slot = slots_[active_];
            for (std::size_t index = cursor; index < segmentEnd; ++index) {
                if (loopEnd_ > loopStart_ && sourcePos_ >= loopEnd_) {
                    sourcePos_ = loopStart_;
                }
                if (sourcePos_ >= slot.frames) {
                    if (loopEnd_ > loopStart_) {
                        sourcePos_ = loopStart_;
                    } else {
                        playing_ = false;
                        break;
                    }
                }
                const std::int64_t sample = static_cast<std::int64_t>(slot.samples[sourcePos_++]) << 16;
                const std::int64_t mixed = std::clamp<std::int64_t>(
                    static_cast<std::int64_t>(output[index]) + sample * levelPpm_ / 1000000,
                    (std::numeric_limits<std::int32_t>::min)(),
                    (std::numeric_limits<std::int32_t>::max)());
                output[index] = static_cast<std::int32_t>(mixed);
            }
        }
        cursor = segmentEnd;
    }
    playingAtomic_.store(playing_, std::memory_order_relaxed);
    sourceFrame_.store(sourcePos_, std::memory_order_relaxed);
}
}

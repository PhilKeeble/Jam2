#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace jam2::midi {

enum class InputMode : std::uint8_t {
    Standard,
    Mpe,
};

struct MpeZone {
    std::uint8_t master_channel = 0;
    std::uint8_t member_begin_channel = 1;
    std::uint8_t member_end_channel = 15;
    std::uint8_t pitch_bend_range_semitones = 48;
};

bool valid_mpe_zone(const MpeZone& zone) noexcept;

// A complete MIDI 1.0 channel-voice message. MPE uses the same messages and
// preserves their channels; it is a source interpretation mode, not another
// device protocol. System exclusive data is deliberately outside the initial
// live-performance contract.
struct Event {
    std::uint64_t monotonic_us = 0;
    std::uint16_t sample_offset = 0;
    std::uint8_t status = 0;
    std::uint8_t data1 = 0;
    std::uint8_t data2 = 0;
    std::uint8_t size = 0;
};

inline constexpr std::size_t kEventQueueCapacity = 2048;
inline constexpr std::size_t kMaximumEventsPerBlock = 512;

class EventQueue final {
public:
    bool push(const Event& event) noexcept;
    bool pop(Event& event) noexcept;
    void clear() noexcept;

    std::size_t depth() const noexcept;
    std::size_t high_water() const noexcept;
    std::uint64_t dropped() const noexcept;

private:
    std::array<Event, kEventQueueCapacity> events_{};
    std::atomic<std::uint32_t> write_{0};
    std::atomic<std::uint32_t> read_{0};
    std::atomic<std::uint32_t> high_water_{0};
    std::atomic<std::uint64_t> dropped_{0};
};

struct BlockResult {
    std::size_t count = 0;
    std::size_t late = 0;
    std::size_t deferred = 0;
};

// Drains timestamped device events into one bounded audio block. Events newer
// than the block end remain pending; events older than the start are delivered
// at offset zero so note-off and reset messages are not silently lost.
class BlockBuilder final {
public:
    BlockResult build(
        EventQueue& queue,
        std::uint64_t block_start_us,
        std::uint64_t block_end_us,
        std::uint32_t block_frames,
        std::span<Event> output) noexcept;
    void reset() noexcept { pending_valid_ = false; }

private:
    Event pending_{};
    bool pending_valid_ = false;
};

struct DeviceInfo {
    std::string id;
    std::string name;
};

class InputDevice {
public:
    virtual ~InputDevice() = default;
    virtual const DeviceInfo& info() const noexcept = 0;
    virtual std::uint64_t short_messages() const noexcept = 0;
    virtual std::uint64_t unsupported_messages() const noexcept = 0;
};

std::vector<DeviceInfo> enumerate_input_devices();
std::unique_ptr<InputDevice> open_input_device(
    const std::string& id,
    EventQueue& destination,
    std::string& error) noexcept;

} // namespace jam2::midi

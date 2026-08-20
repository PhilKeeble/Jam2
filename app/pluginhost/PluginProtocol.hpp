#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace jam2::pluginhost {

inline constexpr std::uint32_t kProtocolMagic = 0x4a325633U; // J2V3
inline constexpr std::uint32_t kProtocolVersion = 4;
inline constexpr std::size_t kMaximumFrames = 2048;
inline constexpr std::size_t kMaximumMidiEvents = 512;
inline constexpr std::size_t kTransportSlots = 4;
inline constexpr std::size_t kIsolationPipelineBlocks = 2;
inline constexpr std::size_t kMaximumPluginLatencyFrames = 96000;

enum class WorkerState : std::uint32_t {
    Starting,
    Ready,
    Failed,
    Stopped,
};

struct MidiWireEvent {
    std::uint16_t sample_offset = 0;
    std::uint8_t status = 0;
    std::uint8_t data1 = 0;
    std::uint8_t data2 = 0;
    std::uint8_t reserved[3]{};
};

#if defined(_MSC_VER)
#pragma warning(push)
// The transport and shared state intentionally occupy whole cache-line-aligned
// regions. MSVC C4324 reports the padding required by that explicit contract.
#pragma warning(disable: 4324)
#endif

struct alignas(64) TransportSlot {
    // A generation is odd while its owner writes and sequence*2 when complete.
    // Readers copy only when equal even values bracket the copy.
    std::atomic<std::uint64_t> request_generation{0};
    std::uint64_t engine_frame = 0;
    std::uint32_t frames = 0;
    std::uint32_t input_channels = 0;
    std::uint32_t midi_count = 0;
    std::uint32_t midi_live_count = 0;
    std::array<std::array<float, kMaximumFrames>, 2> input{};
    std::array<MidiWireEvent, kMaximumMidiEvents> midi{};

    alignas(64) std::atomic<std::uint64_t> response_generation{0};
    std::uint32_t response_frames = 0;
    std::uint32_t output_channels = 0;
    std::uint32_t process_ok = 0;
    std::uint32_t response_reserved = 0;
    std::array<std::array<float, kMaximumFrames>, 2> output{};
};

struct alignas(64) SharedState {
    std::uint32_t magic = kProtocolMagic;
    std::uint32_t version = kProtocolVersion;
    std::uint32_t bytes = sizeof(SharedState);
    std::uint32_t reserved = 0;
    std::atomic<WorkerState> worker_state{WorkerState::Starting};
    std::atomic<bool> shutdown{false};
    std::atomic<std::uint64_t> heartbeat{0};
    std::atomic<std::uint64_t> processed_blocks{0};
    std::atomic<std::uint64_t> failed_blocks{0};
    std::atomic<std::uint64_t> midi_events_consumed{0};
    std::atomic<std::uint32_t> worker_input_peak_ppm{0};
    std::atomic<std::uint64_t> process_time_last_us{0};
    std::atomic<std::uint64_t> process_time_sum_us{0};
    std::atomic<std::uint64_t> process_time_max_us{0};
    std::atomic<std::uint32_t> plugin_latency_frames{0};
    std::atomic<std::uint32_t> negotiated_input_channels{0};
    std::atomic<std::uint32_t> negotiated_output_channels{0};
    // 1 requests open/focus, 2 requests close. The worker resets to zero.
    std::atomic<std::uint32_t> editor_command{0};
    std::atomic<std::uint32_t> editor_state{0}; // 0 closed, 1 open, 2 unavailable
    std::array<char, 256> status_text{};
    std::array<TransportSlot, kTransportSlots> transport_blocks{};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
static_assert(std::is_standard_layout_v<SharedState>);
static_assert(offsetof(TransportSlot, response_generation) == 20544);
static_assert(sizeof(TransportSlot) == 36992);

} // namespace jam2::pluginhost

#pragma once

#include <cstdint>

namespace jam2::gui_control {

constexpr std::uint32_t kMagic = 0x43324d4aU; // JM2C little-endian
constexpr std::uint16_t kVersion = 1;
constexpr std::uint32_t kMaxPayloadBytes = 4096;

enum class MessageType : std::uint16_t {
    Hello = 1,
    Command = 2,
    ClockState = 10,
    Meters = 11,
    TrackState = 12,
    TrackTakeEvent = 13,
    CommandAck = 14,
    TransportState = 15,
};

enum class CommandOpcode : std::uint16_t {
    None = 0,
    MetronomeEnabled = 1,
    MetronomeLeader = 2,
    MetronomeMode = 3,
    MetronomeLevel = 4,
    MetronomePattern = 5,
    RemoteLevel = 6,
    SendLevel = 7,
    MonitorEnabled = 8,
    MonitorLevel = 9,
    RecordJamStart = 10,
    RecordJamStop = 11,
    TrackLoad = 12,
    TrackPlay = 13,
    TrackStop = 14,
    TrackSeek = 15,
    TrackLevel = 16,
    TrackLoop = 17,
    TrackTakeArmInput = 18,
    TrackTakeStart = 19,
    TrackTakeStop = 20,
    TrackTakeCancel = 21,
    Bpm = 22,
    RequestSnapshot = 23,
    TrackRestartQuantized = 24,
    TrackTakeStartQuantized = 25,
    RecordingLatencyAdjustment = 26,
};

enum class TransportAction : std::uint16_t {
    None = 0,
    TrackRestart = 1,
    RecordStart = 2,
};

enum class TrackTakeEventType : std::uint16_t {
    Stopped = 1,
    Error = 2,
};

struct Header {
    std::uint32_t magic = kMagic;
    std::uint16_t version = kVersion;
    std::uint16_t type = 0;
    std::uint32_t payload_size = 0;
    std::uint32_t sequence = 0;
};

} // namespace jam2::gui_control

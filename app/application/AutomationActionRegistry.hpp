#pragma once

#include <QJsonArray>
#include <QString>

#include <cstdint>
#include <optional>
#include <span>

// The native automation action vocabulary has one owner. Scenario validation,
// capability reporting, and native contract tests use this registry so a newly
// implemented action cannot silently be absent from the declared interface.
enum class Jam2AutomationActionType : std::uint8_t {
    MetronomeEnabled,
    MetronomeBpm,
    MetronomeMode,
    MetronomeLevel,
    RemoteLevel,
    TrackSync,
    TrackLoad,
    TrackPlay,
    TrackStop,
    TrackRestart,
    TrackRecordStart,
    RecordingStart,
    RecordingStop,
    Snapshot,
    Shutdown,
};

enum class Jam2AutomationPayload : std::uint8_t {
    None,
    Enabled,
    Bpm,
    MetronomeMode,
    Gain,
    Path,
    OptionalCountInBars,
};

struct Jam2AutomationActionDescriptor {
    Jam2AutomationActionType type;
    const char* name;
    Jam2AutomationPayload payload;
};

std::span<const Jam2AutomationActionDescriptor> jam2AutomationActionDescriptors() noexcept;
std::optional<Jam2AutomationActionType> jam2AutomationActionType(const QString& name) noexcept;
QString jam2AutomationActionName(Jam2AutomationActionType type);
QJsonArray jam2AutomationActionNamesJson();

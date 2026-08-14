#include "AutomationActionRegistry.hpp"

#include <array>

namespace {

constexpr std::array kDescriptors{
    Jam2AutomationActionDescriptor{Jam2AutomationActionType::MetronomeEnabled,
        "metronome.enabled", Jam2AutomationPayload::Enabled},
    Jam2AutomationActionDescriptor{Jam2AutomationActionType::MetronomeBpm,
        "metronome.bpm", Jam2AutomationPayload::Bpm},
    Jam2AutomationActionDescriptor{Jam2AutomationActionType::MetronomeMode,
        "metronome.mode", Jam2AutomationPayload::MetronomeMode},
    Jam2AutomationActionDescriptor{Jam2AutomationActionType::MetronomeLevel,
        "metronome.level", Jam2AutomationPayload::Gain},
    Jam2AutomationActionDescriptor{Jam2AutomationActionType::RemoteLevel,
        "remote.level", Jam2AutomationPayload::Gain},
    Jam2AutomationActionDescriptor{Jam2AutomationActionType::TrackSync,
        "track.sync", Jam2AutomationPayload::Enabled},
    Jam2AutomationActionDescriptor{Jam2AutomationActionType::TrackLoad,
        "track.load", Jam2AutomationPayload::Path},
    Jam2AutomationActionDescriptor{Jam2AutomationActionType::TrackPlay,
        "track.play", Jam2AutomationPayload::None},
    Jam2AutomationActionDescriptor{Jam2AutomationActionType::TrackStop,
        "track.stop", Jam2AutomationPayload::None},
    Jam2AutomationActionDescriptor{Jam2AutomationActionType::TrackRestart,
        "track.restart", Jam2AutomationPayload::OptionalCountInBars},
    Jam2AutomationActionDescriptor{Jam2AutomationActionType::TrackRecordStart,
        "track.record-start", Jam2AutomationPayload::OptionalCountInBars},
    Jam2AutomationActionDescriptor{Jam2AutomationActionType::RecordingStart,
        "recording.start", Jam2AutomationPayload::Path},
    Jam2AutomationActionDescriptor{Jam2AutomationActionType::RecordingStop,
        "recording.stop", Jam2AutomationPayload::None},
    Jam2AutomationActionDescriptor{Jam2AutomationActionType::Snapshot,
        "snapshot", Jam2AutomationPayload::None},
    Jam2AutomationActionDescriptor{Jam2AutomationActionType::Shutdown,
        "shutdown", Jam2AutomationPayload::None},
};

} // namespace

std::span<const Jam2AutomationActionDescriptor> jam2AutomationActionDescriptors() noexcept
{
    return kDescriptors;
}

std::optional<Jam2AutomationActionType> jam2AutomationActionType(const QString& name) noexcept
{
    for (const auto& descriptor : kDescriptors) {
        if (name == QString::fromLatin1(descriptor.name)) {
            return descriptor.type;
        }
    }
    return std::nullopt;
}

QString jam2AutomationActionName(Jam2AutomationActionType type)
{
    for (const auto& descriptor : kDescriptors) {
        if (descriptor.type == type) {
            return QString::fromLatin1(descriptor.name);
        }
    }
    return {};
}

QJsonArray jam2AutomationActionNamesJson()
{
    QJsonArray names;
    for (const auto& descriptor : kDescriptors) {
        names.push_back(QString::fromLatin1(descriptor.name));
    }
    return names;
}

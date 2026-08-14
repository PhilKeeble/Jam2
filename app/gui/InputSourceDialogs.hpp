#pragma once

#include <QString>
#include <QVector>

#include "midi.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>

class QWidget;

namespace jam2::gui {

struct AudioInputDialogSource {
    std::size_t slot = 0;
    std::size_t firstChannel = 0;
    std::size_t secondChannel = 0;
    QString firstName;
    QString secondName;
    bool stereo = false;
    bool included = true;
    int levelPpm = 1000000;
    bool pluginLoaded = false;
};

struct AudioInputDialogState {
    QVector<AudioInputDialogSource> sources;
    bool topologyLocked = false;
};

struct AudioInputDialogCallbacks {
    std::function<AudioInputDialogState()> snapshot;
    std::function<void(std::size_t slot, bool included)> setIncluded;
    std::function<void(std::size_t slot, int levelPpm)> setLevel;
    std::function<void(std::size_t slot)> ungroup;
    std::function<void(std::size_t leftSlot, std::size_t rightSlot)> group;
};

// Owns the transient Audio Inputs editor. The application retains the live
// source graph and supplies bounded state mutations through typed callbacks.
class AudioInputSourcesDialog final {
public:
    static void run(
        AudioInputDialogCallbacks callbacks,
        QWidget* parent = nullptr);

    AudioInputSourcesDialog() = delete;
};

struct MidiInputDialogSource {
    std::size_t routerSlot = 0;
    QString deviceId;
    QString deviceName;
    jam2::midi::InputMode mode = jam2::midi::InputMode::Standard;
    bool included = true;
    int levelPpm = 1000000;
    bool deviceOpen = false;
    bool pluginLoaded = false;
    QString pluginName;
};

struct MidiInputDialogState {
    QVector<MidiInputDialogSource> sources;
    bool topologyLocked = false;
};

struct MidiInputDeviceChoice {
    QString id;
    QString name;
};

struct MidiInputDiscoveryResult {
    QVector<MidiInputDeviceChoice> devices;
    QString error;
};

enum class MidiInputAssignmentError {
    None,
    EngineStopped,
    SourceLimit,
};

struct MidiInputDialogCallbacks {
    using DiscoveryCompletion = std::function<void(MidiInputDiscoveryResult)>;

    std::function<MidiInputDialogState()> snapshot;
    std::function<void(std::size_t slot, jam2::midi::InputMode mode)> setMode;
    std::function<void(std::size_t slot, bool included)> setIncluded;
    std::function<void(std::size_t slot, int levelPpm)> setLevel;
    std::function<void(std::size_t slot)> remove;
    std::function<void(DiscoveryCompletion completion)> discover;
    std::function<MidiInputAssignmentError(
        const MidiInputDeviceChoice& device,
        jam2::midi::InputMode mode)> assign;
};

// Owns the transient MIDI device assignment editor and its nested discovery/
// configuration UI. The application retains device, plugin, router, worker,
// and retirement ownership behind typed callbacks.
class MidiInputSourcesDialog final {
public:
    static void run(
        MidiInputDialogCallbacks callbacks,
        QWidget* parent = nullptr);

    MidiInputSourcesDialog() = delete;
};

enum class InputPluginDialogKind {
    Audio,
    MidiInstrument,
};

struct InputPluginDialogStats {
    std::uint64_t deadlineMisses = 0;
    std::uint64_t deadlineConcealments = 0;
    std::uint32_t workerLatencyFrames = 0;
    std::uint32_t negotiatedInputChannels = 0;
    std::uint32_t negotiatedOutputChannels = 0;
    std::size_t isolationLatencyFrames = 0;
    std::uint64_t workerProcessAverageUs = 0;
    std::uint64_t workerProcessMaxUs = 0;
    std::uint64_t midiDropped = 0;
    std::size_t midiQueueDepth = 0;
    std::size_t midiQueueHighWater = 0;
};

struct InputPluginDialogSource {
    InputPluginDialogKind kind = InputPluginDialogKind::Audio;
    std::size_t slot = 0;
    QString sourceName;
    bool loaded = false;
    QString pluginName;
    bool healthy = false;
    bool bypassed = false;
    InputPluginDialogStats stats;
};

struct InputPluginDialogState {
    QVector<InputPluginDialogSource> audioSources;
    QVector<InputPluginDialogSource> midiSources;
    bool topologyLocked = false;
};

struct InputPluginDialogCallbacks {
    using LoadProgress = std::function<void(int percent, const QString& text)>;
    using LoadFinished = std::function<void()>;

    std::function<InputPluginDialogState()> snapshot;
    std::function<void(InputPluginDialogKind kind, std::size_t slot)> open;
    std::function<void(
        InputPluginDialogKind kind, std::size_t slot, bool bypassed)> setBypassed;
    std::function<void(InputPluginDialogKind kind, std::size_t slot)> remove;
    std::function<bool(
        InputPluginDialogKind kind,
        std::size_t slot,
        LoadProgress progress,
        LoadFinished finished)> load;
};

// Owns the transient Input Plugins editor and its load-progress/button state.
// The application retains hosts, devices, queues, routing, worker ownership,
// recording guards, and retirement behind typed callbacks.
class InputPluginsDialog final {
public:
    static void run(
        InputPluginDialogCallbacks callbacks,
        QWidget* parent = nullptr);

    InputPluginsDialog() = delete;
};

} // namespace jam2::gui

#pragma once

#include "input_source.hpp"
#include "midi.hpp"

#include <QString>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

class QThread;
class QThreadPool;
class QWidget;

namespace jam2::application {

struct InputPluginStats {
    std::uint64_t submittedBlocks = 0;
    std::uint64_t completedBlocks = 0;
    std::uint64_t deadlineMisses = 0;
    std::uint64_t deadlineConcealments = 0;
    std::uint64_t failedBlocks = 0;
    std::uint64_t staleResponses = 0;
    std::uint64_t midiLate = 0;
    std::uint64_t midiDeferred = 0;
    std::uint64_t midiDropped = 0;
    std::uint64_t midiEventsConsumed = 0;
    std::uint32_t workerLatencyFrames = 0;
    std::uint32_t negotiatedInputChannels = 0;
    std::uint32_t negotiatedOutputChannels = 0;
    std::size_t isolationLatencyFrames = 0;
    std::uint64_t workerProcessLastUs = 0;
    std::uint64_t workerProcessAverageUs = 0;
    std::uint64_t workerProcessMaxUs = 0;
    std::size_t midiQueueDepth = 0;
    std::size_t midiQueueHighWater = 0;
};

// The source graph owns this interface. The system implementation wraps the
// isolated PluginHostService; automation uses a deterministic renderer without
// loading third-party code.
class InputPluginHost {
public:
    virtual ~InputPluginHost() = default;

    virtual audio::InputSourceRenderer* renderer() noexcept = 0;
    virtual bool healthy() const noexcept = 0;
    virtual void openEditor() noexcept = 0;
    virtual void closeEditor() noexcept = 0;
    virtual bool editorOpen() const noexcept = 0;
    virtual void requestRetire() noexcept = 0;
    virtual void setAudioBypassed(bool bypassed) noexcept = 0;
    virtual void setMidiQueue(midi::EventQueue* queue) noexcept = 0;
    virtual void setMidiMuted(bool muted) noexcept = 0;
    virtual void requestMidiReset() noexcept = 0;
    virtual InputPluginStats stats() const noexcept = 0;
    virtual QString statusText() const = 0;
    virtual QString errorText() const = 0;
};

struct InputPluginLoadRequest {
    audio::InputSourceKind kind = audio::InputSourceKind::Audio;
    midi::EventQueue* midiQueue = nullptr;
    double sampleRate = 0.0;
    std::size_t maximumFrames = 0;
    std::size_t sourceInputChannels = 0;
};

class InputPluginBackend {
public:
    using Completion = std::function<void(
        std::unique_ptr<InputPluginHost> host,
        QString name)>;
    using Progress = std::function<void(int percent, const QString& text)>;

    virtual ~InputPluginBackend() = default;

    // Called on the GUI thread. Implementations may synchronously present a
    // chooser, then must deliver asynchronous progress/completion on that same
    // thread. The supplied pool is owned and drained by MainWindow.
    virtual bool selectAndStart(
        QWidget& parent,
        QThreadPool& workers,
        QThread* guiThread,
        const InputPluginLoadRequest& request,
        Completion completion,
        Progress progress) = 0;
};

struct SystemInputPluginBackendOptions {
    // An explicit path is used by the hardware test profile. An empty path
    // preserves the normal native file chooser.
    QString pluginPath;
    // Hardware automation selects the scanner's first class deterministically;
    // interactive use continues to ask when a bundle exposes several classes.
    bool selectFirstClass = false;
};

std::unique_ptr<InputPluginBackend> makeSystemInputPluginBackend();
std::unique_ptr<InputPluginBackend> makeSystemInputPluginBackend(
    SystemInputPluginBackendOptions options);
std::unique_ptr<InputPluginBackend> makeSyntheticInputPluginBackend(
    std::chrono::milliseconds loadDelay = std::chrono::milliseconds(200));

} // namespace jam2::application

#include "InputPluginBackend.hpp"

#include "PluginHostService.hpp"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QInputDialog>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QRunnable>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QUuid>
#include <QWidget>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <span>
#include <string>
#include <utility>

namespace jam2::application {
namespace {

InputPluginStats copyStats(const pluginhost::PluginBridgeStats& source) noexcept
{
    InputPluginStats result;
    result.submittedBlocks = source.submitted_blocks;
    result.completedBlocks = source.completed_blocks;
    result.deadlineMisses = source.deadline_misses;
    result.deadlineConcealments = source.deadline_concealments;
    result.failedBlocks = source.failed_blocks;
    result.staleResponses = source.stale_responses;
    result.midiLate = source.midi_late;
    result.midiDeferred = source.midi_deferred;
    result.midiDropped = source.midi_dropped;
    result.midiEventsConsumed = source.midi_events_consumed;
    result.workerLatencyFrames = source.worker_latency_frames;
    result.negotiatedInputChannels = source.negotiated_input_channels;
    result.negotiatedOutputChannels = source.negotiated_output_channels;
    result.isolationLatencyFrames = source.isolation_latency_frames;
    result.workerProcessLastUs = source.worker_process_last_us;
    result.workerProcessAverageUs = source.worker_process_average_us;
    result.workerProcessMaxUs = source.worker_process_max_us;
    result.midiQueueDepth = source.midi_queue_depth;
    result.midiQueueHighWater = source.midi_queue_high_water;
    result.workerInputPeakPpm = source.worker_input_peak_ppm;
    result.wetOutputPeakPpm = source.wet_output_peak_ppm;
    return result;
}

class SystemInputPluginHost final : public InputPluginHost {
public:
    explicit SystemInputPluginHost(
        std::unique_ptr<pluginhost::PluginHostService> service)
        : service_(std::move(service))
    {
    }

    audio::InputSourceRenderer* renderer() noexcept override
    {
        return service_ ? service_->bridge() : nullptr;
    }
    bool healthy() const noexcept override
    {
        return service_ && service_->healthy();
    }
    void openEditor() noexcept override
    {
        if (service_) service_->openEditor();
    }
    void closeEditor() noexcept override
    {
        if (service_) service_->closeEditor();
    }
    bool editorOpen() const noexcept override
    {
        return service_ && service_->editorOpen();
    }
    void requestRetire() noexcept override
    {
        if (service_) service_->requestRetire();
    }
    void setAudioBypassed(bool bypassed) noexcept override
    {
        if (service_ && service_->bridge())
            service_->bridge()->set_bypassed(bypassed);
    }
    void setMidiQueue(midi::EventQueue* queue) noexcept override
    {
        if (service_ && service_->bridge())
            service_->bridge()->set_midi_queue(queue);
    }
    void setMidiMuted(bool muted) noexcept override
    {
        if (service_ && service_->bridge())
            service_->bridge()->set_muted(muted);
    }
    void requestMidiReset() noexcept override
    {
        if (service_ && service_->bridge())
            service_->bridge()->request_midi_reset();
    }
    InputPluginStats stats() const noexcept override
    {
        return service_ && service_->bridge()
            ? copyStats(service_->bridge()->stats()) : InputPluginStats{};
    }
    QString statusText() const override
    {
        return service_ ? service_->statusText() : QString{};
    }
    QString errorText() const override
    {
        return service_ ? service_->errorText() : QString{};
    }

private:
    std::unique_ptr<pluginhost::PluginHostService> service_;
};

bool validRequest(const InputPluginLoadRequest& request) noexcept
{
    const bool validSourceShape =
        (request.kind == audio::InputSourceKind::Audio &&
         request.sourceInputChannels > 0 &&
         request.sourceInputChannels <= audio::kMaximumSourceInputChannels &&
         request.midiQueue == nullptr) ||
        (request.kind == audio::InputSourceKind::MidiInstrument &&
         request.sourceInputChannels == 0 && request.midiQueue != nullptr);
    return validSourceShape &&
        std::isfinite(request.sampleRate) && request.sampleRate > 0.0 &&
        request.maximumFrames > 0 &&
        request.maximumFrames <= pluginhost::kMaximumFrames;
}

class SystemInputPluginBackend final : public InputPluginBackend {
public:
    explicit SystemInputPluginBackend(SystemInputPluginBackendOptions options = {})
        : options_(std::move(options))
    {
    }

    bool selectAndStart(
        QWidget& parent,
        QThreadPool& workers,
        QThread* guiThread,
        const InputPluginLoadRequest& request,
        Completion completion,
        Progress progress) override
    {
        if (!validRequest(request) || !completion || guiThread == nullptr) {
            if (progress) progress(0, QStringLiteral("The plugin load request is invalid."));
            return false;
        }
        QString pluginPath = options_.pluginPath;
        if (pluginPath.isEmpty()) {
#ifdef Q_OS_MACOS
            pluginPath = QFileDialog::getExistingDirectory(
                &parent, QStringLiteral("Select VST3 plugin"),
                QDir::homePath() + QStringLiteral("/Library/Audio/Plug-Ins/VST3"));
#else
            pluginPath = QFileDialog::getOpenFileName(
                &parent, QStringLiteral("Select VST3 plugin"),
                QStringLiteral("C:/Program Files/Common Files/VST3"),
                QStringLiteral("VST3 plugins (*.vst3)"));
#endif
        }
        if (pluginPath.isEmpty()) return false;

        const QString workerPath = pluginhost::PluginHostService::workerExecutablePath();
        const QString resultPath = QDir::temp().absoluteFilePath(
            QStringLiteral("jam2-vst3-probe-%1.txt")
                .arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        if (progress) progress(-1, QStringLiteral("Scanning VST3 in an isolated process…"));

        struct ScanOutcome {
            QList<QStringList> classes;
            QString error;
        };
        auto scan = std::make_shared<ScanOutcome>();
        QPointer<QWidget> owner(&parent);
        QThreadPool* const workerPool = &workers;
        workers.start(QRunnable::create([
            owner, pluginPath, resultPath, workerPath, scan, request,
            completion = std::move(completion), progress = std::move(progress),
            guiThread, workerPool, selectFirstClass = options_.selectFirstClass
        ]() mutable {
            try {
                (void)QFile::remove(resultPath);
                QProcess probe;
                probe.setProcessChannelMode(QProcess::ForwardedChannels);
                probe.setInputChannelMode(QProcess::ForwardedInputChannel);
                probe.start(workerPath,
                    {QStringLiteral("--probe-file"), pluginPath, resultPath});
                if (!probe.waitForStarted(60000)) {
                    scan->error = QStringLiteral(
                        "The isolated VST3 scanner did not start: %1")
                            .arg(probe.errorString());
                } else if (!probe.waitForFinished(120000)) {
                    probe.kill();
                    (void)probe.waitForFinished(5000);
                    scan->error = QStringLiteral(
                        "The isolated VST3 scanner did not finish before its hang deadman.");
                } else if (probe.exitStatus() != QProcess::NormalExit ||
                           probe.exitCode() != 0) {
                    scan->error = QStringLiteral(
                        "The isolated VST3 scanner rejected the plugin (exit %1).")
                            .arg(probe.exitCode());
                } else {
                    QFile result(resultPath);
                    if (!result.open(QIODevice::ReadOnly)) {
                        scan->error = QStringLiteral(
                            "Could not read the private plugin scan result.");
                    } else {
                        const QList<QByteArray> lines = result.readAll().split('\n');
                        for (const QByteArray& line : lines) {
                            const QList<QByteArray> fields = line.trimmed().split('\t');
                            if (fields.size() < 2) continue;
                            QStringList values;
                            for (const QByteArray& field : fields)
                                values.push_back(QString::fromUtf8(field));
                            scan->classes.push_back(std::move(values));
                        }
                    }
                }
                if (scan->classes.isEmpty() && scan->error.isEmpty()) {
                    scan->error = QStringLiteral(
                        "No VST3 audio or instrument class was found.");
                }
            } catch (const std::exception& error) {
                scan->error = QString::fromUtf8(error.what());
            } catch (...) {
                scan->error = QStringLiteral("Unknown isolated VST3 scan failure.");
            }
            (void)QFile::remove(resultPath);
            if (owner.isNull()) return;
            QMetaObject::invokeMethod(owner, [
                owner, pluginPath, scan, request, completion = std::move(completion),
                progress = std::move(progress), guiThread, workerPool,
                selectFirstClass
            ]() mutable {
                if (owner.isNull()) return;
                if (!scan->error.isEmpty()) {
                    if (progress) progress(0, scan->error);
                    else QMessageBox::warning(owner, QStringLiteral("Input plugin"), scan->error);
                    return;
                }
                QStringList classNames;
                for (const QStringList& pluginClass : scan->classes)
                    classNames.push_back(pluginClass.at(1));
                int selected = 0;
                if (scan->classes.size() > 1 && !selectFirstClass) {
                    bool accepted = false;
                    const QString choice = QInputDialog::getItem(owner,
                        QStringLiteral("Plugin class"), QStringLiteral("Class"),
                        classNames, 0, false, &accepted);
                    if (!accepted) {
                        if (progress)
                            progress(0, QStringLiteral("Plugin selection cancelled."));
                        return;
                    }
                    selected = classNames.indexOf(choice);
                    if (selected < 0) {
                        if (progress)
                            progress(0, QStringLiteral("Plugin selection is invalid."));
                        return;
                    }
                }
                if (progress)
                    progress(-1, QStringLiteral("Starting isolated plugin worker…"));

                struct StartOutcome {
                    std::unique_ptr<pluginhost::PluginHostService> service;
                    QString error;
                };
                auto started = std::make_shared<StartOutcome>();
                const QStringList selectedClass = scan->classes.at(selected);
                workerPool->start(QRunnable::create([
                    owner, pluginPath, selectedClass, request,
                    completion = std::move(completion), progress = std::move(progress),
                    started, guiThread
                ]() mutable {
                    try {
                        started->service =
                            std::make_unique<pluginhost::PluginHostService>();
                        started->service->start(
                            pluginPath.toStdString(), selectedClass.at(0).toStdString(),
                            request.sampleRate, request.maximumFrames, request.kind,
                            request.sourceInputChannels);
                        if (request.midiQueue && started->service->bridge())
                            started->service->bridge()->set_midi_queue(request.midiQueue);
                        started->service->moveProcessToThread(guiThread);
                    } catch (const std::exception& error) {
                        started->error = QString::fromUtf8(error.what());
                    } catch (...) {
                        started->error =
                            QStringLiteral("Unknown isolated plugin worker failure.");
                    }
                    if (owner.isNull()) return;
                    QMetaObject::invokeMethod(owner, [
                        owner, selectedClass, completion = std::move(completion),
                        progress = std::move(progress), started
                    ]() mutable {
                        if (owner.isNull()) return;
                        if (!started->error.isEmpty()) {
                            const QString message = QStringLiteral(
                                "The isolated plugin worker rejected the plugin: %1")
                                    .arg(started->error);
                            if (progress) progress(0, message);
                            else QMessageBox::warning(
                                owner, QStringLiteral("Input plugin"), message);
                            return;
                        }
                        const QString name = selectedClass.at(1);
                        if (progress) {
                            progress(100, QStringLiteral(
                                "%1 loaded. You can open its interface now.").arg(name));
                        }
                        completion(std::make_unique<SystemInputPluginHost>(
                            std::move(started->service)), name);
                    }, Qt::QueuedConnection);
                }));
            }, Qt::QueuedConnection);
        }));
        return true;
    }

private:
    SystemInputPluginBackendOptions options_;
};

class SyntheticInputPluginRenderer final : public audio::InputSourceRenderer {
public:
    SyntheticInputPluginRenderer(
        audio::InputSourceKind kind,
        std::size_t inputChannels,
        midi::EventQueue* midiQueue) noexcept
        : kind_(kind)
        , inputChannels_(static_cast<std::uint32_t>(inputChannels))
        , midiQueue_(midiQueue)
    {
    }

    bool render_mono(
        const audio::InputSourceRenderRequest& request,
        std::span<std::int32_t> output) noexcept override
    {
        submittedBlocks_.fetch_add(1, std::memory_order_relaxed);
        if (retired_.load(std::memory_order_acquire) || request.frames == 0 ||
            output.size() < request.frames) {
            failedBlocks_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (kind_ == audio::InputSourceKind::MidiInstrument) {
            drainMidi();
            const std::int32_t sample = !muted_.load(std::memory_order_relaxed) &&
                    noteHeld_.load(std::memory_order_relaxed)
                ? static_cast<std::int32_t>(268435456) : 0;
            std::fill_n(output.begin(), request.frames, sample);
        } else {
            const bool bypassed = bypassed_.load(std::memory_order_relaxed);
            for (std::size_t frame = 0; frame < request.frames; ++frame) {
                std::int64_t sum = 0;
                std::size_t channels = 0;
                for (std::size_t channel = 0;
                     channel < request.input_channels && channel < request.inputs.size();
                     ++channel) {
                    if (!request.inputs[channel]) continue;
                    sum += request.inputs[channel][frame];
                    ++channels;
                }
                const std::int32_t dry = channels == 0 ? 0 :
                    static_cast<std::int32_t>(sum / static_cast<std::int64_t>(channels));
                output[frame] = bypassed ? dry : static_cast<std::int32_t>(-dry / 2);
            }
        }
        completedBlocks_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    void setBypassed(bool value) noexcept
    {
        bypassed_.store(value, std::memory_order_relaxed);
    }
    void setMidiQueue(midi::EventQueue* value) noexcept
    {
        midiQueue_.store(value, std::memory_order_release);
    }
    void setMuted(bool value) noexcept
    {
        muted_.store(value, std::memory_order_relaxed);
        if (value) noteHeld_.store(false, std::memory_order_relaxed);
    }
    void resetMidi() noexcept
    {
        noteHeld_.store(false, std::memory_order_relaxed);
    }
    void retire() noexcept
    {
        retired_.store(true, std::memory_order_release);
        noteHeld_.store(false, std::memory_order_relaxed);
        midiQueue_.store(nullptr, std::memory_order_release);
    }
    InputPluginStats stats() const noexcept
    {
        InputPluginStats result;
        result.submittedBlocks = submittedBlocks_.load(std::memory_order_relaxed);
        result.completedBlocks = completedBlocks_.load(std::memory_order_relaxed);
        result.failedBlocks = failedBlocks_.load(std::memory_order_relaxed);
        result.negotiatedInputChannels = inputChannels_;
        result.negotiatedOutputChannels = 1;
        if (midi::EventQueue* queue = midiQueue_.load(std::memory_order_acquire)) {
            result.midiDropped = queue->dropped();
            result.midiQueueDepth = queue->depth();
            result.midiQueueHighWater = queue->high_water();
        }
        result.midiEventsConsumed = midiEventsConsumed_.load(
            std::memory_order_relaxed);
        return result;
    }

private:
    void drainMidi() noexcept
    {
        midi::EventQueue* queue = midiQueue_.load(std::memory_order_acquire);
        if (!queue) return;
        midi::Event event;
        std::size_t count = 0;
        while (count < midi::kMaximumEventsPerBlock && queue->pop(event)) {
            const std::uint8_t kind = event.status & 0xf0U;
            if (kind == 0x90U && event.data2 != 0)
                noteHeld_.store(true, std::memory_order_relaxed);
            else if (kind == 0x80U || (kind == 0x90U && event.data2 == 0))
                noteHeld_.store(false, std::memory_order_relaxed);
            midiEventsConsumed_.fetch_add(1, std::memory_order_relaxed);
            ++count;
        }
    }

    audio::InputSourceKind kind_ = audio::InputSourceKind::Audio;
    std::uint32_t inputChannels_ = 0;
    std::atomic<midi::EventQueue*> midiQueue_{nullptr};
    std::atomic<bool> bypassed_{false};
    std::atomic<bool> muted_{false};
    std::atomic<bool> noteHeld_{false};
    std::atomic<bool> retired_{false};
    std::atomic<std::uint64_t> midiEventsConsumed_{0};
    std::atomic<std::uint64_t> submittedBlocks_{0};
    std::atomic<std::uint64_t> completedBlocks_{0};
    std::atomic<std::uint64_t> failedBlocks_{0};
};

class SyntheticInputPluginHost final : public InputPluginHost {
public:
    explicit SyntheticInputPluginHost(const InputPluginLoadRequest& request)
        : renderer_(request.kind, request.sourceInputChannels, request.midiQueue)
    {
    }

    audio::InputSourceRenderer* renderer() noexcept override { return &renderer_; }
    bool healthy() const noexcept override
    {
        return !retired_.load(std::memory_order_acquire);
    }
    void openEditor() noexcept override
    {
        if (healthy()) editorOpen_.store(true, std::memory_order_relaxed);
    }
    void closeEditor() noexcept override
    {
        editorOpen_.store(false, std::memory_order_relaxed);
    }
    bool editorOpen() const noexcept override
    {
        return editorOpen_.load(std::memory_order_relaxed);
    }
    void requestRetire() noexcept override
    {
        retired_.store(true, std::memory_order_release);
        editorOpen_.store(false, std::memory_order_relaxed);
        renderer_.retire();
    }
    void setAudioBypassed(bool bypassed) noexcept override
    {
        renderer_.setBypassed(bypassed);
    }
    void setMidiQueue(midi::EventQueue* queue) noexcept override
    {
        renderer_.setMidiQueue(queue);
    }
    void setMidiMuted(bool muted) noexcept override
    {
        renderer_.setMuted(muted);
    }
    void requestMidiReset() noexcept override { renderer_.resetMidi(); }
    InputPluginStats stats() const noexcept override { return renderer_.stats(); }
    QString statusText() const override
    {
        return healthy()
            ? QStringLiteral("deterministic input plugin active")
            : QStringLiteral("deterministic input plugin retired");
    }
    QString errorText() const override { return {}; }

private:
    SyntheticInputPluginRenderer renderer_;
    std::atomic<bool> editorOpen_{false};
    std::atomic<bool> retired_{false};
};

class SyntheticInputPluginBackend final : public InputPluginBackend {
public:
    bool selectAndStart(
        QWidget& parent,
        QThreadPool&,
        QThread*,
        const InputPluginLoadRequest& request,
        Completion completion,
        Progress progress) override
    {
        if (!validRequest(request) || !completion) {
            if (progress) progress(0, QStringLiteral("The plugin load request is invalid."));
            return false;
        }
        const std::uint64_t sequence =
            loadSequence_.fetch_add(1, std::memory_order_relaxed) + 1;
        const QString name = request.kind == audio::InputSourceKind::MidiInstrument
            ? QStringLiteral("Automation MIDI Instrument %1").arg(sequence)
            : QStringLiteral("Automation Audio Effect %1").arg(sequence);
        if (progress) progress(-1, QStringLiteral("Loading deterministic input plugin…"));
        QPointer<QWidget> owner(&parent);
        auto finish = [
            owner, request, completion = std::move(completion),
            progress = std::move(progress), name
        ]() mutable {
            if (owner.isNull()) return;
            auto host = std::make_unique<SyntheticInputPluginHost>(request);
            if (progress) {
                progress(100, QStringLiteral(
                    "%1 loaded. You can open its interface now.").arg(name));
            }
            completion(std::move(host), name);
        };
        if (gateArmed_) {
            gateArmed_ = false;
            gateActive_ = true;
            gatedCompletion_ = std::move(finish);
        } else {
            QTimer::singleShot(0, &parent, std::move(finish));
        }
        return true;
    }

    bool armAutomationCompletionGate(QString& error) noexcept override
    {
        if (gateArmed_ || gateActive_) {
            error = QStringLiteral("an input-plugin completion gate is already armed or active");
            return false;
        }
        gateArmed_ = true;
        error.clear();
        return true;
    }

    bool releaseAutomationCompletionGate(QString& error) noexcept override
    {
        if (!gateActive_ || !gatedCompletion_) {
            error = QStringLiteral("no input-plugin completion gate is active");
            return false;
        }
        gateActive_ = false;
        auto completion = std::move(gatedCompletion_);
        gatedCompletion_ = {};
        error.clear();
        completion();
        return true;
    }

    AutomationCompletionGateState automationCompletionGateState() const noexcept override
    {
        if (gateActive_) return AutomationCompletionGateState::Active;
        if (gateArmed_) return AutomationCompletionGateState::Armed;
        return AutomationCompletionGateState::Idle;
    }

private:
    std::atomic<std::uint64_t> loadSequence_{0};
    bool gateArmed_ = false;
    bool gateActive_ = false;
    std::function<void()> gatedCompletion_;
};

} // namespace

bool InputPluginBackend::armAutomationCompletionGate(QString& error) noexcept
{
    error = QStringLiteral("the system input-plugin backend has no automation completion gate");
    return false;
}

bool InputPluginBackend::releaseAutomationCompletionGate(QString& error) noexcept
{
    error = QStringLiteral("the system input-plugin backend has no automation completion gate");
    return false;
}

AutomationCompletionGateState InputPluginBackend::automationCompletionGateState() const noexcept
{
    return AutomationCompletionGateState::Unsupported;
}

std::unique_ptr<InputPluginBackend> makeSystemInputPluginBackend()
{
    return std::make_unique<SystemInputPluginBackend>();
}

std::unique_ptr<InputPluginBackend> makeSystemInputPluginBackend(
    SystemInputPluginBackendOptions options)
{
    return std::make_unique<SystemInputPluginBackend>(std::move(options));
}

std::unique_ptr<InputPluginBackend> makeSyntheticInputPluginBackend()
{
    return std::make_unique<SyntheticInputPluginBackend>();
}

} // namespace jam2::application

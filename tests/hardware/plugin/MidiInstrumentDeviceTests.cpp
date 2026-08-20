// Native hardware coverage is owned by the repository-level test tree.
#include "InputPluginBackend.hpp"
#include "PluginProtocol.hpp"

#include "engine.hpp"
#include "input_source.hpp"
#include "midi.hpp"

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QProcess>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

int integer_argument(char** argv, int index)
{
    return std::stoi(argv[index]);
}

QString plugin_worker_path()
{
    const QDir directory(QCoreApplication::applicationDirPath());
#ifdef _WIN32
    return directory.absoluteFilePath(QStringLiteral("jam2-plugin-worker.exe"));
#elif defined(__APPLE__)
    return directory.absoluteFilePath(QStringLiteral("../Helpers/jam2-plugin-worker"));
#else
    return directory.absoluteFilePath(QStringLiteral("jam2-plugin-worker"));
#endif
}

void require_worker_command(
    const QString& worker,
    const QStringList& arguments,
    const QByteArray& expected)
{
    QProcess process;
    process.setProgram(worker);
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    if (!process.waitForStarted(60000) || !process.waitForFinished(120000)) {
        process.kill();
        process.waitForFinished(5000);
        throw std::runtime_error(
            "Plugin worker diagnostic command did not finish: " +
            arguments.value(0).toStdString());
    }
    const QByteArray output = process.readAll();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 ||
        !output.contains(expected)) {
        throw std::runtime_error(
            "Plugin worker diagnostic command failed: " +
            arguments.value(0).toStdString() + " output=" + output.toStdString());
    }
}

std::unique_ptr<jam2::application::InputPluginHost> load_instrument(
    QApplication& application,
    QWidget& owner,
    QThreadPool& workers,
    const QString& pluginPath,
    std::size_t blockFrames,
    jam2::midi::EventQueue& midiQueue,
    QString& pluginName)
{
    auto backend = jam2::application::makeSystemInputPluginBackend({
        pluginPath, true});
    jam2::application::InputPluginLoadRequest request;
    request.kind = jam2::audio::InputSourceKind::MidiInstrument;
    request.midiQueue = &midiQueue;
    request.sampleRate = 48000.0;
    request.maximumFrames = blockFrames;
    request.sourceInputChannels = 0;

    std::unique_ptr<jam2::application::InputPluginHost> plugin;
    QString loadError;
    QEventLoop loadLoop;
    QTimer loadDeadman;
    loadDeadman.setSingleShot(true);
    QObject::connect(&loadDeadman, &QTimer::timeout, &loadLoop, &QEventLoop::quit);
    if (!backend->selectAndStart(
            owner,
            workers,
            application.thread(),
            request,
            [&](std::unique_ptr<jam2::application::InputPluginHost> loaded,
                QString name) {
                plugin = std::move(loaded);
                pluginName = std::move(name);
                loadLoop.quit();
            },
            [&](int percent, const QString& message) {
                std::cerr << "instrument progress " << percent << ": "
                          << message.toStdString() << '\n';
                if (percent == 0) {
                    loadError = message;
                    loadLoop.quit();
                }
            })) {
        throw std::runtime_error(
            "Production plugin backend rejected the instrument profile");
    }
    loadDeadman.start(120000);
    loadLoop.exec();
    if (!plugin || !plugin->healthy() || plugin->renderer() == nullptr) {
        throw std::runtime_error(loadError.isEmpty()
            ? "Instrument worker did not become healthy"
            : loadError.toStdString());
    }
    if (!plugin->errorText().isEmpty() || plugin->statusText().isEmpty()) {
        throw std::runtime_error(
            "Instrument backend omitted healthy worker diagnostics");
    }
    return plugin;
}

bool name_matches(const std::string& actual, const QString& selector)
{
    return QString::fromStdString(actual).contains(selector, Qt::CaseInsensitive);
}

} // namespace

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    if (argc != 6) {
        std::cerr << "usage: jam2_midi_instrument_device_tests <instrument.vst3> "
                     "<audio-device-id> <input-channel> <frames> <midi-name>\n";
        return 2;
    }

    try {
        const QString pluginPath = QString::fromLocal8Bit(argv[1]);
        const int audioDeviceId = integer_argument(argv, 2);
        const int userInputChannel = integer_argument(argv, 3);
        const long blockFrames = integer_argument(argv, 4);
        const QString midiSelector = QString::fromLocal8Bit(argv[5]).trimmed();
        if (audioDeviceId < 0 || userInputChannel < 1 || blockFrames < 1 ||
            midiSelector.isEmpty()) {
            throw std::invalid_argument("The MIDI hardware profile is invalid");
        }

        const QString workerPath = plugin_worker_path();
        std::cerr << "phase: validate instrument worker diagnostic commands\n";
        require_worker_command(
            workerPath, {QStringLiteral("--probe"), pluginPath}, QByteArrayLiteral("\t"));
        require_worker_command(
            workerPath, {QStringLiteral("--probe-all"), pluginPath},
            QByteArrayLiteral("Instrument"));
        require_worker_command(
            workerPath, {QStringLiteral("--self-test-instrument"), pluginPath},
            QByteArrayLiteral("OK\t"));

        std::cerr << "phase: enumerate the production CoreMIDI backend\n";
        const std::vector<jam2::midi::DeviceInfo> devices =
            jam2::midi::enumerate_input_devices();
        std::vector<jam2::midi::DeviceInfo> matches;
        for (const auto& device : devices) {
            std::cerr << "CoreMIDI input id=" << device.id
                      << " name=" << device.name << '\n';
            if (name_matches(device.name, midiSelector)) matches.push_back(device);
        }
        if (matches.size() != 1) {
            throw std::runtime_error(
                "The MIDI selector did not identify exactly one CoreMIDI input: " +
                std::to_string(devices.size()) + " inputs enumerated, " +
                std::to_string(matches.size()) + " matched");
        }
        const jam2::midi::DeviceInfo selected = matches.front();

        std::cerr << "phase: open, capture, and classify physical MIDI messages\n";
        jam2::midi::EventQueue captureQueue;
        std::string midiError;
        auto captureDevice = jam2::midi::open_input_device(
            selected.id, captureQueue, midiError);
        if (!captureDevice) {
            throw std::runtime_error(
                "Could not open profiled CoreMIDI input: " + midiError);
        }
        std::cerr << "MIDI_CAPTURE_READY: press and release a key/pad, then turn "
                     "one Xjam knob or control.\n";
        bool sawNoteOn = false;
        bool sawNoteOff = false;
        bool sawContinuousControl = false;
        std::uint64_t capturedEvents = 0;
        const auto captureDeadman =
            std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (std::chrono::steady_clock::now() < captureDeadman &&
               !(sawNoteOn && sawNoteOff && sawContinuousControl)) {
            QCoreApplication::processEvents();
            jam2::midi::Event event;
            while (captureQueue.pop(event)) {
                ++capturedEvents;
                const std::uint8_t kind = event.status & 0xf0U;
                sawNoteOn = sawNoteOn || (kind == 0x90U && event.data2 != 0);
                sawNoteOff = sawNoteOff || kind == 0x80U ||
                    (kind == 0x90U && event.data2 == 0);
                sawContinuousControl = sawContinuousControl ||
                    kind == 0xb0U || kind == 0xe0U || kind == 0xd0U;
            }
            if (!(sawNoteOn && sawNoteOff && sawContinuousControl)) {
                QThread::msleep(5);
            }
        }
        const std::uint64_t captureShortMessages = captureDevice->short_messages();
        const std::uint64_t captureUnsupported = captureDevice->unsupported_messages();
        captureDevice.reset();
        if (!sawNoteOn || !sawNoteOff || !sawContinuousControl ||
            captureShortMessages < capturedEvents || captureUnsupported != 0 ||
            captureQueue.dropped() != 0) {
            throw std::runtime_error(
                "Physical CoreMIDI capture did not return note on/off and control "
                "messages without parser or queue loss");
        }

        std::cerr << "phase: close and reopen CoreMIDI for the real instrument path\n";
        jam2::midi::EventQueue instrumentQueue;
        midiError.clear();
        auto instrumentDevice = jam2::midi::open_input_device(
            selected.id, instrumentQueue, midiError);
        if (!instrumentDevice) {
            throw std::runtime_error(
                "CoreMIDI input did not reopen cleanly: " + midiError);
        }

        QWidget pluginOwner;
        QThreadPool pluginWorkers;
        pluginWorkers.setMaxThreadCount(2);
        QString pluginName;
        auto plugin = load_instrument(
            application,
            pluginOwner,
            pluginWorkers,
            pluginPath,
            static_cast<std::size_t>(blockFrames),
            instrumentQueue,
            pluginName);
        plugin->setAudioBypassed(false);
        plugin->setMidiQueue(&instrumentQueue);
        plugin->setMidiMuted(true);
        plugin->requestMidiReset();

        plugin->openEditor();
        const auto editorOpenDeadman =
            std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (!plugin->editorOpen() &&
               std::chrono::steady_clock::now() < editorOpenDeadman) {
            QCoreApplication::processEvents();
            QThread::msleep(5);
        }
        const bool editorWasOpen = plugin->editorOpen();
        plugin->closeEditor();
        const auto editorCloseDeadman =
            std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (plugin->editorOpen() &&
               std::chrono::steady_clock::now() < editorCloseDeadman) {
            QCoreApplication::processEvents();
            QThread::msleep(5);
        }
        if (!editorWasOpen || plugin->editorOpen()) {
            throw std::runtime_error(
                "Instrument editor lifecycle did not complete");
        }

        jam2::audio::InputSourceRouter router(
            static_cast<std::size_t>(blockFrames), 1U);
        jam2::audio::InputSourceConfiguration source;
        source.kind = jam2::audio::InputSourceKind::MidiInstrument;
        source.enabled = true;
        source.renderer = plugin->renderer();
        if (!router.configure(0, source)) {
            throw std::runtime_error(
                "Could not configure the real MIDI instrument source");
        }

        jam2::Engine engine;
        jam2::EngineConfig config;
        config.audio_device_id = audioDeviceId;
        config.sample_rate = 48000;
        config.audio_buffer_frames = blockFrames;
        config.input_channels = jam2::audio::InputChannels::Mono;
        config.channels.input = {userInputChannel - 1};
        config.channels.output = {0, 1};
        config.diagnostics_enabled = true;
        config.local_monitor_enabled = false;
        config.input_source_router = &router;

        jam2::midi::Event queuedBeforeMeasurement;
        while (instrumentQueue.pop(queuedBeforeMeasurement)) {}
        engine.start(config);

        std::cerr << "MIDI_MUTED_READY: press and release an Xjam key/pad to prove "
                     "that MIDI is consumed while instrument output is muted.\n";
        const std::uint64_t mutedMessageBaseline =
            instrumentDevice->short_messages();
        const std::uint64_t mutedConsumedBaseline =
            plugin->stats().midiEventsConsumed;
        const std::uint64_t mutedRenderedBaseline = router.stats().rendered_blocks;
        bool mutedRoutingObserved = false;
        const auto mutedDeadman =
            std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (std::chrono::steady_clock::now() < mutedDeadman) {
            QCoreApplication::processEvents();
            const auto engineStats = engine.snapshot();
            const auto routerStats = router.stats();
            const auto pluginStats = plugin->stats();
            if (instrumentDevice->short_messages() > mutedMessageBaseline &&
                pluginStats.midiEventsConsumed > mutedConsumedBaseline &&
                routerStats.rendered_blocks > mutedRenderedBaseline &&
                routerStats.peak_ppm == 0 && engineStats.send_peak_ppm == 0) {
                mutedRoutingObserved = true;
                break;
            }
            QThread::msleep(5);
        }
        if (!mutedRoutingObserved) {
            engine.requestStop();
            engine.join();
            throw std::runtime_error(
                "The isolated instrument did not consume physical MIDI while "
                "the production mute path held its routed output at zero");
        }

        plugin->requestMidiReset();
        plugin->setMidiMuted(false);
        const std::uint64_t instrumentMessageBaseline =
            instrumentDevice->short_messages();
        const std::uint64_t instrumentConsumedBaseline =
            plugin->stats().midiEventsConsumed;
        std::cerr << "MIDI_INSTRUMENT_READY: play and briefly hold several Xjam "
                     "keys/pads until the test reports completion.\n";
        int maximumRouterPeak = 0;
        int maximumSendPeak = 0;
        bool instrumentRoutingObserved = false;
        constexpr int minimumInstrumentPeakPpm = 100;
        const auto instrumentDeadman =
            std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (std::chrono::steady_clock::now() < instrumentDeadman) {
            QCoreApplication::processEvents();
            const auto engineStats = engine.snapshot();
            const auto routerStats = router.stats();
            const auto pluginStats = plugin->stats();
            maximumRouterPeak = std::max(maximumRouterPeak, routerStats.peak_ppm);
            maximumSendPeak = std::max(maximumSendPeak, engineStats.send_peak_ppm);
            if (pluginStats.completedBlocks > 0 &&
                pluginStats.midiEventsConsumed > instrumentConsumedBaseline &&
                instrumentDevice->short_messages() > instrumentMessageBaseline &&
                pluginStats.wetOutputPeakPpm >= minimumInstrumentPeakPpm &&
                maximumRouterPeak >= minimumInstrumentPeakPpm &&
                maximumSendPeak >= minimumInstrumentPeakPpm) {
                instrumentRoutingObserved = true;
                break;
            }
            QThread::msleep(5);
        }

        const auto instrumentMessages = instrumentDevice->short_messages() -
            instrumentMessageBaseline;
        const auto instrumentUnsupported = instrumentDevice->unsupported_messages();
        const auto engineStats = engine.snapshot();
        const auto routerStats = router.stats();
        engine.requestStop();
        engine.join();
        const auto pluginStats = plugin->stats();
        instrumentDevice.reset();
        plugin->setMidiQueue(nullptr);
        plugin->requestRetire();
        plugin.reset();
        pluginWorkers.waitForDone();

        constexpr std::uint64_t isolationPipelineBlocks =
            jam2::pluginhost::kIsolationPipelineBlocks;
        const bool exactBlockAccounting =
            pluginStats.submittedBlocks >= isolationPipelineBlocks &&
            pluginStats.completedBlocks + pluginStats.deadlineMisses ==
                pluginStats.submittedBlocks - isolationPipelineBlocks;
        std::cout << "Jam2 real MIDI instrument results; midi_id=" << selected.id
                  << " midi_name=" << selected.name
                  << " plugin=" << pluginName.toStdString()
                  << " audio_device=" << audioDeviceId
                  << " frames=" << engineStats.audio_buffer_frames
                  << " callbacks=" << engineStats.callbacks
                  << " capture_messages=" << captureShortMessages
                  << " instrument_messages=" << instrumentMessages
                  << " midi_consumed=" << pluginStats.midiEventsConsumed
                  << " midi_dropped=" << instrumentQueue.dropped()
                  << " plugin_submitted=" << pluginStats.submittedBlocks
                  << " plugin_completed=" << pluginStats.completedBlocks
                  << " plugin_misses=" << pluginStats.deadlineMisses
                  << " plugin_failed=" << pluginStats.failedBlocks
                  << " plugin_stale=" << pluginStats.staleResponses
                  << " muted_proved=" << mutedRoutingObserved
                  << " instrument_proved=" << instrumentRoutingObserved
                  << " wet_plugin_peak_ppm=" << pluginStats.wetOutputPeakPpm
                  << " router_peak_ppm=" << maximumRouterPeak
                  << " send_peak_ppm=" << maximumSendPeak
                  << " callback_gaps(1.1x/1.5x/2x)="
                  << engineStats.callback_timing.gap_over_1_1x_count << '/'
                  << engineStats.callback_timing.gap_over_1_5x_count << '/'
                  << engineStats.callback_timing.gap_over_2x_count << '\n';
        if (!exactBlockAccounting || engineStats.callbacks == 0 ||
            routerStats.rendered_blocks == 0 || routerStats.renderer_failures != 0 ||
            pluginStats.completedBlocks == 0 || pluginStats.failedBlocks != 0 ||
            pluginStats.staleResponses != 0 || pluginStats.midiEventsConsumed == 0 ||
            !mutedRoutingObserved || !instrumentRoutingObserved ||
            instrumentMessages == 0 || instrumentUnsupported != 0 ||
            instrumentQueue.dropped() != 0 ||
            pluginStats.wetOutputPeakPpm < minimumInstrumentPeakPpm ||
            maximumRouterPeak < minimumInstrumentPeakPpm ||
            maximumSendPeak < minimumInstrumentPeakPpm) {
            throw std::runtime_error(
                "The physical CoreMIDI -> isolated VST3 instrument -> Scarlett "
                "callback path did not produce complete signal evidence");
        }

        std::cout << "Jam2 real CoreMIDI instrument test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

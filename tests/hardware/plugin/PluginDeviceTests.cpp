// Native hardware coverage is owned by the repository-level test tree.
#include "InputPluginBackend.hpp"
#include "PluginProtocol.hpp"

#include "engine.hpp"
#include "input_source.hpp"

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

namespace {

int integer_argument(char** argv, int index, int fallback)
{
    return argv[index] != nullptr ? std::stoi(argv[index]) : fallback;
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

} // namespace

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    if (argc < 2 || argc > 7) {
        std::cerr << "usage: jam2_plugin_device_tests <plugin.vst3> "
                     "[device-id=5] [input-channel=2] [frames=32] "
                     "[second-input-channel=0] [unsupported-buffer=0]\n";
        return 2;
    }

    try {
        const int deviceId = argc >= 3 ? integer_argument(argv, 2, 5) : 5;
        const int userInputChannel = argc >= 4 ? integer_argument(argv, 3, 2) : 2;
        const long blockFrames = argc >= 5 ? integer_argument(argv, 4, 32) : 32;
        const int secondUserInputChannel = argc >= 6 ? integer_argument(argv, 5, 0) : 0;
        const long unsupportedBufferFrames = argc >= 7 ? integer_argument(argv, 6, 0) : 0;
        if (userInputChannel < 1 || secondUserInputChannel < 0 ||
            secondUserInputChannel == userInputChannel || blockFrames < 1 ||
            unsupportedBufferFrames < 0) {
            throw std::invalid_argument("Input channels are one-based and frames must be positive");
        }
        const std::size_t sourceInputChannels =
            secondUserInputChannel > 0 ? 2U : 1U;

        const QString pluginPath = QString::fromLocal8Bit(argv[1]);
        const QString workerPath = QDir(QCoreApplication::applicationDirPath())
#ifdef _WIN32
            .absoluteFilePath(QStringLiteral("jam2-plugin-worker.exe"));
#elif defined(__APPLE__)
            .absoluteFilePath(QStringLiteral("../Helpers/jam2-plugin-worker"));
#else
            .absoluteFilePath(QStringLiteral("jam2-plugin-worker"));
#endif
        std::cerr << "phase: validate plugin worker diagnostic commands\n";
        require_worker_command(
            workerPath, {QStringLiteral("--probe"), pluginPath}, QByteArrayLiteral("\t"));
        require_worker_command(
            workerPath, {QStringLiteral("--probe-all"), pluginPath},
            QByteArrayLiteral("[bytes="));
        require_worker_command(
            workerPath, {QStringLiteral("--self-test"), pluginPath},
            QByteArrayLiteral("OK\t"));

        std::cerr << "phase: scan and start through production plugin backend\n";
        auto backend = jam2::application::makeSystemInputPluginBackend({
            pluginPath, true});
        jam2::application::InputPluginLoadRequest pluginRequest;
        pluginRequest.kind = jam2::audio::InputSourceKind::Audio;
        pluginRequest.sampleRate = 48000.0;
        pluginRequest.maximumFrames = static_cast<std::size_t>(blockFrames);
        pluginRequest.sourceInputChannels = sourceInputChannels;
        QWidget pluginOwner;
        QThreadPool pluginWorkers;
        pluginWorkers.setMaxThreadCount(2);
        std::unique_ptr<jam2::application::InputPluginHost> plugin;
        QString pluginName;
        QString pluginLoadError;
        QEventLoop pluginLoadLoop;
        QTimer pluginLoadTimeout;
        pluginLoadTimeout.setSingleShot(true);
        QObject::connect(
            &pluginLoadTimeout, &QTimer::timeout,
            &pluginLoadLoop, &QEventLoop::quit);
        const bool pluginLoadStarted = backend->selectAndStart(
            pluginOwner, pluginWorkers, application.thread(), pluginRequest,
            [&](std::unique_ptr<jam2::application::InputPluginHost> loaded,
                QString name) {
                plugin = std::move(loaded);
                pluginName = std::move(name);
                pluginLoadLoop.quit();
            },
            [&](int percent, const QString& message) {
                std::cerr << "plugin progress " << percent << ": "
                          << message.toStdString() << '\n';
                if (percent == 0) {
                    pluginLoadError = message;
                    pluginLoadLoop.quit();
                }
            });
        if (!pluginLoadStarted) {
            throw std::runtime_error("Production plugin backend rejected the hardware profile");
        }
        pluginLoadTimeout.start(120000);
        pluginLoadLoop.exec();
        if (!plugin || !plugin->healthy() || plugin->renderer() == nullptr) {
            throw std::runtime_error(pluginLoadError.isEmpty()
                ? "Plugin worker did not become healthy"
                : pluginLoadError.toStdString());
        }
        if (!plugin->errorText().isEmpty() || plugin->statusText().isEmpty()) {
            throw std::runtime_error("Plugin backend omitted healthy worker diagnostics");
        }
        plugin->setAudioBypassed(false);
        plugin->setMidiQueue(nullptr);
        plugin->setMidiMuted(false);
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
            throw std::runtime_error("Plugin editor lifecycle did not complete before audio measurement");
        }

        std::cerr << "phase: configure input router\n";
        jam2::audio::InputSourceRouter router(
            static_cast<std::size_t>(blockFrames), sourceInputChannels);
        jam2::audio::InputSourceConfiguration source;
        source.kind = jam2::audio::InputSourceKind::Audio;
        source.first_channel = 0;
        source.second_channel = secondUserInputChannel > 0
            ? 1U : jam2::audio::kNoInputChannel;
        source.enabled = true;
        source.renderer = plugin->renderer();
        if (!router.configure(0, source)) {
            throw std::runtime_error("Could not configure the plugin input source");
        }

        std::cerr << "phase: start audio device\n";
        jam2::Engine engine;
        jam2::EngineConfig config;
        config.audio_device_id = deviceId;
        config.sample_rate = 48000;
        config.audio_buffer_frames = blockFrames;
        config.input_channels = secondUserInputChannel > 0
            ? jam2::audio::InputChannels::Stereo
            : jam2::audio::InputChannels::Mono;
        config.channels.input = {userInputChannel - 1};
        if (secondUserInputChannel > 0) {
            config.channels.input.push_back(secondUserInputChannel - 1);
        }
        config.channels.output = {0, 1};
        config.diagnostics_enabled = true;
        config.local_monitor_enabled = false;
        config.input_source_router = &router;

        std::cerr << "phase: reject an out-of-range hardware channel\n";
        jam2::EngineConfig invalidChannelConfig = config;
        invalidChannelConfig.channels.input = {999};
        bool invalidChannelRejected = false;
        try {
            jam2::Engine invalidChannelEngine;
            invalidChannelEngine.start(invalidChannelConfig);
            invalidChannelEngine.requestStop();
            invalidChannelEngine.join();
        } catch (const std::exception& error) {
            invalidChannelRejected =
                std::string(error.what()).find("channel") != std::string::npos;
        }
        if (!invalidChannelRejected) {
            throw std::runtime_error(
                "The real device did not reject an out-of-range input channel");
        }

        if (unsupportedBufferFrames > 0) {
            std::cerr << "phase: reject the profiled unsupported hardware buffer\n";
            jam2::EngineConfig invalidBufferConfig = config;
            invalidBufferConfig.audio_buffer_frames = unsupportedBufferFrames;
            bool invalidBufferRejected = false;
            try {
                jam2::Engine invalidBufferEngine;
                invalidBufferEngine.start(invalidBufferConfig);
                invalidBufferEngine.requestStop();
                invalidBufferEngine.join();
            } catch (const std::exception& error) {
                invalidBufferRejected =
                    std::string(error.what()).find("buffer") != std::string::npos;
            }
            if (!invalidBufferRejected) {
                throw std::runtime_error(
                    "The real device did not reject the profiled unsupported buffer");
            }
        }

        const auto provePhysicalInput = [&](int userChannel) {
            jam2::EngineConfig channelConfig = config;
            channelConfig.input_channels = jam2::audio::InputChannels::Mono;
            channelConfig.channels.input = {userChannel - 1};
            channelConfig.input_source_router = nullptr;
            jam2::Engine channelEngine;
            channelEngine.start(channelConfig);
            int maximumPeak = 0;
            const auto inputDeadman =
                std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (std::chrono::steady_clock::now() < inputDeadman) {
                QCoreApplication::processEvents();
                const auto snapshot = channelEngine.snapshot();
                maximumPeak = std::max(maximumPeak, snapshot.input_peak_ppm);
                if (snapshot.callbacks > 0 && maximumPeak >= 100) break;
                QThread::msleep(5);
            }
            channelEngine.requestStop();
            channelEngine.join();
            if (maximumPeak < 100) {
                throw std::runtime_error(
                    "The profiled physical input channel returned no live signal");
            }
            return maximumPeak;
        };

        std::cerr << "phase: prove live signal on Scarlett input "
                  << userInputChannel << '\n';
        const int firstChannelPeak = provePhysicalInput(userInputChannel);
        int secondChannelPeak = 0;
        if (secondUserInputChannel > 0) {
            std::cerr << "phase: prove live signal on Scarlett input "
                      << secondUserInputChannel << '\n';
            secondChannelPeak = provePhysicalInput(secondUserInputChannel);
        }

        engine.start(config);

        std::cerr << "phase: provide live signal to the selected Scarlett input(s)\n";
        int maximumInputPeak = 0;
        int maximumSendPeak = 0;
        int maximumRouterPeak = 0;
        constexpr int minimumLivePeakPpm = 100;
        bool wetProcessingObserved = false;
        bool bypassSignalObserved = false;
        bool wetProcessingRecovered = false;
        std::uint64_t bypassSubmittedBaseline = 0;
        std::uint64_t recoveredCompletedBaseline = 0;
        const auto evidenceDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(45);
        while (std::chrono::steady_clock::now() < evidenceDeadline) {
            QCoreApplication::processEvents();
            const auto engineStats = engine.snapshot();
            const auto routerStats = router.stats();
            const auto currentPluginStats = plugin->stats();
            maximumInputPeak = std::max(maximumInputPeak, engineStats.input_peak_ppm);
            maximumSendPeak = std::max(maximumSendPeak, engineStats.send_peak_ppm);
            maximumRouterPeak = std::max(maximumRouterPeak, routerStats.peak_ppm);
            const bool currentSignal =
                engineStats.input_peak_ppm >= minimumLivePeakPpm &&
                engineStats.send_peak_ppm >= minimumLivePeakPpm &&
                routerStats.peak_ppm >= minimumLivePeakPpm;
            if (!wetProcessingObserved &&
                currentPluginStats.completedBlocks > 0 &&
                currentPluginStats.workerInputPeakPpm >= minimumLivePeakPpm) {
                wetProcessingObserved = true;
                plugin->setAudioBypassed(true);
                bypassSubmittedBaseline = currentPluginStats.submittedBlocks;
                std::cerr << "phase: prove live delayed-dry bypass routing\n";
            } else if (wetProcessingObserved && !bypassSignalObserved && currentSignal &&
                       currentPluginStats.submittedBlocks > bypassSubmittedBaseline) {
                bypassSignalObserved = true;
                plugin->setAudioBypassed(false);
                recoveredCompletedBaseline = currentPluginStats.completedBlocks;
                std::cerr << "phase: prove plugin processing recovers after bypass\n";
            } else if (bypassSignalObserved &&
                       currentPluginStats.completedBlocks > recoveredCompletedBaseline &&
                       currentPluginStats.workerInputPeakPpm >= minimumLivePeakPpm) {
                wetProcessingRecovered = true;
                break;
            }
            QThread::msleep(5);
        }

        std::cerr << "phase: collect and stop\n";
        const auto engineStats = engine.snapshot();
        const auto routerStats = router.stats();
        engine.requestStop();
        engine.join();
        const auto pluginStats = plugin->stats();
        plugin->requestRetire();
        plugin.reset();
        pluginWorkers.waitForDone();

        constexpr std::uint64_t isolationPipelineBlocks =
            jam2::pluginhost::kIsolationPipelineBlocks;
        const bool exactBlockAccounting =
            pluginStats.submittedBlocks >= isolationPipelineBlocks &&
            pluginStats.completedBlocks + pluginStats.deadlineMisses ==
                pluginStats.submittedBlocks - isolationPipelineBlocks;
        std::cout << "Jam2 real-device plugin test results; device=" << deviceId
                  << " inputs=" << userInputChannel;
        if (secondUserInputChannel > 0) {
            std::cout << ',' << secondUserInputChannel;
        }
        std::cout
                  << " frames=" << engineStats.audio_buffer_frames
                  << " callbacks=" << engineStats.callbacks
                  << " physical_channel_peaks_ppm=" << firstChannelPeak << ','
                  << secondChannelPeak
                  << " input_peak_ppm=" << maximumInputPeak
                  << " send_peak_ppm=" << maximumSendPeak
                  << " router_peak_ppm=" << maximumRouterPeak
                  << " worker_input_peak_ppm=" << pluginStats.workerInputPeakPpm
                  << " wet_plugin_peak_ppm=" << pluginStats.wetOutputPeakPpm
                  << " plugin=" << pluginName.toStdString()
                  << " plugin_submitted=" << pluginStats.submittedBlocks
                  << " plugin_completed=" << pluginStats.completedBlocks
                  << " plugin_misses=" << pluginStats.deadlineMisses
                  << " bypass_proved=" << bypassSignalObserved
                  << " processing_recovered=" << wetProcessingRecovered
                  << " callback_gaps(1.1x/1.5x/2x)="
                  << engineStats.callback_timing.gap_over_1_1x_count << '/'
                  << engineStats.callback_timing.gap_over_1_5x_count << '/'
                  << engineStats.callback_timing.gap_over_2x_count
                  << " process_us(last/avg/max)=" << pluginStats.workerProcessLastUs << '/'
                  << pluginStats.workerProcessAverageUs << '/'
                  << pluginStats.workerProcessMaxUs << '\n';
        if (engineStats.callbacks == 0 || routerStats.rendered_blocks == 0 ||
            pluginStats.completedBlocks == 0 || !editorWasOpen ||
            !wetProcessingObserved || !bypassSignalObserved ||
            !wetProcessingRecovered ||
            maximumInputPeak < minimumLivePeakPpm ||
            maximumSendPeak < minimumLivePeakPpm ||
            maximumRouterPeak < minimumLivePeakPpm ||
            pluginStats.workerInputPeakPpm < minimumLivePeakPpm ||
            !exactBlockAccounting || pluginStats.failedBlocks != 0 ||
            pluginStats.staleResponses != 0) {
            throw std::runtime_error(
                "The real device, isolated plugin input/process, bypass route, "
                "or block accounting evidence failed");
        }
        std::cout << "Jam2 real-device plugin test passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

// Native hardware coverage is owned by the repository-level test tree.
#include "InputPluginBackend.hpp"

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
    if (!process.waitForStarted(5000) || !process.waitForFinished(60000)) {
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
    if (argc < 2 || argc > 5) {
        std::cerr << "usage: jam2_plugin_device_tests <plugin.vst3> "
                     "[device-id=5] [input-channel=2] [frames=32]\n";
        return 2;
    }

    try {
        const int deviceId = argc >= 3 ? integer_argument(argv, 2, 5) : 5;
        const int userInputChannel = argc >= 4 ? integer_argument(argv, 3, 2) : 2;
        const long blockFrames = argc >= 5 ? integer_argument(argv, 4, 32) : 32;
        if (userInputChannel < 1 || blockFrames < 1) {
            throw std::invalid_argument("Input channels are one-based and frames must be positive");
        }

        const QString pluginPath = QString::fromLocal8Bit(argv[1]);
        const QString workerPath = QDir(QCoreApplication::applicationDirPath())
#ifdef _WIN32
            .absoluteFilePath(QStringLiteral("jam2-plugin-worker.exe"));
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
        pluginRequest.sourceInputChannels = 1;
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
        pluginLoadTimeout.start(60000);
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
        for (int attempt = 0; attempt < 200 && !plugin->editorOpen(); ++attempt) {
            QCoreApplication::processEvents();
            QThread::msleep(10);
        }
        const bool editorWasOpen = plugin->editorOpen();
        plugin->closeEditor();
        for (int attempt = 0; attempt < 200 && plugin->editorOpen(); ++attempt) {
            QCoreApplication::processEvents();
            QThread::msleep(10);
        }
        if (!editorWasOpen || plugin->editorOpen()) {
            throw std::runtime_error("Plugin editor lifecycle did not complete before audio measurement");
        }

        std::cerr << "phase: configure input router\n";
        jam2::audio::InputSourceRouter router(
            static_cast<std::size_t>(blockFrames), 1U);
        jam2::audio::InputSourceConfiguration source;
        source.kind = jam2::audio::InputSourceKind::Audio;
        source.first_channel = 0;
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
        config.input_channels = jam2::audio::InputChannels::Mono;
        config.channels.input = {userInputChannel - 1};
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

        engine.start(config);

        std::cerr << "phase: run real callbacks for five seconds\n";
        int maximumInputPeak = 0;
        int maximumSendPeak = 0;
        int maximumRouterPeak = 0;
        constexpr auto warmupWindow = std::chrono::milliseconds(500);
        constexpr std::uint64_t maximumWarmupMisses = 2;
        std::uint64_t missesAfterWarmup = 0;
        bool sampledWarmup = false;
        const auto started = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - started < std::chrono::seconds(5)) {
            QCoreApplication::processEvents();
            const auto engineStats = engine.snapshot();
            const auto routerStats = router.stats();
            maximumInputPeak = std::max(maximumInputPeak, engineStats.input_peak_ppm);
            maximumSendPeak = std::max(maximumSendPeak, engineStats.send_peak_ppm);
            maximumRouterPeak = std::max(maximumRouterPeak, routerStats.peak_ppm);
            if (!sampledWarmup &&
                std::chrono::steady_clock::now() - started >= warmupWindow) {
                missesAfterWarmup = plugin->stats().deadlineMisses;
                sampledWarmup = true;
            }
            QThread::msleep(10);
        }

        std::cerr << "phase: collect and stop\n";
        const auto engineStats = engine.snapshot();
        const auto routerStats = router.stats();
        const auto pluginStats = plugin->stats();
        engine.requestStop();
        engine.join();
        plugin->requestRetire();
        plugin.reset();
        pluginWorkers.waitForDone();

        if (engineStats.callbacks == 0 || routerStats.rendered_blocks == 0 ||
            pluginStats.completedBlocks == 0 || !editorWasOpen) {
            throw std::runtime_error("The real device/plugin callback path did not run");
        }

        std::cout << "Jam2 real-device plugin test passed; device=" << deviceId
                  << " input=" << userInputChannel
                  << " frames=" << engineStats.audio_buffer_frames
                  << " callbacks=" << engineStats.callbacks
                  << " input_peak_ppm=" << maximumInputPeak
                  << " send_peak_ppm=" << maximumSendPeak
                  << " plugin_peak_ppm=" << maximumRouterPeak
                  << " plugin=" << pluginName.toStdString()
                  << " plugin_completed=" << pluginStats.completedBlocks
                  << " plugin_misses=" << pluginStats.deadlineMisses
                  << " warmup_misses=" << missesAfterWarmup
                  << " steady_misses=" <<
                         (pluginStats.deadlineMisses - missesAfterWarmup)
                  << " callback_gaps(1.1x/1.5x/2x)="
                  << engineStats.callback_timing.gap_over_1_1x_count << '/'
                  << engineStats.callback_timing.gap_over_1_5x_count << '/'
                  << engineStats.callback_timing.gap_over_2x_count
                  << " process_us(last/avg/max)=" << pluginStats.workerProcessLastUs << '/'
                  << pluginStats.workerProcessAverageUs << '/'
                  << pluginStats.workerProcessMaxUs << '\n';
        return sampledWarmup && missesAfterWarmup <= maximumWarmupMisses &&
                pluginStats.deadlineMisses == missesAfterWarmup
            ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

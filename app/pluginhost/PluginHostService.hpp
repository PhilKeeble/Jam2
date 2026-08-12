#pragma once

#include "PluginAudioBridge.hpp"
#include "PluginSharedMemory.hpp"

#include <QProcess>
#include <QThread>

#include <memory>
#include <string>

namespace jam2::pluginhost {

class PluginHostService final {
public:
    PluginHostService();
    ~PluginHostService();
    PluginHostService(const PluginHostService&) = delete;
    PluginHostService& operator=(const PluginHostService&) = delete;

    void start(const std::string& plugin_path, const std::string& class_id,
        double sample_rate, std::size_t maximum_frames,
        jam2::audio::InputSourceKind kind, std::size_t source_input_channels);
    void stop() noexcept;
    // Signals the worker without waiting for process exit. This is the only
    // shutdown operation used by interactive GUI actions.
    void requestRetire() noexcept;
    // start() may run on a background thread. Move QProcess back to the GUI
    // thread before publishing the service to MainWindow.
    void moveProcessToThread(QThread* thread) noexcept;
    PluginAudioBridge* bridge() noexcept { return bridge_.get(); }
    bool healthy() const noexcept;
    void openEditor() noexcept;
    void closeEditor() noexcept;
    bool editorOpen() const noexcept;
    QString statusText() const;
    QString errorText() const;

    static QString workerExecutablePath();

private:
    QProcess process_;
    PluginSharedMemory memory_;
    std::unique_ptr<PluginAudioBridge> bridge_;
    QString error_;
};

} // namespace jam2::pluginhost

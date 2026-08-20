#include "PluginHostService.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <stdexcept>

namespace jam2::pluginhost {

PluginHostService::PluginHostService()
{
    // Runtime status travels through the bounded shared state. Do not leave
    // plugin stdout/stderr pipes that can fill, inherit fragile handles, or
    // create another blocking dependency between the two processes.
    process_.setProcessChannelMode(QProcess::ForwardedChannels);
    process_.setInputChannelMode(QProcess::ForwardedInputChannel);
}

PluginHostService::~PluginHostService() { stop(); }

QString PluginHostService::workerExecutablePath()
{
#ifdef Q_OS_MACOS
    return QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(
        QStringLiteral("../Helpers/jam2-plugin-worker"));
#else
    const QDir directory(QCoreApplication::applicationDirPath());
    const QString staged = directory.absoluteFilePath(
        QStringLiteral("components/pluginhost/jam2-plugin-worker.exe"));
    return QFileInfo::exists(staged) ? staged :
        directory.absoluteFilePath(QStringLiteral("jam2-plugin-worker.exe"));
#endif
}

void PluginHostService::start(const std::string& path, const std::string& class_id,
    double sample_rate, std::size_t maximum_frames,
    jam2::audio::InputSourceKind kind, std::size_t source_input_channels)
{
    stop();
    error_.clear();
    if (maximum_frames == 0 || maximum_frames > kMaximumFrames)
        throw std::invalid_argument("Plugin block size exceeds the Jam2 transport limit");
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    memory_ = PluginSharedMemory::create(token.toStdString());
    bridge_ = std::make_unique<PluginAudioBridge>(*memory_.get(), maximum_frames, kind);
    process_.start(workerExecutablePath(), {
        QStringLiteral("--run"), token, QString::fromStdString(path),
        QString::fromStdString(class_id), QString::number(sample_rate, 'g', 17),
        QString::number(maximum_frames), QString::number(source_input_channels),
    });
    if (!process_.waitForStarted(60000)) {
        error_ = process_.errorString();
        stop();
        throw std::runtime_error("Could not start isolated plugin worker: " + error_.toStdString());
    }
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 120000) {
        const auto state = memory_.get()->worker_state.load(std::memory_order_acquire);
        if (state == WorkerState::Ready) return;
        if (state == WorkerState::Failed || process_.state() == QProcess::NotRunning) break;
        QThread::msleep(5);
    }
    error_ = QString::fromUtf8(memory_.get()->status_text.data());
    if (error_.isEmpty()) error_ = process_.errorString();
    stop();
    throw std::runtime_error("Plugin worker initialization failed: " + error_.toStdString());
}

void PluginHostService::stop() noexcept
{
    if (memory_) memory_.get()->shutdown.store(true, std::memory_order_release);
    if (process_.state() != QProcess::NotRunning) {
        if (!process_.waitForFinished(2000)) {
            process_.terminate();
            if (!process_.waitForFinished(1000)) {
                process_.kill();
                (void)process_.waitForFinished(1000);
            }
        }
    }
    bridge_.reset();
    memory_ = {};
}

void PluginHostService::requestRetire() noexcept
{
    if (memory_) memory_.get()->shutdown.store(true, std::memory_order_release);
    // A well-behaved worker observes shutdown immediately. A plugin hung
    // inside process() cannot do so, therefore escalate asynchronously without
    // ever holding the GUI event loop.
    QTimer::singleShot(2000, &process_, [this] {
        if (process_.state() == QProcess::NotRunning) return;
        process_.terminate();
        QTimer::singleShot(1000, &process_, [this] {
            if (process_.state() != QProcess::NotRunning) process_.kill();
        });
    });
}

void PluginHostService::moveProcessToThread(QThread* thread) noexcept
{
    if (thread != nullptr && process_.thread() != thread)
        process_.moveToThread(thread);
}

bool PluginHostService::healthy() const noexcept
{
    return memory_ && process_.state() != QProcess::NotRunning &&
        memory_.get()->worker_state.load(std::memory_order_acquire) == WorkerState::Ready;
}

void PluginHostService::openEditor() noexcept
{
    if (memory_ && healthy())
        memory_.get()->editor_command.store(1U, std::memory_order_release);
}

void PluginHostService::closeEditor() noexcept
{
    if (memory_)
        memory_.get()->editor_command.store(2U, std::memory_order_release);
}

bool PluginHostService::editorOpen() const noexcept
{
    return memory_ && memory_.get()->editor_state.load(std::memory_order_acquire) == 1U;
}

QString PluginHostService::statusText() const
{
    return memory_ ? QString::fromUtf8(memory_.get()->status_text.data()) : QString{};
}

QString PluginHostService::errorText() const { return error_; }

} // namespace jam2::pluginhost

#include "Jam2Process.hpp"

#include <QCoreApplication>
#include <QMetaObject>

#include <iostream>
#include <mutex>
#include <streambuf>
#include <string>
#include <vector>

namespace {

class LineStreamBuffer final : public std::streambuf {
public:
    explicit LineStreamBuffer(std::function<void(const QString&)> deliver)
        : deliver_(std::move(deliver))
    {
    }

    ~LineStreamBuffer() override
    {
        flushLine();
    }

protected:
    int overflow(int value) override
    {
        if (value == traits_type::eof()) {
            flushLine();
            return traits_type::not_eof(value);
        }
        const char c = static_cast<char>(value);
        std::lock_guard<std::mutex> lock(mutex_);
        if (c == '\n') {
            emitLocked();
        } else if (c != '\r') {
            pending_.push_back(c);
        }
        return value;
    }

    int sync() override
    {
        flushLine();
        return 0;
    }

private:
    void flushLine()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        emitLocked();
    }

    void emitLocked()
    {
        if (pending_.empty()) {
            return;
        }
        const QString line = QString::fromUtf8(pending_);
        pending_.clear();
        deliver_(line);
    }

    std::function<void(const QString&)> deliver_;
    std::mutex mutex_;
    std::string pending_;
};

} // namespace

Jam2Process::Jam2Process(QObject* parent)
    : QObject(parent)
{
    host_.control_frame = [this](std::uint16_t type, const std::vector<std::uint8_t>& payload) {
        const QByteArray bytes(
            reinterpret_cast<const char*>(payload.data()),
            static_cast<qsizetype>(payload.size()));
        QMetaObject::invokeMethod(this, [this, type, bytes] {
            if (onControlFrame) {
                onControlFrame(type, bytes);
            }
        }, Qt::QueuedConnection);
    };
}

Jam2Process::~Jam2Process()
{
    stop();
    jam2_cli_shutdown_embedded_engine();
}

bool Jam2Process::isRunning() const
{
    return running_.load(std::memory_order_acquire);
}

void Jam2Process::start(const QString& program, const QStringList& arguments, bool installEmbeddedHost)
{
    (void)program;
    if (isRunning()) {
        return;
    }
    if (worker_.joinable()) {
        worker_.join();
    }

    std::vector<std::string> storage;
    storage.reserve(static_cast<std::size_t>(arguments.size()) + 1U);
    storage.push_back(QCoreApplication::applicationFilePath().toUtf8().toStdString());
    for (const QString& argument : arguments) {
        storage.push_back(argument.toUtf8().toStdString());
    }

    running_.store(true, std::memory_order_release);
    worker_ = std::thread([this, storage = std::move(storage), installEmbeddedHost]() mutable {
        std::vector<char*> argv;
        argv.reserve(storage.size());
        for (std::string& value : storage) {
            argv.push_back(value.data());
        }

        LineStreamBuffer stdoutBuffer([this](const QString& line) { deliverOutput(line, false); });
        LineStreamBuffer stderrBuffer([this](const QString& line) { deliverOutput(line, true); });
        std::streambuf* previousOut = std::cout.rdbuf(&stdoutBuffer);
        std::streambuf* previousErr = std::cerr.rdbuf(&stderrBuffer);
        if (installEmbeddedHost) {
            jam2_cli_set_host(&host_);
        }
        const int exitCode = jam2_cli_main(static_cast<int>(argv.size()), argv.data());
        if (installEmbeddedHost) {
            jam2_cli_set_host(nullptr);
        }
        std::cout.rdbuf(previousOut);
        std::cerr.rdbuf(previousErr);
        running_.store(false, std::memory_order_release);
        QMetaObject::invokeMethod(this, [this, exitCode] {
            if (onFinished) {
                onFinished(exitCode);
            }
        }, Qt::QueuedConnection);
    });
}

void Jam2Process::stop()
{
    if (isRunning()) {
        jam2_cli_request_stop();
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    running_.store(false, std::memory_order_release);
}

bool Jam2Process::sendControl(const QByteArray& payload)
{
    return jam2_cli_submit_control(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(payload.constData()),
        static_cast<std::size_t>(payload.size())));
}

bool Jam2Process::updatePeers(const QStringList& peers)
{
    std::vector<std::string> values;
    values.reserve(static_cast<std::size_t>(peers.size()));
    for (const QString& peer : peers) {
        values.push_back(peer.toUtf8().toStdString());
    }
    return jam2_cli_update_peers(values);
}

void Jam2Process::deliverOutput(const QString& line, bool error)
{
    QMetaObject::invokeMethod(this, [this, line, error] {
        if (error) {
            if (onErrorLine && !line.isEmpty()) {
                onErrorLine(line);
            }
        } else {
            if (onOutputLine && !line.isEmpty()) {
                onOutputLine(line);
            }
        }
    }, Qt::QueuedConnection);
}

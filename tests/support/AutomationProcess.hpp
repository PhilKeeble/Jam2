#pragma once

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

class AutomationProcess final {
public:
    static std::unique_ptr<AutomationProcess> launch(
        const QString& executable,
        const QStringList& arguments,
        QString& error);

    ~AutomationProcess();

    AutomationProcess(const AutomationProcess&) = delete;
    AutomationProcess& operator=(const AutomationProcess&) = delete;

    bool send(QJsonObject command, QString& error);
    bool readEvent(QJsonObject& event, std::chrono::milliseconds timeout, QString& error);
    bool waitForExit(std::chrono::milliseconds timeout, int& exitCode, QString& error);
    bool isRunning() const noexcept;

private:
    AutomationProcess() = default;

    bool readExact(void* output, std::size_t bytes,
        std::chrono::steady_clock::time_point deadline, QString& error);
    void stopForCleanup() noexcept;
    void closePipes() noexcept;

#if defined(_WIN32)
    std::uintptr_t processHandle_ = 0;
    std::uintptr_t commandWrite_ = 0;
    std::uintptr_t eventRead_ = 0;
#else
    int processId_ = -1;
    int commandWrite_ = -1;
    int eventRead_ = -1;
#endif
    bool exited_ = false;
    int exitCode_ = -1;
};

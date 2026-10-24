#pragma once

#include <QString>
#include <memory>

class QProcess;

namespace jam2::update {
// Holds the original process handle on Windows, avoiding PID-reuse races.
class ParentProcess final {
public:
    ParentProcess(qint64 pid, const QString& expectedExecutable);
    ~ParentProcess();
    bool exited() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
void preflightWritableFile(const QString& path);
void configureHelperProcess(QProcess& process);
void verifyMacSignature(const QString& bundle);
} // namespace jam2::update

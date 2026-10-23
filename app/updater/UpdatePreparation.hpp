#pragma once
#include "UpdatePackage.hpp"
#include <QObject>
#include <QProcess>
#include <atomic>
#include <functional>
#include <memory>
class QThread;

namespace jam2::update {
enum class PreparationPhase { Staging, Downloading, Unpacking };
struct PreparationStatus { PreparationPhase phase; QString message; int percent = -1; };
// The helper exits after preparation. Installation is launched only when Jam2
// actually exits, so a ready update can wait indefinitely without a process.
class Preparation : public QObject {
public:
    explicit Preparation(QString root, QObject* parent = nullptr);
    ~Preparation() override;
    virtual void start(const Metadata& metadata, const QByteArray& encoded, bool staged = false);
    virtual void cancel();
    virtual bool active() const;
    std::function<void(const PreparationStatus&)> statusChanged;
    std::function<void(const QString&, bool)> finished;
private:
    void consumeOutput();
    void end(const QString& error);
    void cleanupCancelled();
    QString root_;
    Metadata metadata_;
    QProcess process_;
    QByteArray output_;
    QByteArray errors_;
    std::unique_ptr<QThread> worker_;
    std::atomic_bool cancelled_{false};
    bool ending_ = false;
};
}

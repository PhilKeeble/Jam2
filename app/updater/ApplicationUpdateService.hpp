#pragma once
#include "UpdateCheck.hpp"
#include "UpdateLauncher.hpp"
#include "UpdatePreparation.hpp"
#include <QObject>
#include <functional>
#include <memory>

namespace jam2::update {
enum class State { Idle, Checking, Current, Available, Preparing, Downloading, Unpacking,
    Ready, Restarting, Cancelling, Cancelled, Failed };
struct Status {
    State state = State::Idle;
    QString installedVersion;
    QString availableVersion;
    QString message;
    qint64 bytes = 0;
    int percent = -1;
    bool installOnExit = false;
};

class ApplicationUpdateService final : public QObject {
public:
    using Installer = std::function<qint64(const LaunchOptions&)>;
    ApplicationUpdateService(QString root, QString installedVersion, QObject* parent = nullptr,
        QNetworkAccessManager* network = nullptr, Preparation* preparation = nullptr, Installer installer = {});
    ~ApplicationUpdateService() override;
    const Status& status() const { return status_; }
    void check();
    void download();
    void cancel();
    void defer();
    void requestRestart(std::function<bool()> closeWindow);
    QString windowClosing();
    std::function<void()> statusChanged;
private:
    void set(State state, QString message, int percent = -1);
    QString root_;
    Status status_;
    Check checker_;
    std::unique_ptr<Preparation> ownedPreparation_;
    Preparation* preparation_;
    Installer installer_;
    CheckResult available_;
    bool restart_ = false;
    bool launched_ = false;
};
}

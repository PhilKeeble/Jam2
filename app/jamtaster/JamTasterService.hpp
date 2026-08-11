#pragma once

#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QString>

#include <cstdint>
#include <functional>
#include <map>

class JamTasterService final : public QObject {
public:
    static constexpr int kProtocolVersion = 1;

    struct Observer {
        std::function<void(const QString&)> log;
        std::function<void(const QString&)> jobStarted;
        std::function<void(const QJsonObject&)> jobProgress;
        std::function<void(const QJsonObject&)> jobFinished;
        std::function<void(const QString&)> jobFailed;
        std::function<void(const QString&, int, bool)> taskStatusChanged;
        std::function<void()> taskCancelled;
    };

    explicit JamTasterService(QObject* parent = nullptr);
    ~JamTasterService() override;

    std::uint64_t addObserver(Observer observer);
    void removeObserver(std::uint64_t id);

    QString bundleRoot() const;
    QString workerPath() const;
    QString modelsPath() const;
    QString bundleStatus() const;
    QString storageSummary() const;
    bool isAvailable() const;
    bool isBusy() const;
    bool taskActive() const;
    QString taskStatusText() const;
    int taskProgress() const;
    QString taskInputPath() const;
    QString taskProjectRoot() const;
    QString taskDisplayName() const;
    QJsonObject lastJobResult() const;

    bool startJob(const QJsonObject& request, QString& error);
    void cancelTask();

private:
    bool validateBundle(QString& error) const;
    void handleJobOutput();
    void handleJobFinished(int exitCode);
    void failJob(const QString& message);
    void finishCancelledTask();
    void setTaskStatus(const QString& message, int percent, bool active);
    void removeActiveRequest();
    void stopProcessTree();

    template <typename Callback>
    void notify(Callback callback)
    {
        const auto observers = observers_;
        for (const auto& [id, observer] : observers) {
            (void)id;
            callback(observer);
        }
    }

    QProcess jobProcess_;
    QByteArray jobOutput_;
    QJsonObject lastJobResult_;
    QString activeRequestPath_;
    QString taskStatusText_ = QStringLiteral("Ready");
    QString taskInputPath_;
    QString taskProjectRoot_;
    QString taskDisplayName_;
    int taskProgress_ = 0;
    bool taskActive_ = false;
    bool cancelRequested_ = false;
    std::uint64_t nextObserverId_ = 1;
    std::map<std::uint64_t, Observer> observers_;
};

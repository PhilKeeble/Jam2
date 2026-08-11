#pragma once

#include <QFile>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QThread;

class JamTasterService final : public QObject {
    Q_OBJECT

public:
    static constexpr int kProtocolVersion = 1;
    static constexpr const char* kComponentVersion = "1.0.0";

    explicit JamTasterService(QObject* parent = nullptr);
    ~JamTasterService() override;

    QString componentRoot() const;
    QString componentVersion() const;
    QString componentStatus() const;
    QString lastHealthSummary() const;
    bool isInstalled() const;
    bool isBusy() const;
    bool jobRunning() const;
    bool taskActive() const;
    QString taskStatusText() const;
    int taskProgress() const;
    QString taskInputPath() const;
    QString taskProjectRoot() const;
    QString taskDisplayName() const;
    QJsonObject lastJobResult() const;
    QString storageSummary() const;

    void install();
    void repair();
    void checkHealth();
    bool remove(QString& error);
    QString acceleratorRecommendation() const;

    bool startJob(const QJsonObject& request, QString& error);
    void cancelTask();
    void refreshStorageUsage();

signals:
    void componentChanged();
    void componentProgress(const QString& message, int percent);
    void componentLog(const QString& message);
    void componentFailed(const QString& message);
    void healthFinished(const QJsonObject& health);
    void taskStatusChanged(const QString& message, int percent, bool active);
    void taskCancelled();
    void storageUsageChanged(const QString& summary);

    void jobStarted(const QString& action);
    void jobProgress(const QJsonObject& event);
    void jobFinished(const QJsonObject& result);
    void jobFailed(const QString& message);

private:
    enum class ComponentOperation {
        None,
        DevelopmentInstall,
        DevelopmentRepair,
        ModelRepair,
        Health,
        DownloadManifest,
        DownloadArchive,
        ExtractArchive,
    };

    QString manifestPath() const;
    QString developmentEntryPoint() const;
    QJsonObject readManifest() const;
    bool resolveWorker(QString& program, QStringList& arguments, QString& error) const;
    void startDevelopmentInstall(bool cpuOnly);
    void startDevelopmentRepair();
    void startTargetedRepair(const QJsonObject& health);
    void startModelRepair();
    void installVariant(bool cpuOnly, const QString& reason, bool repairing = false);
    void requestInstall(bool repairing);
    void startAcceleratorDetection();
    void handleAcceleratorProbeFinished(int exitCode);
    void finishAcceleratorDetection(const QString& recommendation, bool cpuOnly);
    void startReleaseInstall();
    void downloadReleaseManifest();
    void downloadArchive(const QJsonObject& release);
    void extractDownloadedArchive();
    void startComponentProcess(
        const QString& program,
        const QStringList& arguments,
        ComponentOperation operation);
    void handleComponentFinished(int exitCode);
    void handleComponentOutput();
    void handleJobOutput();
    void handleJobFinished(int exitCode);
    void startHealthCheck(bool installContinuation);
    bool publishOperationRoot(QString& error);
    void scheduleRemoveTree(const QString& path);
    void publishComponentProgress(const QString& message, int percent);
    void setTaskStatus(const QString& message, int percent, bool active);
    void finishCancelledTask();
    static void stopProcessTree(QProcess* process);
    void failComponent(const QString& message);
    void clearDownload();
    bool safeRemoveComponentRoot(QString& error);
    bool safeRemovePath(const QString& path, QString& error);
    QString platformPackageName() const;

    QProcess* componentProcess_ = nullptr;
    QProcess* jobProcess_ = nullptr;
    QProcess* acceleratorProbe_ = nullptr;
    QNetworkAccessManager* network_ = nullptr;
    QPointer<QNetworkReply> downloadReply_;
    QPointer<QThread> storageThread_;
    QFile downloadFile_;
    ComponentOperation componentOperation_ = ComponentOperation::None;
    QByteArray componentOutput_;
    QByteArray componentAllOutput_;
    QByteArray jobOutput_;
    QJsonObject pendingRelease_;
    QJsonObject lastJobResult_;
    QString downloadedArchive_;
    QString activeRequestPath_;
    QString lastHealthSummary_;
    QString taskStatusText_ = QStringLiteral("Ready");
    QString taskInputPath_;
    QString taskProjectRoot_;
    QString taskDisplayName_;
    QString operationRoot_;
    QString storageSummary_;
    int taskProgress_ = 0;
    bool taskActive_ = false;
    bool cancelRequested_ = false;
    bool cancelRemovesPartialInstall_ = false;
    bool cancelRemovesJobWorking_ = false;
    QString acceleratorRecommendation_;
    bool acceleratorDetected_ = false;
    bool acceleratorDetectionRunning_ = false;
    bool acceleratorDetectionTimedOut_ = false;
    bool recommendedCpuOnly_ = true;
    bool pendingInstallation_ = false;
    bool pendingRepair_ = false;
    bool requestedCpuOnly_ = true;
    bool repairing_ = false;
    bool healthInstallContinuation_ = false;
    bool repairHealthCheck_ = false;
    QElapsedTimer downloadTimer_;
};

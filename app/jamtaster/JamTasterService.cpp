#include "JamTasterService.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QOperatingSystemVersion>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUuid>

namespace {

QString releaseManifestUrl(const QString& packageName)
{
    return QStringLiteral(
        "https://github.com/PhilKeeble/Jam2/releases/download/"
        "jamtaster-%1/%2.json")
        .arg(QString::fromLatin1(JamTasterService::kComponentVersion), packageName);
}

QString cleanLine(QString value)
{
    value.replace(QLatin1Char('\r'), QLatin1Char(' '));
    value.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return value.simplified();
}

QString componentFailureDetail(const QByteArray& output)
{
    const QList<QByteArray> lines = output.split('\n');
    QString fallback;
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        const QString line = cleanLine(QString::fromUtf8(*it));
        if (line.isEmpty()) continue;
        const QJsonDocument structured = QJsonDocument::fromJson(it->trimmed());
        if (structured.isObject()) continue;
        if (line.startsWith(QStringLiteral("JamTaster error:"), Qt::CaseInsensitive)) {
            return line.mid(QStringLiteral("JamTaster error:").size()).trimmed().left(800);
        }
        if (fallback.isEmpty()) fallback = line.left(800);
    }
    return fallback;
}

}

JamTasterService::JamTasterService(QObject* parent)
    : QObject(parent)
    , componentProcess_(new QProcess(this))
    , jobProcess_(new QProcess(this))
    , acceleratorProbe_(new QProcess(this))
    , network_(new QNetworkAccessManager(this))
{
    componentProcess_->setProcessChannelMode(QProcess::MergedChannels);
    connect(componentProcess_, &QProcess::readyReadStandardOutput,
            this, &JamTasterService::handleComponentOutput);
    connect(componentProcess_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus) {
                handleComponentFinished(exitCode);
            });
    connect(componentProcess_, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart ||
                    componentOperation_ == ComponentOperation::None) return;
                componentOperation_ = ComponentOperation::None;
                failComponent(QStringLiteral(
                    "Could not start the JamTaster component operation: %1")
                    .arg(componentProcess_->errorString()));
            });
    connect(jobProcess_, &QProcess::readyReadStandardOutput,
            this, &JamTasterService::handleJobOutput);
    connect(jobProcess_, &QProcess::readyReadStandardError, this, [this] {
        const QString detail = cleanLine(QString::fromUtf8(jobProcess_->readAllStandardError()));
        if (!detail.isEmpty()) {
            emit jobProgress(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("log")},
                {QStringLiteral("message"), detail},
            });
        }
    });
    connect(jobProcess_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus) {
                handleJobFinished(exitCode);
            });
    connect(jobProcess_, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart) return;
                if (!activeRequestPath_.isEmpty()) {
                    (void)QFile::remove(activeRequestPath_);
                    activeRequestPath_.clear();
                }
                const QString message = QStringLiteral(
                    "Could not start the isolated JamTaster worker: %1")
                    .arg(jobProcess_->errorString());
                setTaskStatus(message, 0, false);
                emit jobFailed(message);
            });
    acceleratorProbe_->setProcessChannelMode(QProcess::MergedChannels);
    connect(acceleratorProbe_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus) {
                handleAcceleratorProbeFinished(exitCode);
            });
    connect(acceleratorProbe_, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart && acceleratorDetectionRunning_) {
                    finishAcceleratorDetection(
                        QStringLiteral(
                            "CPU runtime selected: NVIDIA capability detection could not start."),
                        true);
                }
            });
    QTimer::singleShot(0, this, &JamTasterService::startAcceleratorDetection);
    refreshStorageUsage();
}

JamTasterService::~JamTasterService()
{
    if (storageThread_) {
        storageThread_->requestInterruption();
        storageThread_->wait();
    }
    if (jobProcess_->state() != QProcess::NotRunning) {
        jobProcess_->kill();
        jobProcess_->waitForFinished(3000);
    }
    if (componentProcess_->state() != QProcess::NotRunning) {
        componentProcess_->kill();
        componentProcess_->waitForFinished(3000);
    }
    if (acceleratorProbe_->state() != QProcess::NotRunning) {
        acceleratorProbe_->kill();
        acceleratorProbe_->waitForFinished(1000);
    }
    if (!activeRequestPath_.isEmpty()) {
        (void)QFile::remove(activeRequestPath_);
    }
}

QString JamTasterService::componentRoot() const
{
    const QString base = QDir(QStandardPaths::writableLocation(
        QStandardPaths::GenericDataLocation)).absoluteFilePath(QStringLiteral("Jam2"));
    return QDir(base).absoluteFilePath(
        QStringLiteral("components/jamtaster/%1").arg(
            QString::fromLatin1(kComponentVersion)));
}

QString JamTasterService::manifestPath() const
{
    return QDir(componentRoot()).absoluteFilePath(QStringLiteral("component.json"));
}

QString JamTasterService::componentVersion() const
{
    return readManifest().value(QStringLiteral("version")).toString();
}

QString JamTasterService::componentStatus() const
{
    if (taskActive_ && (componentProcess_->state() != QProcess::NotRunning || downloadReply_))
        return taskStatusText_;
    if (!isInstalled()) return QStringLiteral("Not installed");
    if (!lastHealthSummary_.isEmpty()) return lastHealthSummary_;
    return QStringLiteral("Installed");
}

QString JamTasterService::lastHealthSummary() const
{
    return lastHealthSummary_;
}

bool JamTasterService::isInstalled() const
{
    QString program;
    QStringList arguments;
    QString error;
    return resolveWorker(program, arguments, error);
}

bool JamTasterService::isBusy() const
{
    return componentProcess_->state() != QProcess::NotRunning ||
        jobProcess_->state() != QProcess::NotRunning || downloadReply_;
}

bool JamTasterService::jobRunning() const
{
    return jobProcess_->state() != QProcess::NotRunning;
}

bool JamTasterService::taskActive() const
{
    return taskActive_;
}

QString JamTasterService::taskStatusText() const
{
    return taskStatusText_;
}

int JamTasterService::taskProgress() const
{
    return taskProgress_;
}

QString JamTasterService::taskInputPath() const
{
    return taskInputPath_;
}

QString JamTasterService::taskProjectRoot() const
{
    return taskProjectRoot_;
}

QString JamTasterService::taskDisplayName() const
{
    return taskDisplayName_;
}

QJsonObject JamTasterService::lastJobResult() const
{
    return lastJobResult_;
}

QString JamTasterService::storageSummary() const
{
    if (!storageSummary_.isEmpty()) return storageSummary_;
#ifdef Q_OS_WIN
    return QStringLiteral(
        "Estimated installed size: about 1.5 GB for CPU or 5 GB for NVIDIA CUDA. "
        "The exact measured size appears after installation.");
#else
    return QStringLiteral(
        "Estimated installed size: about 1.5 GB. The exact measured size appears "
        "after installation.");
#endif
}

QJsonObject JamTasterService::readManifest() const
{
    QFile file(manifestPath());
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) return {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) return {};
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("format")).toString() !=
            QStringLiteral("jamtaster-component-v1") ||
        object.value(QStringLiteral("protocol")).toInt() != kProtocolVersion ||
        object.value(QStringLiteral("version")).toString() !=
            QString::fromLatin1(kComponentVersion)) {
        return {};
    }
    return object;
}

QString JamTasterService::developmentEntryPoint() const
{
    const QDir app(QCoreApplication::applicationDirPath());
    const QStringList candidates{
        app.absoluteFilePath(QStringLiteral("../app/jamtaster/worker/JamTaster.py")),
        app.absoluteFilePath(QStringLiteral("../../app/jamtaster/worker/JamTaster.py")),
    };
    for (const QString& candidate : candidates) {
        if (QFileInfo(candidate).isFile()) return QFileInfo(candidate).absoluteFilePath();
    }
    return {};
}

bool JamTasterService::resolveWorker(
    QString& program,
    QStringList& arguments,
    QString& error) const
{
    const QJsonObject manifest = readManifest();
    if (manifest.isEmpty()) {
        error = QStringLiteral("JamTaster is not installed or is incompatible.");
        return false;
    }
    const QString packagedWorker = manifest.value(QStringLiteral("worker")).toString();
    if (!packagedWorker.isEmpty()) {
        program = QDir(componentRoot()).absoluteFilePath(packagedWorker);
        if (!QFileInfo(program).isFile()) {
            error = QStringLiteral("The packaged JamTaster worker is missing.");
            return false;
        }
        return true;
    }
    program = manifest.value(QStringLiteral("python")).toString();
    const QString entry = manifest.value(QStringLiteral("entry_point")).toString();
    if (!QFileInfo(program).isFile() || !QFileInfo(entry).isFile()) {
        error = QStringLiteral("The private JamTaster runtime or entry point is missing.");
        return false;
    }
    arguments << entry << QStringLiteral("--component-root") << componentRoot();
    return true;
}

QString JamTasterService::acceleratorRecommendation() const
{
    if (acceleratorDetected_) return acceleratorRecommendation_;
    return acceleratorDetectionRunning_
        ? QStringLiteral("Detecting the compatible JamTaster processing runtime...")
        : QStringLiteral("The compatible processing runtime will be detected automatically.");
}

void JamTasterService::startAcceleratorDetection()
{
    if (acceleratorDetected_ || acceleratorDetectionRunning_) return;
    acceleratorDetectionRunning_ = true;
    acceleratorDetectionTimedOut_ = false;
#ifdef Q_OS_WIN
    const QString smi = QStandardPaths::findExecutable(QStringLiteral("nvidia-smi"));
    if (smi.isEmpty()) {
        finishAcceleratorDetection(
            QStringLiteral("CPU runtime selected: no NVIDIA CUDA driver was detected."),
            true);
        return;
    }
    acceleratorProbe_->start(smi, {
        QStringLiteral("--query-gpu=name,compute_cap,driver_version"),
        QStringLiteral("--format=csv,noheader,nounits"),
    }, QIODevice::ReadOnly);
    QTimer::singleShot(2500, this, [this] {
        if (!acceleratorDetectionRunning_ ||
            acceleratorProbe_->state() == QProcess::NotRunning) return;
        acceleratorDetectionTimedOut_ = true;
        acceleratorProbe_->kill();
    });
#elif defined(Q_OS_MACOS)
    finishAcceleratorDetection(
        QStringLiteral(
            "Native macOS runtime selected. It contains no CUDA libraries; analysis "
            "uses CPU while MPS availability is reported by Health."),
        true);
#else
    finishAcceleratorDetection(
        QStringLiteral("CPU runtime selected for this platform."), true);
#endif
}

void JamTasterService::handleAcceleratorProbeFinished(int exitCode)
{
    if (!acceleratorDetectionRunning_) return;
    if (acceleratorDetectionTimedOut_) {
        finishAcceleratorDetection(
            QStringLiteral(
                "CPU runtime selected: NVIDIA capability detection did not complete."),
            true);
        return;
    }
    if (exitCode != 0) {
        finishAcceleratorDetection(
            QStringLiteral(
                "CPU runtime selected: NVIDIA capability detection was unsuccessful."),
            true);
        return;
    }
    const QString first = QString::fromUtf8(acceleratorProbe_->readAllStandardOutput())
        .split(QLatin1Char('\n'), Qt::SkipEmptyParts).value(0).trimmed();
    const QStringList fields = first.split(QLatin1Char(','));
    bool capabilityOk = false;
    bool driverOk = false;
    const double capability = fields.value(1).trimmed().toDouble(&capabilityOk);
    const double driver = fields.value(2).trimmed().toDouble(&driverOk);
    // PyTorch 2.9's CUDA 12.6 wheel is the broadest pinned Windows build.
    if (fields.size() >= 3 && capabilityOk && driverOk &&
        capability >= 5.0 && driver >= 561.17) {
        finishAcceleratorDetection(
            QStringLiteral("NVIDIA CUDA runtime selected: %1 (compute %2, driver %3).")
                .arg(fields.at(0).trimmed(), fields.at(1).trimmed(),
                     fields.at(2).trimmed()),
            false);
    } else {
        finishAcceleratorDetection(
            QStringLiteral(
                "CPU runtime selected: the NVIDIA GPU or driver is not compatible "
                "with the pinned CUDA runtime."),
            true);
    }
}

void JamTasterService::finishAcceleratorDetection(
    const QString& recommendation,
    bool cpuOnly)
{
    if (!acceleratorDetectionRunning_) return;
    acceleratorDetectionRunning_ = false;
    acceleratorDetected_ = true;
    recommendedCpuOnly_ = cpuOnly;
    acceleratorRecommendation_ = recommendation;
    emit componentChanged();
    if (!pendingInstallation_) return;
    const bool repairing = pendingRepair_;
    pendingInstallation_ = false;
    pendingRepair_ = false;
    installVariant(recommendedCpuOnly_, acceleratorRecommendation_, repairing);
}

void JamTasterService::install()
{
    requestInstall(false);
}

void JamTasterService::requestInstall(bool repairing)
{
    if (isBusy() || taskActive_ || pendingInstallation_) return;
    if (acceleratorDetected_) {
        installVariant(recommendedCpuOnly_, acceleratorRecommendation_, repairing);
        return;
    }
    pendingInstallation_ = true;
    pendingRepair_ = repairing;
    setTaskStatus(QStringLiteral("Detecting JamTaster processing hardware"), 0, true);
    publishComponentProgress(
        QStringLiteral("Detecting the compatible CPU or NVIDIA runtime"), 0);
    startAcceleratorDetection();
}

void JamTasterService::installVariant(
    bool cpuOnly,
    const QString& reason,
    bool repairing)
{
    requestedCpuOnly_ = cpuOnly;
    repairing_ = repairing;
    operationRoot_ = repairing
        ? componentRoot() + QStringLiteral(".repairing-%1").arg(
            QUuid::createUuid().toString(QUuid::WithoutBraces))
        : componentRoot();
    setTaskStatus(QStringLiteral("Preparing JamTaster installation"), 0, true);
    publishComponentProgress(reason, 0);
    if (!developmentEntryPoint().isEmpty()) {
        startDevelopmentInstall(cpuOnly);
    } else {
        startReleaseInstall();
    }
}

void JamTasterService::repair()
{
    if (isBusy() || taskActive_) return;
    if (!isInstalled()) {
        operationRoot_ = componentRoot();
        requestedCpuOnly_ = recommendedCpuOnly_;
        repairing_ = false;
        publishComponentProgress(
            QStringLiteral("Completing the partial JamTaster installation"), 1);
        startDevelopmentInstall(requestedCpuOnly_);
        return;
    }
    repairHealthCheck_ = true;
    startHealthCheck(false);
}

void JamTasterService::startDevelopmentInstall(bool cpuOnly)
{
    QString program;
    QStringList arguments;
#ifdef Q_OS_WIN
    program = QStandardPaths::findExecutable(QStringLiteral("py"));
    if (!program.isEmpty()) arguments << QStringLiteral("-3.10");
#endif
    if (program.isEmpty()) {
        program = QStandardPaths::findExecutable(
#ifdef Q_OS_WIN
            QStringLiteral("python")
#else
            QStringLiteral("python3")
#endif
        );
    }
    if (program.isEmpty()) {
        failComponent(QStringLiteral(
            "The source-tree development installer requires Python 3.10. "
            "Release builds download a self-contained component instead."));
        return;
    }
    arguments << developmentEntryPoint()
              << QStringLiteral("--component-root") << operationRoot_
              << QStringLiteral("install");
    if (cpuOnly) arguments << QStringLiteral("--cpu");
    publishComponentProgress(
        QStringLiteral("Installing JamTaster in its private application runtime"), 1);
    startComponentProcess(program, arguments, ComponentOperation::DevelopmentInstall);
}

void JamTasterService::startDevelopmentRepair()
{
    const QString entryPoint = developmentEntryPoint();
    if (entryPoint.isEmpty()) {
        failComponent(QStringLiteral(
            "This packaged JamTaster worker is incomplete and requires a replacement "
            "component download."));
        return;
    }
    QString program;
    QStringList arguments;
    const QString privatePython = readManifest().value(QStringLiteral("python")).toString();
    if (QFileInfo(privatePython).isFile()) {
        program = privatePython;
    }
#ifdef Q_OS_WIN
    if (program.isEmpty()) {
        program = QStandardPaths::findExecutable(QStringLiteral("py"));
        if (!program.isEmpty()) arguments << QStringLiteral("-3.10");
    }
#endif
    if (program.isEmpty()) {
        program = QStandardPaths::findExecutable(
#ifdef Q_OS_WIN
            QStringLiteral("python")
#else
            QStringLiteral("python3")
#endif
        );
    }
    if (program.isEmpty()) {
        failComponent(QStringLiteral(
            "JamTaster could not start its private dependency repair."));
        return;
    }
    const QString accelerator = readManifest().value(
        QStringLiteral("accelerator")).toString();
    const bool cpuOnly = accelerator != QStringLiteral("cuda");
    operationRoot_ = componentRoot();
    requestedCpuOnly_ = cpuOnly;
    repairing_ = false;
    arguments << entryPoint << QStringLiteral("--component-root") << componentRoot()
              << QStringLiteral("repair");
    if (cpuOnly) arguments << QStringLiteral("--cpu");
    publishComponentProgress(
        QStringLiteral("Repairing only unhealthy JamTaster dependencies and models"), 5);
    startComponentProcess(program, arguments, ComponentOperation::DevelopmentRepair);
}

void JamTasterService::startModelRepair()
{
    QString program;
    QStringList arguments;
    QString error;
    if (!resolveWorker(program, arguments, error)) {
        failComponent(error);
        return;
    }
    const bool cuda = readManifest().value(QStringLiteral("accelerator")).toString() ==
        QStringLiteral("cuda");
    operationRoot_ = componentRoot();
    arguments << QStringLiteral("models") << QStringLiteral("fetch")
              << QStringLiteral("--device")
              << (cuda ? QStringLiteral("auto") : QStringLiteral("cpu"));
    publishComponentProgress(QStringLiteral("Downloading missing JamTaster models"), 55);
    startComponentProcess(program, arguments, ComponentOperation::ModelRepair);
}

void JamTasterService::startTargetedRepair(const QJsonObject& health)
{
    const QJsonObject dependencies = health.value(QStringLiteral("dependencies")).toObject();
    bool dependenciesHealthy = !dependencies.isEmpty();
    for (auto it = dependencies.begin(); it != dependencies.end(); ++it) {
        dependenciesHealthy = dependenciesHealthy && it.value().toBool();
    }
    const bool runtimeCompatible = health.value(
        QStringLiteral("runtime_compatible")).toBool();
    const QJsonObject models = health.value(QStringLiteral("models")).toObject();
    bool modelsHealthy = !models.isEmpty();
    for (auto it = models.begin(); it != models.end(); ++it) {
        modelsHealthy = modelsHealthy && it.value().toBool();
    }
    if (dependenciesHealthy && runtimeCompatible && !modelsHealthy) {
        startModelRepair();
        return;
    }
    if (!developmentEntryPoint().isEmpty()) {
        startDevelopmentRepair();
        return;
    }

    // A frozen worker cannot replace one of its own bundled dependencies.
    // Preserve the existing component transactionally and download a
    // replacement only when health proves that worker bundle is damaged.
    requestedCpuOnly_ = readManifest().value(QStringLiteral("accelerator")).toString() !=
        QStringLiteral("cuda");
    repairing_ = true;
    operationRoot_ = componentRoot() + QStringLiteral(".repairing-%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    publishComponentProgress(
        QStringLiteral("Replacing the damaged packaged JamTaster worker"), 5);
    startReleaseInstall();
}

QString JamTasterService::platformPackageName() const
{
    QString os;
#ifdef Q_OS_WIN
    os = QStringLiteral("windows");
#elif defined(Q_OS_MACOS)
    os = QStringLiteral("macos");
#else
    os = QStringLiteral("linux");
#endif
    QString architecture = QSysInfo::currentCpuArchitecture().toLower();
    if (architecture == QStringLiteral("amd64")) architecture = QStringLiteral("x86_64");
    QString name = QStringLiteral("jamtaster-%1-%2-%3")
        .arg(QString::fromLatin1(kComponentVersion), os, architecture);
#ifdef Q_OS_WIN
    name += requestedCpuOnly_ ? QStringLiteral("-cpu") : QStringLiteral("-cuda");
#endif
    return name;
}

void JamTasterService::startReleaseInstall()
{
    downloadReleaseManifest();
}

void JamTasterService::downloadReleaseManifest()
{
    const QString packageName = platformPackageName();
    componentOperation_ = ComponentOperation::DownloadManifest;
    publishComponentProgress(QStringLiteral("Checking the JamTaster component package"), 1);
    QNetworkRequest request(QUrl(releaseManifestUrl(packageName)));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    downloadReply_ = network_->get(request);
    connect(downloadReply_, &QNetworkReply::finished, this, [this] {
        QPointer<QNetworkReply> reply = downloadReply_;
        downloadReply_.clear();
        if (!reply || reply->error() != QNetworkReply::NoError) {
            const QString error = reply ? reply->errorString() : QStringLiteral("download stopped");
            if (reply) reply->deleteLater();
            failComponent(QStringLiteral("Could not download the JamTaster package manifest: %1").arg(error));
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
        reply->deleteLater();
        if (!document.isObject()) {
            failComponent(QStringLiteral("The JamTaster package manifest is invalid."));
            return;
        }
        downloadArchive(document.object());
    });
}

void JamTasterService::downloadArchive(const QJsonObject& release)
{
    if (release.value(QStringLiteral("format")).toString() !=
            QStringLiteral("jamtaster-release-v1") ||
        release.value(QStringLiteral("version")).toString() !=
            QString::fromLatin1(kComponentVersion) ||
        release.value(QStringLiteral("protocol")).toInt() != kProtocolVersion) {
        failComponent(QStringLiteral("The JamTaster package is incompatible with this Jam2 build."));
        return;
    }
    const QUrl url(release.value(QStringLiteral("url")).toString());
    const QString sha = release.value(QStringLiteral("sha256")).toString().toLower();
    if (!url.isValid() || url.scheme() != QStringLiteral("https") || sha.size() != 64) {
        failComponent(QStringLiteral("The JamTaster package manifest has unsafe download metadata."));
        return;
    }
    pendingRelease_ = release;
    const QString downloads = QDir(QFileInfo(componentRoot()).absolutePath())
        .absoluteFilePath(QStringLiteral("downloads"));
    if (!QDir().mkpath(downloads)) {
        failComponent(QStringLiteral("Could not create the JamTaster download directory."));
        return;
    }
    downloadedArchive_ = QDir(downloads).absoluteFilePath(
        platformPackageName() + QStringLiteral(".tar.gz.partial"));
    downloadFile_.setFileName(downloadedArchive_);
    if (!downloadFile_.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        failComponent(QStringLiteral("Could not create the JamTaster package file."));
        return;
    }
    componentOperation_ = ComponentOperation::DownloadArchive;
    downloadTimer_.start();
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    downloadReply_ = network_->get(request);
    connect(downloadReply_, &QNetworkReply::readyRead, this, [this] {
        if (downloadReply_) downloadFile_.write(downloadReply_->readAll());
    });
    connect(downloadReply_, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) {
                const int percent = total > 0
                    ? static_cast<int>(qBound<qint64>(
                        0LL, received * 100 / total, 100LL)) : 0;
                const double seconds = qMax(0.001, downloadTimer_.elapsed() / 1000.0);
                const double megabytes = received / (1024.0 * 1024.0);
                const double totalMegabytes = total > 0
                    ? total / (1024.0 * 1024.0) : 0.0;
                const QString amount = total > 0
                    ? QStringLiteral("%1 / %2 MB").arg(megabytes, 0, 'f', 1)
                        .arg(totalMegabytes, 0, 'f', 1)
                    : QStringLiteral("%1 MB").arg(megabytes, 0, 'f', 1);
                publishComponentProgress(
                    QStringLiteral("Downloading JamTaster: %1 at %2 MB/s")
                        .arg(amount).arg(megabytes / seconds, 0, 'f', 1),
                    5 + percent * 85 / 100);
            });
    connect(downloadReply_, &QNetworkReply::finished, this, [this] {
        QPointer<QNetworkReply> reply = downloadReply_;
        downloadReply_.clear();
        if (reply) downloadFile_.write(reply->readAll());
        downloadFile_.close();
        if (!reply || reply->error() != QNetworkReply::NoError) {
            const QString error = reply ? reply->errorString() : QStringLiteral("download stopped");
            if (reply) reply->deleteLater();
            clearDownload();
            failComponent(QStringLiteral("Could not download JamTaster: %1").arg(error));
            return;
        }
        reply->deleteLater();
        QFile archive(downloadedArchive_);
        if (!archive.open(QIODevice::ReadOnly)) {
            clearDownload();
            failComponent(QStringLiteral("Could not verify the downloaded JamTaster package."));
            return;
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&archive) ||
            QString::fromLatin1(hash.result().toHex()) !=
                pendingRelease_.value(QStringLiteral("sha256")).toString().toLower()) {
            archive.close();
            clearDownload();
            failComponent(QStringLiteral("The downloaded JamTaster package failed SHA-256 verification."));
            return;
        }
        archive.close();
        extractDownloadedArchive();
    });
}

void JamTasterService::extractDownloadedArchive()
{
    const QString tar = QStandardPaths::findExecutable(QStringLiteral("tar"));
    if (tar.isEmpty()) {
        clearDownload();
        failComponent(QStringLiteral("The operating-system archive tool is unavailable."));
        return;
    }
    const QString installing = operationRoot_ + QStringLiteral(".extracting");
    QDir installDir(installing);
    if (installDir.exists()) installDir.removeRecursively();
    if (!QDir().mkpath(installing)) {
        clearDownload();
        failComponent(QStringLiteral("Could not create the temporary component directory."));
        return;
    }
    publishComponentProgress(QStringLiteral("Extracting JamTaster"), 96);
    startComponentProcess(
        tar,
        {QStringLiteral("-xzf"), downloadedArchive_, QStringLiteral("-C"), installing},
        ComponentOperation::ExtractArchive);
}

void JamTasterService::startComponentProcess(
    const QString& program,
    const QStringList& arguments,
    ComponentOperation operation)
{
    componentOperation_ = operation;
    componentOutput_.clear();
    componentAllOutput_.clear();
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("JAMTASTER_COMPONENT_ROOT"), operationRoot_);
    componentProcess_->setProcessEnvironment(environment);
    componentProcess_->start(program, arguments, QIODevice::ReadOnly);
}

void JamTasterService::handleComponentOutput()
{
    const QByteArray received = componentProcess_->readAllStandardOutput();
    componentOutput_.append(received);
    componentAllOutput_.append(received);
    const QList<QByteArray> lines = componentOutput_.split('\n');
    if (lines.size() < 2) return;
    componentOutput_ = lines.last();
    for (int index = 0; index + 1 < lines.size(); ++index) {
        const QString line = cleanLine(QString::fromUtf8(lines.at(index)));
        if (line.isEmpty()) continue;
        const QJsonDocument document = QJsonDocument::fromJson(lines.at(index).trimmed());
        if (document.isObject()) {
            const QJsonObject event = document.object();
            if (event.value(QStringLiteral("format")).toString() ==
                QStringLiteral("jamtaster-install-progress-v1")) {
                publishComponentProgress(
                    event.value(QStringLiteral("message")).toString(),
                    event.value(QStringLiteral("percent")).toInt(-1));
                continue;
            }
        }
        emit componentLog(line);
    }
}

void JamTasterService::handleComponentFinished(int exitCode)
{
    handleComponentOutput();
    if (cancelRequested_) {
        finishCancelledTask();
        return;
    }
    const ComponentOperation completed = componentOperation_;
    componentOperation_ = ComponentOperation::None;
    if (completed == ComponentOperation::None) return;
    if (exitCode != 0 && completed != ComponentOperation::Health) {
        if (completed == ComponentOperation::ExtractArchive) {
            scheduleRemoveTree(operationRoot_ + QStringLiteral(".extracting"));
            clearDownload();
        }
        const QString detail = componentFailureDetail(componentAllOutput_);
        failComponent(detail.isEmpty()
            ? QStringLiteral("The JamTaster component operation failed with exit code %1.")
                .arg(exitCode)
            : QStringLiteral("JamTaster component operation failed: %1").arg(detail));
        return;
    }
    if (completed == ComponentOperation::ExtractArchive) {
        const QString installing = operationRoot_ + QStringLiteral(".extracting");
        const QString extractedRoot = QDir(installing).absoluteFilePath(platformPackageName());
        const QString source = QFileInfo(QDir(extractedRoot).absoluteFilePath(
            QStringLiteral("component.json"))).isFile() ? extractedRoot : installing;
        QString error;
        if (source != operationRoot_) {
            if (QFileInfo::exists(operationRoot_) && !safeRemovePath(operationRoot_, error)) {
                scheduleRemoveTree(installing);
                clearDownload();
                failComponent(error);
                return;
            }
            QDir parent(QFileInfo(source).absolutePath());
            if (!parent.rename(QFileInfo(source).fileName(), operationRoot_)) {
                scheduleRemoveTree(installing);
                clearDownload();
                failComponent(QStringLiteral("Could not stage the verified JamTaster component."));
                return;
            }
        }
        scheduleRemoveTree(installing);
        if (!publishOperationRoot(error)) {
            clearDownload();
            failComponent(error);
            return;
        }
        clearDownload();
        emit componentChanged();
        refreshStorageUsage();
        startHealthCheck(true);
        return;
    }
    if (completed == ComponentOperation::Health) {
        const QList<QByteArray> lines = componentAllOutput_.split('\n');
        QJsonObject health;
        for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
            const QJsonDocument document = QJsonDocument::fromJson(it->trimmed());
            if (document.isObject()) {
                health = document.object();
                break;
            }
        }
        if (health.isEmpty()) {
            repairHealthCheck_ = false;
            lastHealthSummary_ = QStringLiteral("Health check returned invalid data");
            emit componentFailed(lastHealthSummary_);
            setTaskStatus(lastHealthSummary_, 0, false);
        } else {
            const bool healthy = health.value(QStringLiteral("healthy")).toBool();
            emit healthFinished(health);
            if (repairHealthCheck_) {
                repairHealthCheck_ = false;
                if (healthy) {
                    lastHealthSummary_ = QStringLiteral("JamTaster health is OK");
                    setTaskStatus(lastHealthSummary_, 100, false);
                } else {
                    lastHealthSummary_ = QStringLiteral(
                        "Repairing the unhealthy JamTaster component");
                    startTargetedRepair(health);
                    emit componentChanged();
                    return;
                }
            } else {
                lastHealthSummary_ = healthy
                    ? QStringLiteral("Ready") : QStringLiteral("Needs repair");
                setTaskStatus(lastHealthSummary_, 100, false);
            }
        }
        emit componentChanged();
        return;
    }
    if (completed == ComponentOperation::DevelopmentInstall) {
        QString error;
        if (!publishOperationRoot(error)) {
            failComponent(error);
            return;
        }
        emit componentChanged();
        refreshStorageUsage();
        startHealthCheck(true);
        return;
    }
    if (completed == ComponentOperation::DevelopmentRepair ||
        completed == ComponentOperation::ModelRepair) {
        emit componentChanged();
        refreshStorageUsage();
        startHealthCheck(true);
    }
}

void JamTasterService::checkHealth()
{
    repairHealthCheck_ = false;
    startHealthCheck(false);
}

void JamTasterService::startHealthCheck(bool installContinuation)
{
    if (componentProcess_->state() != QProcess::NotRunning) return;
    QString program;
    QStringList arguments;
    QString error;
    if (!resolveWorker(program, arguments, error)) {
        failComponent(error);
        return;
    }
    arguments << QStringLiteral("doctor") << QStringLiteral("--json")
              << QStringLiteral("--progress");
    healthInstallContinuation_ = installContinuation;
    const int start = installContinuation ? 98 : 0;
    setTaskStatus(QStringLiteral("Checking JamTaster health"), start, true);
    publishComponentProgress(QStringLiteral("Checking JamTaster health"), start);
    startComponentProcess(program, arguments, ComponentOperation::Health);
}

bool JamTasterService::startJob(const QJsonObject& request, QString& error)
{
    if (isBusy()) {
        error = QStringLiteral("JamTaster is already busy.");
        return false;
    }
    QString program;
    QStringList arguments;
    if (!resolveWorker(program, arguments, error)) return false;
    const QString projectRoot = request.value(QStringLiteral("project_root")).toString();
    const QString inputPath = request.value(QStringLiteral("input_path")).toString();
    if (projectRoot.isEmpty() || inputPath.isEmpty() || !QFileInfo(inputPath).isFile()) {
        error = QStringLiteral("JamTaster requires an existing WAV and owning project folder.");
        return false;
    }
    const QString working = QDir(projectRoot).absoluteFilePath(QStringLiteral("analysis/.working"));
    if (!QDir().mkpath(working)) {
        error = QStringLiteral("Could not create the project's analysis working directory.");
        return false;
    }
    QJsonObject versioned = request;
    versioned.insert(QStringLiteral("protocol"), kProtocolVersion);
    const QString requestPath = QDir(working).absoluteFilePath(
        QStringLiteral("request-%1.json").arg(
            QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QSaveFile file(requestPath);
    const QByteArray encoded = QJsonDocument(versioned).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(encoded) != encoded.size() || !file.commit()) {
        error = QStringLiteral("Could not write the JamTaster job request.");
        return false;
    }
    activeRequestPath_ = requestPath;
    taskInputPath_ = QFileInfo(inputPath).absoluteFilePath();
    taskProjectRoot_ = QDir(projectRoot).absolutePath();
    taskDisplayName_ = request.value(QStringLiteral("display_name")).toString();
    arguments << QStringLiteral("worker") << QStringLiteral("--request") << requestPath;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("JAMTASTER_COMPONENT_ROOT"), componentRoot());
    jobProcess_->setProcessEnvironment(environment);
    jobOutput_.clear();
    lastJobResult_ = {};
    jobProcess_->setProgram(program);
    jobProcess_->setArguments(arguments);
    jobProcess_->start(QIODevice::ReadOnly);
    setTaskStatus(QStringLiteral("Starting JamTaster analysis"), 0, true);
    emit jobStarted(versioned.value(QStringLiteral("action")).toString());
    return true;
}

void JamTasterService::handleJobOutput()
{
    jobOutput_.append(jobProcess_->readAllStandardOutput());
    while (true) {
        const qsizetype newline = jobOutput_.indexOf('\n');
        if (newline < 0) break;
        const QByteArray line = jobOutput_.left(newline).trimmed();
        jobOutput_.remove(0, newline + 1);
        if (line.isEmpty()) continue;
        const QJsonDocument document = QJsonDocument::fromJson(line);
        if (!document.isObject()) {
            emit jobProgress(QJsonObject{
                {QStringLiteral("type"), QStringLiteral("log")},
                {QStringLiteral("message"), cleanLine(QString::fromUtf8(line))},
            });
            continue;
        }
        const QJsonObject event = document.object();
        const QString type = event.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("result")) {
            lastJobResult_ = event.value(QStringLiteral("result")).toObject();
        } else if (type == QStringLiteral("error")) {
            lastJobResult_.insert(QStringLiteral("error"),
                                  event.value(QStringLiteral("message")));
        }
        if (type == QStringLiteral("progress")) {
            const QString message = event.value(QStringLiteral("message")).toString();
            const int percent = event.contains(QStringLiteral("percent"))
                ? event.value(QStringLiteral("percent")).toInt() : taskProgress_;
            if (!message.isEmpty()) setTaskStatus(message, percent, true);
        }
        emit jobProgress(event);
    }
}

void JamTasterService::handleJobFinished(int exitCode)
{
    handleJobOutput();
    if (!activeRequestPath_.isEmpty()) {
        (void)QFile::remove(activeRequestPath_);
        activeRequestPath_.clear();
    }
    if (cancelRequested_) {
        finishCancelledTask();
        return;
    }
    if (exitCode == 0 && !lastJobResult_.isEmpty() &&
        !lastJobResult_.contains(QStringLiteral("error"))) {
        setTaskStatus(QStringLiteral("JamTaster analysis complete"), 100, false);
        emit jobFinished(lastJobResult_);
    } else {
        const QString detail = lastJobResult_.value(QStringLiteral("error")).toString();
        const QString message = detail.isEmpty()
            ? QStringLiteral("JamTaster exited with code %1.").arg(exitCode)
            : detail;
        setTaskStatus(message, taskProgress_, false);
        emit jobFailed(message);
    }
}

void JamTasterService::cancelTask()
{
    if (!taskActive_ || cancelRequested_) return;
    cancelRequested_ = true;
    pendingInstallation_ = false;
    pendingRepair_ = false;
    repairHealthCheck_ = false;
    cancelRemovesPartialInstall_ =
        componentOperation_ == ComponentOperation::DevelopmentInstall ||
        componentOperation_ == ComponentOperation::DownloadManifest ||
        componentOperation_ == ComponentOperation::DownloadArchive ||
        componentOperation_ == ComponentOperation::ExtractArchive;
    cancelRemovesJobWorking_ = jobProcess_->state() != QProcess::NotRunning;
    setTaskStatus(QStringLiteral("Cancelling JamTaster task"), taskProgress_, true);

    if (downloadReply_) {
        QPointer<QNetworkReply> reply = downloadReply_;
        downloadReply_.clear();
        disconnect(reply, nullptr, this, nullptr);
        reply->abort();
        reply->deleteLater();
        clearDownload();
    }
    if (jobProcess_->state() != QProcess::NotRunning) stopProcessTree(jobProcess_);
    if (componentProcess_->state() != QProcess::NotRunning) {
        stopProcessTree(componentProcess_);
    }
    if (jobProcess_->state() == QProcess::NotRunning &&
        componentProcess_->state() == QProcess::NotRunning && !downloadReply_) {
        finishCancelledTask();
    }
}

bool JamTasterService::remove(QString& error)
{
    if (isBusy()) {
        error = QStringLiteral("Finish the active JamTaster operation before removing it.");
        return false;
    }
    const bool removed = safeRemoveComponentRoot(error);
    if (removed) {
        lastHealthSummary_.clear();
        storageSummary_.clear();
        emit componentChanged();
        refreshStorageUsage();
    }
    return removed;
}

bool JamTasterService::safeRemoveComponentRoot(QString& error)
{
    return safeRemovePath(componentRoot(), error);
}

bool JamTasterService::safeRemovePath(const QString& path, QString& error)
{
    const QString base = QDir(QDir(QStandardPaths::writableLocation(
        QStandardPaths::GenericDataLocation)).absoluteFilePath(QStringLiteral("Jam2")))
        .absoluteFilePath(QStringLiteral("components/jamtaster"));
    const QString target = QDir(path).absolutePath();
    if (target == QDir(base).absolutePath() ||
        !target.startsWith(QDir(base).absolutePath() + QLatin1Char('/'),
                           Qt::CaseInsensitive)) {
        error = QStringLiteral("Refused to remove an unsafe JamTaster component path.");
        return false;
    }
    if (!QFileInfo::exists(target)) return true;
    if (!QDir(target).removeRecursively()) {
        error = QStringLiteral("Could not remove the JamTaster component at %1.").arg(target);
        return false;
    }
    return true;
}

bool JamTasterService::publishOperationRoot(QString& error)
{
    if (operationRoot_.isEmpty()) {
        error = QStringLiteral("The JamTaster installation staging path is missing.");
        return false;
    }
    QFile manifestFile(QDir(operationRoot_).absoluteFilePath(
        QStringLiteral("component.json")));
    if (!manifestFile.open(QIODevice::ReadOnly) || manifestFile.size() > 1024 * 1024) {
        error = QStringLiteral("The completed JamTaster installation has no readable manifest.");
        return false;
    }
    const QJsonDocument manifestDocument = QJsonDocument::fromJson(manifestFile.readAll());
    const QJsonObject manifest = manifestDocument.object();
    if (!manifestDocument.isObject() ||
        manifest.value(QStringLiteral("format")).toString() !=
            QStringLiteral("jamtaster-component-v1") ||
        manifest.value(QStringLiteral("protocol")).toInt() != kProtocolVersion ||
        manifest.value(QStringLiteral("version")).toString() !=
            QString::fromLatin1(kComponentVersion)) {
        error = QStringLiteral("The completed JamTaster installation manifest is incompatible.");
        return false;
    }
    const QString packagedWorker = manifest.value(QStringLiteral("worker")).toString();
    const QString privatePython = manifest.value(QStringLiteral("python")).toString();
    if ((!packagedWorker.isEmpty() && !QFileInfo(
             QDir(operationRoot_).absoluteFilePath(packagedWorker)).isFile()) ||
        (packagedWorker.isEmpty() && !QFileInfo(privatePython).isFile())) {
        error = QStringLiteral("The completed JamTaster worker is missing.");
        return false;
    }
    if (!repairing_ || QDir(operationRoot_).absolutePath() == QDir(componentRoot()).absolutePath()) {
        operationRoot_ = componentRoot();
        repairing_ = false;
        return true;
    }

    const QFileInfo current(componentRoot());
    QDir parent(current.absolutePath());
    const QString previous = componentRoot() + QStringLiteral(".previous-%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    bool movedCurrent = false;
    if (current.exists()) {
        movedCurrent = parent.rename(current.fileName(), QFileInfo(previous).fileName());
        if (!movedCurrent) {
            error = QStringLiteral(
                "Could not preserve the existing JamTaster component before repair.");
            return false;
        }
    }
    if (!parent.rename(QFileInfo(operationRoot_).fileName(), current.fileName())) {
        if (movedCurrent) {
            (void)parent.rename(QFileInfo(previous).fileName(), current.fileName());
        }
        error = QStringLiteral("Could not publish the repaired JamTaster component.");
        return false;
    }
    if (movedCurrent) scheduleRemoveTree(previous);
    operationRoot_ = componentRoot();
    repairing_ = false;
    return true;
}

void JamTasterService::scheduleRemoveTree(const QString& path)
{
    if (path.isEmpty() || !QFileInfo::exists(path)) return;
    const QString base = QDir(QDir(QStandardPaths::writableLocation(
        QStandardPaths::GenericDataLocation)).absoluteFilePath(QStringLiteral("Jam2")))
        .absoluteFilePath(QStringLiteral("components/jamtaster"));
    const QString target = QDir(path).absolutePath();
    if (target == QDir(base).absolutePath() ||
        !target.startsWith(QDir(base).absolutePath() + QLatin1Char('/'),
                           Qt::CaseInsensitive)) return;
    QThread* cleanup = QThread::create([target] {
        (void)QDir(target).removeRecursively();
    });
    connect(cleanup, &QThread::finished, cleanup, &QObject::deleteLater);
    cleanup->start();
}

void JamTasterService::refreshStorageUsage()
{
    if (storageThread_) return;
    const QString root = componentRoot();
    if (!QFileInfo::exists(root)) {
        storageSummary_.clear();
        emit storageUsageChanged(storageSummary());
        return;
    }
    QThread* thread = QThread::create([this, root] {
        quint64 total = 0;
        quint64 models = 0;
        const QString modelsRoot = QDir(root).absoluteFilePath(QStringLiteral("models"));
        const QString checkpointsRoot = QDir(root).absoluteFilePath(
            QStringLiteral("cache/torch/hub/checkpoints"));
        QDirIterator iterator(root, QDir::Files, QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            if (QThread::currentThread()->isInterruptionRequested()) return;
            iterator.next();
            const QFileInfo info = iterator.fileInfo();
            const quint64 bytes = static_cast<quint64>(qMax<qint64>(0, info.size()));
            total += bytes;
            if (info.absoluteFilePath().startsWith(
                    modelsRoot + QLatin1Char('/'), Qt::CaseInsensitive) ||
                info.absoluteFilePath().startsWith(
                    checkpointsRoot + QLatin1Char('/'), Qt::CaseInsensitive)) {
                models += bytes;
            }
        }
        QMetaObject::invokeMethod(this, [this, total, models] {
            const auto readable = [](quint64 bytes) {
                const double gib = bytes / (1024.0 * 1024.0 * 1024.0);
                return gib >= 0.1
                    ? QStringLiteral("%1 GB").arg(gib, 0, 'f', 2)
                    : QStringLiteral("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
            };
            storageSummary_ = QStringLiteral(
                "%1 size: %2 | Downloaded models: %3")
                .arg(isInstalled() ? QStringLiteral("Installed") : QStringLiteral("Partial installation"),
                     readable(total), readable(models));
            emit storageUsageChanged(storageSummary_);
        }, Qt::QueuedConnection);
    });
    storageThread_ = thread;
    connect(thread, &QThread::finished, this, [this, thread] {
        if (storageThread_ == thread) storageThread_.clear();
        thread->deleteLater();
    });
    thread->start();
}

void JamTasterService::failComponent(const QString& message)
{
    componentOperation_ = ComponentOperation::None;
    repairHealthCheck_ = false;
    if (repairing_ && !operationRoot_.isEmpty() &&
        QDir(operationRoot_).absolutePath() != QDir(componentRoot()).absolutePath()) {
        scheduleRemoveTree(operationRoot_ + QStringLiteral(".extracting"));
        scheduleRemoveTree(operationRoot_);
        repairing_ = false;
        operationRoot_.clear();
    }
    lastHealthSummary_ = message;
    setTaskStatus(message, taskProgress_, false);
    emit componentFailed(message);
    emit componentChanged();
}

void JamTasterService::clearDownload()
{
    if (downloadFile_.isOpen()) downloadFile_.close();
    if (!downloadedArchive_.isEmpty()) QFile::remove(downloadedArchive_);
    downloadedArchive_.clear();
    pendingRelease_ = {};
}

void JamTasterService::publishComponentProgress(const QString& message, int percent)
{
    setTaskStatus(message, percent, true);
    emit componentProgress(message, percent);
}

void JamTasterService::setTaskStatus(
    const QString& message,
    int percent,
    bool active)
{
    if (!message.isEmpty()) taskStatusText_ = message;
    if (percent >= 0) taskProgress_ = qBound(0, percent, 100);
    taskActive_ = active;
    emit taskStatusChanged(taskStatusText_, taskProgress_, taskActive_);
}

void JamTasterService::finishCancelledTask()
{
    if (!cancelRequested_) return;
    cancelRequested_ = false;
    componentOperation_ = ComponentOperation::None;
    repairHealthCheck_ = false;
    if (!activeRequestPath_.isEmpty()) {
        (void)QFile::remove(activeRequestPath_);
        activeRequestPath_.clear();
    }
    clearDownload();
    scheduleRemoveTree(operationRoot_ + QStringLiteral(".extracting"));
    if (cancelRemovesPartialInstall_) {
        scheduleRemoveTree(operationRoot_);
    }
    cancelRemovesPartialInstall_ = false;
    repairing_ = false;
    operationRoot_.clear();
    if (cancelRemovesJobWorking_ && !taskProjectRoot_.isEmpty()) {
        const QString working = QDir(taskProjectRoot_).absoluteFilePath(
            QStringLiteral("analysis/.working"));
        const QFileInfo workingInfo(working);
        if (workingInfo.fileName() == QStringLiteral(".working") &&
            workingInfo.dir().dirName() == QStringLiteral("analysis")) {
            QThread* cleanup = QThread::create([working] {
                (void)QDir(working).removeRecursively();
            });
            connect(cleanup, &QThread::finished, cleanup, &QObject::deleteLater);
            cleanup->start();
        }
    }
    cancelRemovesJobWorking_ = false;
    lastJobResult_ = {};
    setTaskStatus(QStringLiteral("JamTaster task cancelled"), 0, false);
    emit taskCancelled();
    emit componentChanged();
    refreshStorageUsage();
}

void JamTasterService::stopProcessTree(QProcess* process)
{
    if (!process || process->state() == QProcess::NotRunning) return;
#ifdef Q_OS_WIN
    const QString taskkill = QStandardPaths::findExecutable(QStringLiteral("taskkill"));
    if (!taskkill.isEmpty() && process->processId() > 0) {
        (void)QProcess::startDetached(taskkill, {
            QStringLiteral("/PID"), QString::number(process->processId()),
            QStringLiteral("/T"), QStringLiteral("/F"),
        });
    }
#endif
    process->terminate();
    QPointer<QProcess> guarded(process);
    QTimer::singleShot(2000, process, [guarded] {
        if (guarded && guarded->state() != QProcess::NotRunning) guarded->kill();
    });
}

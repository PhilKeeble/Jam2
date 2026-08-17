#include "JamTasterService.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>

#include <array>
#include <utility>

namespace {

QString cleanLine(QString value)
{
    value.replace(QLatin1Char('\r'), QLatin1Char(' '));
    value.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return value.simplified();
}

QString workerFilename()
{
#ifdef Q_OS_WIN
    return QStringLiteral("jamtaster-worker.exe");
#else
    return QStringLiteral("jamtaster-worker");
#endif
}

} // namespace

JamTasterService::JamTasterService(QObject* parent)
    : JamTasterService({}, {}, parent)
{
}

JamTasterService::JamTasterService(
    QString bundleRootOverride,
    QString workerPathOverride,
    QObject* parent)
    : QObject(parent),
      bundleRootOverride_(std::move(bundleRootOverride)),
      workerPathOverride_(std::move(workerPathOverride))
{
    jobProcess_.setProcessChannelMode(QProcess::SeparateChannels);
    connect(&jobProcess_, &QProcess::readyReadStandardOutput,
            this, &JamTasterService::handleJobOutput);
    connect(&jobProcess_, &QProcess::readyReadStandardError, this, [this] {
        const QString detail = cleanLine(QString::fromUtf8(jobProcess_.readAllStandardError()));
        if (detail.isEmpty()) return;
        notify([&](const Observer& observer) {
            if (observer.log) observer.log(detail);
        });
        QJsonObject event{
            {QStringLiteral("type"), QStringLiteral("log")},
            {QStringLiteral("message"), detail},
        };
        notify([&](const Observer& observer) {
            if (observer.jobProgress) observer.jobProgress(event);
        });
    });
    connect(&jobProcess_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus) {
                handleJobFinished(exitCode);
            });
    connect(&jobProcess_, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart || !taskActive_) return;
                failJob(QStringLiteral("Could not start the bundled JamTaster worker: %1")
                    .arg(jobProcess_.errorString()));
            });
}

JamTasterService::~JamTasterService()
{
    if (jobProcess_.state() != QProcess::NotRunning) {
        jobProcess_.kill();
        jobProcess_.waitForFinished(3000);
    }
    removeActiveRequest();
}

std::uint64_t JamTasterService::addObserver(Observer observer)
{
    const std::uint64_t id = nextObserverId_++;
    observers_.emplace(id, std::move(observer));
    return id;
}

void JamTasterService::removeObserver(std::uint64_t id)
{
    observers_.erase(id);
}

QString JamTasterService::bundleRoot() const
{
    if (!bundleRootOverride_.isEmpty()) return bundleRootOverride_;
    const QDir executable(QCoreApplication::applicationDirPath());
#ifdef Q_OS_MACOS
    return QDir::cleanPath(executable.absoluteFilePath(
        QStringLiteral("../Resources/jamtaster")));
#else
    return executable.absoluteFilePath(QStringLiteral("components/jamtaster"));
#endif
}

QString JamTasterService::workerPath() const
{
    if (!workerPathOverride_.isEmpty()) return workerPathOverride_;
#ifdef Q_OS_MACOS
    if (!bundleRootOverride_.isEmpty()) {
        return QDir(bundleRootOverride_).absoluteFilePath(workerFilename());
    }
    const QDir executable(QCoreApplication::applicationDirPath());
    return QDir::cleanPath(executable.absoluteFilePath(
        QStringLiteral("../Helpers/") + workerFilename()));
#else
    return QDir(bundleRoot()).absoluteFilePath(workerFilename());
#endif
}

QString JamTasterService::modelsPath() const
{
    return QDir(bundleRoot()).absoluteFilePath(QStringLiteral("models"));
}

bool JamTasterService::validateBundle(QString& error) const
{
    if (!QFileInfo(workerPath()).isFile()) {
        error = QStringLiteral("The bundled JamTaster worker is missing. Reinstall this Jam2 build.");
        return false;
    }
    static constexpr std::array<const char*, 8> models{
        "beat_this.onnx",
        "basic_pitch.onnx",
        "chordmini_btc.onnx",
        "adtof.onnx",
        "htdemucs_ft_0.onnx",
        "htdemucs_ft_1.onnx",
        "htdemucs_ft_2.onnx",
        "htdemucs_ft_3.onnx",
    };
    for (const char* name : models) {
        if (!QFileInfo(QDir(modelsPath()).absoluteFilePath(QString::fromLatin1(name))).isFile()) {
            error = QStringLiteral("The bundled JamTaster model %1 is missing. Reinstall this Jam2 build.")
                .arg(QString::fromLatin1(name));
            return false;
        }
    }
    return true;
}

QString JamTasterService::bundleStatus() const
{
    QString error;
    return validateBundle(error) ? QStringLiteral("Ready") : error;
}

QString JamTasterService::storageSummary() const
{
    quint64 bytes = 0;
    const QDir models(modelsPath());
    const QFileInfoList files = models.entryInfoList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo& file : files) bytes += static_cast<quint64>(file.size());
    const QFileInfo worker(workerPath());
    if (worker.isFile()) bytes += static_cast<quint64>(worker.size());
#ifdef Q_OS_WIN
    const QFileInfo runtime(QDir(bundleRoot()).absoluteFilePath(QStringLiteral("onnxruntime.dll")));
    if (runtime.isFile()) bytes += static_cast<quint64>(runtime.size());
#endif
    if (bytes == 0) return QStringLiteral("Bundled model size unavailable");
    return QStringLiteral("Bundled native models and worker: %1 GB")
        .arg(static_cast<double>(bytes) / 1'000'000'000.0, 0, 'f', 2);
}

bool JamTasterService::isAvailable() const
{
    QString error;
    return validateBundle(error);
}

bool JamTasterService::isBusy() const
{
    return jobProcess_.state() != QProcess::NotRunning;
}

bool JamTasterService::taskActive() const { return taskActive_; }
QString JamTasterService::taskStatusText() const { return taskStatusText_; }
int JamTasterService::taskProgress() const { return taskProgress_; }
QString JamTasterService::taskInputPath() const { return taskInputPath_; }
QString JamTasterService::taskProjectRoot() const { return taskProjectRoot_; }
QString JamTasterService::taskDisplayName() const { return taskDisplayName_; }
QJsonObject JamTasterService::lastJobResult() const { return lastJobResult_; }

bool JamTasterService::startJob(const QJsonObject& request, QString& error)
{
    if (isBusy() || taskActive_) {
        error = QStringLiteral("JamTaster is already busy.");
        return false;
    }
    if (!validateBundle(error)) return false;
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
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(encoded) != encoded.size() || !file.commit()) {
        error = QStringLiteral("Could not write the JamTaster job request.");
        return false;
    }

    activeRequestPath_ = requestPath;
    taskInputPath_ = QFileInfo(inputPath).absoluteFilePath();
    taskProjectRoot_ = QDir(projectRoot).absolutePath();
    taskDisplayName_ = request.value(QStringLiteral("display_name")).toString();
    jobOutput_.clear();
    lastJobResult_ = {};
    cancelRequested_ = false;
    jobProcess_.setProgram(workerPath());
    jobProcess_.setArguments({
        QStringLiteral("worker"),
        QStringLiteral("--request"),
        requestPath,
        QStringLiteral("--models"),
        modelsPath(),
    });
    setTaskStatus(QStringLiteral("Starting JamTaster analysis"), 0, true);
    const QString action = versioned.value(QStringLiteral("action")).toString();
    notify([&](const Observer& observer) {
        if (observer.jobStarted) observer.jobStarted(action);
    });
    jobProcess_.start(QIODevice::ReadOnly);
    return true;
}

void JamTasterService::handleJobOutput()
{
    jobOutput_.append(jobProcess_.readAllStandardOutput());
    while (true) {
        const qsizetype newline = jobOutput_.indexOf('\n');
        if (newline < 0) break;
        const QByteArray line = jobOutput_.left(newline).trimmed();
        jobOutput_.remove(0, newline + 1);
        if (line.isEmpty()) continue;
        const QJsonDocument document = QJsonDocument::fromJson(line);
        if (!document.isObject()) {
            const QString message = cleanLine(QString::fromUtf8(line));
            notify([&](const Observer& observer) {
                if (observer.log) observer.log(message);
            });
            continue;
        }
        const QJsonObject event = document.object();
        if (event.value(QStringLiteral("protocol")).toInt() != kProtocolVersion) {
            lastJobResult_.insert(
                QStringLiteral("error"),
                QStringLiteral("The JamTaster worker returned an incompatible protocol event."));
            continue;
        }
        const QString type = event.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("result")) {
            lastJobResult_ = event.value(QStringLiteral("result")).toObject();
        } else if (type == QStringLiteral("error")) {
            lastJobResult_.insert(
                QStringLiteral("error"), event.value(QStringLiteral("message")).toString());
        } else if (type == QStringLiteral("progress")) {
            const QString message = event.value(QStringLiteral("message")).toString();
            const int percent = event.contains(QStringLiteral("percent"))
                ? event.value(QStringLiteral("percent")).toInt() : taskProgress_;
            if (!message.isEmpty()) setTaskStatus(message, percent, true);
        }
        notify([&](const Observer& observer) {
            if (observer.jobProgress) observer.jobProgress(event);
        });
    }
}

void JamTasterService::handleJobFinished(int exitCode)
{
    handleJobOutput();
    if (!jobOutput_.trimmed().isEmpty()) {
        const QString message = cleanLine(QString::fromUtf8(jobOutput_));
        notify([&](const Observer& observer) {
            if (observer.log) observer.log(message);
        });
        jobOutput_.clear();
    }
    removeActiveRequest();
    if (cancelRequested_) {
        finishCancelledTask();
        return;
    }
    if (exitCode == 0 && !lastJobResult_.isEmpty() &&
        !lastJobResult_.contains(QStringLiteral("error"))) {
        setTaskStatus(QStringLiteral("JamTaster analysis complete"), 100, false);
        const QJsonObject result = lastJobResult_;
        notify([&](const Observer& observer) {
            if (observer.jobFinished) observer.jobFinished(result);
        });
        return;
    }
    const QString detail = lastJobResult_.value(QStringLiteral("error")).toString();
    failJob(detail.isEmpty()
        ? QStringLiteral("JamTaster exited with code %1.").arg(exitCode)
        : detail);
}

void JamTasterService::failJob(const QString& message)
{
    removeActiveRequest();
    setTaskStatus(message, taskProgress_, false);
    notify([&](const Observer& observer) {
        if (observer.jobFailed) observer.jobFailed(message);
    });
}

void JamTasterService::cancelTask()
{
    if (!taskActive_ || cancelRequested_) return;
    cancelRequested_ = true;
    setTaskStatus(QStringLiteral("Cancelling JamTaster task"), taskProgress_, true);
    stopProcessTree();
}

void JamTasterService::finishCancelledTask()
{
    cancelRequested_ = false;
    removeActiveRequest();
    lastJobResult_ = {};
    setTaskStatus(QStringLiteral("JamTaster task cancelled"), 0, false);
    notify([](const Observer& observer) {
        if (observer.taskCancelled) observer.taskCancelled();
    });
}

void JamTasterService::setTaskStatus(const QString& message, int percent, bool active)
{
    if (!message.isEmpty()) taskStatusText_ = message;
    if (percent >= 0) taskProgress_ = qBound(0, percent, 100);
    taskActive_ = active;
    const QString status = taskStatusText_;
    const int progress = taskProgress_;
    notify([&](const Observer& observer) {
        if (observer.taskStatusChanged) observer.taskStatusChanged(status, progress, active);
    });
}

void JamTasterService::removeActiveRequest()
{
    if (activeRequestPath_.isEmpty()) return;
    (void)QFile::remove(activeRequestPath_);
    activeRequestPath_.clear();
}

void JamTasterService::stopProcessTree()
{
    if (jobProcess_.state() == QProcess::NotRunning) {
        finishCancelledTask();
        return;
    }
#ifdef Q_OS_WIN
    const QString taskkill = QStandardPaths::findExecutable(QStringLiteral("taskkill"));
    if (!taskkill.isEmpty() && jobProcess_.processId() > 0) {
        QProcess killer;
        killer.setProgram(taskkill);
        killer.setArguments({
            QStringLiteral("/PID"), QString::number(jobProcess_.processId()),
            QStringLiteral("/T"), QStringLiteral("/F"),
        });
        killer.setStandardOutputFile(QProcess::nullDevice());
        killer.setStandardErrorFile(QProcess::nullDevice());
        (void)killer.startDetached();
    }
#endif
    jobProcess_.terminate();
    QTimer::singleShot(2000, this, [this] {
        if (jobProcess_.state() != QProcess::NotRunning) jobProcess_.kill();
    });
}

#include "JamTasterService.hpp"
#include "TestTiming.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QThread>

#include <array>
#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

void writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
            file.write(bytes) == bytes.size(),
        "service fixture file must be writable");
}

void waitUntil(const std::function<bool()>& predicate, const std::string& message)
{
    const auto timeout = jam2::test::scaledTimeout(std::chrono::seconds(20));
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout.count()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    require(predicate(), message);
}

QJsonObject request(
    const QString& action,
    const QString& input,
    const QString& project,
    const QString& display = QStringLiteral("Service Boundary"))
{
    return {
        {QStringLiteral("action"), action},
        {QStringLiteral("input_path"), input},
        {QStringLiteral("project_root"), project},
        {QStringLiteral("display_name"), display},
    };
}

} // namespace

int main(int argc, char** argv)
{
    try {
        QCoreApplication application(argc, argv);
        if (argc != 2) {
            throw std::runtime_error(
                "usage: jam2_jamtaster_service_tests <test-worker>");
        }
        const QString sourceWorker = QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath();
        const QString root = QDir::temp().absoluteFilePath(
            QStringLiteral("jam2-jamtaster-service-boundary"));
        QDir(root).removeRecursively();
        const QString bundle = QDir(root).absoluteFilePath(QStringLiteral("bundle"));
        const QString models = QDir(bundle).absoluteFilePath(QStringLiteral("models"));
        const QString project = QDir(root).absoluteFilePath(QStringLiteral("project"));
        const QString input = QDir(root).absoluteFilePath(QStringLiteral("input.wav"));
        require(QDir().mkpath(models) && QDir().mkpath(project),
            "service fixture directories must be creatable");
        writeFile(input, "synthetic wav placeholder");
        static constexpr std::array<const char*, 8> modelNames{
            "beat_this.onnx", "basic_pitch.onnx", "chordmini_btc.onnx", "adtof.onnx",
            "htdemucs_ft_0.onnx", "htdemucs_ft_1.onnx",
            "htdemucs_ft_2.onnx", "htdemucs_ft_3.onnx",
        };
        for (const char* name : modelNames) {
            writeFile(QDir(models).absoluteFilePath(QString::fromLatin1(name)), "model");
        }
#ifdef Q_OS_WIN
        const QString expectedWorker = QDir(bundle).absoluteFilePath(
            QStringLiteral("jamtaster-worker.exe"));
        writeFile(QDir(bundle).absoluteFilePath(QStringLiteral("onnxruntime.dll")), "runtime");
#else
        const QString expectedWorker = QDir(bundle).absoluteFilePath(
            QStringLiteral("jamtaster-worker"));
#endif
        require(QFile::copy(sourceWorker, expectedWorker),
            "test service worker must be copied into the injected bundle");

        JamTasterService missingWorker(bundle, QDir(root).absoluteFilePath(
            QStringLiteral("missing-worker")));
        require(!missingWorker.isAvailable() &&
                missingWorker.bundleStatus().contains(QStringLiteral("worker is missing")),
            "service reports a missing worker precisely");
        require(QFile::remove(QDir(models).absoluteFilePath(QStringLiteral("adtof.onnx"))),
            "one model fixture must be removable");
        JamTasterService missingModel(bundle, expectedWorker);
        require(!missingModel.isAvailable() &&
                missingModel.bundleStatus().contains(QStringLiteral("adtof.onnx")),
            "service reports the exact missing bundled model");
        writeFile(QDir(models).absoluteFilePath(QStringLiteral("adtof.onnx")), "model");

        JamTasterService service(bundle, {});
        require(service.bundleRoot() == bundle && service.workerPath() == expectedWorker &&
                service.modelsPath() == models && service.isAvailable() &&
                service.bundleStatus() == QStringLiteral("Ready") &&
                service.storageSummary().startsWith(
                    QStringLiteral("Bundled native models and worker:")) &&
                !service.isBusy() && !service.taskActive() &&
                service.taskStatusText() == QStringLiteral("Ready") &&
                service.taskProgress() == 0 && service.lastJobResult().isEmpty(),
            "service exposes injected bundle paths, storage, and pristine task state");

        int started = 0;
        int finished = 0;
        int failed = 0;
        int cancelled = 0;
        int statusChanges = 0;
        int progressEvents = 0;
        QStringList logs;
        QString lastFailure;
        JamTasterService::Observer observer;
        observer.log = [&](const QString& value) { logs.push_back(value); };
        observer.jobStarted = [&](const QString&) { ++started; };
        observer.jobProgress = [&](const QJsonObject&) { ++progressEvents; };
        observer.jobFinished = [&](const QJsonObject&) { ++finished; };
        observer.jobFailed = [&](const QString& value) {
            ++failed;
            lastFailure = value;
        };
        observer.taskStatusChanged = [&](const QString&, int, bool) { ++statusChanges; };
        observer.taskCancelled = [&] { ++cancelled; };
        const std::uint64_t observerId = service.addObserver(std::move(observer));

        QString error;
        require(!service.startJob(request(QStringLiteral("success"),
                    QDir(root).absoluteFilePath(QStringLiteral("missing.wav")), project), error) &&
                error.contains(QStringLiteral("existing WAV")),
            "service rejects a missing input before creating task state");
        error.clear();
        require(service.startJob(request(QStringLiteral("success"), input, project), error),
            "service starts a valid private worker request");
        QString busyError;
        require(!service.startJob(request(QStringLiteral("success"), input, project), busyError) &&
                busyError == QStringLiteral("JamTaster is already busy."),
            "service rejects a second job while task state is active");
        waitUntil([&] { return !service.taskActive() && !service.isBusy(); },
            "successful service job must finish");
        require(started == 1 && finished == 1 && failed == 0 && cancelled == 0,
            "service publishes exact success/failure/cancellation observer counts");
        require(statusChanges >= 3 && progressEvents >= 3,
            "service publishes startup, worker progress, result, and completion state");
        require(service.taskProgress() == 100 &&
                service.taskStatusText() == QStringLiteral("JamTaster analysis complete") &&
                service.taskInputPath() == QFileInfo(input).absoluteFilePath() &&
                service.taskProjectRoot() == QDir(project).absolutePath() &&
                service.taskDisplayName() == QStringLiteral("Service Boundary") &&
                service.lastJobResult().value(QStringLiteral("format")).toString() ==
                    QStringLiteral("synthetic-result-v1"),
            "service retains complete successful task identity, progress, and result state");
        require(std::any_of(logs.begin(), logs.end(), [](const QString& value) {
                    return value == QStringLiteral("detail one detail two");
                }) &&
                std::any_of(logs.begin(), logs.end(), [](const QString& value) {
                    return value == QStringLiteral("unstructured worker diagnostic");
                }),
            "service normalizes stderr and unstructured stdout diagnostics");
        const QDir working(QDir(project).absoluteFilePath(QStringLiteral("analysis/.working")));
        require(working.entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty(),
            "successful service completion removes its active request file");

        error.clear();
        require(service.startJob(request(QStringLiteral("fail"), input, project), error),
            "service starts a structured failure request");
        waitUntil([&] { return !service.taskActive() && !service.isBusy(); },
            "structured failure must finish");
        require(failed == 1 && lastFailure == QStringLiteral("synthetic worker failure") &&
                service.taskStatusText() == lastFailure,
            "service preserves the worker's structured failure detail");

        error.clear();
        require(service.startJob(request(QStringLiteral("exit-only"), input, project), error),
            "service starts a no-result exit request");
        waitUntil([&] { return !service.taskActive() && !service.isBusy(); },
            "no-result exit must finish");
        require(failed == 2 && lastFailure == QStringLiteral("JamTaster exited with code 7.") &&
                std::any_of(logs.begin(), logs.end(), [](const QString& value) {
                    return value == QStringLiteral("trailing worker detail");
                }),
            "service reports a nonzero exit and flushes trailing unterminated output");

        error.clear();
        require(service.startJob(request(QStringLiteral("sleep"), input, project), error),
            "service starts a cancellable request");
        waitUntil([&] { return service.taskActive() && service.isBusy(); },
            "cancellable worker must become active");
        service.cancelTask();
        service.cancelTask();
        waitUntil([&] { return !service.taskActive() && !service.isBusy(); },
            "cancelled worker must terminate");
        require(cancelled == 1 && service.taskProgress() == 0 &&
                service.taskStatusText() == QStringLiteral("JamTaster task cancelled") &&
                service.lastJobResult().isEmpty() &&
                working.entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty(),
            "service cancellation clears result/request state and notifies exactly once");
        service.cancelTask();

        service.removeObserver(observerId);
        const int statusesBeforeRemoval = statusChanges;
        error.clear();
        require(service.startJob(request(QStringLiteral("success"), input, project), error),
            "service remains reusable after cancellation");
        waitUntil([&] { return !service.taskActive() && !service.isBusy(); },
            "post-cancel success must finish");
        require(statusChanges == statusesBeforeRemoval,
            "removed observer receives no later task callbacks");

        const QString invalidWorker = QDir(root).absoluteFilePath(
            QStringLiteral("invalid-worker.exe"));
        writeFile(invalidWorker, "not an executable");
        JamTasterService failedStart(bundle, invalidWorker);
        QString failedStartDetail;
        JamTasterService::Observer failedStartObserver;
        failedStartObserver.jobFailed = [&](const QString& value) {
            failedStartDetail = value;
        };
        failedStart.addObserver(std::move(failedStartObserver));
        error.clear();
        require(failedStart.startJob(
                    request(QStringLiteral("success"), input, project), error),
            "service accepts a present worker path before the OS start attempt");
        waitUntil([&] { return !failedStart.taskActive() && !failedStart.isBusy(); },
            "failed worker start must clear task state");
        require(failedStartDetail.startsWith(
                    QStringLiteral("Could not start the bundled JamTaster worker:")),
            "service converts an OS worker-start failure into a precise observer error");

        {
            JamTasterService destructWhileActive(bundle, {});
            error.clear();
            require(destructWhileActive.startJob(
                        request(QStringLiteral("sleep"), input, project), error),
                "service starts a worker owned by a short-lived service");
            waitUntil([&] { return destructWhileActive.isBusy(); },
                "short-lived service worker must start before destruction");
        }
        require(working.entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty(),
            "service destruction kills its worker and removes the active request");

        QDir(root).removeRecursively();
        std::cout << "JamTaster service boundary tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "JamTaster service boundary test failed: " << error.what() << '\n';
        return 1;
    }
}

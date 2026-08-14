#include "Hash.hpp"
#include "TestTiming.hpp"
#include "Wav.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

jamtaster::native::AudioBuffer syntheticMusic(int sampleRate, double seconds)
{
    jamtaster::native::AudioBuffer audio;
    audio.sampleRate = sampleRate;
    audio.channels = 1;
    audio.samples.resize(static_cast<std::size_t>(std::llround(sampleRate * seconds)));
    constexpr double pi = 3.1415926535897932384626433832795;
    for (std::size_t frame = 0; frame < audio.frames(); ++frame) {
        const double time = static_cast<double>(frame) / sampleRate;
        float value = static_cast<float>(
            0.24 * std::sin(2.0 * pi * 110.0 * time) +
            0.17 * std::sin(2.0 * pi * 164.813778 * time) +
            0.11 * std::sin(2.0 * pi * 220.0 * time));
        const double phase = std::fmod(time, 0.5);
        if (phase < 0.012) {
            value += static_cast<float>(0.65 * (1.0 - phase / 0.012) *
                std::sin(2.0 * pi * 1400.0 * time));
        }
        audio.samples[frame] = std::clamp(value, -1.0F, 1.0F);
    }
    return audio;
}

struct WorkerRun {
    int exitCode = -1;
    QProcess::ExitStatus exitStatus = QProcess::CrashExit;
    QList<QJsonObject> events;
    QByteArray standardOutput;
    QByteArray standardError;
};

QString writeRequest(
    const QString& root, const QString& name, const QJsonObject& request)
{
    const QString path = QDir(root).absoluteFilePath(name + QStringLiteral(".json"));
    QSaveFile file(path);
    const QByteArray encoded = QJsonDocument(request).toJson(QJsonDocument::Compact);
    require(file.open(QIODevice::WriteOnly) && file.write(encoded) == encoded.size() &&
            file.commit(),
        "worker request fixture must commit atomically");
    return path;
}

WorkerRun runWorker(
    const QString& worker,
    const QString& request,
    const QString& workingDirectory,
    const QString& explicitModels = {})
{
    QProcess process;
    process.setWorkingDirectory(workingDirectory);
    QStringList arguments{
        QStringLiteral("worker"), QStringLiteral("--request"), request};
    if (!explicitModels.isEmpty()) {
        arguments << QStringLiteral("--models") << explicitModels;
    }
    process.start(worker, arguments, QIODevice::ReadOnly);
    const auto timeout = jam2::test::scaledTimeout(std::chrono::seconds(90));
    const int timeoutMs = static_cast<int>(timeout.count());
    require(process.waitForStarted(timeoutMs),
        "staged JamTaster worker must start");
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished();
        throw std::runtime_error("staged JamTaster worker exceeded its focused deadline");
    }
    WorkerRun result;
    result.exitCode = process.exitCode();
    result.exitStatus = process.exitStatus();
    result.standardOutput = process.readAllStandardOutput();
    result.standardError = process.readAllStandardError();
    const QList<QByteArray> lines = result.standardOutput.split('\n');
    for (const QByteArray& line : lines) {
        if (line.trimmed().isEmpty()) continue;
        const QJsonDocument document = QJsonDocument::fromJson(line);
        require(document.isObject(), "worker stdout must contain JSON objects only");
        result.events.push_back(document.object());
    }
    return result;
}

QJsonObject finalEvent(const WorkerRun& run, const QString& type)
{
    for (auto iterator = run.events.crbegin(); iterator != run.events.crend(); ++iterator) {
        if (iterator->value(QStringLiteral("type")).toString() == type) return *iterator;
    }
    return {};
}

bool hasProgress(const WorkerRun& run, const QString& stage, int percent = -1)
{
    return std::any_of(run.events.begin(), run.events.end(), [&](const QJsonObject& event) {
        return event.value(QStringLiteral("type")).toString() == QStringLiteral("progress") &&
            event.value(QStringLiteral("stage")).toString() == stage &&
            (percent < 0 || event.value(QStringLiteral("percent")).toInt() == percent);
    });
}

} // namespace

int main(int argc, char** argv)
{
    try {
        QCoreApplication application(argc, argv);
        if (argc != 2) {
            throw std::runtime_error(
                "usage: jam2_jamtaster_worker_protocol_tests <jamtaster-worker>");
        }
        const QString worker = QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath();
        const QString models = QDir(QFileInfo(worker).absolutePath())
            .absoluteFilePath(QStringLiteral("models"));
        require(QFileInfo(worker).isFile() && QDir(models).exists(),
            "staged worker and colocated model directory must exist");

        const QString root = QDir::temp().absoluteFilePath(
            QStringLiteral("jam2-jamtaster-worker-protocol-boundary"));
        QDir(root).removeRecursively();
        require(QDir().mkpath(root), "worker test root must be creatable");
        const QString project = QDir(root).absoluteFilePath(QStringLiteral("project"));
        const QString input = QDir(root).absoluteFilePath(QStringLiteral("input.wav"));
        jamtaster::native::writeWavPcm16(
            input.toStdWString(), syntheticMusic(22050, 6.0));

        const QJsonObject base{
            {QStringLiteral("protocol"), 1},
            {QStringLiteral("input_path"), input},
            {QStringLiteral("project_root"), project},
        };
        QJsonObject wrongProtocol = base;
        wrongProtocol.insert(QStringLiteral("protocol"), 99);
        wrongProtocol.insert(QStringLiteral("action"), QStringLiteral("detect_bpm"));
        const WorkerRun protocolError = runWorker(worker,
            writeRequest(root, QStringLiteral("wrong-protocol"), wrongProtocol), root);
        require(protocolError.exitStatus == QProcess::NormalExit && protocolError.exitCode == 1 &&
                finalEvent(protocolError, QStringLiteral("error"))
                    .value(QStringLiteral("message")).toString().contains(
                        QStringLiteral("unsupported JamTaster protocol")),
            "worker rejects an incompatible request protocol with a structured event");

        QJsonObject unsupported = base;
        unsupported.insert(QStringLiteral("action"), QStringLiteral("unknown"));
        const WorkerRun unsupportedRun = runWorker(worker,
            writeRequest(root, QStringLiteral("unsupported"), unsupported), root);
        require(unsupportedRun.exitCode == 1 &&
                finalEvent(unsupportedRun, QStringLiteral("error"))
                    .value(QStringLiteral("message")).toString().contains(
                        QStringLiteral("unsupported JamTaster action")),
            "worker rejects an unknown action after validating its paths");

        QJsonObject missingInput = base;
        missingInput.insert(QStringLiteral("action"), QStringLiteral("detect_bpm"));
        missingInput.remove(QStringLiteral("input_path"));
        const WorkerRun missingRun = runWorker(worker,
            writeRequest(root, QStringLiteral("missing-input"), missingInput), root);
        require(missingRun.exitCode == 1 &&
                finalEvent(missingRun, QStringLiteral("error"))
                    .value(QStringLiteral("message")).toString().contains(
                        QStringLiteral("input_path is required")),
            "worker rejects a request without an input path");

        require(QDir().mkpath(QDir(project).absoluteFilePath(QStringLiteral("analysis"))),
            "analysis directory must be creatable");
        QFile malformedIndex(QDir(project).absoluteFilePath(QStringLiteral("analysis/index.json")));
        require(malformedIndex.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                malformedIndex.write("{malformed") > 0,
            "malformed index fixture must be writable");
        malformedIndex.close();

        QJsonObject tempoRequest = base;
        tempoRequest.insert(QStringLiteral("action"), QStringLiteral("detect_bpm"));
        const WorkerRun tempo = runWorker(worker,
            writeRequest(root, QStringLiteral("tempo"), tempoRequest), root);
        const QJsonObject tempoResult = finalEvent(tempo, QStringLiteral("result"))
            .value(QStringLiteral("result")).toObject();
        require(tempo.exitCode == 0 && tempoResult.value(QStringLiteral("format")).toString() ==
                    QStringLiteral("jamtaster-tempo-v1") &&
                tempoResult.value(QStringLiteral("bpm")).toDouble() > 20.0 &&
                hasProgress(tempo, QStringLiteral("tempo"), 5) &&
                hasProgress(tempo, QStringLiteral("tempo"), 100),
            "worker performs real staged tempo inference and emits both checkpoints");
        const WorkerRun cachedTempo = runWorker(worker,
            writeRequest(root, QStringLiteral("tempo-cached"), tempoRequest), root);
        require(cachedTempo.exitCode == 0 &&
                hasProgress(cachedTempo, QStringLiteral("tempo"), 100) &&
                !hasProgress(cachedTempo, QStringLiteral("tempo"), 5),
            "worker returns saved tempo without starting model inference again");
        const QString sourceHash = tempoResult.value(QStringLiteral("source_sha256")).toString();
        require(sourceHash.size() == 64, "tempo result publishes the complete source hash");
        const QString sourceRoot = QDir(project).absoluteFilePath(
            QStringLiteral("analysis/sources/%1").arg(sourceHash));
        const QString stemsRoot = QDir(sourceRoot).absoluteFilePath(QStringLiteral("stems"));
        require(QDir().mkpath(stemsRoot), "cached stem root must be creatable");
        const auto stemAudio = syntheticMusic(22050, 6.0);
        for (const QString& stem : {
                 QStringLiteral("drums"), QStringLiteral("bass"),
                 QStringLiteral("other"), QStringLiteral("vocals")}) {
            jamtaster::native::writeWavPcm16(
                QDir(stemsRoot).absoluteFilePath(stem + QStringLiteral(".wav")).toStdWString(),
                stemAudio);
        }

        QJsonObject analysisRequest = base;
        analysisRequest.insert(QStringLiteral("action"), QStringLiteral("analyze_all"));
        analysisRequest.insert(QStringLiteral("display_name"), QStringLiteral("Worker Boundary"));
        analysisRequest.insert(QStringLiteral("options"), QJsonObject{
            {QStringLiteral("threads"), 1},
            {QStringLiteral("reuse_stems"), true},
            {QStringLiteral("no_time_stretch"), true},
            {QStringLiteral("bpm"), 120.0},
            {QStringLiteral("meter"), 4},
        });
        const WorkerRun analysis = runWorker(worker,
            writeRequest(root, QStringLiteral("analysis"), analysisRequest), root, models);
        const QJsonObject analysisResult = finalEvent(analysis, QStringLiteral("result"))
            .value(QStringLiteral("result")).toObject();
        require(analysis.exitCode == 0 &&
                analysisResult.value(QStringLiteral("format")).toString() ==
                    QStringLiteral("jamtaster-job-result-v1") &&
                analysisResult.value(QStringLiteral("action")).toString() ==
                    QStringLiteral("analyze_all") &&
                QFileInfo(analysisResult.value(QStringLiteral("converted_song")).toString()).isDir() &&
                hasProgress(analysis, QStringLiteral("input_validation"), 2) &&
                hasProgress(analysis, QStringLiteral("separation"), 18) &&
                hasProgress(analysis, QStringLiteral("beat_tracking"), 27) &&
                hasProgress(analysis, QStringLiteral("jamjar_export"), 97) &&
                hasProgress(analysis, QStringLiteral("complete"), 100),
            "worker performs complete stem-reusing analysis with mapped progress stages");

        QJsonObject convertRequest = analysisRequest;
        convertRequest.insert(QStringLiteral("action"), QStringLiteral("convert_song"));
        const WorkerRun converted = runWorker(worker,
            writeRequest(root, QStringLiteral("convert-cached"), convertRequest), root);
        require(converted.exitCode == 0 && hasProgress(converted, QStringLiteral("cached"), 100) &&
                finalEvent(converted, QStringLiteral("result"))
                    .value(QStringLiteral("result")).toObject()
                    .value(QStringLiteral("action")).toString() == QStringLiteral("convert_song"),
            "worker convert alias reuses current pipeline analysis through cached stage mapping");

        QJsonObject splitRequest = base;
        splitRequest.insert(QStringLiteral("action"), QStringLiteral("split_stems"));
        const WorkerRun split = runWorker(worker,
            writeRequest(root, QStringLiteral("split-cached"), splitRequest), root);
        require(split.exitCode == 0 && hasProgress(split, QStringLiteral("separation"), 100) &&
                finalEvent(split, QStringLiteral("result"))
                    .value(QStringLiteral("result")).toObject()
                    .value(QStringLiteral("format")).toString() ==
                        QStringLiteral("jamtaster-stems-v1"),
            "worker returns the saved complete four-stem analysis");

        require(QFile::remove(QDir(stemsRoot).absoluteFilePath(QStringLiteral("vocals.wav"))),
            "one cached stem must be removable for the missing-model boundary");
        const QString missingModels = QDir(root).absoluteFilePath(QStringLiteral("missing-models"));
        require(QDir().mkpath(missingModels), "missing-model fixture directory must exist");
        splitRequest.insert(QStringLiteral("force"), true);
        const WorkerRun modelError = runWorker(worker,
            writeRequest(root, QStringLiteral("split-missing-model"), splitRequest),
            root, missingModels);
        require(modelError.exitCode == 1 &&
                finalEvent(modelError, QStringLiteral("error"))
                    .value(QStringLiteral("message")).toString().contains(
                        QStringLiteral("bundled model is missing")),
            "worker checks every ordered Demucs model before separation");

        const QString invalidModels = QDir(root).absoluteFilePath(QStringLiteral("invalid-models"));
        require(QDir().mkpath(invalidModels), "invalid-model fixture directory must exist");
        for (int index = 0; index < 4; ++index) {
            QFile invalid(QDir(invalidModels).absoluteFilePath(
                QStringLiteral("htdemucs_ft_%1.onnx").arg(index)));
            require(invalid.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                    invalid.write("not-an-onnx-graph") > 0,
                "invalid ONNX fixture must be writable");
        }
        const WorkerRun runtimeError = runWorker(worker,
            writeRequest(root, QStringLiteral("split-invalid-model"), splitRequest),
            root, invalidModels);
        require(runtimeError.exitCode == 3 &&
                finalEvent(runtimeError, QStringLiteral("error"))
                    .value(QStringLiteral("message")).toString().startsWith(
                        QStringLiteral("ONNX Runtime error:")),
            "worker distinguishes an ONNX graph exception from a request failure");

        const QString malformedRequest = QDir(root).absoluteFilePath(
            QStringLiteral("malformed-request.json"));
        QFile malformed(malformedRequest);
        require(malformed.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                malformed.write("{not-json") > 0,
            "malformed request fixture must be writable");
        malformed.close();
        const WorkerRun malformedRun = runWorker(worker, malformedRequest, root);
        require(malformedRun.exitCode == 1 &&
                !finalEvent(malformedRun, QStringLiteral("error"))
                    .value(QStringLiteral("message")).toString().isEmpty(),
            "worker converts malformed request JSON into a structured error event");

        QFile indexFile(QDir(project).absoluteFilePath(QStringLiteral("analysis/index.json")));
        require(indexFile.open(QIODevice::ReadOnly), "worker must publish its analysis index");
        const QJsonObject index = QJsonDocument::fromJson(indexFile.readAll()).object();
        require(index.value(QStringLiteral("format")).toString() ==
                    QStringLiteral("jamtaster-analysis-index-v1") &&
                index.value(QStringLiteral("sources")).toObject().contains(sourceHash),
            "worker repairs a malformed index and records the source's results");

        QDir(root).removeRecursively();
        std::cout << "JamTaster worker protocol boundary tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "JamTaster worker protocol boundary test failed: "
                  << error.what() << '\n';
        return 1;
    }
}

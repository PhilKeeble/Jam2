#include "audio_device.hpp"
#include "engine.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <iostream>
#include <stdexcept>

namespace {
void require(bool value, const QString& reason) { if (!value) throw std::runtime_error(reason.toStdString()); }
QJsonObject jsonFile(const QString& path) {
    QFile file(path); require(file.open(QIODevice::ReadOnly), QStringLiteral("read ") + path);
    QJsonParseError error; const auto document = QJsonDocument::fromJson(file.readAll(), &error);
    require(error.error == QJsonParseError::NoError && document.isObject(), QStringLiteral("invalid JSON: ") + path);
    return document.object();
}
long long measurement(const QByteArray& output, const QString& name) {
    const auto match = QRegularExpression(QRegularExpression::escape(name) + QStringLiteral(": (-?[0-9]+)"))
        .match(QString::fromUtf8(output));
    require(match.hasMatch(), QStringLiteral("missing hardware measurement: ") + name + QLatin1Char('\n') + output);
    return match.captured(1).toLongLong();
}
void run(const QString& executable, const QString& root, int device, int rate,
    const QString& input, const QString& label, bool wasapi) {
    const auto folder = QDir(root).filePath(label);
    const bool physicalCapture = wasapi && input != QStringLiteral("none");
    const int duration = physicalCapture ? 15000 : 3000;
    QStringList arguments{QStringLiteral("local"), QStringLiteral("--audio-device"), QString::number(device),
        QStringLiteral("--sample-rate"), QString::number(rate), QStringLiteral("--audio-buffer-size"), QStringLiteral("128"),
        QStringLiteral("--stream-ms"), QString::number(duration), QStringLiteral("--stats"), QStringLiteral("enabled"),
        QStringLiteral("--record-jam-folder"), folder};
    if (wasapi) arguments << QStringLiteral("--wasapi-input") << input;
    if (input == QStringLiteral("none") || !wasapi)
        arguments << QStringLiteral("--test-input") << QStringLiteral("tone-440") <<
            QStringLiteral("--local-monitor") << QStringLiteral("on");
    QProcess process; process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(executable, arguments);
    require(process.waitForStarted(5000), QStringLiteral("start profiled public Jam2 executable"));
    if (!process.waitForFinished(duration + 12000)) { process.kill(); process.waitForFinished(5000);
        throw std::runtime_error("profiled audio stream failed to stop within its duration plus 12 seconds"); }
    const auto output = process.readAll();
    QFile transcript(QDir(root).filePath(label + QStringLiteral(".log")));
    require(transcript.open(QIODevice::WriteOnly) && transcript.write(output) == output.size(), QStringLiteral("retain hardware transcript"));
    require(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
        QStringLiteral("profiled audio stream failed: ") + output);
    require(measurement(output, QStringLiteral("Audio callbacks")) > 100, QStringLiteral("hardware callbacks did not advance"));
    const auto recording = jsonFile(QDir(folder).filePath(QStringLiteral("recording.json")));
    require(recording.value(QStringLiteral("sample_rate")).toInt() == rate &&
        recording.value(QStringLiteral("frames_written")).toInteger() > rate * 2 &&
        recording.value(QStringLiteral("dropped_frames")).toInteger() == 0 &&
        recording.value(QStringLiteral("writer_errors")).toInteger() == 0,
        QStringLiteral("profiled stream must produce at least two seconds at the jam rate without recording drops/errors"));
    if (wasapi) {
        require(measurement(output, QStringLiteral("WASAPI device error")) == 0 &&
            measurement(output, QStringLiteral("WASAPI capture overrun frames")) == 0,
            QStringLiteral("WASAPI stream must stay healthy with bounded capture"));
        if (input != QStringLiteral("none")) {
            require(measurement(output, QStringLiteral("WASAPI capture queued frames")) > 0 &&
                measurement(output, QStringLiteral("WASAPI capture underrun frames")) < rate / 5,
                QStringLiteral("physical capture must advance with less than 200ms startup/underrun silence"));
        }
    }
    std::cout << label.toStdString() << ": " << recording.value(QStringLiteral("frames_written")).toInteger()
        << " frames, no recording drops/errors\n";
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 3 || QString::fromLocal8Bit(argv[2]).isEmpty()) {
        std::cout << "Set JAM2_WASAPI_HARDWARE_PROFILE to an explicit endpoint profile.\n"; return 77;
    }
    QTemporaryDir artifacts;
    try {
        require(artifacts.isValid(), QStringLiteral("hardware artifact directory"));
        const auto profile = jsonFile(QString::fromLocal8Bit(argv[2]));
        require(profile.value(QStringLiteral("schema")).toString() == QStringLiteral("jam2-wasapi-hardware-v1"),
            QStringLiteral("expected jam2-wasapi-hardware-v1 profile"));
        const auto endpoints = jam2::audio::list_devices();
        const auto find = [&](const QString& identity, jam2::audio::EndpointDirection direction) {
            for (const auto& device : endpoints) if (
                QString::fromStdString(jam2::audio::stable_device_id(device)) == identity && device.direction == direction)
                return device.id;
            throw std::runtime_error("profiled audio endpoint unavailable: " + identity.toStdString());
        };
        const auto output = find(profile.value(QStringLiteral("output_endpoint_id")).toString(), jam2::audio::EndpointDirection::Output);
        const auto executable = QString::fromLocal8Bit(argv[1]);
        {
            jam2::Engine engine;
            jam2::EngineConfig invalid;
            invalid.audio_device_id = output;
            invalid.wasapi.output_endpoint_id = profile.value(QStringLiteral("output_endpoint_id")).toString().toStdString();
            invalid.wasapi.input_endpoint_id = "jam2-intentionally-missing-capture-endpoint";
            bool failed = false;
            try { engine.start(invalid); } catch (const std::exception&) { failed = true; }
            require(failed && engine.snapshot().lifecycle == jam2::EngineLifecycle::Failed,
                QStringLiteral("partial WASAPI start must report failure and release the opened output"));
        }
        // A saved ASIO buffer size must not become a WASAPI period request.
        // Exercise both automatic and explicit processing blocks on the same endpoint.
        for (const long block : {0L, 256L}) {
            jam2::Engine engine;
            jam2::EngineConfig config;
            config.audio_device_id = output;
            config.wasapi.output_endpoint_id = profile.value(QStringLiteral("output_endpoint_id")).toString().toStdString();
            config.audio_buffer_frames = block;
            engine.start(config);
            const auto stream = engine.coldSnapshot().stream;
            engine.requestStop(); engine.join();
            require(stream.output_period_frames > 0, QStringLiteral("automatic WASAPI period must be reported"));
            if (stream.low_latency_shared) {
                for (const auto& endpoint : endpoints) if (endpoint.id == output)
                    require(stream.output_period_frames == endpoint.minimum_period_frames,
                        QStringLiteral("WASAPI must use the endpoint minimum regardless of the processing block"));
            }
        }
        // Subsequent public-executable runs also verify rollback released the
        // endpoint, COM objects, event handles and the startup thread.
        run(executable, artifacts.path(), output, 48000, QStringLiteral("none"), QStringLiteral("wasapi-output-48000"), true);
        run(executable, artifacts.path(), output, 44100, QStringLiteral("none"), QStringLiteral("wasapi-output-44100"), true);
        const auto input = profile.value(QStringLiteral("input_endpoint_id")).toString();
        if (!input.isEmpty()) {
            (void)find(input, jam2::audio::EndpointDirection::Input);
            run(executable, artifacts.path(), output, 48000, input, QStringLiteral("wasapi-split-capture-48000"), true);
        }
        const auto asio = profile.value(QStringLiteral("asio_clsid")).toString();
        if (!asio.isEmpty()) run(executable, artifacts.path(), find(asio, jam2::audio::EndpointDirection::Duplex),
            48000, {}, QStringLiteral("asio-regression-48000"), false);
    } catch (const std::exception& error) {
        artifacts.setAutoRemove(false);
        std::cerr << error.what() << "\nArtifacts: " << artifacts.path().toStdString() << '\n'; return 1;
    }
    artifacts.setAutoRemove(false);
    std::cout << "Hardware artifacts: " << artifacts.path().toStdString() << '\n';
}

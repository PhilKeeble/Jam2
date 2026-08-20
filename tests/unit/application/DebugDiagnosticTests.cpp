#include "TestTiming.hpp"
#include "AssetChunkProtocol.hpp"
#include "ControlProtocol.hpp"
#include "protocol.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <span>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

struct ProcessResult {
    bool started = false;
    bool finished = false;
    QProcess::ExitStatus status = QProcess::CrashExit;
    int code = -1;
    QByteArray output;
};

ProcessResult runProcess(
    const QString& executable,
    const QStringList& arguments,
    std::chrono::seconds normalTimeout)
{
    QProcess process;
    process.setProgram(executable);
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    // This is a process deadman, not an acceptance duration. An instrumented
    // jam2.exe can spend more than 30 seconds starting and reporting one native
    // parser result on older or concurrently loaded Windows hosts.
    normalTimeout = std::max(normalTimeout, std::chrono::seconds(60));
    const int timeout = static_cast<int>(
        jam2::test::deadmanTimeout(normalTimeout).count());
    ProcessResult result;
    result.started = process.waitForStarted(timeout);
    if (result.started) result.finished = process.waitForFinished(timeout);
    if (!result.finished) {
        process.kill();
        process.waitForFinished(5000);
    }
    result.status = process.exitStatus();
    result.code = process.exitCode();
    result.output = process.readAll();
    return result;
}

bool writeBytes(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
        file.write(bytes) == bytes.size() && file.flush();
}

bool writeJson(const QString& path, const QJsonObject& object)
{
    return writeBytes(path, QJsonDocument(object).toJson(QJsonDocument::Indented));
}

QJsonObject readObject(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    return error.error == QJsonParseError::NoError && document.isObject()
        ? document.object()
        : QJsonObject{};
}

QJsonObject singleOutputObject(const QByteArray& output)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(output.trimmed(), &error);
    return error.error == QJsonParseError::NoError && document.isObject()
        ? document.object()
        : QJsonObject{};
}

QByteArray vectorBytes(const std::vector<std::uint8_t>& bytes)
{
    return QByteArray(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<qsizetype>(bytes.size()));
}

void appendLe16(QByteArray& bytes, std::uint16_t value)
{
    bytes.append(static_cast<char>(value & 0xffU));
    bytes.append(static_cast<char>((value >> 8U) & 0xffU));
}

void appendLe32(QByteArray& bytes, std::uint32_t value)
{
    bytes.append(static_cast<char>(value & 0xffU));
    bytes.append(static_cast<char>((value >> 8U) & 0xffU));
    bytes.append(static_cast<char>((value >> 16U) & 0xffU));
    bytes.append(static_cast<char>((value >> 24U) & 0xffU));
}

QByteArray validWavBytes()
{
    QByteArray bytes("RIFF", 4);
    appendLe32(bytes, 38);
    bytes.append("WAVEfmt ", 8);
    appendLe32(bytes, 16);
    appendLe16(bytes, 1);
    appendLe16(bytes, 1);
    appendLe32(bytes, 48000);
    appendLe32(bytes, 96000);
    appendLe16(bytes, 2);
    appendLe16(bytes, 16);
    bytes.append("data", 4);
    appendLe32(bytes, 2);
    appendLe16(bytes, 1234);
    return bytes;
}

void testHelpAndFuzzEntrypoints(
    const QString& executable,
    bool runHelp,
    bool runRejectedFuzz,
    bool runAcceptedFuzz)
{
    if (runHelp) {
        const ProcessResult describe = runProcess(
            executable, {QStringLiteral("debug"), QStringLiteral("describe"),
                         QStringLiteral("--help")}, std::chrono::seconds(10));
        const ProcessResult run = runProcess(
            executable, {QStringLiteral("debug"), QStringLiteral("run"),
                         QStringLiteral("--help")}, std::chrono::seconds(10));
        const ProcessResult fuzzHelp = runProcess(
            executable, {QStringLiteral("debug"), QStringLiteral("fuzz"),
                         QStringLiteral("--help")}, std::chrono::seconds(10));
        expect(describe.code == 0 && describe.output.contains("debug describe --json") &&
                run.code == 0 && run.output.contains("debug run <scenario.json>") &&
                fuzzHelp.code == 0 && fuzzHelp.output.contains("udp-pcm24"),
            "all private debug subcommands publish their exact help contracts");
    }
    if (!runRejectedFuzz && !runAcceptedFuzz) return;

    const int before = failures;
    QTemporaryDir root;
    expect(root.isValid(), "debug fuzz fixture creates a build-local root");
    if (!root.isValid()) return;
    const QString input = QDir(root.path()).absoluteFilePath(QStringLiteral("input.bin"));
    expect(writeBytes(input, QByteArray::fromHex("0102037b")),
        "debug fuzz fixture writes its bounded replay input");
    if (runRejectedFuzz) {
        for (const QString& target : {
                 QStringLiteral("control"),
                 QStringLiteral("udp-pcm16"),
                 QStringLiteral("udp-pcm24"),
                 QStringLiteral("asset"),
                 QStringLiteral("wav")}) {
            const ProcessResult result = runProcess(
                executable,
                {QStringLiteral("debug"), QStringLiteral("fuzz"), target, input},
                std::chrono::seconds(10));
            const QJsonObject output = singleOutputObject(result.output);
            expect(result.started && result.finished &&
                    result.status == QProcess::NormalExit && result.code == 0 &&
                    output.value(QStringLiteral("event")).toString() ==
                        QStringLiteral("fuzz_result") &&
                    output.value(QStringLiteral("target")).toString() == target &&
                    output.value(QStringLiteral("classification")).toString() ==
                        QStringLiteral("rejected") &&
                    output.value(QStringLiteral("input_bytes")).toInt() == 4,
                "each native fuzz target classifies the same bounded invalid input safely");
        }
    }

    if (runAcceptedFuzz) {
      const QByteArray directionKey(32, 'k');
    const QByteArray control = jam2::control_protocol::encodeAuthenticated(
        QJsonObject{
            {QStringLiteral("type"), QStringLiteral("session.heartbeat")},
            {QStringLiteral("sequence"), 1},
        },
        directionKey,
        7);
    std::array<std::uint8_t, 16> udpKey{};
    for (std::size_t index = 0; index < udpKey.size(); ++index) {
        udpKey[index] = static_cast<std::uint8_t>(index);
    }
    const std::array<std::int32_t, 2> samples{123456, -654321};
    const auto pcm16 = jam2::protocol::pack_pcm16(samples);
    const auto pcm24 = jam2::protocol::pack_pcm24(samples);
    const auto udpPacket = [&](jam2::NetworkAudioFormat format,
                               std::span<const std::uint8_t> payload) {
        jam2::protocol::Header header;
        header.type = jam2::protocol::PacketType::Audio;
        header.session_id = 0x0102030405060708ULL;
        header.sequence = 11;
        header.timing_value = 22;
        header.payload_length = static_cast<std::uint16_t>(payload.size());
        return vectorBytes(jam2::protocol::encode_packet(
            header, payload, udpKey, format));
    };
    jam2::application::asset_chunk::Chunk asset;
    asset.sha256 = QString(64, QLatin1Char('a'));
    asset.data = QByteArrayLiteral("asset-fixture");
    const std::array<std::pair<QString, QByteArray>, 5> acceptedInputs{
        std::pair{QStringLiteral("control"), control},
        std::pair{QStringLiteral("udp-pcm16"),
                  udpPacket(jam2::NetworkAudioFormat::Pcm16Mono, pcm16)},
        std::pair{QStringLiteral("udp-pcm24"),
                  udpPacket(jam2::NetworkAudioFormat::Pcm24Mono, pcm24)},
        std::pair{QStringLiteral("asset"),
                  jam2::application::asset_chunk::encode(asset)},
        std::pair{QStringLiteral("wav"), validWavBytes()},
    };
      for (const auto& [target, bytes] : acceptedInputs) {
        const QString acceptedPath = QDir(root.path()).absoluteFilePath(
            target + QStringLiteral(".bin"));
        expect(!bytes.isEmpty() && writeBytes(acceptedPath, bytes),
            "debug fuzz fixture writes a valid current-format input");
        const ProcessResult result = runProcess(
            executable,
            {QStringLiteral("debug"), QStringLiteral("fuzz"), target, acceptedPath},
            std::chrono::seconds(10));
        const QJsonObject output = singleOutputObject(result.output);
        expect(result.started && result.finished &&
                result.status == QProcess::NormalExit && result.code == 0 &&
                output.value(QStringLiteral("target")).toString() == target &&
                output.value(QStringLiteral("classification")).toString() ==
                    QStringLiteral("accepted") &&
                output.value(QStringLiteral("input_bytes")).toInt() == bytes.size(),
            "each native fuzz target accepts its current encoded fixture");
      }
    }
    if (runRejectedFuzz) {
        const ProcessResult unknown = runProcess(
            executable,
            {QStringLiteral("debug"), QStringLiteral("fuzz"),
             QStringLiteral("unknown"), input},
            std::chrono::seconds(10));
        expect(unknown.code == 2 && unknown.output.contains("unknown native fuzz target"),
            "native fuzz dispatch rejects an unknown parser target");
    }
    if (failures != before) {
        root.setAutoRemove(false);
        std::cerr << "debug fuzz artifacts retained at "
                  << root.path().toStdString() << '\n';
    }
}

void testLifecycleEntrypoint(const QString& executable)
{
    const int before = failures;
    QTemporaryDir root;
    expect(root.isValid(), "debug lifecycle fixture creates a build-local root");
    if (!root.isValid()) return;
    const QString artifactRoot = QDir(root.path()).absoluteFilePath(
        QStringLiteral("artifacts"));
    const QString scenarioPath = QDir(root.path()).absoluteFilePath(
        QStringLiteral("lifecycle.json"));
    expect(writeJson(scenarioPath, QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("jam2-debug-scenario")},
        {QStringLiteral("run_id"), QStringLiteral("focused-lifecycle")},
        {QStringLiteral("operation"),
         QStringLiteral("lifecycle.local-network-local")},
        {QStringLiteral("profile"), QStringLiteral("fast")},
        {QStringLiteral("runtime"), QJsonObject{
            {QStringLiteral("headless_audio"), true},
            {QStringLiteral("frame_size"), 64},
        }},
        {QStringLiteral("artifacts"), QJsonObject{
            {QStringLiteral("root"), artifactRoot},
        }},
    }), "debug lifecycle fixture writes its declarative scenario");
    const ProcessResult process = runProcess(
        executable,
        {QStringLiteral("debug"), QStringLiteral("run"), scenarioPath},
        std::chrono::seconds(15));
    const QJsonObject manifest = readObject(
        QDir(artifactRoot).absoluteFilePath(QStringLiteral("native-manifest.json")));
    const QJsonObject result = manifest.value(QStringLiteral("result")).toObject();
    expect(process.started && process.finished &&
            process.status == QProcess::NormalExit && process.code == 0 &&
            manifest.value(QStringLiteral("ok")).toBool() &&
            result.value(QStringLiteral("event")).toString() ==
                QStringLiteral("debug_lifecycle_result") &&
            result.value(QStringLiteral("engine_starts")).toInteger() == 1 &&
            result.value(QStringLiteral("engine_restarts")).toInteger() == 0 &&
            result.value(QStringLiteral("engine_reuses")).toInteger() == 2 &&
            result.value(QStringLiteral("frame_before_network")).toInteger() > 0 &&
            result.value(QStringLiteral("frame_after_return_local")).toInteger() >
                result.value(QStringLiteral("frame_after_network")).toInteger(),
        "staged lifecycle reuses one Engine across local-network-local transitions");
    if (failures != before) {
        root.setAutoRemove(false);
        std::cerr << "debug lifecycle artifacts retained at "
                  << root.path().toStdString() << '\n'
                  << process.output.toStdString();
    }
}

void testMusicCorpusEntrypoint(const QString& executable)
{
    const int before = failures;
    QTemporaryDir root;
    expect(root.isValid(), "music corpus fixture creates a build-local root");
    if (!root.isValid()) return;
    const QString artifactRoot = QDir(root.path()).absoluteFilePath(
        QStringLiteral("artifacts"));
    const QString scenarioPath = QDir(root.path()).absoluteFilePath(
        QStringLiteral("corpus.json"));
    expect(writeJson(scenarioPath, QJsonObject{
        {QStringLiteral("schema"), QStringLiteral("jam2-debug-scenario")},
        {QStringLiteral("run_id"), QStringLiteral("focused-corpus")},
        {QStringLiteral("operation"),
         QStringLiteral("validate.music-full-form-corpus")},
        {QStringLiteral("profile"), QStringLiteral("fast")},
        {QStringLiteral("artifacts"), QJsonObject{
            {QStringLiteral("root"), artifactRoot},
        }},
        {QStringLiteral("corpus"), QJsonObject{
            {QStringLiteral("render_audio"), true},
            {QStringLiteral("drum_only_audio"), false},
            {QStringLiteral("matched_complexity_seeds"), true},
            {QStringLiteral("samples_per_cell"), 2},
            {QStringLiteral("fixed_bars"), 4},
            {QStringLiteral("profile_id"), QStringLiteral("funk_static_pocket")},
        }},
    }), "music corpus fixture writes its bounded single-profile scenario");
    const ProcessResult process = runProcess(
        executable,
        {QStringLiteral("debug"), QStringLiteral("run"), scenarioPath},
        std::chrono::seconds(90));
    const QJsonObject manifest = readObject(
        QDir(artifactRoot).absoluteFilePath(QStringLiteral("native-manifest.json")));
    const QJsonObject result = manifest.value(QStringLiteral("result")).toObject();
    const QString corpusPath = result.value(QStringLiteral("corpus")).toString();
    const QJsonObject corpus = readObject(corpusPath);
    int wavFiles = 0;
    QDirIterator iterator(artifactRoot, {QStringLiteral("*.wav")},
        QDir::Files, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        ++wavFiles;
    }
    expect(process.started && process.finished &&
            process.status == QProcess::NormalExit && process.code == 0 &&
            manifest.value(QStringLiteral("ok")).toBool() &&
            result.value(QStringLiteral("event")).toString() ==
                QStringLiteral("music_full_form_corpus_result") &&
            result.value(QStringLiteral("profiles")).toInt() == 1 &&
            result.value(QStringLiteral("samples")).toInt() == 6 &&
            result.value(QStringLiteral("audio_mixes")).toInt() == 2 &&
            corpus.value(QStringLiteral("samples")).toArray().size() == 6 &&
            wavFiles >= 4,
        "staged single-profile corpus generates structure, full mixes, and drum WAVs");
    if (failures != before) {
        root.setAutoRemove(false);
        std::cerr << "music corpus artifacts retained at "
                  << root.path().toStdString() << '\n'
                  << process.output.toStdString();
    }
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc != 3) {
        std::cerr << "usage: jam2_debug_diagnostic_tests <release-jam2> "
                     "<help|fuzz-rejected|fuzz-accepted|lifecycle|corpus>\n";
        return 2;
    }
    const QString executable = QString::fromLocal8Bit(argv[1]);
    const QString mode = QString::fromLocal8Bit(argv[2]);
    if (mode == QStringLiteral("help")) {
        testHelpAndFuzzEntrypoints(executable, true, false, false);
    } else if (mode == QStringLiteral("fuzz-rejected")) {
        testHelpAndFuzzEntrypoints(executable, false, true, false);
    } else if (mode == QStringLiteral("fuzz-accepted")) {
        testHelpAndFuzzEntrypoints(executable, false, false, true);
    } else if (mode == QStringLiteral("lifecycle")) {
        testLifecycleEntrypoint(executable);
    } else if (mode == QStringLiteral("corpus")) {
        testMusicCorpusEntrypoint(executable);
    } else {
        std::cerr << "unknown debug diagnostic test mode\n";
        return 2;
    }

    if (failures != 0) {
        std::cerr << failures << " debug diagnostic checks failed\n";
        return 1;
    }
    std::cout << "debug diagnostic checks passed\n";
    return 0;
}

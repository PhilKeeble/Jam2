#include "PreparedMixRenderer.hpp"
#include "pcm16_wav.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr int kSampleRate = 48000;
constexpr int kInputFrames = 2 * kSampleRate;
constexpr double kFrequency = 440.0;
constexpr double kPi = 3.14159265358979323846;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

std::filesystem::path nativePath(const QString& path)
{
#if defined(_WIN32)
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toStdString());
#endif
}

void append16(QByteArray& bytes, quint16 value)
{
    bytes.append(static_cast<char>(value));
    bytes.append(static_cast<char>(value >> 8));
}

void append32(QByteArray& bytes, quint32 value)
{
    append16(bytes, static_cast<quint16>(value));
    append16(bytes, static_cast<quint16>(value >> 16));
}

qint16 sourceSample(int frame)
{
    return static_cast<qint16>(std::lrint(
        8000.0 * std::sin(2.0 * kPi * kFrequency * frame / kSampleRate)));
}

LooperProject toneProject(const QString& root, int frames = kInputFrames)
{
    QByteArray bytes;
    bytes.append("RIFF", 4);
    append32(bytes, 36U + static_cast<quint32>(frames * 2));
    bytes.append("WAVEfmt ", 8);
    append32(bytes, 16);
    append16(bytes, 1);
    append16(bytes, 1);
    append32(bytes, kSampleRate);
    append32(bytes, kSampleRate * 2);
    append16(bytes, 2);
    append16(bytes, 16);
    bytes.append("data", 4);
    append32(bytes, static_cast<quint32>(frames * 2));
    for (int frame = 0; frame < frames; ++frame) {
        append16(bytes, static_cast<quint16>(sourceSample(frame)));
    }
    const QString path = QDir(root).absoluteFilePath(
        QStringLiteral("tone-%1.wav").arg(frames));
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
        "could not write source tone");
    file.close();
    LooperLane lane;
    lane.id = QStringLiteral("tone");
    lane.assetPath = path;
    lane.sampleRate = kSampleRate;
    lane.sourceFrames = frames;
    LooperProject project;
    require(project.appendLane(0, lane), "could not append tone lane");
    return project;
}

std::vector<double> readSamples(const PreparedMixResult& result)
{
    const auto inspected = jam2::wav::inspect_pcm16_file(nativePath(result.path));
    require(inspected && inspected.info.sample_rate == kSampleRate &&
            inspected.info.channels == 1 &&
            inspected.info.frames == static_cast<std::uint64_t>(result.frames),
        "rendered file must retain sample rate and reported mono frame count");
    QFile file(result.path);
    require(file.open(QIODevice::ReadOnly) &&
            file.seek(static_cast<qint64>(inspected.info.data_offset)),
        "could not read rendered samples");
    const QByteArray bytes = file.readAll();
    require(static_cast<std::uint64_t>(bytes.size()) == inspected.info.data_bytes,
        "rendered audio must have its exact reported byte length");
    std::vector<double> samples(static_cast<std::size_t>(result.frames));
    for (std::size_t frame = 0; frame < samples.size(); ++frame) {
        const auto value = qFromLittleEndian<quint16>(bytes.constData() + frame * 2);
        const int signedValue = value < 0x8000U ? value : static_cast<int>(value) - 65536;
        samples[frame] = signedValue / 32768.0;
    }
    return samples;
}

double frequency(const std::vector<double>& samples)
{
    // Ignore boundary transients; interpolate positive zero crossings over
    // the central half of the actual rendered waveform.
    double first = 0.0;
    double last = 0.0;
    int crossings = 0;
    for (std::size_t frame = samples.size() / 4 + 1;
         frame < samples.size() * 3 / 4; ++frame) {
        if (samples[frame - 1] <= 0.0 && samples[frame] > 0.0) {
            const double crossing = static_cast<double>(frame - 1) -
                samples[frame - 1] / (samples[frame] - samples[frame - 1]);
            if (crossings == 0) first = crossing;
            last = crossing;
            ++crossings;
        }
    }
    require(crossings > 2 && last > first, "rendered tone must have measurable cycles");
    return (crossings - 1) * kSampleRate / (last - first);
}

double rms(const std::vector<double>& samples, std::size_t begin, std::size_t end)
{
    double sum = 0.0;
    for (std::size_t frame = begin; frame < end; ++frame) sum += samples[frame] * samples[frame];
    return std::sqrt(sum / static_cast<double>(end - begin));
}

void testIndependentSpeedAndPitch(const QString& root, const LooperProject& project)
{
    struct Setting { double speed; int pitchCents; };
    const Setting settings[] = {
        {1.0, 0}, {0.5, 0}, {0.75, 0}, {1.5, 0}, {2.0, 0}, {0.1, 0},
        {1.0, 1200}, {0.5, 1200}, {2.0, 1200},
        {1.0, -1200}, {0.5, -1200}, {0.5, 700}, {1.5, -300},
    };
    for (const Setting setting : settings) {
        SharedTrackModel track;
        track.speed = setting.speed;
        track.pitchCents = setting.pitchCents;
        const auto result = PreparedMixRenderer::render(project, root, kSampleRate,
            QDir(root).absoluteFilePath(QStringLiteral("render-%1-%2.wav")
                .arg(setting.speed).arg(setting.pitchCents)), track);
        require(result.error.isEmpty(), result.error.toStdString());
        require(result.frames == static_cast<qint64>(std::ceil(kInputFrames / setting.speed)),
            "only speed must determine rendered duration");
        const auto samples = readSamples(result);
        const double measured = frequency(samples);
        const double expected = kFrequency * std::pow(2.0, setting.pitchCents / 1200.0);
        std::cout << "speed=" << setting.speed << " pitch_cents=" << setting.pitchCents
                  << " frames=" << result.frames << " frequency_hz=" << measured
                  << " expected_hz=" << expected << '\n';
        // Signalsmith randomizes spectral phases beyond 2x stretching, so
        // the 10x duration boundary allows more waveform-cycle variation.
        const bool extremeStretch = setting.speed < 0.5;
        const double pitchTolerance = extremeStretch ? 0.01 : 0.005;
        require(std::abs(measured - expected) < expected * pitchTolerance,
            "rendered pitch must follow pitch control independently of speed");
        const std::size_t edgeFrames = kSampleRate / 50; // first/last 20 ms
        const double steadyRms = rms(samples, samples.size() / 4, samples.size() * 3 / 4);
        const double openingRms = rms(samples, 0, edgeFrames);
        const double closingRms = rms(samples, samples.size() - edgeFrames, samples.size());
        std::cout << "opening_rms=" << openingRms << " closing_rms=" << closingRms
                  << " steady_rms=" << steadyRms << '\n';
        // Endpoint phase cancellation can reduce a tone's amplitude. Check
        // retained signal and bounded silence rather than equal edge loudness.
        const auto audible = [](double sample) { return std::abs(sample) > 0.001; };
        const auto firstSignal = std::find_if(samples.begin(), samples.end(), audible);
        const auto lastSignal = std::find_if(samples.rbegin(), samples.rend(), audible);
        const auto maximumSilentFrames = kSampleRate / 100; // 10 ms, below processor latency
        require(openingRms > steadyRms * 0.2 && closingRms > steadyRms * 0.2 &&
                std::distance(samples.begin(), firstSignal) < maximumSilentFrames &&
                std::distance(samples.rbegin(), lastSignal) < maximumSilentFrames,
            "stretch must retain audio at both ends without processor warm-up silence");
        if (setting.speed == 1.0 && setting.pitchCents == 0) {
            for (int frame = 0; frame < kInputFrames; ++frame) {
                const float input = sourceSample(frame) / 32768.0f;
                const float limited = static_cast<float>(std::tanh(kPreparedMixMasterPreGain * input));
                const double expectedSample = std::lrint(limited * 32767.0f) / 32768.0;
                require(samples[static_cast<std::size_t>(frame)] == expectedSample,
                    "identity path must preserve samples apart from existing master limiting");
            }
        }
    }
}

void testShortInputAndLimit(const QString& root)
{
    const auto shortProject = toneProject(root, 480);
    SharedTrackModel track;
    track.speed = 0.5;
    const auto shortResult = PreparedMixRenderer::render(shortProject, root, kSampleRate,
        QDir(root).absoluteFilePath(QStringLiteral("short.wav")), track);
    require(shortResult.error.isEmpty() && shortResult.frames == 960,
        "input shorter than processor latency must still render to the requested length");
    const auto shortSamples = readSamples(shortResult);
    require(rms(shortSamples, 0, shortSamples.size()) > 0.01,
        "short stretched input must not become silent");
    const auto project = toneProject(root);
    track.speed = 0.1;
    const QString limitedPath = QDir(root).absoluteFilePath(QStringLiteral("over-limit.wav"));
    const auto limited = PreparedMixRenderer::render(project, root, kSampleRate,
        limitedPath, track, 0, 31LL * kSampleRate);
    require(!limited.error.isEmpty() && limited.path.isEmpty() && !QFile::exists(limitedPath),
        "speed processing beyond the five-minute limit must reject without publishing a file");
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
        const QString artifactRoot = qEnvironmentVariable("JAM2_TEST_ARTIFACT_ROOT");
        require(!artifactRoot.isEmpty(), "CTest omitted build-local artifact root");
        QTemporaryDir root(QDir(artifactRoot).absoluteFilePath(QStringLiteral("mix-render-XXXXXX")));
        require(root.isValid(), "could not create mix-render fixture");
        const auto project = toneProject(root.path());
        testIndependentSpeedAndPitch(root.path(), project);
        testShortInputAndLimit(root.path());
        std::cout << "Prepared mix renderer tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Prepared mix renderer tests failed: " << error.what() << '\n';
        return 1;
    }
}

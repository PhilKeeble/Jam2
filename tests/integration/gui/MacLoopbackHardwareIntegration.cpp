#include "pcm16_wav.hpp"

#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
constexpr int kSampleRate = 48000;
constexpr double kPi = 3.14159265358979323846;

void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

void writeTone(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "could not open test tone WAV");
    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);
    constexpr quint32 frames = kSampleRate * 5;
    out.writeRawData("RIFF", 4);
    out << quint32(36 + frames * 2);
    out.writeRawData("WAVEfmt ", 8);
    out << quint32(16) << quint16(1) << quint16(1) << quint32(kSampleRate)
        << quint32(kSampleRate * 2) << quint16(2) << quint16(16);
    out.writeRawData("data", 4);
    out << quint32(frames * 2);
    for (quint32 frame = 0; frame < frames; ++frame) {
        out << qint16(std::lrint(2048 * std::sin(2 * kPi * 440 * frame / kSampleRate)));
    }
    require(out.status() == QDataStream::Ok && file.flush(), "could not write test tone WAV");
}

struct Processes {
    QProcess recorder;
    QProcess player;
    ~Processes()
    {
        for (auto* process : {&recorder, &player}) {
            if (process->state() != QProcess::NotRunning) {
                process->kill();
                process->waitForFinished(5000);
            }
        }
    }
};
} // namespace

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    try {
        require(argc == 2, "hardware test requires the public Jam2 executable");
        const auto artifacts = qEnvironmentVariable("JAM2_TEST_ARTIFACT_ROOT");
        require(!artifacts.isEmpty(), "hardware test requires CTest artifact root");
        QTemporaryDir root(QDir(artifacts).filePath("mac-loopback-hardware-XXXXXX"));
        require(root.isValid(), "could not create hardware loopback workspace");
        const auto tone = QDir(root.path()).filePath("tone-440.wav");
        const auto recorded = QDir(root.path()).filePath("captured.wav");
        writeTone(tone);
        Processes processes;
        processes.player.start(QStringLiteral("/usr/bin/afplay"), {tone});
        require(processes.player.waitForStarted(5000), "could not play the test tone through the system output");
        processes.recorder.setProcessChannelMode(QProcess::MergedChannels);
        processes.recorder.start(QString::fromLocal8Bit(argv[1]),
            {QStringLiteral("debug"), QStringLiteral("loopback"), recorded, QStringLiteral("3")});
        require(processes.recorder.waitForStarted(5000), "could not start the public Jam2 recorder");
        require(processes.recorder.waitForFinished(30000), "native loopback timed out (check the macOS permission prompt)");
        const auto output = processes.recorder.readAll();
        std::cout << output.constData();
        require(processes.recorder.exitStatus() == QProcess::NormalExit &&
            processes.recorder.exitCode() == 0, "native system-output capture failed");
        require(output.contains("backend=coreaudio") && output.contains("queue_overrun_frames=0"),
            "hardware test must exercise native Core Audio capture without lost frames");
        const auto wav = jam2::wav::inspect_pcm16_file(recorded.toStdString());
        require(wav && wav.info.channels == 1 && wav.info.sample_rate == kSampleRate &&
            wav.info.frames >= kSampleRate && wav.info.frames <= kSampleRate * 4,
            "hardware recording has the wrong format or duration");
        QFile file(recorded);
        require(file.open(QIODevice::ReadOnly) && file.seek(static_cast<qint64>(wav.info.data_offset)),
            "could not inspect captured waveform");
        const auto bytes = file.readAll();
        double bestToneFraction = 0;
        double peak = 0;
        constexpr std::size_t window = 4800;
        for (std::size_t start = 0; start + window <= wav.info.frames; start += window) {
            double energy = 0, sine = 0, cosine = 0;
            for (std::size_t index = 0; index < window; ++index) {
                const double sample = qFromLittleEndian<qint16>(
                    bytes.constData() + (start + index) * 2) / 32768.0;
                peak = std::max(peak, std::abs(sample));
                const double phase = 2 * kPi * 440 * index / kSampleRate;
                sine += sample * std::sin(phase);
                cosine += sample * std::cos(phase);
                energy += sample * sample;
            }
            if (energy > 1.0e-8) bestToneFraction = std::max(bestToneFraction,
                2 * (sine * sine + cosine * cosine) / (window * energy));
        }
        std::cout << "captured_frames=" << wav.info.frames << " peak=" << peak
            << " tone_440_energy_fraction=" << bestToneFraction << '\n';
        require(peak > 1.0e-5 && bestToneFraction > 0.75,
            "captured WAV must contain the played 440 Hz tone (energy fraction > 0.75)");
        std::cout << "Native macOS system-output recording passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "macOS loopback hardware test failed: " << error.what() << '\n';
        return 1;
    }
}

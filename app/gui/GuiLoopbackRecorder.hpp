#pragma once

#include <QString>
#include <QStringList>

#include <atomic>
#include <cstdint>
#include <functional>
#include <span>
#include <thread>
#include <vector>

namespace jam2::gui {

std::vector<std::int16_t> resample_pcm16_interleaved(
    std::span<const std::int16_t> input,
    int channels,
    int sourceSampleRate,
    int targetSampleRate);

std::vector<std::int16_t> resample_pcm16_mono(
    std::span<const std::int16_t> input,
    int sourceSampleRate,
    int targetSampleRate);

std::vector<std::int16_t> trim_loopback_silence_pcm16(
    std::vector<std::int16_t> input,
    double silenceThresholdDb,
    std::uint64_t trailingSilenceFrames,
    bool trimLeading,
    bool trimTrailing);

}

struct GuiLoopbackOptions {
    QString source = QStringLiteral("default");
    QString outputPath;
    int targetSampleRate = 0;
    int durationBars = 0;
    double bpm = 120.0;
    int beatsPerBar = 4;
    int tempoPulseUnits = 1;
    double silenceThresholdDb = -50.0;
    int tailSilenceMs = 1000;
    bool trimLeadingSilence = true;
    bool trimTrailingSilence = true;
};

class GuiLoopbackRecorder {
public:
    using FinishedCallback = std::function<void(
        bool ok,
        const QString& outputPath,
        const QString& error,
        const QString& diagnostics)>;

    GuiLoopbackRecorder();
    ~GuiLoopbackRecorder();

    GuiLoopbackRecorder(const GuiLoopbackRecorder&) = delete;
    GuiLoopbackRecorder& operator=(const GuiLoopbackRecorder&) = delete;

    bool isRunning() const;
    bool start(const GuiLoopbackOptions& options, FinishedCallback finished, QString* error);
    void stop();

    static QStringList listSources(QString* error = nullptr);

private:
    void run(GuiLoopbackOptions options, FinishedCallback finished) noexcept;

    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
    std::thread thread_;
};

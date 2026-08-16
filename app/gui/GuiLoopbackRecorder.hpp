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

namespace jam2::gui {

enum class LoopbackSampleEncoding {
    Unsupported,
    Pcm16,
    Pcm24,
    Pcm32,
    Float32,
};

enum class LoopbackCaptureContent {
    Audio,
    NoFrames,
    SilenceOnly,
};

LoopbackCaptureContent classify_loopback_capture_content(
    std::uint64_t rawFrames,
    std::uint64_t retainedFrames) noexcept;

QString loopback_capture_content_error(LoopbackCaptureContent content);

double decode_loopback_sample(
    std::span<const std::uint8_t> frame,
    LoopbackSampleEncoding encoding,
    int channel) noexcept;

std::int16_t normalized_to_pcm16(double value) noexcept;

void write_loopback_wav_pcm16(
    const QString& outputPath,
    int sampleRate,
    std::span<const std::int16_t> samples);

class LoopbackTakeAccumulator final {
public:
    LoopbackTakeAccumulator(
        const GuiLoopbackOptions& options,
        int sampleRate,
        std::uint64_t targetRecordedFrames);

    void push(std::int16_t sample);
    bool reachedDuration() const noexcept;
    std::uint64_t rawFrames() const noexcept;
    std::uint64_t recordedFrames() const noexcept;
    std::vector<std::int16_t> finish();
    double peakDbfs() const noexcept;

private:
    double silenceThresholdDb_ = -50.0;
    std::uint64_t targetRecordedFrames_ = 0;
    std::uint64_t recordedFrames_ = 0;
    std::uint64_t tailSilenceFrames_ = 0;
    std::uint64_t rawFrames_ = 0;
    double peak_ = 0.0;
    bool trimLeading_ = true;
    bool trimTrailing_ = true;
    std::vector<std::int16_t> samples_;
};

} // namespace jam2::gui

struct GuiLoopbackCaptureResult {
    bool ok = false;
    QString error;
    QString diagnostics;
};

class GuiLoopbackRecorder {
public:
    using FinishedCallback = std::function<void(
        bool ok,
        const QString& outputPath,
        const QString& error,
        const QString& diagnostics)>;
    using CaptureBackend = std::function<GuiLoopbackCaptureResult(
        const GuiLoopbackOptions& options,
        const std::atomic<bool>& stopRequested)>;

    GuiLoopbackRecorder();
    explicit GuiLoopbackRecorder(CaptureBackend captureBackend);
    ~GuiLoopbackRecorder();

    GuiLoopbackRecorder(const GuiLoopbackRecorder&) = delete;
    GuiLoopbackRecorder& operator=(const GuiLoopbackRecorder&) = delete;

    bool isRunning() const;
    bool setCaptureBackendForTesting(CaptureBackend captureBackend, QString* error = nullptr);
    bool start(const GuiLoopbackOptions& options, FinishedCallback finished, QString* error);
    void stop();

    static QStringList listSources(QString* error = nullptr);

private:
    void run(GuiLoopbackOptions options, FinishedCallback finished) noexcept;

    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};
    CaptureBackend captureBackend_;
    std::thread thread_;
};

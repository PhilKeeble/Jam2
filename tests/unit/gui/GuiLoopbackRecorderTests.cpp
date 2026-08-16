#include "GuiLoopbackRecorder.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void requireThrows(Function function, const std::string& message)
{
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

QString canonicalPath(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QTemporaryDir makeRoot(const QString& name)
{
    const QString artifactRoot = qEnvironmentVariable("JAM2_TEST_ARTIFACT_ROOT");
    require(!artifactRoot.isEmpty(), "CTest omitted the build-local artifact root");
    QTemporaryDir root(QDir(artifactRoot).absoluteFilePath(name + QStringLiteral("-XXXXXX")));
    require(root.isValid(), "could not create build-local loopback fixture");
    require(canonicalPath(root.path()).startsWith(
                canonicalPath(artifactRoot) + QLatin1Char('/'), Qt::CaseInsensitive),
        "loopback fixture escaped build/test-artifacts");
    return root;
}

template <typename Function>
bool waitUntil(Function condition, std::chrono::milliseconds timeout = 2000ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(1ms);
    }
    return condition();
}

std::uint16_t readU16(const QByteArray& bytes, qsizetype offset)
{
    require(offset >= 0 && offset + 2 <= bytes.size(), "WAV u16 read escaped fixture");
    return static_cast<std::uint16_t>(
        static_cast<std::uint8_t>(bytes[offset]) |
        (static_cast<std::uint16_t>(static_cast<std::uint8_t>(bytes[offset + 1])) << 8));
}

std::uint32_t readU32(const QByteArray& bytes, qsizetype offset)
{
    require(offset >= 0 && offset + 4 <= bytes.size(), "WAV u32 read escaped fixture");
    return static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset])) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + 1])) << 8) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + 2])) << 16) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + 3])) << 24);
}

QByteArray readAll(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "could not open generated loopback WAV");
    return file.readAll();
}

void testSampleDecodingAndConversion()
{
    using jam2::gui::LoopbackSampleEncoding;
    const std::array<std::uint8_t, 4> pcm16{
        0x00, 0x80, 0x00, 0x40};
    require(jam2::gui::decode_loopback_sample(
                pcm16, LoopbackSampleEncoding::Pcm16, 0) == -1.0 &&
            jam2::gui::decode_loopback_sample(
                pcm16, LoopbackSampleEncoding::Pcm16, 1) == 0.5,
        "PCM16 decoding must preserve signed little-endian channels");

    const std::array<std::uint8_t, 6> pcm24{
        0xff, 0xff, 0x7f, 0x00, 0x00, 0x80};
    require(jam2::gui::decode_loopback_sample(
                pcm24, LoopbackSampleEncoding::Pcm24, 0) > 0.999999 &&
            jam2::gui::decode_loopback_sample(
                pcm24, LoopbackSampleEncoding::Pcm24, 1) == -1.0,
        "PCM24 decoding must preserve maximum and sign-extended minimum values");

    std::array<std::uint8_t, 8> pcm32{};
    const std::int32_t pcm32First = (std::numeric_limits<std::int32_t>::max)();
    const std::int32_t pcm32Second = (std::numeric_limits<std::int32_t>::min)();
    std::memcpy(pcm32.data(), &pcm32First, sizeof(pcm32First));
    std::memcpy(pcm32.data() + sizeof(pcm32First), &pcm32Second, sizeof(pcm32Second));
    require(jam2::gui::decode_loopback_sample(
                pcm32, LoopbackSampleEncoding::Pcm32, 0) > 0.999999999 &&
            jam2::gui::decode_loopback_sample(
                pcm32, LoopbackSampleEncoding::Pcm32, 1) == -1.0,
        "PCM32 decoding must preserve signed full-scale channels");

    std::array<std::uint8_t, 12> floats{};
    const float clipped = 1.5F;
    const float negative = -0.25F;
    const float invalid = std::numeric_limits<float>::quiet_NaN();
    std::memcpy(floats.data(), &clipped, sizeof(clipped));
    std::memcpy(floats.data() + sizeof(clipped), &negative, sizeof(negative));
    std::memcpy(floats.data() + sizeof(clipped) + sizeof(negative), &invalid, sizeof(invalid));
    require(jam2::gui::decode_loopback_sample(
                floats, LoopbackSampleEncoding::Float32, 0) == 1.0 &&
            jam2::gui::decode_loopback_sample(
                floats, LoopbackSampleEncoding::Float32, 1) == -0.25 &&
            jam2::gui::decode_loopback_sample(
                floats, LoopbackSampleEncoding::Float32, 2) == 0.0,
        "float decoding must clamp finite values and silence non-finite input");
    require(jam2::gui::decode_loopback_sample(
                pcm16, LoopbackSampleEncoding::Unsupported, 0) == 0.0 &&
            jam2::gui::decode_loopback_sample(
                pcm16, LoopbackSampleEncoding::Pcm32, 1) == 0.0 &&
            jam2::gui::decode_loopback_sample(
                pcm16, LoopbackSampleEncoding::Pcm16, -1) == 0.0,
        "unsupported, truncated, and negative-channel decode shapes must fail silent");

    require(jam2::gui::normalized_to_pcm16(-2.0) == -32767 &&
            jam2::gui::normalized_to_pcm16(-1.0) == -32767 &&
            jam2::gui::normalized_to_pcm16(0.0) == 0 &&
            jam2::gui::normalized_to_pcm16(0.5) == 16384 &&
            jam2::gui::normalized_to_pcm16(2.0) == 32767 &&
            jam2::gui::normalized_to_pcm16(
                std::numeric_limits<double>::infinity()) == 0 &&
            jam2::gui::normalized_to_pcm16(
                std::numeric_limits<double>::quiet_NaN()) == 0,
        "normalized conversion must clamp finite input and silence non-finite input");
}

void testResamplingTrimmingAndAccumulator()
{
    using jam2::gui::LoopbackCaptureContent;
    require(jam2::gui::classify_loopback_capture_content(0, 0) ==
                LoopbackCaptureContent::NoFrames &&
            jam2::gui::classify_loopback_capture_content(48000, 0) ==
                LoopbackCaptureContent::SilenceOnly &&
            jam2::gui::classify_loopback_capture_content(48000, 1) ==
                LoopbackCaptureContent::Audio,
        "loopback capture classification must distinguish missing frames, trimmed silence, and audio");
    require(jam2::gui::loopback_capture_content_error(
                LoopbackCaptureContent::NoFrames).contains(
                    QStringLiteral("No audio was captured")) &&
            jam2::gui::loopback_capture_content_error(
                LoopbackCaptureContent::SilenceOnly).contains(
                    QStringLiteral("Only silence was detected")) &&
            jam2::gui::loopback_capture_content_error(
                LoopbackCaptureContent::Audio).isEmpty(),
        "capture classification must provide exact user-facing failures only for unusable takes");

    const std::array<std::int16_t, 8> stereo{
        1000, -1000, 2000, -2000, 3000, -3000, 4000, -4000};
    require(jam2::gui::resample_pcm16_interleaved(stereo, 2, 48000, 48000) ==
            std::vector<std::int16_t>(stereo.begin(), stereo.end()),
        "identity resampling must preserve exact interleaved samples");
    const std::vector<std::int16_t> down =
        jam2::gui::resample_pcm16_interleaved(stereo, 2, 48000, 24000);
    const std::vector<std::int16_t> up =
        jam2::gui::resample_pcm16_interleaved(stereo, 2, 48000, 96000);
    require(down.size() == 4 && up.size() == 16 &&
            down[0] > 0 && down[1] < 0 && up[0] > 0 && up[1] < 0,
        "band-limited resampling must preserve frame count and channel polarity");
    require(jam2::gui::resample_pcm16_mono({}, 48000, 44100).empty(),
        "empty mono resampling must stay empty");
    requireThrows([] {
        (void)jam2::gui::resample_pcm16_interleaved({}, 0, 48000, 48000);
    }, "nonpositive channel count must be rejected");
    requireThrows([] {
        const std::array<std::int16_t, 3> unaligned{1, 2, 3};
        (void)jam2::gui::resample_pcm16_interleaved(unaligned, 2, 48000, 48000);
    }, "unaligned interleaved samples must be rejected");

    const std::vector<std::int16_t> shaped{0, 10, 5000, -4000, 20, 0, 0};
    require(jam2::gui::trim_loopback_silence_pcm16(
                shaped, -40.0, 2, true, true) ==
            std::vector<std::int16_t>({5000, -4000}),
        "leading silence and a qualifying quiet tail must be trimmed exactly");
    require(jam2::gui::trim_loopback_silence_pcm16(
                shaped, -40.0, 4, true, true) ==
            std::vector<std::int16_t>({5000, -4000, 20, 0, 0}),
        "a quiet tail shorter than the configured threshold must be retained");
    require(jam2::gui::trim_loopback_silence_pcm16(
                shaped, -40.0, 0, false, false) == shaped,
        "disabled leading/trailing trim must preserve all samples");

    GuiLoopbackOptions options;
    options.silenceThresholdDb = -40.0;
    options.tailSilenceMs = 2;
    jam2::gui::LoopbackTakeAccumulator accumulator(options, 1000, 5);
    require(!accumulator.reachedDuration() && accumulator.rawFrames() == 0 &&
            accumulator.recordedFrames() == 0 &&
            std::isinf(accumulator.peakDbfs()) && accumulator.peakDbfs() < 0.0,
        "empty accumulator must expose zero counts and negative-infinite peak");
    const std::array<std::int16_t, 5> accumulatedSamples{
        0, 0, 1000, static_cast<std::int16_t>(-32768), 0};
    for (std::int16_t sample : accumulatedSamples) {
        accumulator.push(sample);
    }
    require(accumulator.reachedDuration() && accumulator.rawFrames() == 5 &&
            accumulator.recordedFrames() == 5 && accumulator.peakDbfs() == 0.0 &&
            accumulator.finish() ==
                std::vector<std::int16_t>({1000, static_cast<std::int16_t>(-32768), 0}),
        "accumulator must own duration, raw/recorded counts, peak, and trim policy");
}

void testAtomicWavWriter()
{
    QTemporaryDir root = makeRoot(QStringLiteral("loopback-wav"));
    const QString path = QDir(root.path()).absoluteFilePath(
        QStringLiteral("nested/loopback-♫.wav"));
    const std::array<std::int16_t, 4> samples{0, -32768, 32767, 1234};
    jam2::gui::write_loopback_wav_pcm16(path, 48000, samples);
    const QByteArray bytes = readAll(path);
    require(bytes.size() == 44 + static_cast<qsizetype>(samples.size() * 2) &&
            bytes.first(4) == QByteArrayLiteral("RIFF") &&
            readU32(bytes, 4) == 36 + samples.size() * 2 &&
            bytes.mid(8, 4) == QByteArrayLiteral("WAVE") &&
            bytes.mid(12, 4) == QByteArrayLiteral("fmt ") &&
            readU16(bytes, 20) == 1 && readU16(bytes, 22) == 1 &&
            readU32(bytes, 24) == 48000 && readU32(bytes, 28) == 96000 &&
            readU16(bytes, 32) == 2 && readU16(bytes, 34) == 16 &&
            bytes.mid(36, 4) == QByteArrayLiteral("data") &&
            readU32(bytes, 40) == samples.size() * 2 &&
            static_cast<std::int16_t>(readU16(bytes, 46)) == -32768 &&
            static_cast<std::int16_t>(readU16(bytes, 48)) == 32767,
        "atomic WAV writer must emit exact mono PCM16 RIFF shape and samples");
    requireThrows([&] {
        jam2::gui::write_loopback_wav_pcm16({}, 48000, samples);
    }, "blank WAV path must be rejected");
    requireThrows([&] {
        jam2::gui::write_loopback_wav_pcm16(path, 0, samples);
    }, "nonpositive WAV sample rate must be rejected");
    requireThrows([&] {
        jam2::gui::write_loopback_wav_pcm16(root.path(), 48000, samples);
    }, "directory WAV target must be rejected");
    require(readAll(path) == bytes,
        "a separate failed atomic write must not mutate a completed WAV");
}

struct CompletionState {
    std::mutex mutex;
    std::condition_variable changed;
    int count = 0;
    bool ok = false;
    QString path;
    QString error;
    QString diagnostics;
};

GuiLoopbackRecorder::FinishedCallback completionCallback(CompletionState& state)
{
    return [&state](bool ok, const QString& path, const QString& error,
               const QString& diagnostics) {
        {
            const std::lock_guard lock(state.mutex);
            ++state.count;
            state.ok = ok;
            state.path = path;
            state.error = error;
            state.diagnostics = diagnostics;
        }
        state.changed.notify_all();
    };
}

bool waitForCompletions(CompletionState& state, int count)
{
    std::unique_lock lock(state.mutex);
    return state.changed.wait_for(lock, 2s, [&state, count] {
        return state.count >= count;
    });
}

void testRecorderLifecycleWithInjectedAudio()
{
    QTemporaryDir root = makeRoot(QStringLiteral("loopback-lifecycle"));
    GuiLoopbackOptions valid;
    valid.outputPath = QDir(root.path()).absoluteFilePath(QStringLiteral("capture.wav"));
    valid.targetSampleRate = 48000;
    valid.durationBars = 0;

    std::atomic<int> backendCalls{0};
    std::atomic<bool> releaseFirst{false};
    GuiLoopbackRecorder recorder(
        [&backendCalls, &releaseFirst](const GuiLoopbackOptions& options,
            const std::atomic<bool>& stopRequested) {
            const int call = backendCalls.fetch_add(1, std::memory_order_acq_rel) + 1;
            if (call == 1) {
                while (!releaseFirst.load(std::memory_order_acquire) &&
                       !stopRequested.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(1ms);
                }
                const std::array<std::int16_t, 3> samples{100, -200, 300};
                jam2::gui::write_loopback_wav_pcm16(
                    options.outputPath, options.targetSampleRate, samples);
                return GuiLoopbackCaptureResult{
                    true, {}, QStringLiteral("fake injected frames=3")};
            }
            return GuiLoopbackCaptureResult{
                false, QStringLiteral("injected failure"), QStringLiteral("fake failure")};
        });
    CompletionState completion;
    QString error;

    require(!recorder.setCaptureBackendForTesting({}, &error) &&
            error == QStringLiteral("loopback capture backend is required"),
        "recorder must reject an empty replacement backend");

    GuiLoopbackOptions invalid = valid;
    invalid.outputPath.clear();
    require(!recorder.start(invalid, completionCallback(completion), &error) &&
            error.contains(QStringLiteral("output path")) && !recorder.isRunning(),
        "recorder must reject a missing output path synchronously");
    invalid = valid;
    invalid.targetSampleRate = 0;
    require(!recorder.start(invalid, completionCallback(completion), &error) &&
            error.contains(QStringLiteral("sample rate")),
        "recorder must reject a nonpositive target sample rate synchronously");
    invalid = valid;
    invalid.durationBars = -1;
    require(!recorder.start(invalid, completionCallback(completion), nullptr),
        "recorder must reject a negative bar duration with a null error sink");
    invalid = valid;
    invalid.durationBars = 1;
    invalid.bpm = std::numeric_limits<double>::quiet_NaN();
    require(!recorder.start(invalid, completionCallback(completion), &error) &&
            error.contains(QStringLiteral("positive bars")),
        "recorder must reject a timed take with invalid tempo shape");

    require(recorder.start(valid, completionCallback(completion), &error) &&
            waitUntil([&backendCalls] {
                return backendCalls.load(std::memory_order_acquire) == 1;
            }) && recorder.isRunning(),
        "injected recorder must enter running state and invoke its backend once");
    require(!recorder.setCaptureBackendForTesting(
                [](const GuiLoopbackOptions&, const std::atomic<bool>&) {
                    return GuiLoopbackCaptureResult{};
                },
                &error) && error.contains(QStringLiteral("while recording")),
        "recorder must reject backend replacement during an active capture");
    require(!recorder.start(valid, completionCallback(completion), &error) &&
            error == QStringLiteral("loopback recorder is already running"),
        "a concurrent start must be rejected without launching another backend");
    releaseFirst.store(true, std::memory_order_release);
    require(waitForCompletions(completion, 1) &&
            waitUntil([&recorder] { return !recorder.isRunning(); }),
        "successful injected capture did not complete and leave running state");
    {
        const std::lock_guard lock(completion.mutex);
        require(completion.ok && completion.path == valid.outputPath &&
                completion.error.isEmpty() &&
                completion.diagnostics == QStringLiteral("fake injected frames=3"),
            "successful backend result must reach the completion callback exactly");
    }
    require(QFileInfo(valid.outputPath).size() == 50,
        "fake injected audio must pass through the production WAV writer");

    require(recorder.start(valid, completionCallback(completion), &error) &&
            waitForCompletions(completion, 2) &&
            waitUntil([&recorder] { return !recorder.isRunning(); }),
        "recorder must join and reuse a completed worker for a second capture");
    {
        const std::lock_guard lock(completion.mutex);
        require(!completion.ok && completion.error == QStringLiteral("injected failure") &&
                completion.diagnostics == QStringLiteral("fake failure"),
            "backend failure must preserve its diagnostic channels");
    }

    require(recorder.setCaptureBackendForTesting(
                [](const GuiLoopbackOptions& options, const std::atomic<bool>&) {
                    const std::array<std::int16_t, 1> samples{321};
                    jam2::gui::write_loopback_wav_pcm16(
                        options.outputPath, options.targetSampleRate, samples);
                    return GuiLoopbackCaptureResult{
                        true, {}, QStringLiteral("replacement backend")};
                },
                &error) && error.isEmpty(),
        "completed recorder must accept a replacement test backend");
    CompletionState replaced;
    require(recorder.start(valid, completionCallback(replaced), &error) &&
            waitForCompletions(replaced, 1) && replaced.ok &&
            replaced.diagnostics == QStringLiteral("replacement backend"),
        "replacement test backend must own the next capture");

    CompletionState thrown;
    GuiLoopbackRecorder throwing([](const GuiLoopbackOptions&,
                                  const std::atomic<bool>&) -> GuiLoopbackCaptureResult {
        throw std::runtime_error("injected exception");
    });
    require(throwing.start(valid, completionCallback(thrown), &error) &&
            waitForCompletions(thrown, 1),
        "throwing backend did not reach the completion boundary");
    {
        const std::lock_guard lock(thrown.mutex);
        require(!thrown.ok && thrown.error == QStringLiteral("injected exception") &&
                thrown.diagnostics.isEmpty(),
            "worker exception must be caught and reported without diagnostics corruption");
    }

    std::atomic<int> throwingCallbackCalls{0};
    GuiLoopbackRecorder callbackBoundary([](const GuiLoopbackOptions&,
                                           const std::atomic<bool>&) {
        return GuiLoopbackCaptureResult{true, {}, QStringLiteral("callback boundary")};
    });
    require(callbackBoundary.start(valid,
                [&throwingCallbackCalls](bool, const QString&, const QString&, const QString&) {
                    throwingCallbackCalls.fetch_add(1, std::memory_order_release);
                    throw std::runtime_error("observer exception");
                },
                &error) &&
            waitUntil([&throwingCallbackCalls] {
                return throwingCallbackCalls.load(std::memory_order_acquire) == 1;
            }) &&
            waitUntil([&callbackBoundary] { return !callbackBoundary.isRunning(); }),
        "throwing completion observer did not reach the protected thread boundary");
    CompletionState afterThrow;
    require(callbackBoundary.start(valid, completionCallback(afterThrow), &error) &&
            waitForCompletions(afterThrow, 1) && afterThrow.ok,
        "a completion observer exception must not terminate or poison recorder reuse");

    CompletionState stopped;
    std::atomic<bool> stopObserved{false};
    {
        GuiLoopbackRecorder stoppable(
            [&stopObserved](const GuiLoopbackOptions&,
                const std::atomic<bool>& stopRequested) {
                while (!stopRequested.load(std::memory_order_acquire)) {
                    std::this_thread::sleep_for(1ms);
                }
                stopObserved.store(true, std::memory_order_release);
                return GuiLoopbackCaptureResult{
                    false, QStringLiteral("stopped"), QStringLiteral("stop observed")};
            });
        require(stoppable.start(valid, completionCallback(stopped), &error) &&
                waitUntil([&stoppable] { return stoppable.isRunning(); }),
            "stoppable injected recorder did not start");
    }
    require(stopObserved.load(std::memory_order_acquire) &&
            waitForCompletions(stopped, 1),
        "recorder destruction must request stop, join, and deliver final completion");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        QCoreApplication application(argc, argv);
        testSampleDecodingAndConversion();
        testResamplingTrimmingAndAccumulator();
        testAtomicWavWriter();
        testRecorderLifecycleWithInjectedAudio();
        std::cout << "GUI loopback recorder tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "GUI loopback recorder test failed: " << error.what() << '\n';
        return 1;
    }
}

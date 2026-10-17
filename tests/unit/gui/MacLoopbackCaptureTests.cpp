#include "MacLoopbackCapture.hpp"
#include "pcm16_wav.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {
using namespace jam2::gui;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

class FakeApi final : public MacLoopbackApi {
public:
    enum class Failure { None, Tap, Format, Aggregate, Callback, Start, Stop, Remove, ChangeFormat, ChangeDefault };
    Failure failure = Failure::None;
    AudioStreamBasicDescription format{};
    std::vector<std::string> events;
    MacLoopbackOutput chosen;
    bool planar = false;
    bool silence = false;
    bool malformed = false;
    std::size_t frames = 512;
    int formatReads = 0;
    std::atomic<bool>* stopOnStart = nullptr;
    std::atomic<bool>* stopOnTap = nullptr;
    AudioDeviceIOProc callback = nullptr;
    void* context = nullptr;
    bool reverseOutputs = false;

    FakeApi()
    {
        format.mSampleRate = 48000;
        format.mFormatID = kAudioFormatLinearPCM;
        format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        format.mBitsPerChannel = 32;
        format.mChannelsPerFrame = 2;
        format.mBytesPerFrame = 8;
        format.mFramesPerPacket = 1;
        format.mBytesPerPacket = 8;
    }

    std::vector<MacLoopbackOutput> outputs() override
    {
        std::vector<MacLoopbackOutput> result{
            {10, QStringLiteral("speakers[1]"), QStringLiteral("Built-in speakers")},
            {20, QStringLiteral("usb-output"), QStringLiteral("USB output")}};
        if (reverseOutputs) std::reverse(result.begin(), result.end());
        return result;
    }
    AudioDeviceID defaultOutput() override
    { return failure == Failure::ChangeDefault && callback ? 20 : 10; }
    AudioObjectID createTap(const MacLoopbackOutput& output) override
    {
        events.push_back("tap");
        if (failure == Failure::Tap) throw std::runtime_error("permission denied");
        chosen = output;
        if (stopOnTap) stopOnTap->store(true);
        return 30;
    }
    AudioStreamBasicDescription tapFormat(AudioObjectID) override
    {
        ++formatReads;
        if (failure == Failure::Format) throw std::runtime_error("read format failed");
        auto result = format;
        if (failure == Failure::ChangeFormat && formatReads > 1) result.mSampleRate = 44100;
        return result;
    }
    AudioDeviceID createAggregate(AudioObjectID) override
    {
        events.push_back("aggregate");
        if (failure == Failure::Aggregate) throw std::runtime_error("aggregate failed");
        return 40;
    }
    AudioDeviceIOProcID createIoProc(AudioDeviceID, AudioDeviceIOProc cb, void* ctx) override
    {
        events.push_back("callback");
        if (failure == Failure::Callback) throw std::runtime_error("callback failed");
        callback = cb;
        context = ctx;
        return cb;
    }
    void start(AudioDeviceID device, AudioDeviceIOProcID) override
    {
        events.push_back("start");
        if (failure == Failure::Start) throw std::runtime_error("start failed");
        // Feed the real production IOProc with either interleaved or planar
        // stereo, including a silent second channel to check active fold-down.
        std::vector<float> left(frames), right(frames, 0), interleaved(frames * 2);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            left[frame] = silence ? 0 : (frame % 48 < 24 ? 0.25F : -0.25F);
            interleaved[frame * 2] = left[frame];
        }
        struct StereoBuffers { UInt32 count; AudioBuffer buffers[2]; } input{};
        input.count = planar ? 2 : 1;
        input.buffers[0] = {planar ? 1U : 2U,
            static_cast<UInt32>(frames * sizeof(float) * (planar ? 1 : 2)),
            planar ? left.data() : interleaved.data()};
        input.buffers[1] = {1, static_cast<UInt32>(frames * sizeof(float)), right.data()};
        if (malformed) input.buffers[0].mNumberChannels = 3;
        const AudioTimeStamp timestamp{};
        AudioBufferList output{};
        callback(device, &timestamp, reinterpret_cast<AudioBufferList*>(&input), &timestamp,
            &output, &timestamp, context);
        if (stopOnStart) stopOnStart->store(true);
    }
    bool outputAlive(AudioDeviceID) override { return failure != Failure::Remove; }
    OSStatus stop(AudioDeviceID, AudioDeviceIOProcID) noexcept override
    { events.push_back("stop"); return failure == Failure::Stop ? -1 : noErr; }
    OSStatus destroyIoProc(AudioDeviceID, AudioDeviceIOProcID) noexcept override
    { events.push_back("destroy-callback"); return noErr; }
    OSStatus destroyAggregate(AudioDeviceID) noexcept override
    { events.push_back("destroy-aggregate"); return noErr; }
    OSStatus destroyTap(AudioObjectID) noexcept override
    { events.push_back("destroy-tap"); return noErr; }
};

GuiLoopbackOptions options(const QString& path)
{
    GuiLoopbackOptions result;
    result.outputPath = path;
    result.targetSampleRate = 48000;
    result.durationBars = 1;
    result.beatsPerBar = 1;
    result.bpm = 12000; // Exactly 240 source frames.
    return result;
}

void expectClosed(const FakeApi& api)
{
    const std::vector<std::string> suffix{"stop", "destroy-callback", "destroy-aggregate", "destroy-tap"};
    require(api.events.size() >= suffix.size() && std::equal(suffix.begin(), suffix.end(),
        api.events.end() - static_cast<std::ptrdiff_t>(suffix.size())),
        "capture must stop and destroy callback, aggregate, and tap in ownership order");
}

void testRecordedSamples(const QString& root)
{
    for (const bool planar : {false, true}) {
        FakeApi api;
        api.planar = planar;
        if (planar) {
            api.format.mFormatFlags |= kAudioFormatFlagIsNonInterleaved;
            api.format.mBytesPerFrame = api.format.mBytesPerPacket = sizeof(float);
        }
        auto opts = options(QDir(root).filePath(planar ? "planar.wav" : "stereo.wav"));
        std::atomic<bool> stop{false};
        const auto result = capture_mac_loopback(opts, stop, api);
        require(result.ok && result.error.isEmpty(), "native callback capture must produce a WAV");
        const auto wav = jam2::wav::inspect_pcm16_file(opts.outputPath.toStdString());
        require(wav && wav.info.channels == 1 && wav.info.frames == 240 &&
            wav.info.sample_rate == 48000, "captured WAV must have the exact duration and format");
        QFile file(opts.outputPath);
        require(file.open(QIODevice::ReadOnly) && file.seek(static_cast<qint64>(wav.info.data_offset)),
            "captured samples must be readable");
        const auto bytes = file.readAll();
        for (int frame = 0; frame < 240; ++frame) {
            const auto sample = qFromLittleEndian<qint16>(bytes.constData() + frame * 2);
            require(sample == (frame % 48 < 24 ? 8192 : -8192),
                "captured PCM must preserve waveform, polarity, and active-channel amplitude");
        }
        require(result.diagnostics.contains("queue_overrun_frames=0") &&
            result.diagnostics.contains("queue_capacity_frames=96000"),
            "native capture must expose bounded queue capacity and lost frames");
        require(api.chosen.device == 10, "default capture must use the system output");
        expectClosed(api);
    }

    FakeApi api;
    auto opts = options(QDir(root).filePath("resampled.wav"));
    opts.targetSampleRate = 44100;
    std::atomic<bool> stop{false};
    const auto result = capture_mac_loopback(opts, stop, api);
    const auto wav = jam2::wav::inspect_pcm16_file(opts.outputPath.toStdString());
    require(result.ok && wav && wav.info.sample_rate == 44100 && wav.info.frames == 221,
        "native capture must resample to the track sample rate with the correct frame count");
}

void testSourceSelection(const QString& root)
{
    FakeApi api;
    const auto sources = mac_loopback_sources(api);
    require(sources.size() == 3 && sources[0].startsWith("[default]") &&
        sources[1].startsWith("[coreaudio:speakers%5B1%5D]"),
        "sources must offer an automatic default and escaped stable device UIDs");
    auto opts = options(QDir(root).filePath("selected.wav"));
    opts.source = QStringLiteral("coreaudio:usb-output");
    api.reverseOutputs = true;
    std::atomic<bool> stop{false};
    require(capture_mac_loopback(opts, stop, api).ok && api.chosen.device == 20,
        "selected output must survive device enumeration reordering");
    opts.source = QStringLiteral("coreaudio:missing");
    api.events.clear();
    const auto missing = capture_mac_loopback(opts, stop, api);
    require(!missing.ok && missing.error.contains("unavailable") && api.events.empty(),
        "missing output must fail without silently recording another output");
}

void testRollbackAndErrors(const QString& root)
{
    const std::array failures{FakeApi::Failure::Tap, FakeApi::Failure::Format,
        FakeApi::Failure::Aggregate, FakeApi::Failure::Callback, FakeApi::Failure::Start,
        FakeApi::Failure::Stop, FakeApi::Failure::Remove,
        FakeApi::Failure::ChangeFormat, FakeApi::Failure::ChangeDefault};
    int index = 0;
    for (const auto failure : failures) {
        FakeApi api;
        api.failure = failure;
        auto opts = options(QDir(root).filePath(QStringLiteral("failure-%1.wav").arg(index++)));
        std::atomic<bool> stop{false};
        const auto result = capture_mac_loopback(opts, stop, api);
        require(!result.ok && !result.error.isEmpty() && !QFileInfo::exists(opts.outputPath),
            "HAL errors and output changes must not publish a successful or incomplete take");
        if (failure != FakeApi::Failure::Tap) {
            require(api.events.back() == "destroy-tap", "partial startup must release the tap");
        }
        if (failure == FakeApi::Failure::Stop || failure == FakeApi::Failure::Remove ||
            failure == FakeApi::Failure::ChangeFormat || failure == FakeApi::Failure::ChangeDefault) expectClosed(api);
    }
    for (const bool overflow : {false, true}) {
        FakeApi api;
        api.malformed = !overflow;
        api.frames = overflow ? 100000 : 512;
        auto opts = options(QDir(root).filePath(overflow ? "overflow.wav" : "malformed.wav"));
        std::atomic<bool> stop{false};
        const auto result = capture_mac_loopback(opts, stop, api);
        require(!result.ok && !QFileInfo::exists(opts.outputPath),
            "malformed buffers and callback queue overruns must fail without saving damaged audio");
        expectClosed(api);
    }
    FakeApi unsupported;
    unsupported.format.mBitsPerChannel = 16;
    std::atomic<bool> stop{false};
    const auto result = capture_mac_loopback(options(QDir(root).filePath("format.wav")), stop, unsupported);
    require(!result.ok && result.error.contains("Unsupported") && unsupported.events.back() == "destroy-tap",
        "unsupported formats must fail and clean up before starting capture");
}

void testStopAndSilence(const QString& root)
{
    auto opts = options(QDir(root).filePath("manual.wav"));
    opts.durationBars = 0;
    FakeApi api;
    std::atomic<bool> stop{false};
    api.stopOnStart = &stop;
    require(capture_mac_loopback(opts, stop, api).ok,
        "manual stop must drain queued audio before committing the take");
    const auto wav = jam2::wav::inspect_pcm16_file(opts.outputPath.toStdString());
    require(wav && wav.info.frames == 512, "manual stop must retain the final captured block");
    expectClosed(api);

    FakeApi cancelled;
    require(!capture_mac_loopback(opts, stop, cancelled).ok && cancelled.events.empty(),
        "pre-start cancellation must not acquire HAL resources");
    stop.store(false);
    cancelled.stopOnTap = &stop;
    require(!capture_mac_loopback(opts, stop, cancelled).ok &&
        cancelled.events == std::vector<std::string>{"tap", "destroy-tap"},
        "cancellation during startup must roll back without starting capture");

    for (const bool noFrames : {false, true}) {
        FakeApi quiet;
        quiet.silence = true;
        quiet.frames = noFrames ? 0 : 512;
        stop.store(false);
        quiet.stopOnStart = &stop;
        opts.outputPath = QDir(root).filePath(noFrames ? "empty.wav" : "quiet.wav");
        const auto result = capture_mac_loopback(opts, stop, quiet);
        require(!result.ok && result.error.contains(noFrames ? "No audio" : "Only silence") &&
            !QFileInfo::exists(opts.outputPath), "empty/silent capture must not report a usable recording");
        expectClosed(quiet);
    }
}
} // namespace

void runMacLoopbackCaptureTests()
{
    const auto artifacts = qEnvironmentVariable("JAM2_TEST_ARTIFACT_ROOT");
    require(!artifacts.isEmpty(), "native capture tests require the CTest artifact root");
    QTemporaryDir root(QDir(artifacts).filePath("mac-loopback-XXXXXX"));
    require(root.isValid(), "could not create native loopback fixture");
    testRecordedSamples(root.path());
    testSourceSelection(root.path());
    testRollbackAndErrors(root.path());
    testStopAndSilence(root.path());
}

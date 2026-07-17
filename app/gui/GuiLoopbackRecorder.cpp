#include "GuiLoopbackRecorder.hpp"

#include "audio_device.hpp"

#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <audioclient.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <propsys.h>
#include <wrl/client.h>
#endif

namespace {

double ampFromDb(double db)
{
    return std::pow(10.0, db / 20.0);
}

void writeU16(std::ofstream& out, std::uint16_t value)
{
    const std::array<char, 2> bytes{
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8) & 0xffU),
    };
    out.write(bytes.data(), bytes.size());
}

void writeU32(std::ofstream& out, std::uint32_t value)
{
    const std::array<char, 4> bytes{
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8) & 0xffU),
        static_cast<char>((value >> 16) & 0xffU),
        static_cast<char>((value >> 24) & 0xffU),
    };
    out.write(bytes.data(), bytes.size());
}

void writeWav(const QString& outputPath, int sampleRate, const std::vector<std::int16_t>& samples)
{
    const QFileInfo info(outputPath);
    if (!info.absoluteDir().exists() && !QDir().mkpath(info.absolutePath())) {
        throw std::runtime_error("could not create output folder");
    }
    std::ofstream out(outputPath.toStdString(), std::ios::binary | std::ios::trunc);
    if (!out) {
        throw std::runtime_error("could not open output WAV");
    }
    constexpr std::uint16_t channels = 1;
    constexpr std::uint16_t bits = 16;
    const std::uint32_t frames = static_cast<std::uint32_t>(std::min<std::size_t>(
        samples.size(), std::numeric_limits<std::uint32_t>::max() / 2U));
    const std::uint32_t dataBytes = frames * channels * (bits / 8);
    out.write("RIFF", 4);
    writeU32(out, 36U + dataBytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    writeU32(out, 16);
    writeU16(out, 1);
    writeU16(out, channels);
    writeU32(out, static_cast<std::uint32_t>(sampleRate));
    writeU32(out, static_cast<std::uint32_t>(sampleRate * channels * (bits / 8)));
    writeU16(out, channels * (bits / 8));
    writeU16(out, bits);
    out.write("data", 4);
    writeU32(out, dataBytes);
    for (std::uint32_t i = 0; i < frames; ++i) {
        writeU16(out, static_cast<std::uint16_t>(samples[i]));
    }
}

std::int16_t doubleToI16(double value)
{
    return static_cast<std::int16_t>(std::lrint(std::clamp(value, -1.0, 1.0) * 32767.0));
}

class TakeAccumulator {
public:
    TakeAccumulator(const GuiLoopbackOptions& options, int sampleRate)
        : options_(options),
          sampleRate_(std::max(1, sampleRate)),
          triggerThreshold_(ampFromDb(options.triggerThresholdDb)),
          tailThreshold_(ampFromDb(options.tailSilenceDb)),
          triggerHoldFrames_(static_cast<std::uint64_t>(std::max(0, options.triggerHoldMs)) * sampleRate_ / 1000),
          preRollFrames_(static_cast<std::uint64_t>(std::max(0, options.preRollMs)) * sampleRate_ / 1000),
          tailSilenceFrames_(static_cast<std::uint64_t>(std::max(0, options.tailSilenceMs)) * sampleRate_ / 1000)
    {
        preRoll_.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(preRollFrames_, sampleRate_ * 30ULL)));
    }

    void push(std::int16_t sample)
    {
        ++rawFrames_;
        const double amp = std::abs(static_cast<double>(sample)) / 32768.0;
        peak_ = std::max(peak_, amp);
        if (options_.trigger && !triggerFired_) {
            if (preRollFrames_ > 0) {
                if (preRoll_.size() >= preRollFrames_) {
                    preRoll_.erase(preRoll_.begin());
                }
                preRoll_.push_back(sample);
            }
            if (amp >= triggerThreshold_) {
                ++triggerHoldCount_;
            } else {
                triggerHoldCount_ = 0;
            }
            if (triggerHoldCount_ >= std::max<std::uint64_t>(triggerHoldFrames_, 1)) {
                triggerFired_ = true;
                samples_.insert(samples_.end(), preRoll_.begin(), preRoll_.end());
                preRoll_.clear();
            } else {
                return;
            }
        } else {
            triggerFired_ = true;
        }
        samples_.push_back(sample);
    }

    std::vector<std::int16_t> finish()
    {
        if (!triggerFired_) {
            samples_.clear();
            return samples_;
        }
        std::size_t first = 0;
        std::size_t last = samples_.size();
        if (options_.trimLeadingSilence) {
            while (first < last && std::abs(static_cast<double>(samples_[first])) / 32768.0 < triggerThreshold_) {
                ++first;
            }
        }
        if (options_.trimTrailingSilence) {
            std::uint64_t quiet = 0;
            while (last > first) {
                const double amp = std::abs(static_cast<double>(samples_[last - 1])) / 32768.0;
                if (amp < tailThreshold_) {
                    ++quiet;
                    --last;
                    continue;
                }
                break;
            }
            if (quiet < tailSilenceFrames_) {
                last = samples_.size();
            }
        }
        if (first > 0 || last < samples_.size()) {
            samples_ = std::vector<std::int16_t>(samples_.begin() + static_cast<std::ptrdiff_t>(first),
                samples_.begin() + static_cast<std::ptrdiff_t>(last));
        }
        return samples_;
    }

    double peakDbfs() const noexcept
    {
        return peak_ > 0.0
            ? 20.0 * std::log10(peak_)
            : -std::numeric_limits<double>::infinity();
    }

private:
    const GuiLoopbackOptions& options_;
    int sampleRate_ = 48000;
    double triggerThreshold_ = 0.0;
    double tailThreshold_ = 0.0;
    std::uint64_t triggerHoldFrames_ = 0;
    std::uint64_t preRollFrames_ = 0;
    std::uint64_t tailSilenceFrames_ = 0;
    std::uint64_t triggerHoldCount_ = 0;
    std::uint64_t rawFrames_ = 0;
    double peak_ = 0.0;
    bool triggerFired_ = false;
    std::vector<std::int16_t> preRoll_;
    std::vector<std::int16_t> samples_;
};

#if defined(_WIN32)
using Microsoft::WRL::ComPtr;

struct ComApartment {
    ComApartment()
    {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        initialized = SUCCEEDED(hr);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
            throw std::runtime_error("failed to initialize COM");
        }
    }
    ~ComApartment()
    {
        if (initialized) {
            CoUninitialize();
        }
    }
    bool initialized = false;
};

void checkHr(HRESULT hr, const char* message)
{
    if (FAILED(hr)) {
        throw std::runtime_error(message);
    }
}

QString endpointName(IMMDevice* device)
{
    ComPtr<IPropertyStore> store;
    checkHr(device->OpenPropertyStore(STGM_READ, &store), "failed to open endpoint properties");
    PROPVARIANT value;
    PropVariantInit(&value);
    QString name;
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
        name = QString::fromWCharArray(value.pwszVal);
    }
    PropVariantClear(&value);
    return name;
}

ComPtr<IMMDeviceEnumerator> makeEnumerator()
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    checkHr(CoCreateInstance(
                __uuidof(MMDeviceEnumerator),
                nullptr,
                CLSCTX_ALL,
                __uuidof(IMMDeviceEnumerator),
                reinterpret_cast<void**>(enumerator.GetAddressOf())),
        "failed to create WASAPI device enumerator");
    return enumerator;
}

ComPtr<IMMDevice> loopbackDeviceForSource(IMMDeviceEnumerator* enumerator, const QString& source)
{
    if (source.isEmpty() || source == QStringLiteral("default")) {
        ComPtr<IMMDevice> device;
        checkHr(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device), "failed to get default render endpoint");
        return device;
    }
    bool ok = false;
    const int index = source.toInt(&ok);
    if (!ok || index < 0) {
        throw std::runtime_error("loopback source must be default or a numeric source id");
    }
    ComPtr<IMMDeviceCollection> collection;
    checkHr(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection), "failed to enumerate render endpoints");
    UINT count = 0;
    checkHr(collection->GetCount(&count), "failed to count render endpoints");
    if (static_cast<UINT>(index) >= count) {
        throw std::runtime_error("loopback source id out of range");
    }
    ComPtr<IMMDevice> device;
    checkHr(collection->Item(static_cast<UINT>(index), &device), "failed to get render endpoint");
    return device;
}

double readSample(const BYTE* frame, WORD bitsPerSample, WORD formatTag, const GUID* subFormat, WORD channel)
{
    const BYTE* sample = frame + (static_cast<std::size_t>(channel) * bitsPerSample / 8);
    const bool isFloat =
        formatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (formatTag == WAVE_FORMAT_EXTENSIBLE && subFormat != nullptr && IsEqualGUID(*subFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT));
    const bool isPcm =
        formatTag == WAVE_FORMAT_PCM ||
        (formatTag == WAVE_FORMAT_EXTENSIBLE && subFormat != nullptr && IsEqualGUID(*subFormat, KSDATAFORMAT_SUBTYPE_PCM));
    if (isFloat && bitsPerSample == 32) {
        float value = 0.0F;
        std::memcpy(&value, sample, sizeof(value));
        return std::clamp(static_cast<double>(value), -1.0, 1.0);
    }
    if (isPcm && bitsPerSample == 16) {
        std::int16_t value = 0;
        std::memcpy(&value, sample, sizeof(value));
        return static_cast<double>(value) / 32768.0;
    }
    if (isPcm && bitsPerSample == 24) {
        std::int32_t value =
            static_cast<std::int32_t>(sample[0]) |
            (static_cast<std::int32_t>(sample[1]) << 8) |
            (static_cast<std::int32_t>(sample[2]) << 16);
        if ((value & 0x00800000) != 0) {
            value |= static_cast<std::int32_t>(0xff000000);
        }
        return static_cast<double>(value) / 8388608.0;
    }
    if (isPcm && bitsPerSample == 32) {
        std::int32_t value = 0;
        std::memcpy(&value, sample, sizeof(value));
        return static_cast<double>(value) / 2147483648.0;
    }
    return 0.0;
}
#endif

} // namespace

std::vector<std::int16_t> jam2::gui::resample_pcm16_mono(
    std::span<const std::int16_t> input,
    int sourceSampleRate,
    int targetSampleRate)
{
    if (sourceSampleRate <= 0 || targetSampleRate <= 0) {
        throw std::invalid_argument("sample rates must be positive");
    }
    if (input.empty()) {
        return {};
    }
    if (sourceSampleRate == targetSampleRate) {
        return {input.begin(), input.end()};
    }

    const long double exactOutputFrames =
        static_cast<long double>(input.size()) *
        static_cast<long double>(targetSampleRate) /
        static_cast<long double>(sourceSampleRate);
    const long double maximumOutputFrames = std::min(
        static_cast<long double>((std::numeric_limits<std::size_t>::max)()),
        static_cast<long double>((std::numeric_limits<long long>::max)()));
    if (!std::isfinite(exactOutputFrames) ||
        exactOutputFrames > maximumOutputFrames) {
        throw std::length_error("resampled loopback recording is too large");
    }
    const std::size_t outputFrames = std::max<std::size_t>(
        1,
        static_cast<std::size_t>(std::llround(exactOutputFrames)));
    std::vector<std::int16_t> output(outputFrames);

    constexpr int kHalfTaps = 16;
    constexpr int kTapCount = kHalfTaps * 2;
    constexpr int kPhaseCount = 1024;
    constexpr double kPi = 3.1415926535897932384626433832795;
    const double sourcePerTarget =
        static_cast<double>(sourceSampleRate) /
        static_cast<double>(targetSampleRate);
    const double cutoff = 0.94 * std::min(
        1.0,
        static_cast<double>(targetSampleRate) /
            static_cast<double>(sourceSampleRate));
    std::vector<double> kernels(
        static_cast<std::size_t>(kPhaseCount) * kTapCount);
    for (int phase = 0; phase < kPhaseCount; ++phase) {
        const double fractionalPosition =
            static_cast<double>(phase) /
            static_cast<double>(kPhaseCount);
        for (int tapIndex = 0; tapIndex < kTapCount; ++tapIndex) {
            const int tap = tapIndex - kHalfTaps + 1;
            const double distance =
                fractionalPosition - static_cast<double>(tap);
            const double normalizedDistance =
                std::abs(distance) / static_cast<double>(kHalfTaps);
            double weight = 0.0;
            if (normalizedDistance < 1.0) {
                const double sincArgument = cutoff * distance;
                const double sinc = std::abs(sincArgument) < 1.0e-12
                    ? 1.0
                    : std::sin(kPi * sincArgument) /
                        (kPi * sincArgument);
                const double window =
                    0.5 * (1.0 + std::cos(kPi * normalizedDistance));
                weight = cutoff * sinc * window;
            }
            kernels[
                static_cast<std::size_t>(phase) * kTapCount +
                static_cast<std::size_t>(tapIndex)] = weight;
        }
    }

    for (std::size_t outputIndex = 0;
         outputIndex < outputFrames;
         ++outputIndex) {
        const double sourcePosition =
            static_cast<double>(outputIndex) * sourcePerTarget;
        auto centre = static_cast<std::int64_t>(
            std::floor(sourcePosition));
        const double fractionalPosition =
            sourcePosition - static_cast<double>(centre);
        int phase = static_cast<int>(std::llround(
            fractionalPosition * static_cast<double>(kPhaseCount)));
        if (phase >= kPhaseCount) {
            phase = 0;
            ++centre;
        }
        double weightedSample = 0.0;
        double weightSum = 0.0;
        for (int tapIndex = 0; tapIndex < kTapCount; ++tapIndex) {
            const int tap = tapIndex - kHalfTaps + 1;
            const std::int64_t sourceIndex = centre + tap;
            if (sourceIndex < 0 ||
                sourceIndex >= static_cast<std::int64_t>(input.size())) {
                continue;
            }
            const double weight = kernels[
                static_cast<std::size_t>(phase) * kTapCount +
                static_cast<std::size_t>(tapIndex)];
            weightedSample +=
                static_cast<double>(input[static_cast<std::size_t>(sourceIndex)]) *
                weight;
            weightSum += weight;
        }
        const double sample = std::abs(weightSum) > 1.0e-12
            ? weightedSample / weightSum
            : 0.0;
        output[outputIndex] = static_cast<std::int16_t>(std::lrint(
            std::clamp(sample, -32768.0, 32767.0)));
    }
    return output;
}

GuiLoopbackRecorder::GuiLoopbackRecorder() = default;

GuiLoopbackRecorder::~GuiLoopbackRecorder()
{
    stop();
    if (thread_.joinable()) {
        thread_.join();
    }
}

bool GuiLoopbackRecorder::isRunning() const
{
    return running_.load(std::memory_order_acquire);
}

bool GuiLoopbackRecorder::start(const GuiLoopbackOptions& options, FinishedCallback finished, QString* error)
{
    if (isRunning()) {
        if (error) *error = QStringLiteral("loopback recorder is already running");
        return false;
    }
    if (options.outputPath.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("loopback output path is required");
        return false;
    }
    if (options.targetSampleRate <= 0) {
        if (error) *error = QStringLiteral("loopback target sample rate must be positive");
        return false;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    stopRequested_.store(false, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this, options, finished = std::move(finished)] { run(options, finished); });
    return true;
}

void GuiLoopbackRecorder::stop()
{
    stopRequested_.store(true, std::memory_order_release);
}

QStringList GuiLoopbackRecorder::listSources(QString* error)
{
#if defined(_WIN32)
    try {
        ComApartment apartment;
        auto enumerator = makeEnumerator();
        auto defaultDevice = loopbackDeviceForSource(enumerator.Get(), QStringLiteral("default"));
        QStringList out;
        out << QStringLiteral("[default] WASAPI %1").arg(endpointName(defaultDevice.Get()));
        ComPtr<IMMDeviceCollection> collection;
        checkHr(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection), "failed to enumerate render endpoints");
        UINT count = 0;
        checkHr(collection->GetCount(&count), "failed to count render endpoints");
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> device;
            checkHr(collection->Item(i, &device), "failed to get render endpoint");
            out << QStringLiteral("[%1] WASAPI %2").arg(i).arg(endpointName(device.Get()));
        }
        return out;
    } catch (const std::exception& ex) {
        if (error) *error = QString::fromUtf8(ex.what());
        return {};
    }
#else
    if (error) *error = QStringLiteral("internal loopback recording is currently implemented on Windows only");
    return {};
#endif
}

void GuiLoopbackRecorder::run(GuiLoopbackOptions options, FinishedCallback finished) noexcept
{
    bool ok = false;
    QString error;
    QString diagnostics;
    try {
#if defined(_WIN32)
        ComApartment apartment;
        auto enumerator = makeEnumerator();
        auto device = loopbackDeviceForSource(enumerator.Get(), options.source);

        ComPtr<IAudioClient> audioClient;
        checkHr(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(audioClient.GetAddressOf())),
            "failed to activate WASAPI audio client");

        WAVEFORMATEX* rawFormat = nullptr;
        checkHr(audioClient->GetMixFormat(&rawFormat), "failed to get WASAPI mix format");
        std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)> mixFormat(rawFormat, CoTaskMemFree);
        const auto* extensible = mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE
            ? reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mixFormat.get())
            : nullptr;
        const GUID* subFormat = extensible != nullptr ? &extensible->SubFormat : nullptr;
        const int sampleRate = static_cast<int>(mixFormat->nSamplesPerSec);
        const WORD channels = mixFormat->nChannels;
        const WORD bitsPerSample = mixFormat->wBitsPerSample;
        const WORD validBitsPerSample = extensible != nullptr
            ? extensible->Samples.wValidBitsPerSample
            : bitsPerSample;
        const DWORD channelMask = extensible != nullptr
            ? extensible->dwChannelMask
            : 0;
        const QString selectedEndpoint = endpointName(device.Get());
        const std::uint64_t targetFrames = options.durationMs > 0
            ? static_cast<std::uint64_t>((static_cast<std::int64_t>(sampleRate) * options.durationMs) / 1000)
            : std::numeric_limits<std::uint64_t>::max();

        checkHr(audioClient->Initialize(
                    AUDCLNT_SHAREMODE_SHARED,
                    AUDCLNT_STREAMFLAGS_LOOPBACK,
                    10000000,
                    0,
                    mixFormat.get(),
                    nullptr),
            "failed to initialize WASAPI loopback capture");

        ComPtr<IAudioCaptureClient> captureClient;
        checkHr(audioClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(captureClient.GetAddressOf())),
            "failed to get WASAPI capture client");

        checkHr(audioClient->Start(), "failed to start WASAPI loopback capture");
        std::uint64_t written = 0;
        std::uint64_t signalPackets = 0;
        std::size_t minimumActiveChannels = channels;
        std::size_t maximumActiveChannels = 0;
        std::vector<double> channelPeaks(channels, 0.0);
        TakeAccumulator capture(options, sampleRate);
        while (written < targetFrames && !stopRequested_.load(std::memory_order_acquire)) {
            UINT32 packetFrames = 0;
            checkHr(captureClient->GetNextPacketSize(&packetFrames), "failed to read WASAPI packet size");
            if (packetFrames == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            BYTE* data = nullptr;
            UINT32 framesAvailable = 0;
            DWORD flags = 0;
            checkHr(captureClient->GetBuffer(&data, &framesAvailable, &flags, nullptr, nullptr), "failed to get WASAPI capture buffer");
            const UINT32 framesToWrite = static_cast<UINT32>(std::min<std::uint64_t>(framesAvailable, targetFrames - written));
            std::fill(channelPeaks.begin(), channelPeaks.end(), 0.0);
            double loudestPeak = 0.0;
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0) {
                for (UINT32 frame = 0; frame < framesToWrite; ++frame) {
                    const BYTE* frameData =
                        data + static_cast<std::size_t>(frame) * mixFormat->nBlockAlign;
                    for (WORD channel = 0; channel < channels; ++channel) {
                        const double peak = std::abs(readSample(
                            frameData,
                            bitsPerSample,
                            mixFormat->wFormatTag,
                            subFormat,
                            channel));
                        channelPeaks[channel] = std::max(channelPeaks[channel], peak);
                        loudestPeak = std::max(loudestPeak, peak);
                    }
                }
            }
            const std::size_t activeChannels =
                jam2::audio::mono_downmix_active_channels(
                    std::span<const double>(
                        channelPeaks.data(),
                        channelPeaks.size()));
            if (loudestPeak > 0.0) {
                ++signalPackets;
                minimumActiveChannels =
                    std::min(minimumActiveChannels, activeChannels);
                maximumActiveChannels =
                    std::max(maximumActiveChannels, activeChannels);
            }
            for (UINT32 frame = 0; frame < framesToWrite; ++frame) {
                double mono = 0.0;
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0 &&
                    activeChannels > 0) {
                    const BYTE* frameData = data + static_cast<std::size_t>(frame) * mixFormat->nBlockAlign;
                    for (WORD channel = 0; channel < channels; ++channel) {
                        if (!jam2::audio::mono_downmix_channel_active(
                                channelPeaks[channel],
                                loudestPeak)) {
                            continue;
                        }
                        mono += readSample(frameData, bitsPerSample, mixFormat->wFormatTag, subFormat, channel);
                    }
                    mono /= static_cast<double>(activeChannels);
                }
                capture.push(doubleToI16(mono));
            }
            written += framesToWrite;
            checkHr(captureClient->ReleaseBuffer(framesAvailable), "failed to release WASAPI capture buffer");
        }
        checkHr(audioClient->Stop(), "failed to stop WASAPI loopback capture");
        std::vector<std::int16_t> sourceSamples = capture.finish();
        std::vector<std::int16_t> outputSamples =
            jam2::gui::resample_pcm16_mono(
                sourceSamples,
                sampleRate,
                options.targetSampleRate);
        diagnostics = QStringLiteral(
            "loopback capture endpoint=\"%1\" channels=%2 channel_mask=0x%3 "
            "source_sample_rate=%4 target_sample_rate=%5 resampled=%6 "
            "resample_ratio=%7 source_frames=%8 output_frames=%9 "
            "bits=%10 valid_bits=%11 fold_down=active-average-30dB "
            "signal_packets=%12 active_channels_min=%13 active_channels_max=%14 "
            "recorded_peak_dbfs=%15")
            .arg(selectedEndpoint)
            .arg(channels)
            .arg(QString::number(channelMask, 16))
            .arg(sampleRate)
            .arg(options.targetSampleRate)
            .arg(sampleRate == options.targetSampleRate
                ? QStringLiteral("no")
                : QStringLiteral("yes"))
            .arg(
                static_cast<double>(options.targetSampleRate) /
                    static_cast<double>(sampleRate),
                0,
                'f',
                8)
            .arg(static_cast<qulonglong>(sourceSamples.size()))
            .arg(static_cast<qulonglong>(outputSamples.size()))
            .arg(bitsPerSample)
            .arg(validBitsPerSample)
            .arg(signalPackets)
            .arg(signalPackets > 0 ? minimumActiveChannels : 0)
            .arg(maximumActiveChannels)
            .arg(capture.peakDbfs(), 0, 'f', 2);
        writeWav(
            options.outputPath,
            options.targetSampleRate,
            outputSamples);
        ok = true;
#else
        throw std::runtime_error("internal loopback recording is currently implemented on Windows only");
#endif
    } catch (const std::exception& ex) {
        error = QString::fromUtf8(ex.what());
    }
    running_.store(false, std::memory_order_release);
    if (finished) {
        finished(ok, options.outputPath, error, diagnostics);
    }
}

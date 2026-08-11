#include "Wav.hpp"
#include "FileSystem.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <signalsmith-stretch.h>

namespace jamtaster::native {
namespace {

struct ChannelBufferView {
    std::vector<std::vector<float>>& channels;
    std::size_t offset = 0;

    float* operator[](int channel)
    {
        return channels.at(static_cast<std::size_t>(channel)).data() + offset;
    }
};

std::uint16_t readU16(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    if (offset + 2 > bytes.size()) throw std::runtime_error("truncated WAV field");
    return static_cast<std::uint16_t>(bytes[offset]) |
        (static_cast<std::uint16_t>(bytes[offset + 1]) << 8U);
}

std::uint32_t readU32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    if (offset + 4 > bytes.size()) throw std::runtime_error("truncated WAV field");
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3]) << 24U);
}

bool marker(const std::vector<std::uint8_t>& bytes, std::size_t offset, const char* value)
{
    return offset + 4 <= bytes.size() &&
        std::memcmp(bytes.data() + offset, value, 4) == 0;
}

void writeU16(std::ofstream& output, std::uint16_t value)
{
    const std::array<char, 2> bytes{
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
    };
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void writeU32(std::ofstream& output, std::uint32_t value)
{
    const std::array<char, 4> bytes{
        static_cast<char>(value & 0xffU),
        static_cast<char>((value >> 8U) & 0xffU),
        static_cast<char>((value >> 16U) & 0xffU),
        static_cast<char>((value >> 24U) & 0xffU),
    };
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

} // namespace

std::size_t AudioBuffer::frames() const noexcept
{
    return channels > 0 ? samples.size() / static_cast<std::size_t>(channels) : 0;
}

std::vector<float> AudioBuffer::mono() const
{
    if (channels <= 0 || samples.size() % static_cast<std::size_t>(channels) != 0) {
        throw std::runtime_error("invalid interleaved audio buffer");
    }
    std::vector<float> result(frames(), 0.0F);
    for (std::size_t frame = 0; frame < frames(); ++frame) {
        double sum = 0.0;
        for (int channel = 0; channel < channels; ++channel) {
            sum += samples[frame * static_cast<std::size_t>(channels) +
                static_cast<std::size_t>(channel)];
        }
        result[frame] = static_cast<float>(sum / static_cast<double>(channels));
    }
    return result;
}

AudioBuffer readWav(const std::filesystem::path& path)
{
    std::ifstream input(filesystemIoPath(path), std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("could not open WAV: " + path.string());
    const auto end = input.tellg();
    const auto byteCount = end - std::streampos(0);
    if (byteCount < 44) throw std::runtime_error("WAV is too short");
    if (static_cast<std::uintmax_t>(byteCount) >
        static_cast<std::uintmax_t>((std::numeric_limits<std::size_t>::max)())) {
        throw std::runtime_error("WAV is too large for this process");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(byteCount));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input || !marker(bytes, 0, "RIFF") || !marker(bytes, 8, "WAVE")) {
        throw std::runtime_error("input is not a little-endian RIFF/WAVE file");
    }

    std::size_t formatOffset = 0;
    std::size_t formatSize = 0;
    std::size_t dataOffset = 0;
    std::size_t dataSize = 0;
    for (std::size_t cursor = 12; cursor + 8 <= bytes.size();) {
        const std::size_t chunkSize = readU32(bytes, cursor + 4);
        const std::size_t payload = cursor + 8;
        if (payload > bytes.size() || chunkSize > bytes.size() - payload) {
            throw std::runtime_error("WAV contains a truncated chunk");
        }
        if (marker(bytes, cursor, "fmt ")) {
            formatOffset = payload;
            formatSize = chunkSize;
        } else if (marker(bytes, cursor, "data") && dataOffset == 0) {
            dataOffset = payload;
            dataSize = chunkSize;
        }
        const std::size_t padded = chunkSize + (chunkSize & 1U);
        if (padded > bytes.size() - payload) break;
        cursor = payload + padded;
    }
    if (formatOffset == 0 || formatSize < 16 || dataOffset == 0) {
        throw std::runtime_error("WAV is missing fmt or data");
    }

    std::uint16_t format = readU16(bytes, formatOffset);
    const int channels = readU16(bytes, formatOffset + 2);
    const auto sampleRateValue = readU32(bytes, formatOffset + 4);
    const int bits = readU16(bytes, formatOffset + 14);
    if (format == 0xfffeU && formatSize >= 40) {
        format = readU16(bytes, formatOffset + 24);
    }
    if (channels <= 0 || channels > 32 || sampleRateValue < 8000 || sampleRateValue > 384000) {
        throw std::runtime_error("unsupported WAV channel count or sample rate");
    }
    if (!((format == 1U && (bits == 16 || bits == 24 || bits == 32)) ||
          (format == 3U && bits == 32))) {
        throw std::runtime_error("WAV must contain PCM16/24/32 or float32 samples");
    }
    const std::size_t bytesPerSample = static_cast<std::size_t>(bits / 8);
    const std::size_t frameBytes = bytesPerSample * static_cast<std::size_t>(channels);
    if (frameBytes == 0 || dataSize % frameBytes != 0) {
        throw std::runtime_error("WAV data is not frame aligned");
    }
    const std::size_t sampleCount = dataSize / bytesPerSample;
    AudioBuffer result;
    result.sampleRate = static_cast<int>(sampleRateValue);
    result.channels = channels;
    result.samples.resize(sampleCount);
    for (std::size_t index = 0; index < sampleCount; ++index) {
        const std::size_t offset = dataOffset + index * bytesPerSample;
        float value = 0.0F;
        if (format == 3U) {
            std::uint32_t raw = readU32(bytes, offset);
            std::memcpy(&value, &raw, sizeof(value));
        } else if (bits == 16) {
            const auto raw = static_cast<std::int16_t>(readU16(bytes, offset));
            value = static_cast<float>(raw) / 32768.0F;
        } else if (bits == 24) {
            std::int32_t raw = static_cast<std::int32_t>(bytes[offset]) |
                (static_cast<std::int32_t>(bytes[offset + 1]) << 8) |
                (static_cast<std::int32_t>(bytes[offset + 2]) << 16);
            if ((raw & 0x00800000) != 0) raw |= ~0x00ffffff;
            value = static_cast<float>(raw) / 8388608.0F;
        } else {
            const auto raw = static_cast<std::int32_t>(readU32(bytes, offset));
            value = static_cast<float>(static_cast<double>(raw) / 2147483648.0);
        }
        result.samples[index] = std::isfinite(value) ? std::clamp(value, -1.0F, 1.0F) : 0.0F;
    }
    return result;
}

void writeWavPcm16(const std::filesystem::path& path, const AudioBuffer& audio)
{
    if (audio.sampleRate <= 0 || audio.channels <= 0 ||
        audio.samples.size() % static_cast<std::size_t>(audio.channels) != 0) {
        throw std::runtime_error("invalid audio passed to WAV writer");
    }
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
    const std::uint64_t dataBytes64 = audio.samples.size() * 2ULL;
    if (dataBytes64 > (std::numeric_limits<std::uint32_t>::max)() - 36ULL) {
        throw std::runtime_error("WAV output exceeds RIFF size limit");
    }
    const auto dataBytes = static_cast<std::uint32_t>(dataBytes64);
    std::ofstream output(filesystemIoPath(path), std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("could not create WAV: " + path.string());
    output.write("RIFF", 4);
    writeU32(output, 36U + dataBytes);
    output.write("WAVEfmt ", 8);
    writeU32(output, 16);
    writeU16(output, 1);
    writeU16(output, static_cast<std::uint16_t>(audio.channels));
    writeU32(output, static_cast<std::uint32_t>(audio.sampleRate));
    writeU32(output, static_cast<std::uint32_t>(audio.sampleRate * audio.channels * 2));
    writeU16(output, static_cast<std::uint16_t>(audio.channels * 2));
    writeU16(output, 16);
    output.write("data", 4);
    writeU32(output, dataBytes);
    for (float sample : audio.samples) {
        const auto value = static_cast<std::int16_t>(std::lrint(
            std::clamp(sample, -1.0F, 1.0F) * 32767.0F));
        writeU16(output, static_cast<std::uint16_t>(value));
    }
    if (!output) throw std::runtime_error("failed while writing WAV: " + path.string());
}

AudioBuffer mixMono(const AudioBuffer& first, const AudioBuffer& second)
{
    if (first.sampleRate != second.sampleRate) {
        throw std::runtime_error("cannot mix audio with different sample rates");
    }
    AudioBuffer result;
    result.sampleRate = first.sampleRate;
    result.channels = 1;
    const auto a = first.mono();
    const auto b = second.mono();
    result.samples.resize(std::max(a.size(), b.size()), 0.0F);
    for (std::size_t index = 0; index < result.samples.size(); ++index) {
        const float left = index < a.size() ? a[index] : 0.0F;
        const float right = index < b.size() ? b[index] : 0.0F;
        // Match the PCM16 saturation performed by the Python source wrapper.
        result.samples[index] = std::clamp(left + right, -1.0F, 1.0F);
    }
    return result;
}

AudioBuffer cropAudio(const AudioBuffer& audio, double startSeconds, double endSeconds)
{
    if (endSeconds <= startSeconds || audio.sampleRate <= 0) {
        throw std::runtime_error("invalid audio crop interval");
    }
    const auto first = std::min(audio.frames(), static_cast<std::size_t>(std::max(
        0.0, std::round(startSeconds * audio.sampleRate))));
    const auto last = std::min(audio.frames(), static_cast<std::size_t>(std::max(
        0.0, std::round(endSeconds * audio.sampleRate))));
    if (last <= first) throw std::runtime_error("audio crop is empty");
    AudioBuffer result;
    result.sampleRate = audio.sampleRate;
    result.channels = audio.channels;
    const auto begin = audio.samples.begin() + static_cast<std::ptrdiff_t>(first * audio.channels);
    const auto end = audio.samples.begin() + static_cast<std::ptrdiff_t>(last * audio.channels);
    result.samples.assign(begin, end);
    return result;
}

AudioBuffer stretchAudio(const AudioBuffer& audio, std::size_t outputFrames)
{
    if (audio.channels <= 0 || audio.frames() == 0 || outputFrames == 0) {
        throw std::runtime_error("invalid audio passed to time stretcher");
    }
    if (audio.frames() == outputFrames) return audio;
    signalsmith::stretch::SignalsmithStretch<float> stretch;
    // Match python-stretch's default preset and latency-compensation sequence.
    stretch.presetDefault(audio.channels, static_cast<float>(audio.sampleRate));
    const int inputLatency = stretch.inputLatency();
    const int outputLatency = stretch.outputLatency();
    if (inputLatency < 0 || outputLatency < 0) {
        throw std::runtime_error("Signalsmith reported invalid latency");
    }
    const auto inputLatencyFrames = static_cast<std::size_t>(inputLatency);
    const auto outputLatencyFrames = static_cast<std::size_t>(outputLatency);
    std::vector<std::vector<float>> input(static_cast<std::size_t>(audio.channels));
    std::vector<std::vector<float>> output(static_cast<std::size_t>(audio.channels));
    for (int channel = 0; channel < audio.channels; ++channel) {
        input[static_cast<std::size_t>(channel)].assign(
            audio.frames() + inputLatencyFrames, 0.0F);
        output[static_cast<std::size_t>(channel)].assign(
            outputFrames + outputLatencyFrames, 0.0F);
        for (std::size_t frame = 0; frame < audio.frames(); ++frame)
            input[static_cast<std::size_t>(channel)][frame] =
                audio.samples[frame * static_cast<std::size_t>(audio.channels) +
                    static_cast<std::size_t>(channel)];
    }
    if (audio.frames() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
        outputFrames > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        throw std::runtime_error("audio is too long for Signalsmith Stretch");
    }
    const double playbackRate = audio.frames() / static_cast<double>(outputFrames);
    ChannelBufferView inputStart{input, 0};
    stretch.seek(inputStart, inputLatency, playbackRate);
    ChannelBufferView inputAfterLatency{input, inputLatencyFrames};
    ChannelBufferView outputStart{output, 0};
    stretch.process(inputAfterLatency, static_cast<int>(audio.frames()),
        outputStart, static_cast<int>(outputFrames));
    ChannelBufferView outputTail{output, outputFrames};
    stretch.flush(outputTail, outputLatency);
    AudioBuffer result;
    result.sampleRate = audio.sampleRate;
    result.channels = audio.channels;
    result.samples.resize(outputFrames * static_cast<std::size_t>(audio.channels));
    for (std::size_t frame = 0; frame < outputFrames; ++frame)
        for (int channel = 0; channel < audio.channels; ++channel)
            result.samples[frame * static_cast<std::size_t>(audio.channels) +
                static_cast<std::size_t>(channel)] = output[static_cast<std::size_t>(channel)][
                    outputLatencyFrames + frame];
    return result;
}

AudioBuffer stretchAudioAnchored(
    const AudioBuffer& audio,
    const std::vector<std::size_t>& inputBoundaries,
    const std::vector<std::size_t>& outputBoundaries)
{
    if (audio.channels <= 0 || audio.sampleRate <= 0 || audio.frames() == 0 ||
        inputBoundaries.size() < 2 || inputBoundaries.size() != outputBoundaries.size() ||
        inputBoundaries.front() != 0 || inputBoundaries.back() != audio.frames() ||
        outputBoundaries.front() != 0 || outputBoundaries.back() == 0) {
        throw std::runtime_error("invalid anchored stretch boundaries");
    }
    for (std::size_t index = 1; index < inputBoundaries.size(); ++index) {
        if (inputBoundaries[index] <= inputBoundaries[index - 1] ||
            outputBoundaries[index] <= outputBoundaries[index - 1]) {
            throw std::runtime_error("anchored stretch boundaries must increase");
        }
    }

    AudioBuffer result;
    result.sampleRate = audio.sampleRate;
    result.channels = audio.channels;
    result.samples.assign(outputBoundaries.back() *
        static_cast<std::size_t>(audio.channels), 0.0F);

    // Match the proven Python wrapper: each bar is an independent complete
    // latency-compensated stretch, then the exact-length results are concatenated.
    for (std::size_t segment = 1; segment < inputBoundaries.size(); ++segment) {
        const std::size_t inputFirst = inputBoundaries[segment - 1];
        const std::size_t inputLast = inputBoundaries[segment];
        const std::size_t outputFirst = outputBoundaries[segment - 1];
        const std::size_t outputFrames = outputBoundaries[segment] - outputFirst;
        AudioBuffer input;
        input.sampleRate = audio.sampleRate;
        input.channels = audio.channels;
        const auto sampleFirst = inputFirst * static_cast<std::size_t>(audio.channels);
        const auto sampleLast = inputLast * static_cast<std::size_t>(audio.channels);
        input.samples.assign(audio.samples.begin() + static_cast<std::ptrdiff_t>(sampleFirst),
            audio.samples.begin() + static_cast<std::ptrdiff_t>(sampleLast));
        auto rendered = stretchAudio(input, outputFrames);
        for (std::size_t frame = 0; frame < outputFrames; ++frame) {
            for (int channel = 0; channel < audio.channels; ++channel) {
                result.samples[(outputFirst + frame) * static_cast<std::size_t>(audio.channels) +
                    static_cast<std::size_t>(channel)] = rendered.samples[
                    frame * static_cast<std::size_t>(audio.channels) +
                    static_cast<std::size_t>(channel)];
            }
        }
    }
    return result;
}

} // namespace jamtaster::native

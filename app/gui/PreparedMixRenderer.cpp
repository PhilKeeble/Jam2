#include "PreparedMixRenderer.hpp"
#include "runtime_limits.hpp"

#include "pcm16_wav.hpp"
#include "signalsmith-stretch/signalsmith-stretch.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QSaveFile>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <vector>

namespace {
void put16(QByteArray& b, quint16 v) { b.append(char(v)); b.append(char(v >> 8)); }
void put32(QByteArray& b, quint32 v) { b.append(char(v)); b.append(char(v >> 8)); b.append(char(v >> 16)); b.append(char(v >> 24)); }
QString absoluteAsset(const QString& folder, const QString& path) { return QFileInfo(path).isAbsolute() ? path : QDir(folder).absoluteFilePath(path); }

std::filesystem::path nativeFilePath(const QString& path)
{
#if defined(_WIN32)
    return std::filesystem::path(path.toStdWString());
#else
    const QByteArray utf8 = path.toUtf8();
    return std::filesystem::path(utf8.constData());
#endif
}

struct FocusState {
    double x1 = 0.0;
    double x2 = 0.0;
    double y1 = 0.0;
    double y2 = 0.0;
};

void applyFocus(std::vector<float>& samples, int sampleRate, const SharedTrackModel& track)
{
    if (!track.focusEnabled || std::abs(track.focusGainDb) < 0.001 || samples.empty() || sampleRate <= 0) {
        return;
    }
    const double frequency = qBound(20.0, track.focusFrequencyHz, static_cast<double>(sampleRate) * 0.45);
    const double q = qBound(0.1, track.focusQ, 20.0);
    const double omega = 2.0 * 3.14159265358979323846 * frequency / static_cast<double>(sampleRate);
    const double sinOmega = std::sin(omega);
    const double cosOmega = std::cos(omega);
    const double alpha = sinOmega / (2.0 * q);
    const double makeup = std::pow(10.0, qBound(-24.0, track.focusGainDb, 24.0) / 20.0);

    const double b0 = alpha;
    const double b1 = 0.0;
    const double b2 = -alpha;
    const double a0 = 1.0 + alpha;
    const double a1 = -2.0 * cosOmega;
    const double a2 = 1.0 - alpha;
    const double focusA0 = makeup * b0 / a0;
    const double focusA1 = makeup * b1 / a0;
    const double focusA2 = makeup * b2 / a0;
    const double focusB1 = a1 / a0;
    const double focusB2 = a2 / a0;

    FocusState state;
    for (float& sample : samples) {
        const double input = sample;
        const double output = focusA0 * input + focusA1 * state.x1 + focusA2 * state.x2 - focusB1 * state.y1 - focusB2 * state.y2;
        state.x2 = state.x1;
        state.x1 = input;
        state.y2 = state.y1;
        state.y1 = output;
        sample = static_cast<float>(qBound(-1.0, output, 1.0));
    }
}

std::vector<float> processTrack(std::vector<float> input, int sampleRate, const SharedTrackModel& track, QString& error)
{
    const double speed = qBound(0.10, track.speed, 2.0);
    const int pitchCents = qBound(-1200, track.pitchCents, 1200);
    const qint64 outputFrames = static_cast<qint64>(std::ceil(static_cast<double>(input.size()) / speed));
    const qint64 maxFrames = static_cast<qint64>(std::max(1, sampleRate)) * 60LL * 5LL;
    if (outputFrames > maxFrames) {
        error = QStringLiteral("prepared mix exceeds five-minute limit after speed processing");
        return {};
    }
    if (outputFrames <= 0) {
        return {};
    }

    std::vector<float> output(static_cast<std::size_t>(outputFrames), 0.0f);
    if (pitchCents == 0) {
        for (qint64 frame = 0; frame < outputFrames; ++frame) {
            const double source = static_cast<double>(frame) * speed;
            const qint64 left = static_cast<qint64>(std::floor(source));
            const qint64 right = left + 1;
            const double frac = source - static_cast<double>(left);
            const double a = left >= 0 && left < static_cast<qint64>(input.size()) ? input[static_cast<std::size_t>(left)] : 0.0;
            const double b = right >= 0 && right < static_cast<qint64>(input.size()) ? input[static_cast<std::size_t>(right)] : a;
            output[static_cast<std::size_t>(frame)] = static_cast<float>(a + (b - a) * frac);
        }
    } else {
        signalsmith::stretch::SignalsmithStretch<float> stretch;
        stretch.presetDefault(1, static_cast<float>(std::max(1, sampleRate)), true);
        stretch.setTransposeSemitones(static_cast<float>(pitchCents) / 100.0f);
        std::vector<std::vector<float>> inputBlock(1);
        std::vector<std::vector<float>> outputBlock(1);
        inputBlock[0] = std::move(input);
        outputBlock[0].assign(static_cast<std::size_t>(outputFrames), 0.0f);
        stretch.process(inputBlock, static_cast<int>(inputBlock[0].size()), outputBlock, static_cast<int>(outputFrames));
        output = std::move(outputBlock[0]);
    }
    applyFocus(output, sampleRate, track);
    return output;
}
}

PreparedMixResult PreparedMixRenderer::render(const LooperProject& project, const QString& projectFolder, int sampleRate, const QString& outputPath, const SharedTrackModel& track)
{
    PreparedMixResult result;
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    if (!jam2::limits::valid_sample_rate(sampleRate)) {
        result.error = QStringLiteral("prepared mix sample rate must be within %1..%2 Hz")
                           .arg(jam2::limits::kMinimumSampleRate)
                           .arg(jam2::limits::kMaximumSampleRate);
        return result;
    }
    const LooperBank& bank = project.banks().at(project.activeBankIndex());
    bool anySolo = false;
    for (const LooperLane& lane : bank.lanes) {
        anySolo = anySolo || (lane.sampleRateCompatible && lane.solo && !lane.muted);
    }

    const qint64 maxFrames = 5LL * 60LL * qMax(1, sampleRate);
    struct Source {
        LooperLane lane;
        QString path;
        jam2::wav::Pcm16Info info;
        qint64 sourceStart = 0;
        qint64 sourceEnd = 0;
        qint64 outputStart = 0;
        qint64 outputEnd = 0;
    };
    std::vector<Source> sources;
    sources.reserve(static_cast<std::size_t>(bank.lanes.size()));
    qint64 length = 0;

    for (const LooperLane& lane : bank.lanes) {
        if (!lane.sampleRateCompatible || lane.muted || (anySolo && !lane.solo) ||
            lane.assetPath.trimmed().isEmpty()) {
            continue;
        }
        const QString path = absoluteAsset(projectFolder, lane.assetPath);
        const jam2::wav::InspectResult inspected = jam2::wav::inspect_pcm16_file(nativeFilePath(path));
        if (!inspected) {
            result.error = QStringLiteral("invalid lane WAV %1: %2")
                .arg(lane.name, QString::fromStdString(inspected.error));
            return result;
        }
        if (inspected.info.sample_rate != static_cast<std::uint32_t>(sampleRate)) {
            result.error = QStringLiteral("WAV must be PCM16 at %1 Hz: %2").arg(sampleRate).arg(lane.name);
            return result;
        }
        if (inspected.info.frames == 0 ||
            inspected.info.frames > static_cast<std::uint64_t>(maxFrames) ||
            inspected.info.frames > static_cast<std::uint64_t>(std::numeric_limits<qint64>::max())) {
            result.error = QStringLiteral("lane WAV exceeds the five-minute frame limit: %1").arg(lane.name);
            return result;
        }
        const qint64 frames = static_cast<qint64>(inspected.info.frames);
        if (lane.startFrame < 0 || lane.startFrame > maxFrames) {
            result.error = QStringLiteral("lane start frame is outside the prepared-mix limit: %1").arg(lane.name);
            return result;
        }
        const qint64 sourceStart = lane.loopStartFrame >= 0
            ? qBound<qint64>(0, lane.loopStartFrame, frames - 1)
            : 0;
        const qint64 sourceEnd = lane.loopEndFrame > sourceStart
            ? qBound<qint64>(sourceStart + 1, lane.loopEndFrame, frames)
            : frames;
        const qint64 visibleFrames = sourceEnd - sourceStart;
        qint64 outputEnd = lane.stopFrame;
        if (outputEnd < 0) {
            if (visibleFrames > maxFrames - lane.startFrame) {
                result.error = QStringLiteral("prepared mix exceeds five-minute limit: %1").arg(lane.name);
                return result;
            }
            outputEnd = lane.startFrame + visibleFrames;
        }
        if (outputEnd < lane.startFrame || outputEnd > maxFrames) {
            result.error = QStringLiteral("lane stop frame is outside the prepared-mix limit: %1").arg(lane.name);
            return result;
        }
        length = qMax(length, outputEnd);
        sources.push_back(Source{
            lane,
            path,
            inspected.info,
            sourceStart,
            sourceEnd,
            lane.startFrame,
            outputEnd,
        });
    }
    length = qMax<qint64>(1, length);
    constexpr std::uint64_t maxWorkingBytes = 512ULL * 1024ULL * 1024ULL;
    constexpr std::uint64_t workingBytesPerOutputFrame =
        sizeof(qint32) + 2ULL * sizeof(float);
    if (static_cast<std::uint64_t>(length) > maxWorkingBytes / workingBytesPerOutputFrame) {
        result.error = QStringLiteral("prepared mix exceeds the bounded worker-memory limit");
        return result;
    }
    std::vector<qint32> mix(static_cast<std::size_t>(length), 0);

    constexpr qint64 decodeBlockFrames = 4096;
    for (const Source& source : sources) {
        QFile file(source.path);
        const qint64 visibleFrames = source.sourceEnd - source.sourceStart;
        const std::uint64_t sourceByteOffset =
            source.info.data_offset + static_cast<std::uint64_t>(source.sourceStart) * source.info.block_align;
        const std::uint64_t residentMixBytes = static_cast<std::uint64_t>(length) * sizeof(qint32);
        const std::uint64_t availableDecodeBytes = maxWorkingBytes - residentMixBytes;
        if (visibleFrames <= 0 ||
            static_cast<std::uint64_t>(visibleFrames) > availableDecodeBytes / sizeof(qint16) ||
            sourceByteOffset > static_cast<std::uint64_t>(std::numeric_limits<qint64>::max()) ||
            !file.open(QIODevice::ReadOnly) ||
            !file.seek(static_cast<qint64>(sourceByteOffset))) {
            result.error = QStringLiteral("cannot open lane WAV data: %1").arg(source.lane.name);
            return result;
        }
        std::vector<qint16> mono(static_cast<std::size_t>(visibleFrames), 0);
        QByteArray bytes;
        bytes.resize(static_cast<qsizetype>(decodeBlockFrames * source.info.block_align));
        qint64 decodedFrames = 0;
        while (decodedFrames < visibleFrames) {
            const qint64 framesThisBlock = qMin(
                decodeBlockFrames,
                visibleFrames - decodedFrames);
            const qint64 bytesThisBlock = framesThisBlock * source.info.block_align;
            if (file.read(bytes.data(), bytesThisBlock) != bytesThisBlock) {
                result.error = QStringLiteral("truncated lane WAV data: %1").arg(source.lane.name);
                return result;
            }
            const auto* raw = reinterpret_cast<const unsigned char*>(bytes.constData());
            for (qint64 frame = 0; frame < framesThisBlock; ++frame) {
                qint32 sum = 0;
                for (std::uint16_t channel = 0; channel < source.info.channels; ++channel) {
                    const qint64 offset =
                        frame * source.info.block_align + static_cast<qint64>(channel) * 2;
                    const std::uint16_t encoded = static_cast<std::uint16_t>(
                        raw[offset] | (static_cast<std::uint16_t>(raw[offset + 1]) << 8));
                    const qint32 decoded = encoded < 0x8000U
                        ? static_cast<qint32>(encoded)
                        : static_cast<qint32>(encoded) - 65536;
                    sum += decoded;
                }
                mono[static_cast<std::size_t>(decodedFrames + frame)] = static_cast<qint16>(
                    sum / static_cast<qint32>(source.info.channels));
            }
            decodedFrames += framesThisBlock;
        }

        const double gain = std::pow(10.0, qBound(-60.0, source.lane.gainDb, 12.0) / 20.0);
        for (qint64 out = source.outputStart; out < source.outputEnd; ++out) {
            qint64 in = source.sourceStart + (out - source.lane.startFrame);
            if (source.lane.loopEnabled && in >= source.sourceEnd) {
                in = source.sourceStart + (in - source.sourceStart) % visibleFrames;
            }
            if (in < source.sourceStart || in >= source.sourceEnd) {
                continue;
            }
            const qint64 monoIndex = in - source.sourceStart;
            const std::int64_t summed =
                static_cast<std::int64_t>(mix[static_cast<std::size_t>(out)]) +
                static_cast<std::int64_t>(std::llround(mono[static_cast<std::size_t>(monoIndex)] * gain));
            mix[static_cast<std::size_t>(out)] = static_cast<qint32>(std::clamp<std::int64_t>(
                summed,
                std::numeric_limits<qint32>::min(),
                std::numeric_limits<qint32>::max()));
        }
    }

    std::vector<float> processed(static_cast<std::size_t>(length), 0.0f);
    for (qint64 i = 0; i < length; ++i) {
        processed[static_cast<std::size_t>(i)] =
            static_cast<float>(qBound<qint32>(-32768, mix[static_cast<std::size_t>(i)], 32767) / 32768.0);
    }
    processed = processTrack(std::move(processed), sampleRate, track, result.error);
    if (!result.error.isEmpty()) {
        return result;
    }
    length = static_cast<qint64>(processed.size());
    const std::uint64_t dataBytes = static_cast<std::uint64_t>(length) * 2ULL;
    if (dataBytes > std::numeric_limits<quint32>::max() - 36ULL) {
        result.error = QStringLiteral("prepared mix exceeds RIFF size limits");
        return result;
    }

    QDir().mkpath(QFileInfo(outputPath).absolutePath());
    QSaveFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly)) {
        result.error = QStringLiteral("cannot open prepared mix output");
        return result;
    }
    QByteArray header;
    header.reserve(44);
    header.append("RIFF", 4);
    put32(header, static_cast<quint32>(36ULL + dataBytes));
    header.append("WAVEfmt ", 8);
    put32(header, 16);
    put16(header, 1);
    put16(header, 1);
    put32(header, static_cast<quint32>(sampleRate));
    put32(header, static_cast<quint32>(sampleRate * 2));
    put16(header, 2);
    put16(header, 16);
    header.append("data", 4);
    put32(header, static_cast<quint32>(dataBytes));

    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(header);
    if (output.write(header) != header.size()) {
        result.error = QStringLiteral("cannot write prepared mix header");
        return result;
    }
    constexpr std::size_t outputBlockFrames = 32768;
    QByteArray outputBytes;
    outputBytes.reserve(static_cast<qsizetype>(outputBlockFrames * 2));
    for (std::size_t offset = 0; offset < processed.size(); offset += outputBlockFrames) {
        outputBytes.clear();
        const std::size_t end = std::min(processed.size(), offset + outputBlockFrames);
        for (std::size_t frame = offset; frame < end; ++frame) {
            put16(outputBytes, static_cast<quint16>(qBound(
                -32768,
                static_cast<int>(std::lrint(processed[frame] * 32767.0f)),
                32767)));
        }
        hash.addData(outputBytes);
        if (output.write(outputBytes) != outputBytes.size()) {
            result.error = QStringLiteral("cannot write prepared mix audio");
            return result;
        }
    }
    if (!output.commit()) {
        result.error = QStringLiteral("cannot atomically commit prepared mix");
        return result;
    }

    result.path = outputPath;
    result.frames = length;
    result.fileBytes = static_cast<qint64>(44ULL + dataBytes);
    result.sampleRate = sampleRate;
    result.durationMs = sampleRate > 0
        ? static_cast<int>(length * 1000LL / sampleRate)
        : 0;
    result.sha256 = QString::fromLatin1(hash.result().toHex());
    result.renderMs = QDateTime::currentMSecsSinceEpoch() - started;
    return result;
}

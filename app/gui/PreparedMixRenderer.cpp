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
#include <QUuid>
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

bool PreparedMixRenderer::hasRenderableSources(const LooperProject& project)
{
    return hasRenderableSources(project, project.activeBankIndex());
}

bool PreparedMixRenderer::hasRenderableSources(const LooperProject& project, int bankIndex)
{
    if (bankIndex < 0 || bankIndex >= project.banks().size()) return false;
    const LooperBank& bank = project.banks().at(bankIndex);
    const bool anySolo = std::any_of(
        bank.lanes.cbegin(), bank.lanes.cend(), [](const LooperLane& lane) {
            return lane.sampleRateCompatible && lane.solo && !lane.muted;
        });
    return std::any_of(
        bank.lanes.cbegin(), bank.lanes.cend(), [anySolo](const LooperLane& lane) {
            return lane.sampleRateCompatible && !lane.muted &&
                (!anySolo || lane.solo) && !lane.assetPath.trimmed().isEmpty();
        });
}

QString PreparedMixRenderer::outputPath(
    const QString& workspaceFolder,
    int bankIndex,
    std::uint64_t generation)
{
    return QDir(workspaceFolder).absoluteFilePath(
        QStringLiteral("prepared/active-bank-%1-generation-%2.wav")
            .arg(qMax(0, bankIndex))
            .arg(generation));
}

PreparedMixResult PreparedMixRenderer::render(
    const LooperProject& project,
    const QString& projectFolder,
    int sampleRate,
    const QString& outputPath,
    const SharedTrackModel& track,
    int bankIndex,
    qint64 exactOutputFrames)
{
    PreparedMixResult result;
    result.bankIndex = bankIndex >= 0 ? bankIndex : project.activeBankIndex();
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    if (!jam2::limits::valid_sample_rate(sampleRate)) {
        result.error = QStringLiteral("prepared mix sample rate must be within %1..%2 Hz")
                           .arg(jam2::limits::kMinimumSampleRate)
                           .arg(jam2::limits::kMaximumSampleRate);
        return result;
    }
    if (!hasRenderableSources(project, result.bankIndex)) {
        result.error = QStringLiteral(
            "prepared mix requires at least one playable WAV-backed lane");
        return result;
    }
    const LooperBank& bank = project.banks().at(result.bankIndex);
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
            result.warnings.append(QStringLiteral("skipped invalid lane WAV %1: %2")
                .arg(lane.name, QString::fromStdString(inspected.error)));
            continue;
        }
        if (inspected.info.sample_rate != static_cast<std::uint32_t>(sampleRate)) {
            result.warnings.append(QStringLiteral("skipped lane with non-%1 Hz WAV: %2")
                .arg(sampleRate).arg(lane.name));
            continue;
        }
        if (inspected.info.frames == 0 ||
            inspected.info.frames > static_cast<std::uint64_t>(maxFrames) ||
            inspected.info.frames > static_cast<std::uint64_t>(std::numeric_limits<qint64>::max())) {
            result.warnings.append(QStringLiteral("skipped lane WAV outside the five-minute limit: %1")
                .arg(lane.name));
            continue;
        }
        const qint64 frames = static_cast<qint64>(inspected.info.frames);
        if (lane.startFrame < 0 || lane.startFrame > maxFrames) {
            result.warnings.append(QStringLiteral("skipped lane with invalid start frame: %1")
                .arg(lane.name));
            continue;
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
                result.warnings.append(QStringLiteral("skipped lane extending beyond five minutes: %1")
                    .arg(lane.name));
                continue;
            }
            outputEnd = lane.startFrame + visibleFrames;
        }
        // A Section can be extended by a longer recording or a moved clip.
        // Generated references keep their recorded musical duration; the new
        // trailing bars stay silent unless the user explicitly extends the
        // lane's stop frame or generates a new reference for the longer idea.
        if (outputEnd < lane.startFrame || outputEnd > maxFrames) {
            result.warnings.append(QStringLiteral("skipped lane with invalid stop frame: %1")
                .arg(lane.name));
            continue;
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
    if (sources.empty()) {
        result.error = result.warnings.isEmpty()
            ? QStringLiteral("prepared mix has no playable WAV-backed lanes")
            : QStringLiteral("prepared mix has no valid WAV-backed lanes");
        return result;
    }
    if (exactOutputFrames > 0) {
        length = qBound<qint64>(1, exactOutputFrames, maxFrames);
    } else {
        length = qMax<qint64>(1, length);
    }
    constexpr std::uint64_t maxWorkingBytes = 512ULL * 1024ULL * 1024ULL;
    constexpr std::uint64_t workingBytesPerOutputFrame =
        sizeof(qint32) + 2ULL * sizeof(float);
    if (static_cast<std::uint64_t>(length) > maxWorkingBytes / workingBytesPerOutputFrame) {
        result.error = QStringLiteral("prepared mix exceeds the bounded worker-memory limit");
        return result;
    }
    std::vector<qint32> mix(static_cast<std::size_t>(length), 0);

    constexpr qint64 decodeBlockFrames = 4096;
    int mixedSources = 0;
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
            result.warnings.append(QStringLiteral("skipped unreadable lane WAV data: %1")
                .arg(source.lane.name));
            continue;
        }
        std::vector<qint16> mono(static_cast<std::size_t>(visibleFrames), 0);
        QByteArray bytes;
        bytes.resize(static_cast<qsizetype>(decodeBlockFrames * source.info.block_align));
        qint64 decodedFrames = 0;
        bool truncated = false;
        while (decodedFrames < visibleFrames) {
            const qint64 framesThisBlock = qMin(
                decodeBlockFrames,
                visibleFrames - decodedFrames);
            const qint64 bytesThisBlock = framesThisBlock * source.info.block_align;
            if (file.read(bytes.data(), bytesThisBlock) != bytesThisBlock) {
                truncated = true;
                break;
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
        if (truncated) {
            result.warnings.append(QStringLiteral("skipped truncated lane WAV data: %1")
                .arg(source.lane.name));
            continue;
        }
        ++mixedSources;

        const double gain = std::pow(10.0, qBound(-60.0, source.lane.gainDb, 12.0) / 20.0);
        // An exact musical duration deliberately crops lanes retained from a
        // longer bank/arrangement. Never let their placement extend beyond
        // the exact-sized mix buffer.
        const qint64 mixStart = qBound<qint64>(0, source.outputStart, length);
        const qint64 mixEnd = qBound<qint64>(mixStart, source.outputEnd, length);
        for (qint64 out = mixStart; out < mixEnd; ++out) {
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
    if (mixedSources == 0) {
        result.error = QStringLiteral("prepared mix could not read any playable lane WAV data");
        return result;
    }

    std::vector<float> processed(static_cast<std::size_t>(length), 0.0f);
    for (qint64 i = 0; i < length; ++i) {
        processed[static_cast<std::size_t>(i)] =
            static_cast<float>(
                mix[static_cast<std::size_t>(i)] / 32768.0);
    }
    processed = processTrack(std::move(processed), sampleRate, track, result.error);
    if (!result.error.isEmpty()) {
        return result;
    }
    result.masterPreGain = kPreparedMixMasterPreGain;
    for (float& sample : processed) {
        result.preMasterPeak =
            qMax(result.preMasterPeak, std::abs(sample));
        const double driven =
            kPreparedMixMasterPreGain * sample;
        if (std::abs(driven) > 1.0) {
            ++result.overUnitySamples;
        }
        sample = static_cast<float>(std::tanh(driven));
        result.outputPeak =
            qMax(result.outputPeak, std::abs(sample));
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

PreparedMixResult PreparedMixRenderer::renderSequence(
    const LooperProject& project,
    const QString& projectFolder,
    int sampleRate,
    const QString& outputPath,
    const SharedTrackModel& track,
    const QVector<PreparedMixSequenceSegment>& segments)
{
    PreparedMixResult result;
    result.bankIndex = -1;
    result.sampleRate = sampleRate;
    const qint64 started = QDateTime::currentMSecsSinceEpoch();
    if (!jam2::limits::valid_sample_rate(sampleRate)) {
        result.error = QStringLiteral("export sample rate must be within %1..%2 Hz")
            .arg(jam2::limits::kMinimumSampleRate)
            .arg(jam2::limits::kMaximumSampleRate);
        return result;
    }
    if (segments.isEmpty() || segments.size() > 64) {
        result.error = QStringLiteral("export requires between 1 and 64 arrangement rows");
        return result;
    }

    constexpr qint64 kMaximumExportSeconds = 60LL * 60LL;
    const qint64 durationLimitFrames =
        static_cast<qint64>(sampleRate) * kMaximumExportSeconds;
    const qint64 riffLimitFrames = static_cast<qint64>(
        (static_cast<std::uint64_t>(std::numeric_limits<quint32>::max()) - 36ULL) / 2ULL);
    const qint64 maximumFrames = qMin(durationLimitFrames, riffLimitFrames);
    qint64 totalFrames = 0;
    for (const PreparedMixSequenceSegment& segment : segments) {
        if (segment.bankIndex < 0 || segment.bankIndex >= project.banks().size() ||
            segment.repeats < 1 || segment.repeats > 64 ||
            segment.exactOutputFrames <= 0 ||
            segment.exactOutputFrames > maximumFrames) {
            result.error = QStringLiteral("export contains an invalid section, repeat, or duration");
            return result;
        }
        if (segment.exactOutputFrames >
            (maximumFrames - totalFrames) / segment.repeats) {
            result.error = QStringLiteral("export exceeds the one-hour WAV duration limit");
            return result;
        }
        totalFrames += segment.exactOutputFrames * segment.repeats;
    }

    struct TemporaryBankFiles {
        ~TemporaryBankFiles()
        {
            for (const QString& path : paths) {
                (void)QFile::remove(path);
            }
        }
        QStringList paths;
    } temporary;
    const QString temporaryToken =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    std::array<QString, 4> renderedPaths;
    std::array<bool, 4> renderedBanks{};
    for (const PreparedMixSequenceSegment& segment : segments) {
        const int bank = segment.bankIndex;
        if (renderedBanks[static_cast<std::size_t>(bank)] ||
            !hasRenderableSources(project, bank)) {
            continue;
        }
        const QString bankPath = QFileInfo(outputPath).dir().absoluteFilePath(
            QStringLiteral(".jam2-export-%1-bank-%2.wav")
                .arg(temporaryToken).arg(bank));
        temporary.paths.append(bankPath);
        const PreparedMixResult bankResult = render(
            project,
            projectFolder,
            sampleRate,
            bankPath,
            track,
            bank,
            segment.exactOutputFrames);
        if (!bankResult.error.isEmpty()) {
            result.error = QStringLiteral("Section %1: %2")
                .arg(QChar(QLatin1Char('A').unicode() + bank), bankResult.error);
            return result;
        }
        for (const QString& warning : bankResult.warnings) {
            result.warnings.append(QStringLiteral("Section %1: %2")
                .arg(QChar(QLatin1Char('A').unicode() + bank), warning));
        }
        renderedBanks[static_cast<std::size_t>(bank)] = true;
        renderedPaths[static_cast<std::size_t>(bank)] = bankPath;
        result.preMasterPeak = qMax(result.preMasterPeak, bankResult.preMasterPeak);
        result.outputPeak = qMax(result.outputPeak, bankResult.outputPeak);
        result.overUnitySamples += bankResult.overUnitySamples;
    }

    const std::uint64_t dataBytes = static_cast<std::uint64_t>(totalFrames) * 2ULL;
    QDir().mkpath(QFileInfo(outputPath).absolutePath());
    QSaveFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly)) {
        result.error = QStringLiteral("cannot open export output");
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
        result.error = QStringLiteral("cannot write export WAV header");
        return result;
    }

    static constexpr qint64 kCopyBlockBytes = 64LL * 1024LL;
    const QByteArray silence(static_cast<qsizetype>(kCopyBlockBytes), '\0');
    const auto writeSilence = [&output, &hash, &silence](qint64 frames) {
        qint64 bytesRemaining = frames * 2;
        while (bytesRemaining > 0) {
            const qint64 count = qMin(bytesRemaining, kCopyBlockBytes);
            const QByteArrayView block(silence.constData(), count);
            hash.addData(block);
            if (output.write(block.data(), block.size()) != block.size()) return false;
            bytesRemaining -= count;
        }
        return true;
    };
    const auto writeBank = [
        &output,
        &hash,
        &writeSilence,
        sampleRate
    ](const QString& path, qint64 exactFrames, QString& error) {
        if (path.isEmpty()) {
            if (!writeSilence(exactFrames)) {
                error = QStringLiteral("cannot write silent section audio");
                return false;
            }
            return true;
        }
        const jam2::wav::InspectResult inspected =
            jam2::wav::inspect_pcm16_file(nativeFilePath(path));
        if (!inspected || inspected.info.channels != 1 ||
            inspected.info.sample_rate != static_cast<std::uint32_t>(sampleRate)) {
            error = QStringLiteral("temporary section render is not compatible mono PCM16");
            return false;
        }
        QFile source(path);
        if (inspected.info.data_offset >
                static_cast<std::uint64_t>(std::numeric_limits<qint64>::max()) ||
            !source.open(QIODevice::ReadOnly) ||
            !source.seek(static_cast<qint64>(inspected.info.data_offset))) {
            error = QStringLiteral("cannot read temporary section render");
            return false;
        }
        const qint64 availableFrames = static_cast<qint64>(qMin<std::uint64_t>(
            inspected.info.frames,
            static_cast<std::uint64_t>(std::numeric_limits<qint64>::max())));
        qint64 bytesRemaining = qMin(exactFrames, availableFrames) * 2;
        QByteArray block;
        block.resize(static_cast<qsizetype>(kCopyBlockBytes));
        while (bytesRemaining > 0) {
            const qint64 count = qMin(bytesRemaining, kCopyBlockBytes);
            if (source.read(block.data(), count) != count) {
                error = QStringLiteral("temporary section render ended unexpectedly");
                return false;
            }
            const QByteArrayView view(block.constData(), count);
            hash.addData(view);
            if (output.write(view.data(), view.size()) != view.size()) {
                error = QStringLiteral("cannot write section audio to export");
                return false;
            }
            bytesRemaining -= count;
        }
        const qint64 paddingFrames = exactFrames - qMin(exactFrames, availableFrames);
        if (paddingFrames > 0 && !writeSilence(paddingFrames)) {
            error = QStringLiteral("cannot pad section audio to its boundary");
            return false;
        }
        return true;
    };

    for (const PreparedMixSequenceSegment& segment : segments) {
        const QString bankPath = renderedPaths[static_cast<std::size_t>(segment.bankIndex)];
        for (int repeat = 0; repeat < segment.repeats; ++repeat) {
            if (!writeBank(bankPath, segment.exactOutputFrames, result.error)) {
                return result;
            }
        }
    }
    if (!output.commit()) {
        result.error = QStringLiteral("cannot atomically commit exported WAV");
        return result;
    }
    result.path = outputPath;
    result.frames = totalFrames;
    result.fileBytes = static_cast<qint64>(44ULL + dataBytes);
    result.durationMs = static_cast<int>(qMin<qint64>(
        std::numeric_limits<int>::max(), totalFrames * 1000LL / sampleRate));
    result.sha256 = QString::fromLatin1(hash.result().toHex());
    result.renderMs = QDateTime::currentMSecsSinceEpoch() - started;
    return result;
}

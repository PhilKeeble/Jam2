#include "TrackWorkspaceSupport.hpp"

#include "GuiLoopbackRecorder.hpp"

#include "pcm16_wav.hpp"
#include "runtime_limits.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {

std::filesystem::path nativeFilePath(const QString& path)
{
#if defined(_WIN32)
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(path.toUtf8().constData());
#endif
}

QString sha256FileHex(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray block = file.read(1024 * 1024);
        if (block.isEmpty() && file.error() != QFileDevice::NoError) return {};
        hash.addData(block);
    }
    return QString::fromLatin1(hash.result().toHex());
}

Qt::CaseSensitivity pathCaseSensitivity()
{
#if defined(_WIN32)
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

QString normalizedPath(const QString& path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path.trimmed()));
}

bool samePath(const QString& first, const QString& second)
{
    return normalizedPath(first).compare(
        normalizedPath(second), pathCaseSensitivity()) == 0;
}

bool isSafeAssetFolder(const QString& folder)
{
    if (folder.isEmpty() || folder == QStringLiteral(".") ||
        folder == QStringLiteral("..")) {
        return false;
    }
    return std::all_of(folder.cbegin(), folder.cend(), [](QChar character) {
        return character.isLetterOrNumber() || character == QLatin1Char('-') ||
            character == QLatin1Char('_');
    });
}

void appendLe16(QByteArray& bytes, std::uint16_t value)
{
    bytes.append(static_cast<char>(value & 0xffU));
    bytes.append(static_cast<char>((value >> 8U) & 0xffU));
}

void appendLe32(QByteArray& bytes, std::uint32_t value)
{
    bytes.append(static_cast<char>(value & 0xffU));
    bytes.append(static_cast<char>((value >> 8U) & 0xffU));
    bytes.append(static_cast<char>((value >> 16U) & 0xffU));
    bytes.append(static_cast<char>((value >> 24U) & 0xffU));
}

QByteArray pcm16WavHeader(
    std::uint16_t channels,
    std::uint32_t sampleRate,
    std::uint32_t dataBytes)
{
    QByteArray header;
    header.reserve(44);
    header.append("RIFF", 4);
    appendLe32(header, 36U + dataBytes);
    header.append("WAVEfmt ", 8);
    appendLe32(header, 16U);
    appendLe16(header, 1U);
    appendLe16(header, channels);
    appendLe32(header, sampleRate);
    const std::uint16_t blockAlign = static_cast<std::uint16_t>(channels * 2U);
    appendLe32(header, sampleRate * static_cast<std::uint32_t>(blockAlign));
    appendLe16(header, blockAlign);
    appendLe16(header, 16U);
    header.append("data", 4);
    appendLe32(header, dataBytes);
    return header;
}

std::vector<std::int16_t> readPcm16Samples(
    const QString& path,
    const jam2::wav::Pcm16Info& info)
{
    if (info.data_bytes > static_cast<std::uint64_t>(
            (std::numeric_limits<std::size_t>::max)())) {
        throw std::length_error("WAV audio data is too large to import");
    }
    std::vector<std::int16_t> samples(
        static_cast<std::size_t>(info.data_bytes / sizeof(std::int16_t)));
    QFile source(path);
    if (!source.open(QIODevice::ReadOnly) ||
        !source.seek(static_cast<qint64>(info.data_offset))) {
        throw std::runtime_error("could not open WAV audio data for resampling");
    }
    char* destination = reinterpret_cast<char*>(samples.data());
    qint64 remaining = static_cast<qint64>(info.data_bytes);
    while (remaining > 0) {
        const qint64 read = source.read(destination, remaining);
        if (read <= 0) {
            throw std::runtime_error("failed while reading WAV audio data for resampling");
        }
        destination += read;
        remaining -= read;
    }
#if Q_BYTE_ORDER == Q_BIG_ENDIAN
    for (std::int16_t& sample : samples) {
        sample = qFromLittleEndian(sample);
    }
#endif
    return samples;
}

QByteArray pcm16Bytes(const std::vector<std::int16_t>& samples)
{
    const auto byteCount = static_cast<qsizetype>(
        samples.size() * sizeof(std::int16_t));
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
    return QByteArray::fromRawData(
        reinterpret_cast<const char*>(samples.data()), byteCount);
#else
    QByteArray bytes(byteCount, Qt::Uninitialized);
    for (std::size_t index = 0; index < samples.size(); ++index) {
        qToLittleEndian(samples[index], bytes.data() + index * sizeof(std::int16_t));
    }
    return bytes;
#endif
}

bool isManagedPracticeReference(const LooperLane& lane)
{
    return !lane.referenceKind.isEmpty() ||
        lane.name == QStringLiteral("Practice Chords") ||
        lane.name == QStringLiteral("Practice Drums") ||
        lane.name == QStringLiteral("Practice Melody") ||
        lane.name == QStringLiteral("Practice Bass") ||
        lane.name == QStringLiteral("Practice Support");
}

} // namespace

WavMetadata readWavMetadata(const QString& path)
{
    const jam2::wav::InspectResult inspected = jam2::wav::inspect_pcm16_file(nativeFilePath(path));
    if (!inspected) throw std::runtime_error(inspected.error);
    WavMetadata meta;
    meta.audioFormat = 1;
    meta.sampleRate = static_cast<int>(inspected.info.sample_rate);
    meta.channels = static_cast<int>(inspected.info.channels);
    meta.bitsPerSample = 16;
    meta.dataBytes = static_cast<qint64>(inspected.info.data_bytes);
    meta.frames = static_cast<qint64>(inspected.info.frames);
    const std::uint64_t durationMs = inspected.info.frames * 1000ULL / inspected.info.sample_rate;
    meta.durationMs = static_cast<int>(std::min<std::uint64_t>(
        durationMs,
        std::numeric_limits<int>::max()));
    meta.sha256 = sha256FileHex(path);
    if (meta.sha256.isEmpty()) throw std::runtime_error("failed to hash WAV");
    return meta;
}

QJsonObject readTrackSidecarJson(const QString& wavPath)
{
    QFile file(wavPath + QStringLiteral(".json"));
    constexpr qint64 kMaxSidecarBytes = 1024 * 1024;
    if (!file.open(QIODevice::ReadOnly) ||
        file.size() < 0 || file.size() > kMaxSidecarBytes) {
        return {};
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject{};
}

StagedPcm16Asset stagePcm16Asset(
    const QString& sourcePath,
    const QString& stagingFolder,
    int expectedSampleRate,
    const QString& assetFolder)
{
    StagedPcm16Asset result;
    const QFileInfo sourceInfo(sourcePath);
    result.sourcePath = sourceInfo.absoluteFilePath();
    result.displayName = sourceInfo.completeBaseName();
    const QString trimmedStagingFolder = stagingFolder.trimmed();
    if (trimmedStagingFolder.isEmpty() ||
        !QFileInfo(trimmedStagingFolder).isAbsolute()) {
        result.error = QStringLiteral("WAV staging folder must be an absolute path");
        return result;
    }
    if (!isSafeAssetFolder(assetFolder)) {
        result.error = QStringLiteral(
            "WAV asset folder must be one safe directory name");
        return result;
    }
    if (expectedSampleRate != 0 &&
        !jam2::limits::valid_sample_rate(expectedSampleRate)) {
        result.error = QStringLiteral("requested WAV sample rate is outside the supported range");
        return result;
    }
    try {
        result.metadata = readWavMetadata(result.sourcePath);
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
        return result;
    }
    if (result.metadata.dataBytes <= 0) {
        result.error = QStringLiteral("WAV contains no audio frames");
        return result;
    }
    result.sourceSampleRate = result.metadata.sampleRate;
    result.sourceFrames = result.metadata.frames;
    if (expectedSampleRate > 0 && result.metadata.sampleRate != expectedSampleRate) {
        try {
            const jam2::wav::InspectResult inspected =
                jam2::wav::inspect_pcm16_file(nativeFilePath(result.sourcePath));
            if (!inspected) throw std::runtime_error(inspected.error);
            const std::uint64_t bytesPerFrame =
                static_cast<std::uint64_t>(inspected.info.channels) * sizeof(std::int16_t);
            const std::uint64_t maximumDataBytes = std::min<std::uint64_t>(
                std::numeric_limits<std::uint32_t>::max() - 36ULL,
                jam2::wav::kDefaultMaxFileBytes - 44ULL);
            const std::uint64_t maximumOutputFrames = maximumDataBytes / bytesPerFrame;
            const long double exactOutputFrames =
                static_cast<long double>(inspected.info.frames) *
                static_cast<long double>(expectedSampleRate) /
                static_cast<long double>(inspected.info.sample_rate);
            if (!std::isfinite(exactOutputFrames) ||
                exactOutputFrames > static_cast<long double>(maximumOutputFrames) + 0.499L) {
                throw std::length_error("resampled WAV exceeds the managed file-size limit");
            }
            const std::uint64_t expectedOutputFrames = std::max<std::uint64_t>(
                1ULL,
                static_cast<std::uint64_t>(std::llround(exactOutputFrames)));
            std::vector<std::int16_t> sourceSamples =
                readPcm16Samples(result.sourcePath, inspected.info);
            std::vector<std::int16_t> converted =
                jam2::gui::resample_pcm16_interleaved(
                    sourceSamples,
                    static_cast<int>(inspected.info.channels),
                    static_cast<int>(inspected.info.sample_rate),
                    expectedSampleRate);
            if (converted.size() != expectedOutputFrames * inspected.info.channels) {
                throw std::runtime_error("resampler produced an unexpected frame count");
            }

            const std::uint64_t dataBytes64 =
                static_cast<std::uint64_t>(converted.size()) * sizeof(std::int16_t);
            if (dataBytes64 > std::numeric_limits<std::uint32_t>::max() - 36ULL ||
                dataBytes64 + 44ULL > jam2::wav::kDefaultMaxFileBytes) {
                throw std::length_error("resampled WAV exceeds the managed file-size limit");
            }
            const std::uint32_t dataBytes = static_cast<std::uint32_t>(dataBytes64);
            const QByteArray header = pcm16WavHeader(
                inspected.info.channels,
                static_cast<std::uint32_t>(expectedSampleRate),
                dataBytes);
            const QByteArray audioBytes = pcm16Bytes(converted);
            QCryptographicHash hash(QCryptographicHash::Sha256);
            hash.addData(header);
            hash.addData(audioBytes);
            result.sha256 = QString::fromLatin1(hash.result().toHex());
            result.stagedPath = QDir(trimmedStagingFolder).absoluteFilePath(
                assetFolder + QLatin1Char('/') + result.sha256 + QStringLiteral(".wav"));
            if (!QDir().mkpath(QFileInfo(result.stagedPath).absolutePath())) {
                throw std::runtime_error("could not create the WAV staging folder");
            }
            if (!QFileInfo::exists(result.stagedPath) ||
                sha256FileHex(result.stagedPath) != result.sha256) {
                QSaveFile destination(result.stagedPath);
                if (!destination.open(QIODevice::WriteOnly) ||
                    destination.write(header) != header.size() ||
                    destination.write(audioBytes) != audioBytes.size() ||
                    !destination.commit()) {
                    throw std::runtime_error("could not atomically write the resampled WAV");
                }
                result.stagedFileCreated = true;
            }

            result.metadata.audioFormat = 1;
            result.metadata.sampleRate = expectedSampleRate;
            result.metadata.channels = static_cast<int>(inspected.info.channels);
            result.metadata.bitsPerSample = 16;
            result.metadata.dataBytes = static_cast<qint64>(dataBytes);
            result.metadata.frames = static_cast<qint64>(
                converted.size() / inspected.info.channels);
            result.metadata.durationMs = static_cast<int>(
                std::min<std::uint64_t>(
                    static_cast<std::uint64_t>(result.metadata.frames) * 1000ULL /
                        static_cast<std::uint64_t>(expectedSampleRate),
                    std::numeric_limits<int>::max()));
            result.metadata.sha256 = result.sha256;
            result.resampled = true;
            return result;
        } catch (const std::exception& error) {
            result.stagedPath.clear();
            result.sha256.clear();
            result.error = QStringLiteral("could not resample WAV from %1 Hz to %2 Hz: %3")
                .arg(result.sourceSampleRate)
                .arg(expectedSampleRate)
                .arg(QString::fromUtf8(error.what()));
            return result;
        }
    }
    result.sha256 = result.metadata.sha256;
    const QString managedFolder = QDir(trimmedStagingFolder).absoluteFilePath(assetFolder);
    if (assetFolder == QStringLiteral("recorded") &&
        samePath(sourceInfo.absolutePath(), managedFolder)) {
        result.stagedPath = result.sourcePath;
        return result;
    }
    result.stagedPath = QDir(trimmedStagingFolder).absoluteFilePath(
        assetFolder + QLatin1Char('/') + result.sha256 + QStringLiteral(".wav"));
    if (!QDir().mkpath(QFileInfo(result.stagedPath).absolutePath())) {
        result.error = QStringLiteral("could not create the WAV staging folder");
        return result;
    }
    if (QFileInfo::exists(result.stagedPath) &&
        sha256FileHex(result.stagedPath) == result.sha256) {
        return result;
    }
    QFile source(result.sourcePath);
    QSaveFile destination(result.stagedPath);
    if (!source.open(QIODevice::ReadOnly) || !destination.open(QIODevice::WriteOnly)) {
        result.error = QStringLiteral("could not open the WAV for atomic staging");
        return result;
    }
    constexpr qint64 kCopyBlockBytes = 1024 * 1024;
    while (!source.atEnd()) {
        const QByteArray block = source.read(kCopyBlockBytes);
        if (block.isEmpty() && source.error() != QFileDevice::NoError) {
            result.error = QStringLiteral("failed while reading the WAV for staging");
            return result;
        }
        if (destination.write(block) != block.size()) {
            result.error = QStringLiteral("failed while writing the staged WAV");
            return result;
        }
    }
    if (!destination.commit()) {
        result.error = QStringLiteral("could not atomically commit the staged WAV");
        return result;
    }
    result.stagedFileCreated = true;
    if (sha256FileHex(result.stagedPath) != result.sha256) {
        result.error = QStringLiteral(
            "source WAV changed while it was being staged");
    }
    return result;
}

int mergeSynchronizedLooperLanes(
    QJsonObject& song,
    const LooperProject& localProject)
{
    LooperProject received;
    const QJsonObject receivedLooper = song.value(QStringLiteral("looper")).toObject();
    if (receivedLooper.isEmpty() || !received.loadJson(receivedLooper)) return 0;

    int preserved = 0;
    for (int bankIndex = 0;
         bankIndex < localProject.banks().size() && bankIndex < received.banks().size();
         ++bankIndex) {
        auto& remoteLanes = received.banks()[bankIndex].lanes;
        const QVector<LooperLane>& localLanes = localProject.banks().at(bankIndex).lanes;
        for (LooperLane& remote : remoteLanes) {
            auto match = std::find_if(
                localLanes.cbegin(), localLanes.cend(),
                [&remote](const LooperLane& local) {
                    if (!remote.assetHash.isEmpty() &&
                        local.assetHash == remote.assetHash) {
                        return true;
                    }
                    if (remote.id.isEmpty() || local.id != remote.id) return false;
                    if (!local.localOnly && remote.assetHash.isEmpty() &&
                        remote.assetPath.trimmed().isEmpty()) {
                        return true;
                    }
                    return local.assetHash == remote.assetHash ||
                        (local.assetHash.isEmpty() && local.assetPath.trimmed().isEmpty());
                });
            if (match != localLanes.cend()) {
                const double gainDb = match->gainDb;
                const bool muted = match->muted;
                const bool solo = match->solo;
                const bool localOnly = match->localOnly;
                const QString localPath = match->assetPath;
                remote.gainDb = gainDb;
                remote.muted = muted;
                remote.solo = solo;
                remote.localOnly = localOnly;
                if (!localPath.trimmed().isEmpty() && !remote.assetHash.isEmpty()) {
                    remote.assetPath = localPath;
                }
            }
        }

        for (const LooperLane& local : localLanes) {
            const bool alreadyPresent = std::any_of(
                remoteLanes.cbegin(), remoteLanes.cend(),
                [&local](const LooperLane& remote) {
                    if (!local.assetHash.isEmpty() &&
                        remote.assetHash == local.assetHash) {
                        return true;
                    }
                    if (local.id.isEmpty() || remote.id != local.id) return false;
                    if (!local.localOnly && remote.assetHash.isEmpty() &&
                        remote.assetPath.trimmed().isEmpty()) {
                        return true;
                    }
                    return remote.assetHash == local.assetHash ||
                        (local.assetHash.isEmpty() && local.assetPath.trimmed().isEmpty());
                });
            if (alreadyPresent) continue;

            const bool conflictingSameLaneId = std::any_of(
                remoteLanes.cbegin(), remoteLanes.cend(),
                [&local](const LooperLane& remote) {
                    return !local.id.isEmpty() && remote.id == local.id;
                });
            // An incoming authoritative arrangement may deliberately omit a
            // managed reference. Re-adding every omitted reference here makes
            // deletions bounce back from another peer indefinitely.
            if (!conflictingSameLaneId) {
                continue;
            }

            LooperLane preservedLane = local;
            if (conflictingSameLaneId) preservedLane.id.clear();
            if (received.appendLane(bankIndex, std::move(preservedLane))) ++preserved;
        }
    }
    song.insert(QStringLiteral("looper"), received.toJson());
    return preserved;
}

int mergeLocalOnlyLooperLanes(
    QJsonObject& song,
    const LooperProject& localProject)
{
    LooperProject received;
    const QJsonObject receivedLooper = song.value(QStringLiteral("looper")).toObject();
    if (receivedLooper.isEmpty() || !received.loadJson(receivedLooper)) return 0;
    int preserved = 0;
    for (int bankIndex = 0;
         bankIndex < localProject.banks().size() && bankIndex < received.banks().size();
        ++bankIndex) {
        for (const LooperLane& local : localProject.banks().at(bankIndex).lanes) {
            if (local.assetPath.trimmed().isEmpty() ||
                isManagedPracticeReference(local) ||
                !local.localOnly) {
                continue;
            }
            const bool alreadyPresent = std::any_of(
                received.banks().at(bankIndex).lanes.cbegin(),
                received.banks().at(bankIndex).lanes.cend(),
                [&local](const LooperLane& candidate) {
                    if (!local.assetHash.isEmpty()) {
                        return candidate.assetHash == local.assetHash;
                    }
                    return candidate.assetHash.isEmpty() &&
                        !candidate.assetPath.trimmed().isEmpty() &&
                        samePath(candidate.assetPath, local.assetPath);
                });
            if (alreadyPresent) continue;
            LooperLane preservedLane = local;
            const bool idCollision = std::any_of(
                received.banks().at(bankIndex).lanes.cbegin(),
                received.banks().at(bankIndex).lanes.cend(),
                [&preservedLane](const LooperLane& candidate) {
                    return !preservedLane.id.isEmpty() && candidate.id == preservedLane.id;
                });
            if (idCollision) preservedLane.id.clear();
            if (received.appendLane(bankIndex, std::move(preservedLane))) ++preserved;
        }
    }
    if (preserved > 0) song.insert(QStringLiteral("looper"), received.toJson());
    return preserved;
}

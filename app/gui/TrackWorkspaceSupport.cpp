#include "TrackWorkspaceSupport.hpp"

#include "pcm16_wav.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <stdexcept>

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
    while (!file.atEnd()) hash.addData(file.read(1024 * 1024));
    return QString::fromLatin1(hash.result().toHex());
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
    const std::uint64_t durationMs = inspected.info.frames * 1000ULL / inspected.info.sample_rate;
    meta.durationMs = static_cast<int>(std::min<std::uint64_t>(
        durationMs,
        std::numeric_limits<int>::max()));
    meta.sha256 = sha256FileHex(path);
    if (meta.sha256.isEmpty()) throw std::runtime_error("failed to hash WAV");
    return meta;
}

StagedPcm16Asset stagePcm16Asset(
    const QString& sourcePath,
    const QString& stagingFolder,
    int expectedSampleRate)
{
    StagedPcm16Asset result;
    const QFileInfo sourceInfo(sourcePath);
    result.sourcePath = sourceInfo.absoluteFilePath();
    result.displayName = sourceInfo.completeBaseName();
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
    if (expectedSampleRate > 0 && result.metadata.sampleRate != expectedSampleRate) {
        result.error = QStringLiteral(
            "Sample-rate mismatch: this jam uses %1 Hz but the WAV is %2 Hz. "
            "The WAV was not loaded; convert it or use a %1 Hz source.")
            .arg(expectedSampleRate)
            .arg(result.metadata.sampleRate);
        return result;
    }
    result.sha256 = result.metadata.sha256;
    result.stagedPath = QDir(stagingFolder).absoluteFilePath(
        QStringLiteral("wavs/") + result.sha256 + QStringLiteral(".wav"));
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
    }
    return result;
}

int mergeQuarantinedLocalLanes(
    QJsonObject& song,
    const LooperProject& localProject,
    int expectedSampleRate)
{
    if (expectedSampleRate <= 0) return 0;
    LooperProject received;
    const QJsonObject receivedLooper = song.value(QStringLiteral("looper")).toObject();
    if (receivedLooper.isEmpty() || !received.loadJson(receivedLooper)) return 0;
    int preserved = 0;
    for (int bankIndex = 0;
         bankIndex < localProject.banks().size() && bankIndex < received.banks().size();
         ++bankIndex) {
        for (const LooperLane& local : localProject.banks().at(bankIndex).lanes) {
            if (local.assetPath.trimmed().isEmpty() || local.sampleRate == expectedSampleRate) continue;
            const bool alreadyPresent = std::any_of(
                received.banks().at(bankIndex).lanes.cbegin(),
                received.banks().at(bankIndex).lanes.cend(),
                [&local](const LooperLane& candidate) {
                    return !local.assetHash.isEmpty() && candidate.assetHash == local.assetHash;
                });
            if (alreadyPresent) continue;
            LooperLane quarantined = local;
            const bool idCollision = std::any_of(
                received.banks().at(bankIndex).lanes.cbegin(),
                received.banks().at(bankIndex).lanes.cend(),
                [&quarantined](const LooperLane& candidate) {
                    return !quarantined.id.isEmpty() && candidate.id == quarantined.id;
                });
            if (idCollision) quarantined.id.clear();
            quarantined.sampleRateCompatible = false;
            if (received.appendLane(bankIndex, std::move(quarantined))) ++preserved;
        }
    }
    if (preserved > 0) song.insert(QStringLiteral("looper"), received.toJson());
    return preserved;
}

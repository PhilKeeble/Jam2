#include "TrackWorkspaceSupport.hpp"
#include "LooperAssetMaterializer.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

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

QByteArray pcm16Wav(int sampleRate, int channels, int frames)
{
    require(sampleRate > 0 && channels > 0 && frames >= 0,
        "invalid PCM16 fixture request");
    QByteArray audio;
    audio.reserve(frames * channels * 2);
    for (int frame = 0; frame < frames; ++frame) {
        for (int channel = 0; channel < channels; ++channel) {
            const int value = ((frame * 97 + channel * 503) % 60001) - 30000;
            appendLe16(audio, static_cast<std::uint16_t>(
                static_cast<std::int16_t>(value)));
        }
    }

    QByteArray bytes;
    bytes.reserve(44 + audio.size());
    bytes.append("RIFF", 4);
    appendLe32(bytes, static_cast<std::uint32_t>(36 + audio.size()));
    bytes.append("WAVEfmt ", 8);
    appendLe32(bytes, 16U);
    appendLe16(bytes, 1U);
    appendLe16(bytes, static_cast<std::uint16_t>(channels));
    appendLe32(bytes, static_cast<std::uint32_t>(sampleRate));
    const std::uint16_t blockAlign = static_cast<std::uint16_t>(channels * 2);
    appendLe32(bytes, static_cast<std::uint32_t>(sampleRate) * blockAlign);
    appendLe16(bytes, blockAlign);
    appendLe16(bytes, 16U);
    bytes.append("data", 4);
    appendLe32(bytes, static_cast<std::uint32_t>(audio.size()));
    bytes.append(audio);
    return bytes;
}

void writeBytes(const QString& path, const QByteArray& bytes)
{
    require(QDir().mkpath(QFileInfo(path).absolutePath()),
        "could not create test-file parent");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
            file.write(bytes) == bytes.size(),
        "could not write test bytes");
}

QByteArray readBytes(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "could not read test file");
    return file.readAll();
}

QString sha256(const QByteArray& bytes)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QTemporaryDir makeRoot()
{
    const QString artifactRoot = qEnvironmentVariable("JAM2_TEST_ARTIFACT_ROOT");
    require(!artifactRoot.isEmpty(), "CTest omitted the build-local artifact root");
    QTemporaryDir root(QDir(artifactRoot).absoluteFilePath(
        QStringLiteral("track-workspace-support-XXXXXX")));
    require(root.isValid(), "could not create build-local workspace fixture");
    require(canonicalPath(root.path()).startsWith(
                canonicalPath(artifactRoot) + QLatin1Char('/'),
                Qt::CaseInsensitive),
        "workspace fixture escaped build/test-artifacts");
    return root;
}

void testMetadataAndRejection(const QString& root)
{
    const QByteArray sourceBytes = pcm16Wav(44100, 2, 441);
    const QString source = QDir(root).absoluteFilePath(
        QStringLiteral("inputs/t\u00e9st-\u6f14\u594f.wav"));
    writeBytes(source, sourceBytes);

    const WavMetadata metadata = readWavMetadata(source);
    require(metadata.audioFormat == 1 && metadata.sampleRate == 44100 &&
            metadata.channels == 2 && metadata.bitsPerSample == 16 &&
            metadata.dataBytes == 1764 && metadata.frames == 441 &&
            metadata.durationMs == 10 && metadata.sha256 == sha256(sourceBytes),
        "strict WAV metadata must preserve format, duration, frames, and exact hash");

    requireThrows([&] {
        (void)readWavMetadata(QDir(root).absoluteFilePath(
            QStringLiteral("missing.wav")));
    }, "missing WAV metadata must reject");

    const QString malformed = QDir(root).absoluteFilePath(
        QStringLiteral("inputs/malformed.wav"));
    writeBytes(malformed, QByteArrayLiteral("not a WAV"));
    requireThrows([&] { (void)readWavMetadata(malformed); },
        "malformed WAV metadata must reject");

    const QString empty = QDir(root).absoluteFilePath(
        QStringLiteral("inputs/empty.wav"));
    writeBytes(empty, pcm16Wav(48000, 1, 0));
    const StagedPcm16Asset emptyResult = stagePcm16Asset(
        empty, QDir(root).absoluteFilePath(QStringLiteral("workspace")), 48000);
    require(!emptyResult.error.isEmpty() && emptyResult.stagedPath.isEmpty(),
        "empty PCM data must never become a staged asset");
}

void testTrackSidecarJson(const QString& root)
{
    const QString wavPath = QDir(root).absoluteFilePath(
        QStringLiteral("sidecars/take.wav"));
    const QString sidecarPath = wavPath + QStringLiteral(".json");

    require(readTrackSidecarJson(wavPath).isEmpty(),
        "a missing track sidecar must be treated as absent");

    writeBytes(sidecarPath, QByteArrayLiteral("not-json"));
    require(readTrackSidecarJson(wavPath).isEmpty(),
        "a malformed track sidecar must be rejected");

    writeBytes(sidecarPath, QByteArrayLiteral("[1,2,3]"));
    require(readTrackSidecarJson(wavPath).isEmpty(),
        "a non-object track sidecar must be rejected");

    writeBytes(sidecarPath, QByteArrayLiteral(
        "{\"title\":\"Shared take\",\"duration_ms\":1250}"));
    const QJsonObject valid = readTrackSidecarJson(wavPath);
    require(valid.value(QStringLiteral("title")).toString() ==
                QStringLiteral("Shared take") &&
            valid.value(QStringLiteral("duration_ms")).toInt() == 1250,
        "a bounded object sidecar must preserve its typed fields");

    writeBytes(sidecarPath, QByteArray(1024 * 1024 + 1, 'x'));
    require(readTrackSidecarJson(wavPath).isEmpty(),
        "a sidecar above the one-MiB input bound must be rejected");
}

void testExactStagingAndRepair(const QString& root)
{
    const QByteArray sourceBytes = pcm16Wav(44100, 2, 441);
    const QString source = QDir(root).absoluteFilePath(
        QStringLiteral("inputs/exact.wav"));
    const QString workspace = QDir(root).absoluteFilePath(
        QStringLiteral("workspace"));
    writeBytes(source, sourceBytes);

    const StagedPcm16Asset first = stagePcm16Asset(
        source, workspace, 44100, QStringLiteral("imported"));
    const QString expectedPath = QDir(workspace).absoluteFilePath(
        QStringLiteral("imported/%1.wav").arg(sha256(sourceBytes)));
    require(first.error.isEmpty() && !first.resampled && first.stagedFileCreated &&
            first.sourceSampleRate == 44100 && first.sourceFrames == 441 &&
            first.sha256 == sha256(sourceBytes) &&
            first.metadata.sha256 == first.sha256 &&
            canonicalPath(first.stagedPath) == canonicalPath(expectedPath) &&
            readBytes(first.stagedPath) == sourceBytes,
        "matching-rate staging must atomically retain exact source bytes and identity");

    const StagedPcm16Asset repeated = stagePcm16Asset(
        source, workspace, 44100, QStringLiteral("imported"));
    require(repeated.error.isEmpty() && !repeated.stagedFileCreated &&
            repeated.stagedPath == first.stagedPath &&
            readBytes(repeated.stagedPath) == sourceBytes,
        "matching-rate staging must be idempotent");

    writeBytes(first.stagedPath, QByteArrayLiteral("corrupt destination"));
    const StagedPcm16Asset repaired = stagePcm16Asset(
        source, workspace, 44100, QStringLiteral("imported"));
    require(repaired.error.isEmpty() && repaired.stagedFileCreated &&
            repaired.stagedPath == first.stagedPath &&
            readBytes(repaired.stagedPath) == sourceBytes,
        "staging must atomically replace a corrupt hash-named destination");

#if defined(_WIN32)
    const QString recordedFolder = QStringLiteral("RECORDED");
#else
    const QString recordedFolder = QStringLiteral("recorded");
#endif
    const QString recorded = QDir(workspace).absoluteFilePath(
        recordedFolder + QStringLiteral("/capture.wav"));
    writeBytes(recorded, sourceBytes);
    const StagedPcm16Asset owned = stagePcm16Asset(
        recorded, workspace, 44100, QStringLiteral("recorded"));
    require(owned.error.isEmpty() && !owned.stagedFileCreated &&
            canonicalPath(owned.stagedPath) == canonicalPath(recorded) &&
            readBytes(recorded) == sourceBytes,
        "an already-owned recording must not be copied or renamed");
}

void testResampledStagingAndRepair(const QString& root)
{
    const QByteArray sourceBytes = pcm16Wav(44100, 2, 441);
    const QString source = QDir(root).absoluteFilePath(
        QStringLiteral("inputs/resample.wav"));
    const QString workspace = QDir(root).absoluteFilePath(
        QStringLiteral("resampled-workspace"));
    writeBytes(source, sourceBytes);

    const StagedPcm16Asset first = stagePcm16Asset(
        source, workspace, 48000, QStringLiteral("active-rate"));
    require(first.error.isEmpty() && first.resampled && first.stagedFileCreated &&
            first.sourceSampleRate == 44100 && first.sourceFrames == 441 &&
            first.metadata.sampleRate == 48000 && first.metadata.channels == 2 &&
            first.metadata.frames == 480 && first.metadata.dataBytes == 1920 &&
            first.sha256 == first.metadata.sha256 &&
            QFileInfo(first.stagedPath).completeBaseName() == first.sha256,
        "resampled staging must publish exact source and converted metadata");
    const QByteArray convertedBytes = readBytes(first.stagedPath);
    require(first.sha256 == sha256(convertedBytes),
        "resampled staging filename and metadata must hash the complete output WAV");
    const WavMetadata inspected = readWavMetadata(first.stagedPath);
    require(inspected.sampleRate == 48000 && inspected.channels == 2 &&
            inspected.frames == 480 && inspected.dataBytes == 1920 &&
            inspected.sha256 == first.sha256,
        "resampled output must pass the same strict PCM16 parser used for imports");

    const StagedPcm16Asset repeated = stagePcm16Asset(
        source, workspace, 48000, QStringLiteral("active-rate"));
    require(repeated.error.isEmpty() && repeated.resampled &&
            !repeated.stagedFileCreated &&
            repeated.stagedPath == first.stagedPath &&
            readBytes(repeated.stagedPath) == convertedBytes,
        "resampled staging must be byte-idempotent");

    writeBytes(first.stagedPath, QByteArrayLiteral("corrupt conversion"));
    const StagedPcm16Asset repaired = stagePcm16Asset(
        source, workspace, 48000, QStringLiteral("active-rate"));
    require(repaired.error.isEmpty() && repaired.stagedFileCreated &&
            repaired.stagedPath == first.stagedPath &&
            readBytes(repaired.stagedPath) == convertedBytes,
        "resampling must atomically repair a corrupt hash-named conversion");
}

void testStagingBoundaryValidation(const QString& root)
{
    const QByteArray sourceBytes = pcm16Wav(48000, 1, 48);
    const QString source = QDir(root).absoluteFilePath(
        QStringLiteral("inputs/boundary.wav"));
    const QString workspace = QDir(root).absoluteFilePath(
        QStringLiteral("boundary-workspace"));
    writeBytes(source, sourceBytes);

    const StagedPcm16Asset unrestricted = stagePcm16Asset(
        source, workspace, 0, QStringLiteral("no-required-rate"));
    require(unrestricted.error.isEmpty() && !unrestricted.resampled &&
            unrestricted.metadata.sampleRate == 48000 &&
            readBytes(unrestricted.stagedPath) == sourceBytes,
        "zero requested rate must retain the source rate and exact bytes");
    for (int sampleRate : {8000, 384000}) {
        const StagedPcm16Asset boundary = stagePcm16Asset(
            source, workspace, sampleRate,
            QStringLiteral("rate-%1").arg(sampleRate));
        require(boundary.error.isEmpty() && boundary.resampled &&
                boundary.metadata.sampleRate == sampleRate &&
                boundary.metadata.frames == sampleRate / 1000,
            "maintained minimum and maximum sample rates must remain stageable");
    }
    for (int sampleRate : {-1, 1, 7999, 384001}) {
        const StagedPcm16Asset invalid = stagePcm16Asset(
            source, workspace, sampleRate, QStringLiteral("imported"));
        require(!invalid.error.isEmpty() && invalid.stagedPath.isEmpty(),
            "invalid requested sample rates must reject before filesystem mutation");
    }
    for (const QString& invalidRoot : {QString{}, QStringLiteral("   "),
            QStringLiteral("relative-staging")}) {
        const StagedPcm16Asset invalid = stagePcm16Asset(
            source, invalidRoot, 48000, QStringLiteral("imported"));
        require(!invalid.error.isEmpty() && invalid.stagedPath.isEmpty(),
            "blank or relative staging roots must reject");
    }
    for (const QString& invalidFolder : {QString{}, QStringLiteral("."),
            QStringLiteral(".."), QStringLiteral("../escape"),
            QStringLiteral("nested/folder"), QStringLiteral("C:\\escape")}) {
        const StagedPcm16Asset invalid = stagePcm16Asset(
            source, workspace, 48000, invalidFolder);
        require(!invalid.error.isEmpty() && invalid.stagedPath.isEmpty(),
            "asset staging must reject traversal, absolute, and nested folder names");
    }
    require(!QFileInfo::exists(QDir(root).absoluteFilePath(
                QStringLiteral("escape/%1.wav").arg(sha256(sourceBytes)))),
        "unsafe asset folders must not escape the workspace");

    const QString blockedRoot = QDir(root).absoluteFilePath(
        QStringLiteral("blocked-workspace"));
    const QString blocker = QDir(blockedRoot).absoluteFilePath(
        QStringLiteral("imported"));
    writeBytes(blocker, QByteArrayLiteral("owned blocker"));
    const StagedPcm16Asset blocked = stagePcm16Asset(
        source, blockedRoot, 48000, QStringLiteral("imported"));
    require(!blocked.error.isEmpty() && readBytes(blocker) == QByteArrayLiteral("owned blocker"),
        "a staging-directory collision must reject without replacing the blocking file");
}

LooperLane materializedLane(
    const QString& id,
    const QString& name,
    const QString& path,
    const QByteArray& bytes)
{
    LooperLane lane;
    lane.id = id;
    lane.name = name;
    lane.assetPath = path;
    lane.assetHash = sha256(bytes);
    lane.sampleRate = 48000;
    lane.sourceFrames = 48;
    lane.originKind = QStringLiteral("imported");
    return lane;
}

void testTransactionalAssetMaterialization(const QString& root)
{
    const QByteArray occupiedBytes = pcm16Wav(48000, 1, 12);
    const QByteArray firstBytes = pcm16Wav(48000, 1, 48);
    const QByteArray secondBytes = pcm16Wav(48000, 2, 31);
    const QString sourceRoot = QDir(root).absoluteFilePath(
        QStringLiteral("materialize-source"));
    const QString targetRoot = QDir(root).absoluteFilePath(
        QStringLiteral("materialize-target"));
    const QString occupied = QDir(targetRoot).absoluteFilePath(
        QStringLiteral("imported/Same.wav"));
    const QString first = QDir(sourceRoot).absoluteFilePath(
        QStringLiteral("first.wav"));
    const QString firstAlias = QDir(sourceRoot).absoluteFilePath(
        QStringLiteral("first-alias.wav"));
    const QString second = QDir(sourceRoot).absoluteFilePath(
        QStringLiteral("second.wav"));
    writeBytes(occupied, occupiedBytes);
    writeBytes(first, firstBytes);
    writeBytes(firstAlias, firstBytes);
    writeBytes(second, secondBytes);

    LooperProject project;
    require(project.appendLane(0, materializedLane(
                QStringLiteral("first"), QStringLiteral("Same"), first, firstBytes)) &&
            project.appendLane(0, materializedLane(
                QStringLiteral("duplicate"), QStringLiteral("Alias"), firstAlias, firstBytes)) &&
            project.appendLane(0, materializedLane(
                QStringLiteral("second"), QStringLiteral("Same"), second, secondBytes)),
        "could not construct materialization fixture");

    const jam2::gui::LooperAssetMaterializationResult result =
        jam2::gui::materializeLooperAssets(project, sourceRoot, targetRoot);
    require(result.succeeded() && result.createdPaths.size() == 2,
        "valid external assets must materialize exactly once per content hash");
    const QVector<LooperLane>& lanes = result.project.banks().at(0).lanes;
    require(lanes.at(0).assetPath == QStringLiteral("imported/Same-2.wav") &&
            lanes.at(1).assetPath == lanes.at(0).assetPath &&
            lanes.at(2).assetPath == QStringLiteral("imported/Same-3.wav"),
        "portable filename collisions and duplicate hashes must resolve deterministically");
    require(readBytes(occupied) == occupiedBytes &&
            readBytes(QDir(targetRoot).absoluteFilePath(lanes.at(0).assetPath)) == firstBytes &&
            readBytes(QDir(targetRoot).absoluteFilePath(lanes.at(2).assetPath)) == secondBytes,
        "materialization must preserve occupied files and copy exact WAV bytes");

    const jam2::gui::LooperAssetMaterializationResult repeated =
        jam2::gui::materializeLooperAssets(result.project, targetRoot, targetRoot);
    require(repeated.succeeded() && repeated.createdPaths.isEmpty() &&
            repeated.project.toJson() == result.project.toJson(),
        "already-materialized project assets must be an idempotent no-copy operation");

    const QString rollbackRoot = QDir(root).absoluteFilePath(
        QStringLiteral("materialize-rollback"));
    LooperProject failing = project;
    failing.banks()[0].lanes[2].assetHash = QString(64, QLatin1Char('f'));
    const jam2::gui::LooperAssetMaterializationResult failed =
        jam2::gui::materializeLooperAssets(failing, sourceRoot, rollbackRoot);
    require(!failed.succeeded() && failed.createdPaths.isEmpty() &&
            QDir(QDir(rollbackRoot).absoluteFilePath(QStringLiteral("imported")))
                .entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty(),
        "a later invalid asset must roll back every file created by the transaction");

    const QString explicitRollbackRoot = QDir(root).absoluteFilePath(
        QStringLiteral("materialize-explicit-rollback"));
    const jam2::gui::LooperAssetMaterializationResult committedCandidate =
        jam2::gui::materializeLooperAssets(project, sourceRoot, explicitRollbackRoot);
    require(committedCandidate.succeeded() &&
            !committedCandidate.createdPaths.isEmpty() &&
            jam2::gui::rollbackLooperAssetMaterialization(
                committedCandidate.createdPaths).isEmpty(),
        "a caller-abandoned successful candidate must support exact rollback");
    for (const QString& path : committedCandidate.createdPaths) {
        require(!QFileInfo::exists(path),
            "explicit materialization rollback left a created asset behind");
    }

    const jam2::gui::LooperAssetMaterializationResult invalidTarget =
        jam2::gui::materializeLooperAssets(
            project, sourceRoot, QStringLiteral("relative-target"));
    require(!invalidTarget.succeeded() && invalidTarget.createdPaths.isEmpty(),
        "relative materialization roots must reject before filesystem mutation");
}

LooperLane lane(
    const QString& id,
    const QString& hash,
    const QString& path,
    const QString& name)
{
    LooperLane value;
    value.id = id;
    value.assetHash = hash;
    value.assetPath = path;
    value.name = name;
    value.sampleRate = path.isEmpty() ? 0 : 48000;
    value.sourceFrames = path.isEmpty() ? 0 : 48000;
    return value;
}

const LooperLane* findHash(const LooperProject& project, const QString& hash)
{
    const auto& lanes = project.banks().at(0).lanes;
    const auto found = std::find_if(lanes.cbegin(), lanes.cend(),
        [&hash](const LooperLane& candidate) {
            return candidate.assetHash == hash;
        });
    return found == lanes.cend() ? nullptr : &*found;
}

void testSynchronizedLaneMerge()
{
    const QString hashA(64, QLatin1Char('a'));
    const QString hashB(64, QLatin1Char('b'));
    const QString hashC(64, QLatin1Char('c'));
    const QString hashD(64, QLatin1Char('d'));
    const QString hashE(64, QLatin1Char('e'));

    LooperProject local;
    LooperLane sameHash = lane(
        QStringLiteral("local-hash"), hashA, QStringLiteral("C:/local/a.wav"),
        QStringLiteral("Local mix state"));
    sameHash.gainDb = -7.5;
    sameHash.muted = true;
    sameHash.solo = true;
    sameHash.localOnly = true;
    sameHash.originKind = QStringLiteral("recorded");
    LooperLane conflict = lane(
        QStringLiteral("conflict"), hashB, QStringLiteral("C:/local/b.wav"),
        QStringLiteral("Local conflict"));
    conflict.gainDb = -3.0;
    LooperLane removedWav = lane(
        QStringLiteral("removed-wav"), hashC, QStringLiteral("C:/local/c.wav"),
        QStringLiteral("Retained lane"));
    removedWav.gainDb = -9.0;
    removedWav.solo = true;
    const LooperLane unrelated = lane(
        QStringLiteral("omitted"), hashE, QStringLiteral("C:/local/e.wav"),
        QStringLiteral("Authoritatively omitted"));
    require(local.appendLane(0, sameHash) && local.appendLane(0, conflict) &&
            local.appendLane(0, removedWav) && local.appendLane(0, unrelated),
        "could not construct local synchronized lanes");

    LooperProject remote;
    LooperLane remoteHash = lane(
        QStringLiteral("remote-hash"), hashA, QStringLiteral("remote/a.wav"),
        QStringLiteral("Remote arrangement name"));
    remoteHash.gainDb = 0.0;
    LooperLane remoteConflict = lane(
        QStringLiteral("conflict"), hashD, QStringLiteral("remote/d.wav"),
        QStringLiteral("Remote conflict"));
    LooperLane withoutWav = lane(
        QStringLiteral("removed-wav"), {}, {}, QStringLiteral("Retained lane"));
    require(remote.appendLane(0, remoteHash) && remote.appendLane(0, remoteConflict) &&
            remote.appendLane(0, withoutWav),
        "could not construct remote synchronized lanes");

    QJsonObject song{{QStringLiteral("looper"), remote.toJson(true)}};
    require(mergeSynchronizedLooperLanes(song, local) == 1,
        "one same-ID/different-asset conflict must preserve one re-keyed local lane");
    LooperProject merged;
    require(merged.loadJson(song.value(QStringLiteral("looper")).toObject()) &&
            merged.banks().at(0).lanes.size() == 4,
        "synchronized merge must retain remote authority plus the real conflict");

    const LooperLane* matched = findHash(merged, hashA);
    const LooperLane* localConflict = findHash(merged, hashB);
    const LooperLane* remoteConflictResult = findHash(merged, hashD);
    require(matched != nullptr && matched->name == QStringLiteral("Remote arrangement name") &&
            matched->assetPath == sameHash.assetPath && matched->localOnly &&
            matched->muted && matched->solo &&
            std::abs(matched->gainDb - sameHash.gainDb) < 0.000001,
        "same-hash merge must keep remote arrangement data and exact local mix/path state");
    require(localConflict != nullptr && remoteConflictResult != nullptr &&
            localConflict->id != QStringLiteral("conflict") &&
            remoteConflictResult->id == QStringLiteral("conflict") &&
            localConflict->assetPath == conflict.assetPath,
        "same-ID/different-asset merge must keep both lanes under unique stable IDs");
    require(findHash(merged, hashE) == nullptr,
        "authoritatively omitted unrelated lanes must not bounce back");
    const auto removed = std::find_if(
        merged.banks().at(0).lanes.cbegin(), merged.banks().at(0).lanes.cend(),
        [](const LooperLane& candidate) {
            return candidate.id == QStringLiteral("removed-wav");
        });
    require(removed != merged.banks().at(0).lanes.cend() &&
            removed->assetHash.isEmpty() && removed->assetPath.isEmpty() &&
            removed->solo && std::abs(removed->gainDb + 9.0) < 0.000001,
        "authoritative WAV removal must keep lane-local mix without cloning deleted audio");

    const QString conflictId = localConflict->id;
    require(mergeSynchronizedLooperLanes(song, local) == 0,
        "repeating a synchronized merge must not preserve another clone");
    LooperProject repeated;
    require(repeated.loadJson(song.value(QStringLiteral("looper")).toObject()) &&
            repeated.banks().at(0).lanes.size() == 4 &&
            findHash(repeated, hashB) != nullptr &&
            findHash(repeated, hashB)->id == conflictId,
        "repeated synchronized merge must be lane- and ID-idempotent");

    QJsonObject invalid{{QStringLiteral("title"), QStringLiteral("unchanged")},
        {QStringLiteral("looper"), QJsonObject{{QStringLiteral("banks"), QJsonArray{}}}}};
    const QJsonObject original = invalid;
    require(mergeSynchronizedLooperLanes(invalid, local) == 0 && invalid == original &&
            mergeLocalOnlyLooperLanes(invalid, local) == 0 && invalid == original,
        "invalid incoming looper data must reject without mutating its song object");
}

void testLocalOnlyLaneMerge(const QString& root)
{
    const QString hashA(64, QLatin1Char('a'));
    const QString hashB(64, QLatin1Char('b'));
    const QString hashC(64, QLatin1Char('c'));
    const QString hashD(64, QLatin1Char('d'));
    const QString hashE(64, QLatin1Char('e'));
    const QString localFolder = QDir(root).absoluteFilePath(QStringLiteral("local"));

    LooperProject local;
    LooperLane hashed = lane(
        QStringLiteral("hashed-local"), hashA,
        QDir(localFolder).absoluteFilePath(QStringLiteral("hashed.wav")),
        QStringLiteral("Hashed local take"));
    hashed.localOnly = true;
    hashed.originKind = QStringLiteral("recorded");
    LooperLane pathOnly = lane(
        QStringLiteral("path-only"), {},
        QDir(localFolder).absoluteFilePath(QStringLiteral("path-only.wav")),
        QStringLiteral("Path-only local take"));
    pathOnly.localOnly = true;
    pathOnly.originKind = QStringLiteral("recorded");
    LooperLane reference = lane(
        QStringLiteral("reference"), hashB,
        QDir(localFolder).absoluteFilePath(QStringLiteral("reference.wav")),
        QStringLiteral("Reference"));
    reference.localOnly = true;
    reference.referenceKind = QStringLiteral("chords");
    LooperLane legacyReference = lane(
        QStringLiteral("legacy-reference"), hashC,
        QDir(localFolder).absoluteFilePath(QStringLiteral("legacy.wav")),
        QStringLiteral("Practice Drums"));
    legacyReference.localOnly = true;
    LooperLane nonLocal = lane(
        QStringLiteral("non-local"), hashE,
        QDir(localFolder).absoluteFilePath(QStringLiteral("non-local.wav")),
        QStringLiteral("Shared lane"));
    LooperLane noPath = lane(
        QStringLiteral("no-path"), {}, {}, QStringLiteral("No path"));
    noPath.localOnly = true;
    require(local.appendLane(0, hashed) && local.appendLane(0, pathOnly) &&
            local.appendLane(0, reference) && local.appendLane(0, legacyReference) &&
            local.appendLane(0, nonLocal) && local.appendLane(0, noPath),
        "could not construct local-only lane fixtures");

    LooperProject remote;
    LooperLane collision = lane(
        QStringLiteral("path-only"), hashD, QStringLiteral("remote/collision.wav"),
        QStringLiteral("Remote collision"));
    require(remote.appendLane(0, collision), "could not construct remote collision");
    QJsonObject song{{QStringLiteral("looper"), remote.toJson()}};

    require(mergeLocalOnlyLooperLanes(song, local) == 2,
        "only the hashed and path-only local takes must be preserved");
    LooperProject merged;
    require(merged.loadJson(song.value(QStringLiteral("looper")).toObject()) &&
            merged.banks().at(0).lanes.size() == 3 &&
            findHash(merged, hashA) != nullptr && findHash(merged, hashB) == nullptr &&
            findHash(merged, hashC) == nullptr && findHash(merged, hashE) == nullptr,
        "local-only merge must exclude managed references and non-local lanes");
    const auto pathMatches = std::count_if(
        merged.banks().at(0).lanes.cbegin(), merged.banks().at(0).lanes.cend(),
        [&pathOnly](const LooperLane& candidate) {
            return candidate.assetPath == pathOnly.assetPath;
        });
    const auto preservedPath = std::find_if(
        merged.banks().at(0).lanes.cbegin(), merged.banks().at(0).lanes.cend(),
        [&pathOnly](const LooperLane& candidate) {
            return candidate.assetPath == pathOnly.assetPath;
        });
    require(pathMatches == 1 && preservedPath != merged.banks().at(0).lanes.cend() &&
            preservedPath->id != pathOnly.id && preservedPath->localOnly,
        "a path-only local lane must survive an ID collision exactly once under a new ID");
    const QString preservedPathId = preservedPath->id;

    require(mergeLocalOnlyLooperLanes(song, local) == 0,
        "repeating a local-only merge must not append hashless WAV lanes again");
    LooperProject repeated;
    require(repeated.loadJson(song.value(QStringLiteral("looper")).toObject()) &&
            repeated.banks().at(0).lanes.size() == 3,
        "repeated local-only merge must preserve the exact lane count");
    const auto repeatedPath = std::find_if(
        repeated.banks().at(0).lanes.cbegin(), repeated.banks().at(0).lanes.cend(),
        [&pathOnly](const LooperLane& candidate) {
            return candidate.assetPath == pathOnly.assetPath;
        });
    require(repeatedPath != repeated.banks().at(0).lanes.cend() &&
            repeatedPath->id == preservedPathId,
        "repeated path-only preservation must retain the first generated lane identity");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
        QTemporaryDir root = makeRoot();
        testMetadataAndRejection(root.path());
        testTrackSidecarJson(root.path());
        testExactStagingAndRepair(root.path());
        testResampledStagingAndRepair(root.path());
        testStagingBoundaryValidation(root.path());
        testTransactionalAssetMaterialization(root.path());
        testSynchronizedLaneMerge();
        testLocalOnlyLaneMerge(root.path());
        std::cout << "Track workspace support tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Track workspace support tests failed: " << error.what() << '\n';
        return 1;
    }
}

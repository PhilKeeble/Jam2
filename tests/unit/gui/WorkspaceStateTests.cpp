#include "GuiPresentation.hpp"
#include "JamStorage.hpp"
#include "LooperProject.hpp"
#include "SharedTrackController.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QMap>
#include <QTemporaryDir>

#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

QString canonicalPath(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
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

QTemporaryDir makeRoot()
{
    const QString artifactRoot = qEnvironmentVariable("JAM2_TEST_ARTIFACT_ROOT");
    require(!artifactRoot.isEmpty(), "CTest omitted the build-local artifact root");
    QTemporaryDir root(QDir(artifactRoot).absoluteFilePath(
        QStringLiteral("workspace-state-XXXXXX")));
    require(root.isValid(), "could not create build-local workspace fixture");
    require(canonicalPath(root.path()).startsWith(
                canonicalPath(artifactRoot) + QLatin1Char('/'),
                Qt::CaseInsensitive),
        "workspace fixture escaped build/test-artifacts");
    return root;
}

void testStorageLifecycle(const QString& releaseRoot)
{
    JamStorage storage;
    storage.startNew(QStringLiteral("  Alpha Jam  "));
    const QString alphaRoot = QDir(releaseRoot).absoluteFilePath(
        QStringLiteral("tracks/Alpha_Jam"));
    require(storage.displayName() == QStringLiteral("Alpha Jam") &&
            canonicalPath(storage.rootFolder()) == canonicalPath(alphaRoot) &&
            !storage.isSaved() && !storage.hasArtifacts() &&
            storage.projectFilePath().isEmpty(),
        "new jam must own a normalized unsaved workspace without a project file");

    const QMap<JamStorage::AssetKind, QString> assetFolders{
        {JamStorage::AssetKind::Generated, QStringLiteral("generated")},
        {JamStorage::AssetKind::Received, QStringLiteral("received")},
        {JamStorage::AssetKind::Imported, QStringLiteral("imported")},
        {JamStorage::AssetKind::Recorded, QStringLiteral("recorded")},
        {JamStorage::AssetKind::Prepared, QStringLiteral("prepared")},
        {JamStorage::AssetKind::JamRecordings, QStringLiteral("jam_recordings")},
    };
    for (auto it = assetFolders.cbegin(); it != assetFolders.cend(); ++it) {
        require(canonicalPath(storage.assetFolder(it.key())) ==
                canonicalPath(QDir(alphaRoot).absoluteFilePath(it.value())),
            "every storage asset kind must map below the jam root");
    }

    const QString retained = QDir(alphaRoot).absoluteFilePath(
        QStringLiteral("recorded/retained.wav"));
    writeBytes(retained, QByteArrayLiteral("retained"));
    storage.markArtifactCreated();
    require(storage.hasArtifacts(), "artifact creation must be observable");
    storage.clearArtifactState();
    require(!storage.hasArtifacts(), "artifact state must clear explicitly");

    QString error;
    require(!storage.rename(QStringLiteral("   "), error) &&
            error.contains(QStringLiteral("cannot be empty")),
        "blank jam names must reject without mutation");
    error.clear();
    require(storage.rename(QStringLiteral("Beta Jam"), error),
        "unsaved jam rename must succeed");
    const QString betaTracks = QDir(releaseRoot).absoluteFilePath(
        QStringLiteral("tracks/Beta_Jam"));
    require(canonicalPath(storage.rootFolder()) == canonicalPath(betaTracks) &&
            QFileInfo::exists(QDir(betaTracks).absoluteFilePath(
                QStringLiteral("recorded/retained.wav"))) &&
            !QFileInfo::exists(alphaRoot),
        "unsaved rename must move exactly the owned workspace and retain its bytes");

    require(storage.moveToSongs(error) && storage.isSaved() &&
            !storage.hasArtifacts(),
        "first save must move the workspace into songs and clear artifact state");
    const QString betaSongs = QDir(releaseRoot).absoluteFilePath(
        QStringLiteral("songs/Beta_Jam"));
    const QString betaProject = QDir(betaSongs).absoluteFilePath(
        QStringLiteral("Beta_Jam.jamjar"));
    require(canonicalPath(storage.rootFolder()) == canonicalPath(betaSongs) &&
            canonicalPath(storage.projectFilePath()) == canonicalPath(betaProject) &&
            !QFileInfo::exists(betaTracks),
        "first save must publish its exact canonical project path");
    writeBytes(betaProject, QByteArrayLiteral("project"));

    error.clear();
    require(storage.rename(QStringLiteral("Gamma Jam"), error),
        "saved managed jam rename must succeed");
    const QString gammaRoot = QDir(releaseRoot).absoluteFilePath(
        QStringLiteral("songs/Gamma_Jam"));
    const QString gammaProject = QDir(gammaRoot).absoluteFilePath(
        QStringLiteral("Gamma_Jam.jamjar"));
    require(canonicalPath(storage.rootFolder()) == canonicalPath(gammaRoot) &&
            canonicalPath(storage.projectFilePath()) == canonicalPath(gammaProject) &&
            QFileInfo::exists(gammaProject) && !QFileInfo::exists(betaProject) &&
            QFileInfo::exists(QDir(gammaRoot).absoluteFilePath(
                QStringLiteral("recorded/retained.wav"))),
        "managed rename must move the folder and its JamJar filename together");

    const QString collisionRoot = QDir(releaseRoot).absoluteFilePath(
        QStringLiteral("songs/Collision"));
    require(QDir().mkpath(collisionRoot), "could not create collision fixture");
    error.clear();
    require(!storage.rename(QStringLiteral("Collision"), error) &&
            canonicalPath(storage.rootFolder()) == canonicalPath(gammaRoot) &&
            canonicalPath(storage.projectFilePath()) == canonicalPath(gammaProject),
        "managed rename collision must reject without changing owned paths");
    require(storage.moveToSongs(error), "saving an already-saved jam must be idempotent");
    require(storage.discardUnsaved(error) && QFileInfo::exists(gammaRoot),
        "discard must never delete a saved jam");
}

void testExternalOpenSafety(const QString& releaseRoot, const QString& fixtureRoot)
{
    const QString externalFolder = QDir(fixtureRoot).absoluteFilePath(
        QStringLiteral("external-parent"));
    const QString openedPath = QDir(externalFolder).absoluteFilePath(
        QStringLiteral("different-file-name.jamjar"));
    const QString siblingPath = QDir(externalFolder).absoluteFilePath(
        QStringLiteral("unrelated-user-file.txt"));
    writeBytes(openedPath, QByteArrayLiteral("opened"));
    writeBytes(siblingPath, QByteArrayLiteral("sibling"));

    JamStorage storage;
    storage.openSaved(openedPath, QStringLiteral("Internal Project Title"));
    require(storage.isSaved() &&
            canonicalPath(storage.rootFolder()) == canonicalPath(externalFolder) &&
            canonicalPath(storage.projectFilePath()) == canonicalPath(openedPath),
        "opening a JamJar must retain its exact selected file and adjacent asset root");

    QString error;
    require(storage.rename(QStringLiteral("Changed Display Title"), error) &&
            storage.displayName() == QStringLiteral("Changed Display Title") &&
            canonicalPath(storage.rootFolder()) == canonicalPath(externalFolder) &&
            canonicalPath(storage.projectFilePath()) == canonicalPath(openedPath) &&
            QFileInfo::exists(openedPath) && QFileInfo::exists(siblingPath),
        "renaming an external project must not move its parent or change its selected file");
    require(!QFileInfo::exists(QDir(releaseRoot).absoluteFilePath(
                QStringLiteral("songs/Changed_Display_Title"))),
        "external rename must not silently import or duplicate a project");
}

void testTakeNamesDiscardAndPruning(const QString& releaseRoot)
{
    JamStorage takes;
    takes.startNew(QStringLiteral("Take Fixture"));
    const QString recordings = takes.assetFolder(JamStorage::AssetKind::JamRecordings);
    require(QDir().mkpath(QDir(recordings).absoluteFilePath(QStringLiteral("Take-1"))) &&
            takes.nextTakeName() == QStringLiteral("Take-2"),
        "automatic take numbering must skip existing folders");
    require(QDir().mkpath(QDir(recordings).absoluteFilePath(QStringLiteral("Named_Take"))) &&
            takes.uniqueTakeFolder(QStringLiteral(" Named Take ")).endsWith(
                QStringLiteral("Named_Take-2")),
        "requested take names must be portable and collision-free");

    const QString disposable = QDir(takes.rootFolder()).absoluteFilePath(
        QStringLiteral("recorded/take.wav"));
    writeBytes(disposable, QByteArrayLiteral("take"));
    takes.markArtifactCreated();
    QString error;
    const QString disposableRoot = takes.rootFolder();
    require(takes.discardUnsaved(error) && !QFileInfo::exists(disposableRoot) &&
            !takes.hasArtifacts(),
        "discard must remove only the owned unsaved workspace and clear artifacts");

    const QString tracks = QDir(releaseRoot).absoluteFilePath(QStringLiteral("tracks"));
    require(QDir().mkpath(QDir(tracks).absoluteFilePath(
                QStringLiteral("empty-one/nested"))) &&
            QDir().mkpath(QDir(tracks).absoluteFilePath(
                QStringLiteral("empty-two"))),
        "could not create empty workspace fixtures");
    const QString kept = QDir(tracks).absoluteFilePath(
        QStringLiteral("kept/deep/artifact.bin"));
    writeBytes(kept, QByteArrayLiteral("keep"));
    require(JamStorage::pruneEmptyUnsavedWorkspaces() == 2 &&
            QFileInfo::exists(kept) &&
            !QFileInfo::exists(QDir(tracks).absoluteFilePath(
                QStringLiteral("empty-one"))) &&
            !QFileInfo::exists(QDir(tracks).absoluteFilePath(
                QStringLiteral("empty-two"))),
        "workspace pruning must remove recursive empties and retain nested artifacts");

    require(JamStorage::portableSlug(QStringLiteral(" CON ")) ==
                QStringLiteral("_CON") &&
            JamStorage::portableSlug(QStringLiteral(" ../bad:* name. ")) ==
                QStringLiteral("bad_name") &&
            JamStorage::portableSlug(QString(200, QLatin1Char('a'))).size() == 120,
        "portable jam slugs must reject reserved names, unsafe characters, and excess length");
    const QString randomName = JamStorage::randomDisplayName();
    require(randomName.split(QLatin1Char(' '), Qt::SkipEmptyParts).size() == 2,
        "random jam display names must preserve the two-word contract");
}

void testSharedTrackState()
{
    using Phase = SharedTrackController::PlaybackPhase;
    SharedTrackController controller;
    const SharedTrackController& constController = controller;
    require(&controller.model() == &constController.model() &&
            controller.playbackStatusText(true) == QStringLiteral("Shared: stopped") &&
            controller.playbackStatusText(false) == QStringLiteral("Independent: stopped"),
        "shared-track controller must expose one model and explicit stopped scopes");

    controller.waitForAssets(7, true);
    require(controller.playback().arrangementRevision == 7 &&
            controller.playbackStatusText(true) ==
                QStringLiteral("Shared: waiting for assets"),
        "asset wait must retain requested playback and arrangement revision");
    controller.preparedForTransport(8);
    require(controller.playback().phase == Phase::WaitingForAssets,
        "stale preparation completion must not advance a newer arrangement");
    controller.prepareMix(7, true);
    require(controller.playbackStatusText(false) ==
                QStringLiteral("Independent: preparing"),
        "mix preparation must have a distinct visible phase");
    controller.preparedForTransport(7);
    require(controller.playbackStatusText(true) ==
                QStringLiteral("Shared: waiting to play"),
        "matching preparation must advance to transport wait");
    require(controller.observeEnginePlaying(true) &&
            controller.playback().phase == Phase::Playing &&
            controller.playbackStatusText(true) == QStringLiteral("Shared: playing") &&
            !controller.observeEnginePlaying(true),
        "engine observation must enter playing once without false repeated changes");

    controller.requestPlayback(false, 9);
    require(controller.playback().arrangementRevision == 9 &&
            controller.playbackStatusText(true) ==
                QStringLiteral("Shared: waiting to stop"),
        "stop request must retain actual playback until the engine confirms it");
    require(controller.observeEnginePlaying(false) &&
            controller.playback().phase == Phase::Stopped,
        "engine stop confirmation must converge requested and actual state");

    controller.requestPlayback(true);
    require(controller.playback().arrangementRevision == 9 &&
            controller.playback().phase == Phase::WaitingForTransport,
        "revision-free playback request must retain the owned arrangement revision");
    controller.waitForAssets(10, false);
    controller.preparedForTransport(10);
    require(controller.playback().phase == Phase::Stopped,
        "asset completion for a stopped inactive engine must settle immediately");

    controller.model().sampleRateCompatible = false;
    require(controller.playbackStatusText(true).contains(
                QStringLiteral("WAV conversion failed")),
        "incompatible audio must override the ordinary playback phase status");
}

void testSharedTrackLoopAndProjectState(const QString& fixtureRoot)
{
    SharedTrackController controller;
    require(!controller.setLoopStartAtMilliseconds(100) &&
            !controller.setLoopEndAtMilliseconds(100),
        "loop points must not be created without a positive track duration");

    controller.model().durationMs = 10000;
    require(controller.setLoopEndAtMilliseconds(1000) &&
            controller.model().loopEnabled &&
            controller.model().loopStartSeconds == -1.0 &&
            controller.model().loopEndSeconds == 1.0,
        "loop end must accept a bounded open-start region");
    require(controller.setLoopStartAtMilliseconds(2000) &&
            controller.model().loopStartSeconds == 2.0 &&
            controller.model().loopEndSeconds == -1.0,
        "a later loop start must clear an older conflicting end");
    require(controller.setLoopEndAtMilliseconds(3000) &&
            controller.model().loopStartSeconds == 2.0 &&
            controller.model().loopEndSeconds == 3.0,
        "ordered loop points must form a finite region");
    require(controller.setLoopEndAtMilliseconds(1000) &&
            controller.model().loopStartSeconds == -1.0 &&
            controller.model().loopEndSeconds == 1.0,
        "an earlier loop end must clear an older conflicting start");
    require(controller.setLoopStartAtMilliseconds(10000) &&
            controller.model().loopStartSeconds == 9.999 &&
            controller.model().loopEndSeconds == -1.0,
        "loop start at end-of-track must remain inside the last millisecond");

    controller.model().loopStartSeconds = 2.0;
    controller.model().loopEndSeconds = 3.0;
    const SharedTrackController::EffectiveLoop finite =
        controller.effectiveLoop(48000, 480000);
    require(finite.enabled && finite.startFrame == 96000 &&
            finite.endFrame == 144000,
        "effective loop conversion must preserve exact bounded frame positions");
    controller.model().loopStartSeconds =
        std::numeric_limits<double>::quiet_NaN();
    controller.model().loopEndSeconds =
        std::numeric_limits<double>::infinity();
    const SharedTrackController::EffectiveLoop nonFinite =
        controller.effectiveLoop(48000, 480000);
    require(nonFinite.enabled && nonFinite.startFrame == 0 &&
            nonFinite.endFrame == 480000,
        "effective loop conversion must safely recover non-finite mutable state");
    require(!controller.effectiveLoop(0, 480000).enabled &&
            !controller.effectiveLoop(48000, 0).enabled,
        "effective loop conversion must reject missing sample or frame domains");
    controller.clearLoop();
    require(!controller.model().loopEnabled &&
            controller.model().loopStartSeconds == -1.0 &&
            controller.model().loopEndSeconds == -1.0 &&
            !controller.effectiveLoop(48000, 480000).enabled,
        "clear loop must atomically disable and remove both loop points");
    controller.setLoopEnabled(true);
    require(controller.effectiveLoop(48000, 480000).enabled &&
            controller.effectiveLoop(48000, 480000).startFrame == 0 &&
            controller.effectiveLoop(48000, 480000).endFrame == 480000,
        "enabled unset loop points must represent the whole prepared track");
    controller.model().loopStartSeconds = 1.0;
    controller.model().loopEndSeconds = 2.0;
    controller.setWholeTrackLoop();
    require(controller.model().loopEnabled &&
            controller.model().loopStartSeconds == -1.0 &&
            controller.model().loopEndSeconds == -1.0,
        "whole-track reset must atomically remove an older finite loop region");

    SharedTrackModel projectModel;
    projectModel.fileName = QStringLiteral("Reference.wav");
    projectModel.filePath = QStringLiteral("assets/reference.wav");
    projectModel.fileBytes = 4096;
    projectModel.sampleRate = 48000;
    projectModel.durationMs = 10000;
    projectModel.sha256 = QString(64, QLatin1Char('A'));
    projectModel.guessedBpm = 123.5;
    projectModel.acceptedBpm = 124.0;
    projectModel.key = QStringLiteral("D minor");
    projectModel.speed = 1.25;
    projectModel.pitchCents = -300;
    projectModel.trackGainDb = -8.5;
    projectModel.loopEnabled = true;
    projectModel.loopStartSeconds = 2.0;
    projectModel.loopEndSeconds = 8.0;
    projectModel.focusEnabled = true;
    projectModel.focusPreset = QStringLiteral("bass");
    projectModel.focusFrequencyHz = 180.0;
    projectModel.focusGainDb = 6.0;
    projectModel.focusQ = 4.0;
    projectModel.highpassHz = 50.0;
    projectModel.lowpassHz = 500.0;
    controller.replaceModel(projectModel);
    const QJsonObject encoded = controller.projectJson();
    const SharedTrackController::ProjectDecodeResult decoded =
        SharedTrackController::decodeProjectJson(encoded, fixtureRoot, false);
    require(decoded.valid && !decoded.normalized && decoded.error.isEmpty() &&
            decoded.model.fileName == projectModel.fileName &&
            decoded.model.filePath == QDir(fixtureRoot).absoluteFilePath(
                QStringLiteral("assets/reference.wav")) &&
            decoded.model.fileBytes == projectModel.fileBytes &&
            decoded.model.sampleRate == projectModel.sampleRate &&
            decoded.model.durationMs == projectModel.durationMs &&
            decoded.model.sha256 == projectModel.sha256.toLower() &&
            decoded.model.acceptedBpm == projectModel.acceptedBpm &&
            decoded.model.speed == projectModel.speed &&
            decoded.model.pitchCents == projectModel.pitchCents &&
            decoded.model.loopStartSeconds == projectModel.loopStartSeconds &&
            decoded.model.loopEndSeconds == projectModel.loopEndSeconds &&
            decoded.model.userProvidedSource &&
            decoded.model.sampleRateCompatible && !decoded.model.syncControls,
        "track project state must round-trip through one validated relative-path owner");

    const SharedTrackController::ProjectDecodeResult missing =
        SharedTrackController::decodeProjectJson(
            QJsonValue(QJsonValue::Undefined), fixtureRoot, true);
    require(missing.valid && !missing.normalized &&
            missing.model.filePath.isEmpty() && missing.model.fileBytes == 0 &&
            missing.model.durationMs == 0 && missing.model.syncControls,
        "missing project track state must produce a clean model, not retain another project");

    QJsonObject normalized = encoded;
    normalized.insert(QStringLiteral("duration_ms"), 1000);
    normalized.insert(QStringLiteral("loop_start_seconds"), 0.9);
    normalized.insert(QStringLiteral("loop_end_seconds"), 0.5);
    const SharedTrackController::ProjectDecodeResult repaired =
        SharedTrackController::decodeProjectJson(normalized, fixtureRoot, true);
    require(repaired.valid && repaired.normalized &&
            repaired.model.loopStartSeconds == -1.0 &&
            repaired.model.loopEndSeconds == -1.0,
        "persisted reversed loop bounds must normalize to a safe whole-track loop");

    QJsonObject invalid = encoded;
    invalid.insert(QStringLiteral("speed"),
        std::numeric_limits<double>::infinity());
    require(!SharedTrackController::decodeProjectJson(
                invalid, fixtureRoot, true).valid,
        "non-finite persisted processing state must be rejected");
    invalid = encoded;
    invalid.insert(QStringLiteral("sample_rate"), 1);
    require(!SharedTrackController::decodeProjectJson(
                invalid, fixtureRoot, true).valid,
        "unsupported persisted sample rates must be rejected");
    invalid = encoded;
    invalid.insert(QStringLiteral("sha256"), QStringLiteral("not-a-digest"));
    require(!SharedTrackController::decodeProjectJson(
                invalid, fixtureRoot, true).valid,
        "malformed persisted track content identity must be rejected");
    require(!SharedTrackController::decodeProjectJson(
                QJsonValue(QStringLiteral("track")), fixtureRoot, true).valid,
        "non-object persisted track state must be rejected");
    require(controller.model().fileName == projectModel.fileName &&
            controller.model().filePath == projectModel.filePath,
        "failed project decoding must not mutate the live shared-track model");
}

void testLooperRename()
{
    LooperProject project;
    project.ensureInitialEmptyLanes();
    const QString original = project.banks().at(0).lanes.at(0).name;
    require(!project.renameLane(-1, 0, QStringLiteral("bad")) &&
            !project.renameLane(0, -1, QStringLiteral("bad")) &&
            !project.renameLane(0, 0, QStringLiteral("   ")) &&
            project.banks().at(0).lanes.at(0).name == original,
        "looper lane rename must reject invalid identity and blank names without mutation");
    require(project.renameLane(0, 0, QStringLiteral("  Rhythm take  ")) &&
            project.banks().at(0).lanes.at(0).name == QStringLiteral("Rhythm take"),
        "looper lane rename must trim and apply a valid name");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        QCoreApplication application(argc, argv);
        QTemporaryDir root = makeRoot();
        const QString releaseRoot = QDir(root.path()).absoluteFilePath(
            QStringLiteral("release-root"));
        QString error;
        require(setAppReleaseRootForTesting(releaseRoot, error),
            "could not install isolated release root: " + error.toStdString());
        testStorageLifecycle(releaseRoot);
        testExternalOpenSafety(releaseRoot, root.path());
        testTakeNamesDiscardAndPruning(releaseRoot);
        testSharedTrackState();
        testSharedTrackLoopAndProjectState(root.path());
        testLooperRename();
        std::cout << "Workspace state tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Workspace state test failed: " << error.what() << '\n';
        return 1;
    }
}

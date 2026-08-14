#include "ProjectPersistenceCoordinator.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QThreadPool>

#include <iostream>
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
        "could not write test file");
    file.close();
}

QByteArray readBytes(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "could not read test file");
    return file.readAll();
}

QTemporaryDir makeRoot(const QString& name)
{
    const QString artifactRoot = qEnvironmentVariable("JAM2_TEST_ARTIFACT_ROOT");
    require(!artifactRoot.isEmpty(), "CTest omitted the build-local artifact root");
    QTemporaryDir root(QDir(artifactRoot).absoluteFilePath(name + QStringLiteral("-XXXXXX")));
    require(root.isValid(), "could not create build-local persistence fixture");
    require(canonicalPath(root.path()).startsWith(
                canonicalPath(artifactRoot) + QLatin1Char('/'), Qt::CaseInsensitive),
        "persistence fixture escaped build/test-artifacts");
    return root;
}

void testWorkspaceInitializationAndState()
{
    QTemporaryDir root = makeRoot(QStringLiteral("project-state"));
    const QString workspace = QDir(root.path()).absoluteFilePath(QStringLiteral("workspace"));
    const QString received = QDir(workspace).absoluteFilePath(QStringLiteral("received"));
    const QString abandoned = QDir(received).absoluteFilePath(
        QString(64, QLatin1Char('a')) + QStringLiteral(
            ".wav.partial.12345678-1234-1234-1234-123456789abc"));
    const QString unrelated = QDir(received).absoluteFilePath(
        QStringLiteral("ordinary.wav.partial.not-a-transfer-id"));
    writeBytes(abandoned, QByteArrayLiteral("partial"));
    writeBytes(unrelated, QByteArrayLiteral("keep"));

    ProjectPersistenceCoordinator coordinator;
    coordinator.initializeWorkspace(workspace);
    require(coordinator.workspaceFolder() == canonicalPath(workspace),
        "workspace initialization must publish its absolute root");
    require(!QFileInfo::exists(abandoned) && QFileInfo::exists(unrelated),
        "startup cleanup must remove only exact abandoned transfer-partial names");
    require(coordinator.projectFolder().isEmpty(),
        "initial workspace must not imply an opened project folder");

    coordinator.useWorkspaceAsProjectFolderIfUnset();
    require(coordinator.projectFolder() == canonicalPath(workspace),
        "explicit fallback must adopt the workspace as project folder");
    const QString otherFolder = QDir(root.path()).absoluteFilePath(
        QStringLiteral("projects/../projects/current"));
    coordinator.setProjectFolder(otherFolder);
    require(coordinator.projectFolder() == canonicalPath(otherFolder),
        "project folder setter must normalize an explicit folder");
    coordinator.setProjectFolder(QStringLiteral("   "));
    require(coordinator.projectFolder().isEmpty(),
        "blank project folder input must clear project ownership");

    const QString openedPath = QDir(root.path()).absoluteFilePath(
        QStringLiteral("opened/session.jamjar"));
    coordinator.setProjectLocation(openedPath);
    require(coordinator.projectFolder() == canonicalPath(QFileInfo(openedPath).absolutePath()),
        "project location must publish its containing folder");
    coordinator.acceptNewProject(QByteArrayLiteral("new-snapshot"));
    require(coordinator.projectFolder().isEmpty() &&
            !coordinator.hasUnsavedChanges(QByteArrayLiteral("new-snapshot")) &&
            coordinator.hasUnsavedChanges(QByteArrayLiteral("changed")),
        "new-project acceptance must clear location and own the exact saved snapshot");
    coordinator.acceptOpenedProject(openedPath, QByteArrayLiteral("opened-snapshot"));
    require(coordinator.projectFolder() == canonicalPath(QFileInfo(openedPath).absolutePath()) &&
            !coordinator.hasUnsavedChanges(QByteArrayLiteral("opened-snapshot")),
        "opened-project acceptance must own its folder and exact saved snapshot");
}

void testTransientOwnershipAndRelocation()
{
    QTemporaryDir root = makeRoot(QStringLiteral("project-relocation"));
    const QString oldWorkspace = QDir(root.path()).absoluteFilePath(QStringLiteral("old"));
    const QString persistent = QDir(oldWorkspace).absoluteFilePath(
        QStringLiteral("imported/persistent.wav"));
    const QString deferred = QDir(oldWorkspace).absoluteFilePath(
        QStringLiteral("recorded/deferred.wav"));
    const QString live = QDir(oldWorkspace).absoluteFilePath(
        QStringLiteral("generated/live.wav"));
    const QString external = QDir(root.path()).absoluteFilePath(
        QStringLiteral("external/outside.wav"));
    writeBytes(persistent, QByteArrayLiteral("persistent"));
    writeBytes(deferred, QByteArrayLiteral("deferred"));
    writeBytes(live, QByteArrayLiteral("live"));
    writeBytes(external, QByteArrayLiteral("external"));

    ProjectPersistenceCoordinator coordinator;
    coordinator.initializeWorkspace(oldWorkspace);
    coordinator.registerTransientWav(QStringLiteral("   "));
    coordinator.registerTransientWav(persistent);
    coordinator.registerTransientWav(deferred);
    coordinator.acceptSavedProject(
        QDir(oldWorkspace).absoluteFilePath(QStringLiteral("song.jamjar")),
        QByteArrayLiteral("saved"),
        QSet<QString>{QDir(QFileInfo(persistent).absolutePath()).absoluteFilePath(
            QStringLiteral("./persistent.wav"))});
    require(!coordinator.ownsTransientWav(persistent) &&
            coordinator.ownsTransientWav(deferred) &&
            !coordinator.hasExistingTransientWavs(),
        "saving must release persistent assets and defer only obsolete transient assets");
    coordinator.registerTransientWav(live);
    coordinator.registerTransientWav(external);
    require(coordinator.hasExistingTransientWavs(),
        "an existing active transient WAV must be visible to unsaved-change checks");

    const QString newWorkspace = QDir(root.path()).absoluteFilePath(QStringLiteral("renamed"));
    require(QDir(root.path()).rename(QStringLiteral("old"), QStringLiteral("renamed")),
        "could not rename relocation fixture");
    coordinator.relocateWorkspace(newWorkspace);
    const QString relocatedDeferred = QDir(newWorkspace).absoluteFilePath(
        QStringLiteral("recorded/deferred.wav"));
    const QString relocatedLive = QDir(newWorkspace).absoluteFilePath(
        QStringLiteral("generated/live.wav"));
    require(coordinator.workspaceFolder() == canonicalPath(newWorkspace) &&
            coordinator.projectFolder() == canonicalPath(newWorkspace) &&
            coordinator.ownsTransientWav(relocatedDeferred) &&
            coordinator.ownsTransientWav(relocatedLive) &&
            !coordinator.ownsTransientWav(deferred) &&
            !coordinator.ownsTransientWav(live) &&
            coordinator.ownsTransientWav(external),
        "workspace relocation must retarget only workspace-owned active/deferred WAVs");

    coordinator.clearTransientTracking();
    require(!coordinator.ownsTransientWav(relocatedDeferred) &&
            !coordinator.ownsTransientWav(relocatedLive) &&
            !coordinator.ownsTransientWav(external) &&
            QFileInfo::exists(relocatedDeferred) && QFileInfo::exists(relocatedLive) &&
            QFileInfo::exists(external),
        "clearing ownership must never delete tracked bytes");
}

void testDiscardAndScheduledCleanup()
{
    QTemporaryDir root = makeRoot(QStringLiteral("project-cleanup"));
    const QString workspace = QDir(root.path()).absoluteFilePath(QStringLiteral("discard"));
    const QString wav = QDir(workspace).absoluteFilePath(QStringLiteral("recorded/take.WAV"));
    const QString unowned = QDir(workspace).absoluteFilePath(QStringLiteral("imported/user.wav"));
    const QString unsafe = QDir(workspace).absoluteFilePath(QStringLiteral("generated/not-a-wav.txt"));
    const QString wavDirectory = QDir(workspace).absoluteFilePath(QStringLiteral("received/folder.wav"));
    writeBytes(wav, QByteArrayLiteral("wav"));
    writeBytes(unowned, QByteArrayLiteral("user"));
    writeBytes(unsafe, QByteArrayLiteral("text"));
    require(QDir().mkpath(wavDirectory), "could not create directory-shaped WAV fixture");

    ProjectPersistenceCoordinator coordinator;
    coordinator.initializeWorkspace(workspace);
    require(!coordinator.discardTransientWav(unowned) && QFileInfo::exists(unowned),
        "discard must reject and preserve an unowned WAV");
    coordinator.registerTransientWav(wav);
    require(coordinator.ownsTransientWav(
                QDir(QFileInfo(wav).absolutePath()).absoluteFilePath(QStringLiteral("./take.WAV"))) &&
            coordinator.discardTransientWav(wav) && !QFileInfo::exists(wav) &&
            !coordinator.ownsTransientWav(wav),
        "discard must canonicalize, remove, and release an owned WAV");
    const QString missing = QDir(workspace).absoluteFilePath(
        QStringLiteral("prepared/already-gone.wav"));
    coordinator.registerTransientWav(missing);
    require(coordinator.discardTransientWav(missing) && !coordinator.ownsTransientWav(missing),
        "discard must release an owned WAV that is already absent");
    coordinator.registerTransientWav(unsafe);
    coordinator.registerTransientWav(wavDirectory);
    require(!coordinator.discardTransientWav(unsafe) &&
            !coordinator.discardTransientWav(wavDirectory) &&
            QFileInfo::exists(unsafe) && QFileInfo(wavDirectory).isDir(),
        "discard must retain ownership and bytes for non-WAV or non-file targets");
    coordinator.clearTransientTracking();

    const QString cleanupWorkspace = QDir(root.path()).absoluteFilePath(
        QStringLiteral("scheduled"));
    const QString deferred = QDir(cleanupWorkspace).absoluteFilePath(
        QStringLiteral("received/deferred.wav"));
    const QString active = QDir(cleanupWorkspace).absoluteFilePath(
        QStringLiteral("recorded/active.wav"));
    const QString retained = QDir(cleanupWorkspace).absoluteFilePath(
        QStringLiteral("imported/retained.txt"));
    writeBytes(deferred, QByteArrayLiteral("deferred"));
    writeBytes(active, QByteArrayLiteral("active"));
    writeBytes(retained, QByteArrayLiteral("retained"));
    ProjectPersistenceCoordinator cleanup;
    cleanup.initializeWorkspace(cleanupWorkspace);
    cleanup.registerTransientWav(deferred);
    cleanup.acceptSavedProject(
        QDir(cleanupWorkspace).absoluteFilePath(QStringLiteral("saved.jamjar")),
        QByteArrayLiteral("saved"), {});
    cleanup.registerTransientWav(active);
    cleanup.registerTransientWav(retained);
    QThreadPool pool;
    pool.setMaxThreadCount(1);
    cleanup.scheduleTransientCleanup(pool);
    require(pool.waitForDone(5000), "scheduled transient cleanup did not finish");
    require(!QFileInfo::exists(deferred) && !QFileInfo::exists(active) &&
            QFileInfo::exists(retained) && !cleanup.ownsTransientWav(deferred) &&
            !cleanup.ownsTransientWav(active) && !cleanup.ownsTransientWav(retained),
        "scheduled cleanup must consume active/deferred ownership and delete only WAV files");
    require(QFile::remove(retained), "could not remove retained safety fixture");
    cleanup.pruneEmptyWorkspaceDirectories();
    require(!QFileInfo::exists(cleanupWorkspace),
        "explicit pruning must remove empty managed folders and workspace root");
}

void testJsonBoundaries()
{
    QTemporaryDir root = makeRoot(QStringLiteral("project-json"));
    const QString song = QDir(root.path()).absoluteFilePath(QStringLiteral("song.jamjar"));
    const QByteArray json = QByteArrayLiteral(
        "{\"schema_version\":3,\"title\":\"Persistence boundary\"}");
    QString error;
    require(ProjectPersistenceCoordinator::writeSongJson(song, json, error) &&
            readBytes(song) == json,
        "atomic song write must preserve exact JSON bytes");
    QJsonObject object;
    require(ProjectPersistenceCoordinator::readSongJson(song, object, error) &&
            object.value(QStringLiteral("schema_version")).toInt() == 3 &&
            object.value(QStringLiteral("title")).toString() ==
                QStringLiteral("Persistence boundary"),
        "song read must return the complete root object");

    const QString malformed = QDir(root.path()).absoluteFilePath(QStringLiteral("bad.jamjar"));
    writeBytes(malformed, QByteArrayLiteral("{not-json"));
    require(!ProjectPersistenceCoordinator::readSongJson(malformed, object, error) &&
            error == QStringLiteral("Invalid JamJar JSON."),
        "malformed JSON must be rejected with its stable diagnostic");
    const QString array = QDir(root.path()).absoluteFilePath(QStringLiteral("array.jamjar"));
    writeBytes(array, QByteArrayLiteral("[]"));
    require(!ProjectPersistenceCoordinator::readSongJson(array, object, error),
        "a nonobject JamJar document must be rejected");
    require(!ProjectPersistenceCoordinator::readSongJson(
                QDir(root.path()).absoluteFilePath(QStringLiteral("missing.jamjar")),
                object, error),
        "a missing song file must be rejected");

    constexpr qsizetype maximumBytes = 4 * 1024 * 1024;
    const QByteArray oversized(maximumBytes + 1, 'x');
    const QString oversizedInput = QDir(root.path()).absoluteFilePath(
        QStringLiteral("oversized-input.jamjar"));
    writeBytes(oversizedInput, oversized);
    require(!ProjectPersistenceCoordinator::readSongJson(oversizedInput, object, error) &&
            error.contains(QStringLiteral("4 MiB")),
        "song read must reject input beyond the exact 4 MiB safety bound");
    const QString oversizedOutput = QDir(root.path()).absoluteFilePath(
        QStringLiteral("oversized-output.jamjar"));
    require(!ProjectPersistenceCoordinator::writeSongJson(
                oversizedOutput, oversized, error) &&
            !QFileInfo::exists(oversizedOutput) && error.contains(QStringLiteral("4 MiB")),
        "song write must not create a file that its own reader will reject");

    const QString missingParent = QDir(root.path()).absoluteFilePath(
        QStringLiteral("missing/parent/song.jamjar"));
    require(!ProjectPersistenceCoordinator::writeSongJson(
                missingParent, json, error) && !error.isEmpty(),
        "atomic song write must report an unavailable parent directory");
    require(!ProjectPersistenceCoordinator::writeSongJson(root.path(), json, error),
        "atomic song write must reject a directory target");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        QCoreApplication application(argc, argv);
        testWorkspaceInitializationAndState();
        testTransientOwnershipAndRelocation();
        testDiscardAndScheduledCleanup();
        testJsonBoundaries();
        std::cout << "Project persistence coordinator tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Project persistence test failed: " << error.what() << '\n';
        return 1;
    }
}

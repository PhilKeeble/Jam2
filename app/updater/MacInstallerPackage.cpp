#include "MacInstallerPackage.hpp"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <stdexcept>

namespace jam2::update {
namespace {
void require(bool value, const QString& message)
{
    if (!value) throw std::runtime_error(message.toStdString());
}
MacToolResult run(const QString& program, const QStringList& arguments,
    const std::atomic_bool& cancelled, const MacToolRunner& runner)
{
    require(!cancelled.load(), QStringLiteral("Update cancelled."));
    if (runner) return runner(program, arguments);
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, arguments);
    require(process.waitForStarted(5000), QStringLiteral("Could not start %1: %2").arg(program, process.errorString()));
    QElapsedTimer timer;
    timer.start();
    while (process.state() != QProcess::NotRunning && !process.waitForFinished(100)) {
        if (cancelled.load() || timer.elapsed() > 60000) {
            process.kill();
            process.waitForFinished(5000);
            throw std::runtime_error(cancelled.load() ? "Update cancelled." : "macOS package verification timed out.");
        }
    }
    require(process.exitStatus() == QProcess::NormalExit,
        QStringLiteral("macOS package tool stopped unexpectedly: %1").arg(program));
    return {process.exitCode(), QString::fromUtf8(process.readAll()).left(16384)};
}
QByteArray readSmall(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly) && file.size() <= 65536,
        QStringLiteral("Could not read prepared installer metadata: %1").arg(path));
    return file.readAll();
}
}

void verifyMacInstaller(const QString& root, const Metadata& metadata,
    const std::atomic_bool& cancelled, const MacToolRunner& runner)
{
    const QString package = confinedPath(root, downloadRelativePath());
    require(QFileInfo(package).isFile() && QFileInfo(package).size() == metadata.bytes &&
            sha256File(package, &cancelled) == metadata.sha256,
        QStringLiteral("The macOS installer failed its size/SHA-256 verification. Download it again."));
    const auto signature = run(QStringLiteral("/usr/sbin/pkgutil"),
        {QStringLiteral("--check-signature"), package}, cancelled, runner);
    require(signature.exitCode == 0 && signature.output.contains(QStringLiteral("Developer ID Installer:")),
        QStringLiteral("The update has no trusted Developer ID Installer signature: %1").arg(signature.output));
    const auto trust = run(QStringLiteral("/usr/sbin/spctl"),
        {QStringLiteral("--assess"), QStringLiteral("--type"), QStringLiteral("install"),
         QStringLiteral("--verbose=2"), package}, cancelled, runner);
    require(trust.exitCode == 0, QStringLiteral("macOS rejected the update installer: %1").arg(trust.output));
}

void prepareMacInstaller(const QString& root, const Metadata& metadata,
    const std::atomic_bool& cancelled, const MacToolRunner& runner)
{
    verifyMacInstaller(root, metadata, cancelled, runner);
    const auto bytes = QJsonDocument(QJsonObject{{"schema", 1}, {"version", metadata.version},
        {"installer_sha256", QString::fromLatin1(metadata.sha256)}}).toJson();
    QSaveFile marker(confinedPath(root, QStringLiteral("update/prepared.json"), true));
    require(marker.open(QIODevice::WriteOnly) && marker.write(bytes) == bytes.size() && marker.commit(),
        QStringLiteral("Could not save the verified installer marker."));
}

Metadata verifyPreparedMacInstaller(const QString& root, const QString& version,
    const std::atomic_bool& cancelled, const MacToolRunner& runner)
{
    const auto metadata = parseMetadata(readSmall(confinedPath(root,
        downloadRelativePath() + QStringLiteral(".json"))), version);
    const auto marker = QJsonDocument::fromJson(readSmall(confinedPath(root,
        QStringLiteral("update/prepared.json")))).object();
    require(marker.value(QStringLiteral("schema")).toInt() == 1 &&
            marker.value(QStringLiteral("version")).toString() == version &&
            marker.value(QStringLiteral("installer_sha256")).toString().toLatin1() == metadata.sha256,
        QStringLiteral("The prepared installer does not match the approved update."));
    verifyMacInstaller(root, metadata, cancelled, runner);
    return metadata;
}

void openMacInstaller(const QString& root, const Metadata& metadata,
    const std::atomic_bool& cancelled, const MacToolRunner& runner)
{
    verifyMacInstaller(root, metadata, cancelled, runner);
    const auto opened = run(QStringLiteral("/usr/bin/open"),
        {QStringLiteral("-a"), QStringLiteral("/System/Library/CoreServices/Installer.app"),
         confinedPath(root, downloadRelativePath())}, cancelled, runner);
    require(opened.exitCode == 0,
        QStringLiteral("Could not open Apple's Installer: %1. Open the downloaded PKG manually.").arg(opened.output));
    // This is a handoff, not evidence of installation. Installer owns user
    // authorization, progress, cancellation and its final success/error screen.
}
}

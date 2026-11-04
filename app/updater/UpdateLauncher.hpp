#pragma once

#include <QString>
#include <QStringList>
#include <optional>
#include <functional>

namespace jam2::update {
struct LaunchOptions {
    QString root;
    QString version;
    qint64 waitPid = 0;
    bool restart = false;
    bool staged = false;
    bool prepareOnly = false;
    QString installationRoot;
    QString parentExecutable;
};
struct Result {
    bool success = false;
    bool reported = false;
    QString version;
    QString error;
    bool installerOpened = false;
};
// Caller holds temp/updater.lock while copying or launching this runtime.
QString prepareHelper(const QString& root, const QString& installationRoot = {});
qint64 launch(const LaunchOptions& options);
QString temporaryHelperPath(const QString& root);
QStringList helperArguments(const LaunchOptions& options);
qint64 launchPrepared(const LaunchOptions& options);
void writeResult(const QString& root, const Result& result);
std::optional<Result> readResult(const QString& root);
std::optional<Result> confirmInstalledVersion(const QString& root, const QString& installedVersion);
bool cleanupCompleted(const QString& root, const std::function<bool()>& installerRunning = {});
int runUpdateCommand(const QStringList& arguments);
} // namespace jam2::update

#pragma once

#include <QString>
#include <QStringList>
#include <optional>

namespace jam2::update {
struct LaunchOptions {
    QString root;
    QString version;
    qint64 waitPid = 0;
    bool restart = false;
    bool staged = false;
    bool prepareOnly = false;
};
struct Result {
    bool success = false;
    bool reported = false;
    QString version;
    QString error;
};
// Caller holds temp/updater.lock while copying or launching this runtime.
QString prepareHelper(const QString& root);
qint64 launch(const LaunchOptions& options);
QString temporaryHelperPath(const QString& root);
QStringList helperArguments(const LaunchOptions& options);
qint64 launchPrepared(const LaunchOptions& options);
void writeResult(const QString& root, const Result& result);
std::optional<Result> readResult(const QString& root);
bool cleanupCompleted(const QString& root);
int runUpdateCommand(const QStringList& arguments);
} // namespace jam2::update

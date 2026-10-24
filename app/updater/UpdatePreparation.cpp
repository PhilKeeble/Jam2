#include "UpdatePreparation.hpp"
#include "UpdateLauncher.hpp"
#include "UpdaterPlatform.hpp"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QThread>
#include <stdexcept>
#include <algorithm>

namespace jam2::update {
Preparation::Preparation(QString root, QObject* parent) : QObject(parent), root_(std::move(root))
{
    connect(&process_, &QProcess::started, this, [this] {
        if (statusChanged) statusChanged({PreparationPhase::Downloading,
            QStringLiteral("Checking the approved release and starting the download..."), -1});
    });
    connect(&process_, &QProcess::readyReadStandardOutput, this, &Preparation::consumeOutput);
    connect(&process_, &QProcess::readyReadStandardError, this, [this] {
        errors_.append(process_.readAllStandardError());
        if (errors_.size() > 65536) errors_ = errors_.right(65536);
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) end(QStringLiteral("Could not start the update helper: %1").arg(process_.errorString()));
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, QProcess::ExitStatus status) {
        consumeOutput();
        if (cancelled_.load()) { cleanupCancelled(); return; }
        if (status != QProcess::NormalExit || code != 0) {
            QString error = QString::fromUtf8(errors_).trimmed();
            try { if (auto result = readResult(root_)) error = result->error; } catch (...) {}
            end(error.isEmpty() ? QStringLiteral("The update helper stopped before the download was ready.") : error); return;
        }
        try {
            QFile marker(confinedPath(root_, QStringLiteral("update/prepared.json")));
            if (!marker.open(QIODevice::ReadOnly) || marker.size() > 4096) throw std::runtime_error("The verified update marker is missing.");
            const auto object = QJsonDocument::fromJson(marker.readAll()).object();
            if (object.value(QStringLiteral("schema")).toInt() != 1 ||
                object.value(QStringLiteral("version")).toString() != metadata_.version ||
                object.value(QStringLiteral("manifest_sha256")).toString().size() != 64)
                throw std::runtime_error("The prepared update does not match the requested version.");
            end({});
        } catch (const std::exception& error) { end(QString::fromUtf8(error.what())); }
    });
}
Preparation::~Preparation()
{
    const bool wasActive = active();
    statusChanged = {}; finished = {}; cancelled_.store(true);
    if (worker_) { worker_->wait(); worker_.reset(); }
    process_.disconnect(this);
    if (process_.state() != QProcess::NotRunning) { process_.kill(); process_.waitForFinished(3000); }
    if (wasActive) {
        try {
            QLockFile lock(confinedPath(root_, QStringLiteral("temp/updater.lock"), true));
            if (lock.tryLock()) {
                for (const auto& relative : {QStringLiteral("update"), QStringLiteral("temp/updater")}) {
                    const auto path = confinedPath(root_, relative);
                    if (QFileInfo(path).exists()) QDir(path).removeRecursively();
                }
            }
        } catch (...) {}
    }
}
bool Preparation::active() const { return worker_ || process_.state() != QProcess::NotRunning; }
void Preparation::start(const Metadata& metadata, const QByteArray& encoded, bool staged)
{
    if (active()) return;
    metadata_ = metadata; output_.clear(); errors_.clear(); ending_ = false; cancelled_.store(false);
    if (statusChanged) statusChanged({PreparationPhase::Staging, QStringLiteral("Preparing the update helper..."), -1});
    worker_.reset(QThread::create([this, encoded, staged] {
        QString helper; QString error;
        try {
            QLockFile lock(confinedPath(root_, QStringLiteral("temp/updater.lock"), true));
            if (!lock.tryLock()) throw std::runtime_error("An updater is already running for this installation.");
            if (!cancelled_.load()) {
                helper = prepareHelper(root_);
                QSaveFile file(confinedPath(root_, QStringLiteral("temp/updater/approved.json")));
                if (!file.open(QIODevice::WriteOnly) || file.write(encoded) != encoded.size() || !file.commit())
                    throw std::runtime_error("Could not save the approved release metadata.");
            }
        } catch (const std::exception& exception) { error = QString::fromUtf8(exception.what()); }
        catch (...) { error = QStringLiteral("Unexpected error preparing the updater."); }
        QMetaObject::invokeMethod(this, [this, helper, error, staged] {
            worker_->wait(); worker_.reset();
            if (cancelled_.load()) { cleanupCancelled(); return; }
            if (!error.isEmpty()) { end(error); return; }
            configureHelperProcess(process_);
            process_.setProgram(helper);
            process_.setArguments(helperArguments({root_, metadata_.version, 0, false, staged, true}));
            process_.setWorkingDirectory(root_);
            process_.start();
        }, Qt::QueuedConnection);
    }));
    worker_->start(QThread::LowPriority);
}
void Preparation::consumeOutput()
{
    output_.append(process_.readAllStandardOutput());
    if (output_.size() > 65536) { errors_ = "The helper returned an oversized progress record."; process_.kill(); return; }
    qsizetype newline;
    while ((newline = output_.indexOf('\n')) >= 0) {
        const auto line = output_.left(newline); output_.remove(0, newline + 1);
        const auto object = QJsonDocument::fromJson(line).object();
        const auto state = object.value(QStringLiteral("state")).toString();
        if (state == QStringLiteral("starting")) continue;
        if (object.value(QStringLiteral("version")).toString() != metadata_.version) continue;
        PreparationPhase phase = PreparationPhase::Unpacking;
        if (state == QStringLiteral("downloading")) phase = PreparationPhase::Downloading;
        else if (state != QStringLiteral("unpacking") && state != QStringLiteral("ready")) continue;
        const int percent = object.value(QStringLiteral("percent")).toInt(-1);
        if (statusChanged) statusChanged({phase, object.value(QStringLiteral("message")).toString(),
            percent < 0 ? -1 : std::clamp(percent, 0, 100)});
    }
}
void Preparation::end(const QString& error)
{
    if (ending_) return;
    ending_ = true;
    if (finished) finished(error, cancelled_.load());
}
void Preparation::cleanupCancelled()
{
    if (worker_) return;
    worker_.reset(QThread::create([this] {
        try {
            QLockFile lock(confinedPath(root_, QStringLiteral("temp/updater.lock"), true));
            if (lock.tryLock()) {
                for (const auto& relative : {QStringLiteral("update"), QStringLiteral("temp/updater")}) {
                    const auto path = confinedPath(root_, relative);
                    if (QFileInfo(path).exists()) QDir(path).removeRecursively();
                }
            }
        } catch (...) {}
        QMetaObject::invokeMethod(this, [this] {
            worker_->wait(); worker_.reset(); end({});
        }, Qt::QueuedConnection);
    }));
    worker_->start(QThread::LowPriority);
}
void Preparation::cancel()
{
    if (!active()) ending_ = false;
    cancelled_.store(true);
    if (process_.state() != QProcess::NotRunning) process_.kill();
    else if (!worker_) cleanupCancelled();
}
}

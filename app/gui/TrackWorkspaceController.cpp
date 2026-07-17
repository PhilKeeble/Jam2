#include "TrackWorkspaceController.hpp"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QRunnable>

#include <exception>
#include <memory>

TrackWorkspaceController::TrackWorkspaceController(
    ApplicationRuntime& runtime,
    QObject* parent)
    : QObject(parent)
    , recordingWorkflow(runtime)
    , assetTransfer(*this)
{
    fileWorkers.setMaxThreadCount(2);
    fileWorkers.setExpiryTimeout(30000);
}

TrackWorkspaceController::~TrackWorkspaceController()
{
    assetTransfer.cancel();
    fileWorkers.waitForDone();
}

void TrackWorkspaceController::setCallbacks(Callbacks callbacks)
{
    callbacks_ = std::move(callbacks);
}

void TrackWorkspaceController::cancelPendingTrackPlayback() noexcept
{
    playPreparedMixWhenReady = false;
    pendingPreparedTrackReadyRevision = 0;
    pendingSharedTrackRevision = 0;
    pendingSharedTrackHostReady = true;
    pendingSharedTrackReadyTokens.clear();
    publishStoppedTrackStateWhenApplied = false;
    pendingSongTrackRestart = false;
    trackController.requestPlayback(false);
}

bool TrackWorkspaceController::startFileTask(
    std::function<void()> work,
    std::function<void()> complete,
    std::function<void(const QString&)> failed)
{
    constexpr int kMaximumFileWorkerTasks = 2;
    if (fileWorkerTasksActive >= kMaximumFileWorkerTasks) {
        ++fileWorkerTasksRejected;
        appendAssetLog(QStringLiteral("file worker saturated: active=%1 capacity=%2 rejected=%3")
            .arg(fileWorkerTasksActive)
            .arg(kMaximumFileWorkerTasks)
            .arg(fileWorkerTasksRejected));
        return false;
    }
    ++fileWorkerTasksActive;
    fileWorkerTasksHighWater = qMax(fileWorkerTasksHighWater, fileWorkerTasksActive);
    QPointer<TrackWorkspaceController> self(this);
    auto unexpectedError = std::make_shared<QString>();
    fileWorkers.start(QRunnable::create([
        self,
        work = std::move(work),
        complete = std::move(complete),
        failed = std::move(failed),
        unexpectedError
    ]() mutable {
        try {
            work();
        } catch (const std::exception& error) {
            *unexpectedError = QString::fromUtf8(error.what());
        } catch (...) {
            *unexpectedError = QStringLiteral("unknown worker exception");
        }
        if (self.isNull()) {
            return;
        }
        QMetaObject::invokeMethod(self, [
            self,
            complete = std::move(complete),
            failed = std::move(failed),
            unexpectedError
        ]() mutable {
            if (self.isNull()) {
                return;
            }
            self->fileWorkerTasksActive = qMax(0, self->fileWorkerTasksActive - 1);
            ++self->fileWorkerTasksCompleted;
            if (!unexpectedError->isEmpty()) {
                self->appendAssetLog(
                    QStringLiteral("file worker failed: %1 active=%2 high_water=%3 completed=%4 rejected=%5")
                        .arg(*unexpectedError)
                        .arg(self->fileWorkerTasksActive)
                        .arg(self->fileWorkerTasksHighWater)
                        .arg(self->fileWorkerTasksCompleted)
                        .arg(self->fileWorkerTasksRejected));
                if (failed) {
                    failed(*unexpectedError);
                }
                return;
            }
            complete();
        }, Qt::QueuedConnection);
    }));
    return true;
}

void TrackWorkspaceController::waitForWorkers()
{
    fileWorkers.waitForDone();
}

QObject* TrackWorkspaceController::dispatchContext() noexcept { return this; }
bool TrackWorkspaceController::trackSyncEnabled() const noexcept
{
    return looperProject.trackSyncEnabled();
}

int TrackWorkspaceController::sessionSampleRate() const noexcept
{
    return callbacks_.sessionSampleRate ? callbacks_.sessionSampleRate() : 0;
}

QString TrackWorkspaceController::assetPathForSend(const QString& hash) const
{
    QString path = trackOfferAssetPaths.value(hash);
    for (const LooperBank& bank : looperProject.banks()) {
        if (!path.isEmpty()) {
            break;
        }
        for (const LooperLane& lane : bank.lanes) {
            if (lane.assetHash != hash) {
                continue;
            }
            path = lane.assetPath;
            if (!QFileInfo(path).isAbsolute() && !persistence.projectFolder().isEmpty()) {
                path = QDir(persistence.projectFolder()).absoluteFilePath(path);
            }
            break;
        }
    }
    return path;
}

QString TrackWorkspaceController::incomingAssetPath(const QString& hash) const
{
    return QDir(persistence.workspaceFolder()).absoluteFilePath(
        QStringLiteral("wavs/") + hash + QStringLiteral(".wav"));
}

bool TrackWorkspaceController::incomingAssetExpected(
    const QString& hash,
    const QString& sourcePeerToken) const
{
    if (!pendingLooperAssetHashes.contains(hash)) {
        return false;
    }
    const QString expectedSource = pendingTrackAssetSources.value(hash);
    return expectedSource.isEmpty() || expectedSource == sourcePeerToken;
}

void TrackWorkspaceController::abandonIncomingAsset(const QString& hash)
{
    if (pendingTrackAssetSources.remove(hash) > 0) {
        pendingLooperAssetHashes.removeAll(hash);
    }
}

void TrackWorkspaceController::acceptIncomingAsset(const QString& hash, const QString& path)
{
    persistence.registerTransientWav(path);
    looperWaveformCache.remove(path);
    pendingLooperAssetHashes.removeAll(hash);
    if (pendingTrackAssetSources.remove(hash) > 0) {
        validatedTrackAssetHashes.insert(hash);
    }
    appendAssetLog(QStringLiteral("received looper asset: ") + path);
    if (callbacks_.incomingAssetAccepted) {
        callbacks_.incomingAssetAccepted();
    }
}

void TrackWorkspaceController::appendAssetLog(const QString& message)
{
    if (callbacks_.log) {
        callbacks_.log(message);
    }
}

bool TrackWorkspaceController::startAssetFileTask(
    std::function<void()> work,
    std::function<void()> complete,
    std::function<void(const QString&)> failed)
{
    return startFileTask(std::move(work), std::move(complete), std::move(failed));
}

bool TrackWorkspaceController::canQueueAssetControl(
    const QString& peerToken,
    qint64 estimatedBytes) const
{
    return callbacks_.canQueueControl && callbacks_.canQueueControl(peerToken, estimatedBytes);
}

bool TrackWorkspaceController::sendAssetControl(
    const QString& peerToken,
    const QJsonObject& message)
{
    return callbacks_.sendControl && callbacks_.sendControl(peerToken, message);
}

bool TrackWorkspaceController::sendAssetBinary(
    const QString& peerToken,
    const QByteArray& payload)
{
    return callbacks_.sendBinary && callbacks_.sendBinary(peerToken, payload);
}

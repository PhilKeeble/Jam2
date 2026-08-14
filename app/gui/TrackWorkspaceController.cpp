#include "TrackWorkspaceController.hpp"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
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
    preparedMixLifecycle.setPlayWhenReady(false);
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

QObject* TrackWorkspaceController::dispatchContext() noexcept { return this; }
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
        QStringLiteral("received/") + hash + QStringLiteral(".wav"));
}

bool TrackWorkspaceController::incomingAssetExpected(
    const QString& hash,
    const QString& sourcePeerToken) const
{
    return incomingAssetWorkflow != IncomingAssetWorkflow::None &&
        incomingAssetHash == hash &&
        incomingAssetSourcePeerToken == sourcePeerToken;
}

void TrackWorkspaceController::abandonIncomingAsset(const QString& hash)
{
    const QString failedSource = incomingAssetSourcePeerToken;
    pendingTrackAssetSources.remove(hash);
    if (incomingAssetHash == hash) {
        appendAssetLog(QStringLiteral("cancelled incoming looper asset: workflow=%1 hash=%2 source=%3")
            .arg(incomingAssetWorkflow == IncomingAssetWorkflow::TrackContribution
                    ? QStringLiteral("track-share")
                    : QStringLiteral("arrangement"),
                hash,
                incomingAssetSourcePeerToken));
        incomingAssetWorkflow = IncomingAssetWorkflow::None;
        incomingAssetHash.clear();
        incomingAssetSourcePeerToken.clear();
    }
    if (callbacks_.incomingAssetAbandoned) {
        callbacks_.incomingAssetAbandoned(hash, failedSource);
    }
}

void TrackWorkspaceController::acceptIncomingAsset(
    const QString& hash,
    const QString& path,
    qint64 sourceFrames)
{
    persistence.registerTransientWav(path);
    looperWaveformCache.remove(path);
    pendingLooperAssetHashes.removeAll(hash);
    pendingTrackAssetSources.remove(hash);
    validatedTrackAssetHashes.insert(hash);
    incomingAssetRetryAttempts.remove(hash);
    incomingAssetRetrySources.remove(hash);
    const auto reconcileRegion = [sourceFrames](
        qint64& loopStart, qint64& loopEnd, bool& loopEnabled) {
        if (sourceFrames <= 0 || loopStart < 0 || loopEnd < 0) {
            loopStart = -1;
            loopEnd = -1;
            loopEnabled = false;
            return;
        }
        loopStart = qBound<qint64>(0, loopStart, sourceFrames - 1);
        loopEnd = qBound<qint64>(loopStart + 1, loopEnd, sourceFrames);
    };
    for (auto it = pendingTrackContributions.begin();
         it != pendingTrackContributions.end(); ++it) {
        if (it->assetHash == hash) {
            it->sourceFrames = sourceFrames;
            reconcileRegion(it->loopStartFrame, it->loopEndFrame, it->loopEnabled);
        }
    }
    for (LooperBank& bank : looperProject.banks()) {
        for (LooperLane& lane : bank.lanes) {
            if (lane.assetHash == hash) {
                lane.sourceFrames = sourceFrames;
                reconcileRegion(
                    lane.loopStartFrame, lane.loopEndFrame, lane.loopEnabled);
            }
        }
    }
    if (!pendingSongSet.isEmpty()) {
        QJsonObject looper = pendingSongSet.value(QStringLiteral("looper")).toObject();
        QJsonArray banks = looper.value(QStringLiteral("banks")).toArray();
        for (int bankIndex = 0; bankIndex < banks.size(); ++bankIndex) {
            QJsonObject bank = banks.at(bankIndex).toObject();
            QJsonArray lanes = bank.value(QStringLiteral("lanes")).toArray();
            for (int laneIndex = 0; laneIndex < lanes.size(); ++laneIndex) {
                QJsonObject lane = lanes.at(laneIndex).toObject();
                if (lane.value(QStringLiteral("asset_hash")).toString() == hash) {
                    lane.insert(QStringLiteral("source_frames"),
                        QString::number(sourceFrames));
                    qint64 loopStart = lane.value(
                        QStringLiteral("loop_start_frame")).toString().toLongLong();
                    qint64 loopEnd = lane.value(
                        QStringLiteral("loop_end_frame")).toString().toLongLong();
                    bool loopEnabled = lane.value(
                        QStringLiteral("loop_enabled")).toBool();
                    reconcileRegion(loopStart, loopEnd, loopEnabled);
                    lane.insert(QStringLiteral("loop_start_frame"),
                        QString::number(loopStart));
                    lane.insert(QStringLiteral("loop_end_frame"),
                        QString::number(loopEnd));
                    lane.insert(QStringLiteral("loop_enabled"), loopEnabled);
                    lanes.replace(laneIndex, lane);
                }
            }
            bank.insert(QStringLiteral("lanes"), lanes);
            banks.replace(bankIndex, bank);
        }
        looper.insert(QStringLiteral("banks"), banks);
        pendingSongSet.insert(QStringLiteral("looper"), looper);
    }
    const QString workflow = incomingAssetWorkflow == IncomingAssetWorkflow::TrackContribution
        ? QStringLiteral("track-share")
        : QStringLiteral("arrangement");
    const QString source = incomingAssetSourcePeerToken;
    incomingAssetWorkflow = IncomingAssetWorkflow::None;
    incomingAssetHash.clear();
    incomingAssetSourcePeerToken.clear();
    appendAssetLog(QStringLiteral("accepted incoming looper asset: workflow=%1 hash=%2 source=%3 path=%4")
        .arg(workflow, hash, source, path));
    if (callbacks_.incomingAssetAccepted) {
        callbacks_.incomingAssetAccepted();
    }
}

void TrackWorkspaceController::noteAssetProgress(
    const QString& hash,
    const QString& peerToken,
    bool receiving)
{
    if (callbacks_.assetProgress) {
        callbacks_.assetProgress(hash, peerToken, receiving);
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

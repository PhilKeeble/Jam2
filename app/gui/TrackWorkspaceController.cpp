#include "TrackWorkspaceController.hpp"
#include "LooperAssetFiles.hpp"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QMetaObject>
#include <QPointer>
#include <QRunnable>

#include <exception>
#include <memory>
#include <cmath>
#include <limits>

TrackWorkspaceController::TrackWorkspaceController(
    ApplicationRuntime& runtime,
    QObject* parent)
    : QObject(parent)
    , listeningTools(this)
    , recordingWorkflow(runtime)
    , assetTransfer(*this)
{
    fileWorkers.setMaxThreadCount(2);
    fileWorkers.setExpiryTimeout(30000);
}

QVector<TrackLanePresentation> TrackWorkspaceController::presentedLanes(int bankIndex) const
{
    if (bankIndex < 0 || bankIndex >= looperProject.banks().size()) return {};
    QVector<PendingTrackLane> pending;
    // Keep the latest composed host snapshot, including scoped updates that
    // arrive while the preceding WAV is still being downloaded.
    const QJsonObject previewSong = !heldTrackShareSongSet.isEmpty()
        ? stagedHostSong
        : pendingSongSet;
    LooperProject preview;
    if (!previewSong.isEmpty() &&
        preview.loadJson(previewSong.value(QStringLiteral("looper")).toObject())) {
        const auto& bankId = looperProject.banks().at(bankIndex).id;
        for (const auto& bank : preview.banks()) {
            if (bank.id != bankId) continue;
            for (const auto& lane : bank.lanes) pending.append({lane});
        }
    }
    for (const auto& contribution : pendingTrackContributions) {
        if (contribution.bankIndex != bankIndex) continue;
        LooperLane lane;
        lane.id = contribution.targetLaneId.isEmpty()
            ? contribution.contributionId : contribution.targetLaneId;
        lane.assetHash = contribution.assetHash;
        lane.name = contribution.name;
        lane.sampleRate = contribution.sampleRate;
        lane.sourceFrames = contribution.sourceFrames;
        lane.startFrame = contribution.startFrame;
        lane.stopFrame = contribution.stopFrame;
        lane.loopStartFrame = contribution.loopStartFrame;
        lane.loopEndFrame = contribution.loopEndFrame;
        lane.loopEnabled = contribution.loopEnabled;
        lane.originKind = QStringLiteral("peer");
        contribution.reference.applyTo(lane);
        pending.append({std::move(lane), true, contribution.contributionId});
    }
    const auto progress = assetTransfer.incomingProgress();
    return presentPendingTrackLanes(looperProject.banks().at(bankIndex).lanes,
        pending, validatedTrackAssetHashes, incomingAssetHash,
        progress.receivedBytes, progress.totalBytes);
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

bool TrackWorkspaceController::updateSectionPlaybackTiming(int bankIndex, int sampleRate)
{
    if (bankIndex < 0 || bankIndex >= songModel.sections().size() || sampleRate <= 0) return false;
    const int beats = looperProject.sectionPlaybackBeats(bankIndex, songModel.section(bankIndex).beats, sampleRate);
    if (beats > songModel.section(bankIndex).beats) songModel.resizeSection(bankIndex, beats);
    if (bankIndex != looperProject.activeBankIndex()) return true;
    auto& track = trackController.model();
    const double speed = listeningTools.appliedSettings().speedPpm / 1000000.0;
    const auto frames = static_cast<qint64>(std::llround(
        looperProject.sectionPlaybackFrames(bankIndex, songModel.section(bankIndex).beats, sampleRate) / speed));
    track.durationMs = static_cast<int>(qMin<qint64>((std::numeric_limits<int>::max)(), frames * 1000 / sampleRate));
    track.acceptedBpm = looperProject.resolvedTiming(bankIndex).bpm;
    if (preparedMixLifecycle.active().path.isEmpty()) return true;
    const auto loop = trackController.effectiveLoop(sampleRate, frames);
    return recordingWorkflow.setPreparedLoop(loop.enabled, loop.startFrame, loop.endFrame);
}

void TrackWorkspaceController::cancelPendingTrackPlayback() noexcept
{
    playAfterListening = false;
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
    // QThreadPool still runs at most two file jobs concurrently. Keep a bounded
    // outstanding queue so a valid multi-WAV arrangement is not rejected just
    // because validation and transfer completions briefly arrive together.
    constexpr int kMaximumFileWorkerTasks = 64;
    if (fileWorkerTasksActive >= kMaximumFileWorkerTasks) {
        ++fileWorkerTasksRejected;
        appendAssetLog(QStringLiteral("file worker saturated: outstanding=%1 capacity=%2 rejected=%3")
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
    QStringList lanePaths;
    for (const LooperBank& bank : looperProject.banks()) {
        for (const LooperLane& lane : bank.lanes) {
            if (lane.assetHash == hash && !lane.assetPath.isEmpty()) {
                lanePaths.append(lane.assetPath);
            }
        }
    }
    return jam2::gui::looper_asset_files::sendPath(
        persistence.projectFolder(), trackOfferAssetPaths.value(hash), lanePaths);
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

QString TrackWorkspaceController::peerName(const QString& token) const
{
    return callbacks_.peerName ? callbacks_.peerName(token) : QString{};
}

void TrackWorkspaceController::discardPendingAssetRequestFromPeer(const QString& peerToken)
{
    // Requests awaiting START have no ReceiveSequence yet, so the transfer
    // service cannot identify their owner during disconnect cleanup.
    if (incomingAssetWorkflow == IncomingAssetWorkflow::None ||
        incomingAssetSourcePeerToken != peerToken || assetTransfer.incomingTransferActive()) return;
    const auto hash = incomingAssetHash;
    ++incomingAssetRequestGeneration;
    incomingAssetWorkflow = IncomingAssetWorkflow::None;
    incomingAssetHash.clear();
    incomingAssetSourcePeerToken.clear();
    pendingTrackAssetSources.remove(hash);
    appendAssetLog(QStringLiteral("released pending looper asset request after peer interruption: hash=%1 source=%2")
        .arg(hash, peerName(peerToken)));
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
                peerName(incomingAssetSourcePeerToken)));
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
        .arg(workflow, hash, peerName(source), path));
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

void TrackWorkspaceController::assetWorkStateChanged(bool pending)
{
    if (callbacks_.assetWorkStateChanged) {
        callbacks_.assetWorkStateChanged(pending);
    }
}

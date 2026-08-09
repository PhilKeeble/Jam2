#include "GuiControlMessageRouter.hpp"

#include "application/ControlMessageValidation.hpp"
#include "AssetTransferService.hpp"
#include "BeatGridModel.hpp"

void GuiControlMessageRouter::dispatch(
    const Handlers& handlers,
    const QJsonObject& message,
    const QString& sourcePeerToken)
{
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type != QStringLiteral("song.set") &&
        jam2::application::isTrackSyncControlMessageType(type) &&
        !jam2::application::isManualTrackShareControlMessageType(type) &&
        handlers.trackSyncEnabled && !handlers.trackSyncEnabled()) {
        if (handlers.log) handlers.log(QStringLiteral("ignored remote track sync while local sync is disabled"));
        return;
    }
    if (type == QStringLiteral("session.error")) {
        const QString text = message.value(QStringLiteral("message")).toString(
            QStringLiteral("Session error"));
        if (handlers.log) handlers.log(QStringLiteral("peer session error: ") + text);
        if (handlers.warning) handlers.warning(text);
    } else if (type == QStringLiteral("beat.set")) {
        const QString lane = message.value(QStringLiteral("lane")).toString();
        BeatGridModel* model = handlers.beatModel;
        if (lane == QStringLiteral("chord") || lane == QStringLiteral("target")) {
            model = handlers.chordModel;
        } else if (lane == QStringLiteral("lyric")) {
            model = handlers.lyricModel;
        }
        if (model == nullptr) return;
        model->setCell(
            message.value(QStringLiteral("section")).toInt(),
            lane,
            message.value(QStringLiteral("beat")).toInt(),
            message.value(QStringLiteral("text")).toString());
        if (handlers.refreshSongView) handlers.refreshSongView(lane);
    } else if (type == QStringLiteral("grid.resize")) {
        const QString lane = message.value(QStringLiteral("lane")).toString(
            QStringLiteral("beat"));
        BeatGridModel* model = lane == QStringLiteral("chord")
            ? handlers.chordModel : handlers.beatModel;
        if (model == nullptr) return;
        model->resizeSection(
            message.value(QStringLiteral("section")).toInt(),
            message.value(QStringLiteral("beats")).toInt(8));
        if (handlers.refreshSongView) handlers.refreshSongView(lane);
    } else if (type == QStringLiteral("beat.hit")) {
        if (handlers.beatModel == nullptr) return;
        handlers.beatModel->setBeatHit(
            message.value(QStringLiteral("section")).toInt(),
            message.value(QStringLiteral("beat")).toInt(),
            message.value(QStringLiteral("lane")).toInt(),
            message.value(QStringLiteral("text")).toString());
        if (handlers.refreshSongView) handlers.refreshSongView(QStringLiteral("beat"));
    } else if (type == QStringLiteral("beat.division")) {
        if (handlers.beatModel == nullptr) return;
        handlers.beatModel->setBeatDivision(
            message.value(QStringLiteral("section")).toInt(),
            message.value(QStringLiteral("beat")).toInt(),
            message.value(QStringLiteral("division")).toInt(4));
        if (handlers.refreshSongView) handlers.refreshSongView(QStringLiteral("beat"));
    } else if (type == QStringLiteral("music.division")) {
        if (handlers.chordModel == nullptr) return;
        handlers.chordModel->setMusicalDivision(
            message.value(QStringLiteral("section")).toInt(),
            message.value(QStringLiteral("beat")).toInt(),
            message.value(QStringLiteral("division")).toInt(1));
        if (handlers.refreshSongView) handlers.refreshSongView(QStringLiteral("chord"));
    } else if (type == QStringLiteral("music.step")) {
        if (handlers.chordModel == nullptr) return;
        handlers.chordModel->setMusicalStep(
            message.value(QStringLiteral("section")).toInt(),
            message.value(QStringLiteral("beat")).toInt(),
            message.value(QStringLiteral("step")).toInt(),
            message.value(QStringLiteral("lane")).toString(),
            message.value(QStringLiteral("text")).toString());
        if (handlers.refreshSongView) handlers.refreshSongView(QStringLiteral("chord"));
    } else if (type == QStringLiteral("song.set")) {
        if (handlers.songSet) handlers.songSet(message, sourcePeerToken);
    } else if (type == QStringLiteral("looper.track.share.request")) {
        if (handlers.shareLocalTracks) handlers.shareLocalTracks(message);
    } else if (type == QStringLiteral("looper.track.batch.offer")) {
        if (handlers.trackBatchOffer) handlers.trackBatchOffer(message, sourcePeerToken);
    } else if (type == QStringLiteral("looper.track.batch.complete")) {
        if (handlers.trackBatchComplete) handlers.trackBatchComplete(message, sourcePeerToken);
    } else if (type == QStringLiteral("looper.recording.state")) {
        if (handlers.recordingState) handlers.recordingState(message, sourcePeerToken);
    } else if (type == QStringLiteral("looper.asset.request")) {
        if (handlers.assetTransfer) handlers.assetTransfer->handleRequest(message, sourcePeerToken);
    } else if (type == QStringLiteral("looper.asset.start")) {
        if (handlers.assetTransfer) handlers.assetTransfer->receiveStart(message, sourcePeerToken);
    } else if (type == QStringLiteral("looper.asset.done")) {
        if (handlers.assetTransfer) handlers.assetTransfer->receiveDone(message, sourcePeerToken);
    }
}

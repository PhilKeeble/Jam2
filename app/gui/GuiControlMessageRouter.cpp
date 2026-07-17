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
        handlers.trackSyncEnabled && !handlers.trackSyncEnabled()) {
        if (handlers.log) handlers.log(QStringLiteral("ignored remote track sync while local sync is disabled"));
        return;
    }
    if (type == QStringLiteral("session.error")) {
        const QString text = message.value(QStringLiteral("message")).toString(
            QStringLiteral("Session error"));
        if (handlers.log) handlers.log(QStringLiteral("peer session error: ") + text);
        if (handlers.warning) handlers.warning(text);
    } else if (type == QStringLiteral("metronome.settings")) {
        if (handlers.metronomeSettings) handlers.metronomeSettings(message);
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
    } else if (type == QStringLiteral("lyrics.set")) {
        if (handlers.lyricModel == nullptr) return;
        handlers.lyricModel->setLyricsText(message.value(QStringLiteral("text")).toString());
        if (handlers.refreshSongView) handlers.refreshSongView(QStringLiteral("lyric"));
    } else if (type == QStringLiteral("song.set")) {
        if (handlers.songSet) handlers.songSet(message, sourcePeerToken);
    } else if (type == QStringLiteral("track.ready")) {
        if (handlers.trackReady) handlers.trackReady(message, sourcePeerToken);
    } else if (type == QStringLiteral("looper.track.share.request")) {
        if (sourcePeerToken.isEmpty()) {
            if (handlers.shareLocalTracks) handlers.shareLocalTracks();
        } else {
            if (handlers.log) handlers.log(QStringLiteral("rejected Share Tracks request from a joiner"));
        }
    } else if (type == QStringLiteral("looper.recording.offer")) {
        if (handlers.trackOffer) handlers.trackOffer(message, sourcePeerToken);
    } else if (type == QStringLiteral("looper.asset.request")) {
        if (handlers.assetTransfer) handlers.assetTransfer->handleRequest(message, sourcePeerToken);
    } else if (type == QStringLiteral("looper.asset.start")) {
        if (handlers.assetTransfer) handlers.assetTransfer->receiveStart(message, sourcePeerToken);
    } else if (type == QStringLiteral("looper.asset.done")) {
        if (handlers.assetTransfer) handlers.assetTransfer->receiveDone(message, sourcePeerToken);
    }
}

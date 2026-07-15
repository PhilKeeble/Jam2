#include "GuiControlMessageRouter.hpp"

#include "application/ControlMessageValidation.hpp"
#include "MainWindow.hpp"

#include <QMessageBox>

void GuiControlMessageRouter::dispatch(
    MainWindow& w,
    const QJsonObject& message,
    const QString& sourcePeerToken)
{
    const QString type = message.value(QStringLiteral("type")).toString();
    if (type != QStringLiteral("song.set") &&
        jam2::application::isTrackSyncControlMessageType(type) &&
        !w.looperProject_.trackSyncEnabled()) {
        w.appendLog(QStringLiteral("ignored remote track sync while local sync is disabled"));
        return;
    }
    if (type == QStringLiteral("session.error")) {
        const QString text = message.value(QStringLiteral("message")).toString(
            QStringLiteral("Session error"));
        w.appendLog(QStringLiteral("peer session error: ") + text);
        QMessageBox::warning(&w, QStringLiteral("Jam2"), text);
    } else if (type == QStringLiteral("metronome.settings")) {
        w.applyRemoteMetronomeSettings(message);
    } else if (type == QStringLiteral("beat.set")) {
        const QString lane = message.value(QStringLiteral("lane")).toString();
        BeatGridModel* model = &w.beatModel_;
        if (lane == QStringLiteral("chord")) {
            model = &w.chordModel_;
        } else if (lane == QStringLiteral("lyric")) {
            model = &w.lyricModel_;
        }
        model->setCell(
            message.value(QStringLiteral("section")).toInt(),
            lane,
            message.value(QStringLiteral("beat")).toInt(),
            message.value(QStringLiteral("text")).toString());
        w.refreshSongView(lane);
    } else if (type == QStringLiteral("grid.resize")) {
        const QString lane = message.value(QStringLiteral("lane")).toString(
            QStringLiteral("beat"));
        BeatGridModel* model = lane == QStringLiteral("chord")
            ? &w.chordModel_ : &w.beatModel_;
        model->resizeSection(
            message.value(QStringLiteral("section")).toInt(),
            message.value(QStringLiteral("beats")).toInt(8));
        w.refreshSongView(lane);
    } else if (type == QStringLiteral("beat.hit")) {
        w.beatModel_.setBeatHit(
            message.value(QStringLiteral("section")).toInt(),
            message.value(QStringLiteral("beat")).toInt(),
            message.value(QStringLiteral("lane")).toInt(),
            message.value(QStringLiteral("text")).toString());
        w.refreshSongView(QStringLiteral("beat"));
    } else if (type == QStringLiteral("beat.division")) {
        w.beatModel_.setBeatDivision(
            message.value(QStringLiteral("section")).toInt(),
            message.value(QStringLiteral("beat")).toInt(),
            message.value(QStringLiteral("division")).toInt(4));
        w.refreshSongView(QStringLiteral("beat"));
    } else if (type == QStringLiteral("lyrics.set")) {
        w.lyricModel_.setLyricsText(message.value(QStringLiteral("text")).toString());
        w.refreshSongView(QStringLiteral("lyric"));
    } else if (type == QStringLiteral("song.set")) {
        w.handleSongSet(message, sourcePeerToken);
    } else if (type == QStringLiteral("track.ready")) {
        w.handleTrackReady(message, sourcePeerToken);
    } else if (type == QStringLiteral("looper.track.share.request")) {
        if (sourcePeerToken.isEmpty()) {
            w.shareLocalTracks();
        } else {
            w.appendLog(QStringLiteral("rejected Share Tracks request from a joiner"));
        }
    } else if (type == QStringLiteral("looper.recording.offer")) {
        w.handleTrackOffer(message, sourcePeerToken);
    } else if (type == QStringLiteral("looper.asset.request")) {
        w.assetTransfer_.handleRequest(message, sourcePeerToken);
    } else if (type == QStringLiteral("looper.asset.start")) {
        w.assetTransfer_.receiveStart(message, sourcePeerToken);
    } else if (type == QStringLiteral("looper.asset.done")) {
        w.assetTransfer_.receiveDone(message, sourcePeerToken);
    }
}

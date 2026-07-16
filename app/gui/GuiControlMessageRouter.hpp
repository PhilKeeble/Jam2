#pragma once

#include <functional>

class QJsonObject;
class QString;
class AssetTransferService;
class BeatGridModel;

class GuiControlMessageRouter final {
public:
    struct Handlers {
        std::function<bool()> trackSyncEnabled;
        std::function<void(const QString&)> log;
        std::function<void(const QString&)> warning;
        std::function<void(const QJsonObject&)> metronomeSettings;
        BeatGridModel* chordModel = nullptr;
        BeatGridModel* beatModel = nullptr;
        BeatGridModel* lyricModel = nullptr;
        std::function<void(const QString&)> refreshSongView;
        std::function<void(const QJsonObject&, const QString&)> songSet;
        std::function<void(const QJsonObject&, const QString&)> trackReady;
        std::function<void()> shareLocalTracks;
        std::function<void(const QJsonObject&, const QString&)> trackOffer;
        AssetTransferService* assetTransfer = nullptr;
    };

    static void dispatch(
        const Handlers& handlers,
        const QJsonObject& message,
        const QString& sourcePeerToken);
};

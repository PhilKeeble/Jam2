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
        BeatGridModel* chordModel = nullptr;
        BeatGridModel* beatModel = nullptr;
        BeatGridModel* lyricModel = nullptr;
        std::function<void(const QString&)> refreshSongView;
        std::function<void(const QJsonObject&, const QString&)> songSet;
        std::function<void(const QJsonObject&)> shareLocalTracks;
        std::function<void(const QJsonObject&, const QString&)> trackBatchOffer;
        std::function<void(const QJsonObject&)> trackBatchComplete;
        AssetTransferService* assetTransfer = nullptr;
    };

    static void dispatch(
        const Handlers& handlers,
        const QJsonObject& message,
        const QString& sourcePeerToken);
};

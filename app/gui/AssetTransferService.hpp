#pragma once

#include "AssetChunkProtocol.hpp"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QByteArray>
#include <QString>
#include <QTimer>

#include <memory>
#include <functional>

class QObject;

class AssetTransferContext {
public:
    virtual ~AssetTransferContext() = default;
    virtual QObject* dispatchContext() noexcept = 0;
    virtual bool trackSyncEnabled() const noexcept = 0;
    virtual int sessionSampleRate() const noexcept = 0;
    virtual QString assetPathForSend(const QString& hash) const = 0;
    virtual QString incomingAssetPath(const QString& hash) const = 0;
    virtual bool incomingAssetExpected(
        const QString& hash,
        const QString& sourcePeerToken) const = 0;
    virtual void abandonIncomingAsset(const QString& hash) = 0;
    virtual void acceptIncomingAsset(const QString& hash, const QString& path) = 0;
    virtual void appendAssetLog(const QString& message) = 0;
    virtual bool startAssetFileTask(
        std::function<void()> work,
        std::function<void()> complete,
        std::function<void(const QString&)> failed) = 0;
    virtual bool canQueueAssetControl(const QString& peerToken, qint64 estimatedBytes) const = 0;
    virtual bool sendAssetControl(const QString& peerToken, const QJsonObject& message) = 0;
    virtual bool sendAssetBinary(const QString& peerToken, const QByteArray& payload) = 0;
};

class AssetTransferService {
public:
    explicit AssetTransferService(AssetTransferContext& context);
    ~AssetTransferService();

    void handleRequest(const QJsonObject& message, const QString& sourcePeerToken);
    void queueSend(const QString& hash, const QString& targetPeerToken);
    void continueSend();
    void cancel();
    void peerDisconnected(const QString& peerToken);
    void resetIncoming();
    void receiveStart(const QJsonObject& message, const QString& sourcePeerToken);
    void receiveChunk(const QByteArray& payload, const QString& sourcePeerToken);
    void receiveDone(const QJsonObject& message, const QString& sourcePeerToken);

private:
    struct IncomingWorkerState;
    void scheduleIncomingWrite();
    void finalizeIncoming();
    void clearIncoming(bool abandonExpected);
    void resetOutgoing();

    AssetTransferContext& context_;

    std::shared_ptr<IncomingWorkerState> incomingWorkerState_;
    jam2::application::asset_chunk::ReceiveSequence incomingSequence_;
    QString incomingLooperAssetHash_;
    qint64 incomingLooperAssetBytesExpected_ = 0;
    QList<QPair<int, QByteArray>> incomingLooperAssetQueue_;
    bool incomingLooperAssetWritePending_ = false;
    bool incomingLooperAssetDonePending_ = false;
    int incomingLooperAssetDoneChunks_ = 0;
    quint64 incomingLooperAssetGeneration_ = 0;
    QTimer incomingLooperAssetTimer_;

    QList<QPair<QString, QString>> outgoingLooperAssetQueue_;
    QString outgoingLooperAssetPath_;
    QString outgoingLooperAssetHash_;
    QString outgoingLooperAssetTargetToken_;
    QString outgoingLooperAssetPendingHash_;
    QString outgoingLooperAssetPendingTargetToken_;
    bool outgoingLooperAssetValidationPending_ = false;
    qint64 outgoingLooperAssetBytes_ = 0;
    qint64 outgoingLooperAssetOffset_ = 0;
    int outgoingLooperAssetNextChunk_ = 0;
    bool outgoingLooperAssetReadPending_ = false;
    QByteArray outgoingLooperAssetPreparedChunk_;
    QElapsedTimer outgoingLooperAssetProgress_;
    QTimer outgoingLooperAssetTimer_;
    quint64 outgoingLooperAssetGeneration_ = 0;
};

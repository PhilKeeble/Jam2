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
    virtual int sessionSampleRate() const noexcept = 0;
    virtual QString assetPathForSend(const QString& hash) const = 0;
    virtual QString incomingAssetPath(const QString& hash) const = 0;
    virtual bool incomingAssetExpected(
        const QString& hash,
        const QString& sourcePeerToken) const = 0;
    virtual void abandonIncomingAsset(const QString& hash) = 0;
    virtual void acceptIncomingAsset(
        const QString& hash,
        const QString& path,
        qint64 sourceFrames) = 0;
    virtual void noteAssetProgress(
        const QString& hash,
        const QString& peerToken,
        bool receiving) = 0;
    virtual void appendAssetLog(const QString& message) = 0;
    virtual bool startAssetFileTask(
        std::function<void()> work,
        std::function<void()> complete,
        std::function<void(const QString&)> failed) = 0;
    virtual bool canQueueAssetControl(const QString& peerToken, qint64 estimatedBytes) const = 0;
    virtual bool sendAssetControl(const QString& peerToken, const QJsonObject& message) = 0;
    virtual bool sendAssetBinary(const QString& peerToken, const QByteArray& payload) = 0;
    virtual void assetWorkStateChanged(bool) {}
};

class AssetTransferService {
public:
    explicit AssetTransferService(AssetTransferContext& context);
    ~AssetTransferService();

    void handleRequest(const QJsonObject& message, const QString& sourcePeerToken);
    void queueSend(const QString& hash, const QString& targetPeerToken);
    void continueSend();
    void cancel();
    void discardOutgoingHash(const QString& hash);
    void peerDisconnected(const QString& peerToken);
    void discardIncoming();
    void resetIncoming();
    void receiveStart(const QJsonObject& message, const QString& sourcePeerToken);
    void receiveAck(const QJsonObject& message, const QString& sourcePeerToken);
    void receiveChunk(const QByteArray& payload, const QString& sourcePeerToken);
    void receiveDone(const QJsonObject& message, const QString& sourcePeerToken);

    // Private GUI-agent controls used only by native real-process tests. A gate
    // stops at an observed lifecycle boundary until the test explicitly
    // releases it; neither the gate nor start suppression changes a wire frame
    // or decoder.
    bool armAutomationPause(const QString& point, QString& error);
    bool releaseAutomationPause(QString& error);
    bool armAutomationDropOutgoingStarts(int count, QString& error);
    void clearAutomationPause();
    QJsonObject automationSnapshot() const;
    bool incomingTransferActive() const noexcept;
    bool workPending() const noexcept;

private:
    enum class AutomationPausePoint {
        None,
        OutgoingValidation,
        IncomingChunk,
        IncomingFinalize,
    };

    struct IncomingWorkerState;
    void scheduleIncomingWrite();
    void finalizeIncoming();
    void clearIncoming(bool abandonExpected);
    void resetOutgoing();
    void publishWorkState();
    bool pauseForAutomation(
        AutomationPausePoint point,
        std::function<void()> resume);
    static QString automationPausePointName(AutomationPausePoint point);

    AssetTransferContext& context_;

    std::shared_ptr<IncomingWorkerState> incomingWorkerState_;
    jam2::application::asset_chunk::ReceiveSequence incomingSequence_;
    QString incomingLooperAssetHash_;
    QString incomingLooperAssetSourceToken_;
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
    int outgoingLooperAssetAckedChunks_ = 0;
    bool outgoingLooperAssetReadPending_ = false;
    QByteArray outgoingLooperAssetPreparedChunk_;
    QElapsedTimer outgoingLooperAssetProgress_;
    QTimer outgoingLooperAssetTimer_;
    quint64 outgoingLooperAssetGeneration_ = 0;
    bool publishedWorkPending_ = false;

    AutomationPausePoint automationPauseArmed_ = AutomationPausePoint::None;
    AutomationPausePoint automationPauseActive_ = AutomationPausePoint::None;
    std::function<void()> automationPauseResume_;
    int automationDropOutgoingStartsRemaining_ = 0;
    quint64 automationDroppedOutgoingStarts_ = 0;
    QString automationLastDroppedOutgoingTargetToken_;
};

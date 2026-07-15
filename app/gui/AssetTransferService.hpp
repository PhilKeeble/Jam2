#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QByteArray>
#include <QString>
#include <QTimer>

#include <memory>

class MainWindow;
class AssetTransferService {
public:
    explicit AssetTransferService(MainWindow& window);
    ~AssetTransferService();

    void handleRequest(const QJsonObject& message, const QString& sourcePeerToken);
    void queueSend(const QString& hash, const QString& targetPeerToken);
    void continueSend();
    void cancel();
    void resetIncoming();
    void receiveStart(const QJsonObject& message, const QString& sourcePeerToken);
    void receiveChunk(const QJsonObject& message, const QString& sourcePeerToken);
    void receiveDone(const QJsonObject& message, const QString& sourcePeerToken);

private:
    struct IncomingWorkerState;
    void scheduleIncomingWrite();
    void finalizeIncoming();
    void resetOutgoing();

    MainWindow& window_;

    std::shared_ptr<IncomingWorkerState> incomingWorkerState_;
    QString incomingLooperAssetHash_;
    QString incomingLooperAssetSourceToken_;
    qint64 incomingLooperAssetBytesExpected_ = 0;
    qint64 incomingLooperAssetBytesReceived_ = 0;
    int incomingLooperAssetChunkSize_ = 0;
    int incomingLooperAssetNextChunk_ = 0;
    QList<QPair<int, QString>> incomingLooperAssetQueue_;
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

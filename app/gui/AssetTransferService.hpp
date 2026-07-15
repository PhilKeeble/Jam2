#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <QTimer>

#include <memory>

class MainWindow;
class QFile;
class QCryptographicHash;
class QTemporaryFile;

class AssetTransferService {
public:
    explicit AssetTransferService(MainWindow& window);
    ~AssetTransferService();

    void handleRequest(const QJsonObject& message, const QString& sourcePeerToken);
    void queueSend(const QString& hash, const QString& targetPeerToken);
    void continueSend();
    void resetIncoming();
    void receiveStart(const QJsonObject& message, const QString& sourcePeerToken);
    void receiveChunk(const QJsonObject& message, const QString& sourcePeerToken);
    void receiveDone(const QJsonObject& message, const QString& sourcePeerToken);

private:
    MainWindow& window_;

    std::unique_ptr<QTemporaryFile> incomingLooperAssetFile_;
    std::unique_ptr<QCryptographicHash> incomingLooperAssetHasher_;
    QString incomingLooperAssetHash_;
    QString incomingLooperAssetSourceToken_;
    qint64 incomingLooperAssetBytesExpected_ = 0;
    qint64 incomingLooperAssetBytesReceived_ = 0;
    int incomingLooperAssetChunkSize_ = 0;
    int incomingLooperAssetNextChunk_ = 0;
    QTimer incomingLooperAssetTimer_;

    QList<QPair<QString, QString>> outgoingLooperAssetQueue_;
    std::unique_ptr<QFile> outgoingLooperAssetFile_;
    QString outgoingLooperAssetHash_;
    QString outgoingLooperAssetTargetToken_;
    QString outgoingLooperAssetPendingHash_;
    QString outgoingLooperAssetPendingTargetToken_;
    bool outgoingLooperAssetValidationPending_ = false;
    qint64 outgoingLooperAssetBytes_ = 0;
    int outgoingLooperAssetNextChunk_ = 0;
    QElapsedTimer outgoingLooperAssetProgress_;
    QTimer outgoingLooperAssetTimer_;
};


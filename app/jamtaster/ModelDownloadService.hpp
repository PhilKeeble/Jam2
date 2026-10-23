#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

class QFile;
class QLockFile;
class QNetworkAccessManager;
class QNetworkReply;
class QThread;

// Owned by the JamTaster dialog. Network requests use Qt's event loop; model
// hashing, archive inspection, extraction and installation run on a worker.
class ModelDownloadService final : public QObject {
public:
    enum class State {
        Idle, Checking, FetchingMetadata, AwaitingApproval, Downloading,
        Installing, Ready, Failed, Cancelled
    };
    struct Status {
        State state = State::Idle;
        QString message;
        int percent = -1;
        qint64 received = 0;
        qint64 total = 0;
    };
    struct ModelFile {
        QString name;
        qint64 bytes = 0;
        QByteArray sha256;
    };
    struct Manifest {
        QString tag;
        QUrl archiveUrl;
        QVector<ModelFile> files;
        QByteArray sha256;
    };

    ModelDownloadService(QString installationRoot, QString modelsDirectory,
        QObject* parent = nullptr, QNetworkAccessManager* network = nullptr);
    ~ModelDownloadService() override;

    const Status& status() const { return status_; }
    bool busy() const;
    qint64 downloadBytes() const { return archiveBytes_; }
    void check();
    void download();
    void cancel();
    std::function<void()> statusChanged;

private:
    enum class Outcome { Ready, Missing, Installed, Failed, Cancelled };
    struct Result { Outcome outcome; QString error; };
    void setStatus(State state, QString message, int percent = -1,
        qint64 received = 0, qint64 total = 0);
    void postProgress(State state, QString message, int percent);
    void runWorker(std::function<Result()> work, std::function<void(Result)> done);
    void requestMetadata();
    bool readArchiveData();
    void fail(const QString& error);
    void cleanStaging();
    Result installArchive();

    QString root_;
    QString modelsDirectory_;
    QString staging_;
    Manifest manifest_;
    Status status_;
    qint64 archiveBytes_ = 0;
    QByteArray archiveHash_;
    QNetworkAccessManager* network_ = nullptr;
    QPointer<QNetworkReply> reply_;
    std::unique_ptr<QFile> archive_;
    std::unique_ptr<QLockFile> lock_;
    std::unique_ptr<QThread> worker_;
    std::atomic_bool cancelled_{false};
};

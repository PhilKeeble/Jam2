#pragma once

#include "UpdatePackage.hpp"

#include <QObject>
#include <QPointer>
#include <functional>
#include <memory>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;
class QSaveFile;

namespace jam2::update {
class Download final : public QObject {
public:
    explicit Download(QString root, QObject* parent = nullptr, QNetworkAccessManager* network = nullptr);
    ~Download() override;
    void start(const QString& version, std::optional<Metadata> approved = std::nullopt);
    void cancel();
    std::function<void(qint64, qint64)> progress;
    std::function<void(const Metadata&, const QString&)> finished;
private:
    void getMetadata(const QString& version);
    void getArchive();
    bool readArchive();
    void fail(const QString& error);
    QString root_;
    QNetworkAccessManager* network_;
    QPointer<QNetworkReply> reply_;
    Metadata metadata_;
    std::optional<Metadata> approved_;
    std::unique_ptr<QSaveFile> output_;
    qint64 received_ = 0;
    bool cancelled_ = false;
};
} // namespace jam2::update

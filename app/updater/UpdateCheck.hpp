#pragma once
#include "UpdatePackage.hpp"
#include <QObject>
#include <QPointer>
#include <functional>
#include <optional>
class QNetworkAccessManager;
class QNetworkReply;

namespace jam2::update {
struct CheckResult { std::optional<Metadata> available; QByteArray encodedMetadata; };
bool newerVersion(const QString& advertised, const QString& installed);
class Check final : public QObject {
public:
    explicit Check(QObject* parent = nullptr, QNetworkAccessManager* network = nullptr);
    ~Check() override;
    void start(const QString& installed);
    void cancel();
    std::function<void(const CheckResult&, const QString&)> finished;
private:
    void get(const QUrl& url, qint64 limit, std::function<void(QByteArray)> next);
    void fail(const QString& error);
    QNetworkAccessManager* network_;
    QPointer<QNetworkReply> reply_;
};
}

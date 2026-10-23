#include "UpdateCheck.hpp"
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <stdexcept>
#include <memory>

namespace jam2::update {
bool newerVersion(const QString& advertised, const QString& installed)
{
    if (!validVersion(advertised) || !validVersion(installed)) throw std::runtime_error("Invalid update version number.");
    const auto first = advertised.split(QLatin1Char('.'));
    const auto second = installed.split(QLatin1Char('.'));
    for (qsizetype i = 0; i < first.size(); ++i)
        if (first[i].toUInt() != second[i].toUInt()) return first[i].toUInt() > second[i].toUInt();
    return false;
}
Check::Check(QObject* parent, QNetworkAccessManager* network)
    : QObject(parent), network_(network ? network : new QNetworkAccessManager(this)) {}
Check::~Check() { finished = {}; cancel(); }
void Check::cancel()
{
    if (reply_) { auto* reply = reply_.data(); reply_.clear(); reply->disconnect(this); reply->abort(); reply->deleteLater(); }
}
void Check::fail(const QString& error) { cancel(); if (finished) finished({}, error); }
void Check::get(const QUrl& url, qint64 limit, std::function<void(QByteArray)> next)
{
    QNetworkRequest request(url);
    request.setTransferTimeout(15000);
    request.setRawHeader("User-Agent", "Jam2-update-check");
    request.setRawHeader("Cache-Control", "no-cache");
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setMaximumRedirectsAllowed(8);
    reply_ = network_->get(request);
    reply_->setReadBufferSize(limit + 1);
    auto bytes = std::make_shared<QByteArray>();
    connect(reply_, &QNetworkReply::readyRead, this, [this, bytes, limit] {
        bytes->append(reply_->readAll());
        if (bytes->size() > limit) fail(QStringLiteral("The update response is too large."));
    });
    connect(reply_, &QNetworkReply::finished, this, [this, bytes, limit, next = std::move(next)] {
        auto* reply = reply_.data();
        bytes->append(reply->readAll());
        const auto error = reply->error();
        const auto detail = reply->errorString();
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply_.clear(); reply->deleteLater();
        if (error != QNetworkReply::NoError || http != 200) {
            fail(QStringLiteral("Could not check for updates (HTTP %1): %2").arg(http).arg(detail)); return;
        }
        if (bytes->size() > limit) { fail(QStringLiteral("The update response is too large.")); return; }
        try { next(*bytes); } catch (const std::exception& exception) { fail(QString::fromUtf8(exception.what())); }
    });
}
void Check::start(const QString& installed)
{
    cancel();
    if (!validVersion(installed)) { fail(QStringLiteral("The installed version is invalid.")); return; }
    get(QUrl(QStringLiteral("https://jam2gether.com/version.txt")), 128, [this, installed](QByteArray bytes) {
        const auto version = QString::fromUtf8(bytes).trimmed();
        if (!newerVersion(version, installed)) { if (finished) finished({}, {}); return; }
        get(QUrl(QStringLiteral("https://github.com/PhilKeeble/Jam2/releases/download/v%1/%2.json").arg(version, assetName())),
            65536, [this, version](QByteArray metadata) {
                const auto parsed = parseMetadata(metadata, version);
                if (finished) finished({parsed, metadata}, {});
            });
    });
}
}

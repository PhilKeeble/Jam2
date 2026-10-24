#include "UpdateDownload.hpp"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QStorageInfo>

#include <stdexcept>

namespace jam2::update {
namespace {
QNetworkRequest request(const QUrl& url)
{
    QNetworkRequest request(url);
    request.setTransferTimeout(60000);
    request.setRawHeader("User-Agent", "Jam2-updater");
    request.setRawHeader("Cache-Control", "no-cache");
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setMaximumRedirectsAllowed(8);
    return request;
}
}

Download::Download(QString root, QObject* parent, QNetworkAccessManager* network)
    : QObject(parent), root_(std::move(root)), network_(network ? network : new QNetworkAccessManager(this)) {}

Download::~Download()
{
    finished = {};
    if (reply_) { reply_->disconnect(this); reply_->abort(); reply_->deleteLater(); }
}

void Download::start(const QString& version, std::optional<Metadata> approved)
{
    if (reply_) return;
    cancelled_ = false;
    received_ = 0;
    approved_ = std::move(approved);
    try {
        if (!validVersion(version)) throw std::runtime_error("Expected a major.minor.patch update version.");
        clearDirectory(root_, QStringLiteral("update"));
        getMetadata(version);
    } catch (const std::exception& error) { fail(QString::fromUtf8(error.what())); }
}

void Download::getMetadata(const QString& version)
{
    const QUrl url(QStringLiteral("https://github.com/PhilKeeble/Jam2/releases/download/v%1/%2.json").arg(version, assetName()));
    reply_ = network_->get(request(url));
    reply_->setReadBufferSize(65537);
    auto bytes = std::make_shared<QByteArray>();
    connect(reply_, &QNetworkReply::readyRead, this, [this, bytes] {
        bytes->append(reply_->readAll());
        if (bytes->size() > 65536) fail(QStringLiteral("The release metadata exceeds 64 KiB."));
    });
    connect(reply_, &QNetworkReply::finished, this, [this, bytes, version] {
        auto* reply = reply_.data();
        bytes->append(reply->readAll());
        const auto error = reply->error();
        const auto detail = reply->errorString();
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply_.clear();
        reply->deleteLater();
        if (cancelled_) { fail(QStringLiteral("Update download cancelled.")); return; }
        if (error != QNetworkReply::NoError || http != 200) {
            fail(QStringLiteral("Could not fetch release metadata (HTTP %1): %2").arg(http).arg(detail)); return;
        }
        try {
            metadata_ = parseMetadata(*bytes, version);
            if (approved_ && (metadata_.url != approved_->url || metadata_.bytes != approved_->bytes ||
                    metadata_.sha256 != approved_->sha256 || metadata_.modelsManifestSha256 != approved_->modelsManifestSha256))
                throw std::runtime_error("This release changed since you approved it. Check for updates again before downloading.");
            QSaveFile file(confinedPath(root_, QStringLiteral("update/package.zip.json")));
            if (!file.open(QIODevice::WriteOnly) || file.write(*bytes) != bytes->size() || !file.commit())
                throw std::runtime_error("Could not save the release metadata in update/.");
            const QStorageInfo storage(QDir(root_).filePath(QStringLiteral("update")));
            if (storage.isValid() && storage.bytesAvailable() >= 0 && storage.bytesAvailable() < metadata_.bytes * 3)
                throw std::runtime_error("Not enough free space to download and unpack this update.");
            getArchive();
        } catch (const std::exception& exception) { fail(QString::fromUtf8(exception.what())); }
    });
}

void Download::getArchive()
{
    output_ = std::make_unique<QSaveFile>(confinedPath(root_, QStringLiteral("update/package.zip")));
    if (!output_->open(QIODevice::WriteOnly)) { fail(QStringLiteral("Could not create the update download.")); return; }
    reply_ = network_->get(request(metadata_.url));
    reply_->setReadBufferSize(1024 * 1024);
    connect(reply_, &QNetworkReply::readyRead, this, [this] { readArchive(); });
    connect(reply_, &QNetworkReply::finished, this, [this] {
        if (!readArchive()) return;
        auto* reply = reply_.data();
        const auto error = reply->error();
        const auto detail = reply->errorString();
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply_.clear();
        reply->deleteLater();
        if (cancelled_ || error != QNetworkReply::NoError || http != 200 || received_ != metadata_.bytes) {
            fail(cancelled_ ? QStringLiteral("Update download cancelled.") :
                QStringLiteral("The update download did not complete (HTTP %1): %2").arg(http).arg(detail)); return;
        }
        if (!output_->commit()) { fail(QStringLiteral("Could not finish writing the update ZIP.")); return; }
        output_.reset();
        if (finished) finished(metadata_, {});
    });
}

bool Download::readArchive()
{
    const auto bytes = reply_->readAll();
    if (received_ + bytes.size() > metadata_.bytes || output_->write(bytes) != bytes.size()) {
        fail(QStringLiteral("The update exceeded its expected size or could not be saved. Check free space and permissions.")); return false;
    }
    received_ += bytes.size();
    if (progress) progress(received_, metadata_.bytes);
    return true;
}

void Download::fail(const QString& error)
{
    if (reply_) { auto* reply = reply_.data(); reply_.clear(); reply->disconnect(this); reply->abort(); reply->deleteLater(); }
    output_.reset();
    if (finished) finished(metadata_, error);
}

void Download::cancel()
{
    cancelled_ = true;
    if (reply_) reply_->abort();
}
} // namespace jam2::update

#include "ModelDownloadService.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QStorageInfo>
#include <QThread>
#include <QtEndian>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace {
constexpr qint64 metadataLimit = 64 * 1024;
constexpr qint64 chunkSize = 1024 * 1024;

void require(bool valid, const QString& message)
{
    if (!valid) throw std::runtime_error(message.toStdString());
}

bool validHash(const QByteArray& hash)
{
    return QRegularExpression(QStringLiteral("^[0-9a-f]{64}$"))
        .match(QString::fromLatin1(hash)).hasMatch();
}

QJsonObject jsonObject(const QByteArray& bytes)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    require(error.error == QJsonParseError::NoError && document.isObject(),
        QStringLiteral("The model metadata is not valid JSON."));
    return document.object();
}

// Check every existing path component before creating or writing anything.
void confinedPath(const QString& root, const QString& path, bool createDirectory)
{
    const QDir base(root);
    const QString relative = base.relativeFilePath(QDir::cleanPath(path));
    require(relative != QStringLiteral("..") && !relative.startsWith(QStringLiteral("../")) &&
        !QDir::isAbsolutePath(relative), QStringLiteral("The model path escaped the Jam2 folder."));
    require(QFileInfo(root).isDir() && !QFileInfo(root).isSymLink() && !QFileInfo(root).isJunction(),
        QStringLiteral("The Jam2 folder is missing or is a filesystem link."));
    QString current = root;
    const auto parts = relative.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (qsizetype i = 0; i < parts.size(); ++i) {
        if (parts[i] == QStringLiteral(".")) continue;
        current = QDir(current).filePath(parts[i]);
        const QFileInfo info(current);
        require(!info.isSymLink() && !info.isJunction(),
            QStringLiteral("A model path is a filesystem link: %1").arg(current));
        if (i + 1 < parts.size() || createDirectory) {
            require((info.exists() && info.isDir()) || (!info.exists() && QDir().mkdir(current)),
                QStringLiteral("Could not create the model folder: %1").arg(current));
        }
    }
}

ModelDownloadService::Manifest readManifest(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly) && file.size() <= metadataLimit,
        QStringLiteral("Jam2's models.json is missing or unreadable. Reinstall the application package."));
    const QByteArray bytes = file.readAll();
    const auto json = jsonObject(bytes);
    ModelDownloadService::Manifest manifest;
    manifest.sha256 = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
    manifest.tag = json.value(QStringLiteral("tag")).toString();
    require(json.value(QStringLiteral("schema")).toInt() == 1 &&
        json.value(QStringLiteral("archive_root")).toString() == QStringLiteral("models/") &&
        json.value(QStringLiteral("asset")).toString() == QStringLiteral("Jam2-models.zip") &&
        QRegularExpression(QStringLiteral("^models-[0-9a-f]{16}$")).match(manifest.tag).hasMatch(),
        QStringLiteral("Jam2's model manifest has an unsupported format."));
    manifest.archiveUrl = QUrl(QStringLiteral(
        "https://github.com/PhilKeeble/Jam2/releases/download/%1/Jam2-models.zip").arg(manifest.tag));
    require(json.value(QStringLiteral("url")).toString() == manifest.archiveUrl.toString(),
        QStringLiteral("The model manifest contains an unexpected download URL."));
    const auto files = json.value(QStringLiteral("files")).toArray();
    require(!files.isEmpty() && files.size() <= 32, QStringLiteral("The model manifest has no valid file list."));
    QSet<QString> names;
    qint64 total = 0;
    for (const auto& entry : files) {
        const auto object = entry.toObject();
        ModelDownloadService::ModelFile model{
            object.value(QStringLiteral("path")).toString(),
            object.value(QStringLiteral("bytes")).toInteger(),
            object.value(QStringLiteral("sha256")).toString().toLatin1()};
        require(QRegularExpression(QStringLiteral("^[a-z0-9_]+\\.onnx$"))
                .match(model.name).hasMatch() && !names.contains(model.name) &&
                model.bytes > 0 && model.bytes < 2LL * 1024 * 1024 * 1024 && validHash(model.sha256),
            QStringLiteral("Invalid model file metadata: %1").arg(model.name));
        names.insert(model.name);
        total += model.bytes;
        manifest.files.push_back(std::move(model));
    }
    require(total == json.value(QStringLiteral("unpacked_bytes")).toInteger() &&
        total < 4LL * 1024 * 1024 * 1024, QStringLiteral("Invalid model installation size."));
    return manifest;
}

QByteArray hashFile(const QString& path, const std::atomic_bool& cancelled,
    const std::function<void(qint64, qint64)>& progress = {})
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), QStringLiteral("Could not read %1: %2").arg(path, file.errorString()));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        if (cancelled.load()) return {};
        const auto bytes = file.read(chunkSize);
        require(!bytes.isEmpty(), QStringLiteral("Could not read %1: %2").arg(path, file.errorString()));
        hash.addData(bytes);
        if (progress) progress(file.pos(), file.size());
    }
    return hash.result().toHex();
}

// Inspect the ZIP before allowing the platform extractor to write files. Only
// our explicit plain-file model payload is accepted, including local headers.
void validateZip(const QString& path, const ModelDownloadService::Manifest& manifest)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), QStringLiteral("Could not open the model ZIP."));
    const qint64 tailOffset = std::max<qint64>(0, file.size() - 65557);
    require(file.seek(tailOffset), QStringLiteral("Could not inspect the model ZIP."));
    const auto tail = file.readAll();
    const qsizetype end = tail.lastIndexOf(QByteArray::fromHex("504b0506"));
    const auto u16 = [](const QByteArray& data, qsizetype offset) {
        return qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(data.constData() + offset));
    };
    const auto u32 = [](const QByteArray& data, qsizetype offset) {
        return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(data.constData() + offset));
    };
    require(end >= 0 && end + 22 <= tail.size(), QStringLiteral("The model download is not a supported ZIP."));
    const auto count = u16(tail, end + 10);
    const auto centralBytes = u32(tail, end + 12);
    const auto centralOffset = u32(tail, end + 16);
    require(u16(tail, end + 4) == 0 && u16(tail, end + 6) == 0 &&
        u16(tail, end + 8) == count && count <= manifest.files.size() * 2 + 2 &&
        centralBytes <= metadataLimit && qint64(centralOffset) + centralBytes == tailOffset + end &&
        end + 22 + u16(tail, end + 20) == tail.size(),
        QStringLiteral("Unsupported or damaged model ZIP directory."));
    require(file.seek(centralOffset), QStringLiteral("Could not inspect the model ZIP directory."));
    const auto central = file.read(centralBytes);
    require(central.size() == centralBytes, QStringLiteral("The model ZIP is truncated."));
    QSet<QString> allowed{QStringLiteral("models/MODEL_NOTICES.md")};
    for (const auto& model : manifest.files) {
        allowed.insert(QStringLiteral("models/") + model.name);
        allowed.insert(QStringLiteral("models/") + model.name + QStringLiteral(".json"));
    }
    QSet<QString> seen;
    QVector<std::pair<qint64, qint64>> ranges;
    qsizetype offset = 0;
    for (quint16 i = 0; i < count; ++i) {
        require(offset + 46 <= central.size() && u32(central, offset) == 0x02014b50,
            QStringLiteral("Damaged model ZIP entry."));
        const auto flags = u16(central, offset + 8);
        const auto method = u16(central, offset + 10);
        const auto compressed = u32(central, offset + 20);
        const auto unpacked = u32(central, offset + 24);
        const auto nameBytes = u16(central, offset + 28);
        const auto extraBytes = u16(central, offset + 30);
        const auto commentBytes = u16(central, offset + 32);
        const auto type = (u32(central, offset + 38) >> 16) & 0170000;
        const auto localOffset = u32(central, offset + 42);
        require(offset + 46 + nameBytes + extraBytes + commentBytes <= central.size(),
            QStringLiteral("Truncated model ZIP entry."));
        const auto nameData = central.mid(offset + 46, nameBytes);
        const auto name = QString::fromUtf8(nameData);
        const bool directory = name == QStringLiteral("models/");
        require(!seen.contains(name) && (directory || allowed.contains(name)) &&
            (type == 0 || type == (directory ? 0040000u : 0100000u)) &&
            (flags & 0x41) == 0 && (method == 0 || method == 8) &&
            compressed != 0xffffffff && unpacked != 0xffffffff &&
            u16(central, offset + 34) == 0,
            QStringLiteral("Unsafe or unexpected model ZIP entry: %1").arg(name));
        qint64 limit = directory ? 0 : metadataLimit;
        if (name == QStringLiteral("models/MODEL_NOTICES.md")) limit = chunkSize;
        for (const auto& model : manifest.files) {
            if (name == QStringLiteral("models/") + model.name) {
                require(unpacked == model.bytes, QStringLiteral("Incorrect model size in ZIP: %1").arg(name));
                limit = model.bytes;
            }
        }
        require(unpacked <= limit && qint64(localOffset) + 30 < centralOffset && file.seek(localOffset),
            QStringLiteral("Invalid model ZIP entry size or location."));
        const auto local = file.read(30);
        require(local.size() == 30 && u32(local, 0) == 0x04034b50u &&
            u16(local, 6) == flags && u16(local, 8) == method &&
            u16(local, 26) == nameBytes && file.read(nameBytes) == nameData,
            QStringLiteral("The model ZIP has inconsistent file headers."));
        const qint64 dataStart = qint64(localOffset) + 30 + nameBytes + u16(local, 28);
        require(dataStart + compressed <= centralOffset,
            QStringLiteral("The model ZIP has invalid compressed data."));
        ranges.push_back({localOffset, dataStart + compressed});
        seen.insert(name);
        offset += 46 + nameBytes + extraBytes + commentBytes;
    }
    require(offset == central.size() && (seen - QSet<QString>{QStringLiteral("models/")}) == allowed,
        QStringLiteral("The model ZIP is missing required files."));
    std::sort(ranges.begin(), ranges.end());
    for (qsizetype i = 1; i < ranges.size(); ++i) {
        require(ranges[i].first >= ranges[i - 1].second,
            QStringLiteral("The model ZIP contains overlapping files."));
    }
}
} // namespace

ModelDownloadService::ModelDownloadService(QString root, QString modelsDirectory,
    QObject* parent, QNetworkAccessManager* network)
    : QObject(parent), root_(QDir::cleanPath(std::move(root))),
      modelsDirectory_(QDir::cleanPath(std::move(modelsDirectory))),
      staging_(QDir(root_).filePath(QStringLiteral("temp/jamtaster-models"))),
      network_(network ? network : new QNetworkAccessManager(this))
{
}

ModelDownloadService::~ModelDownloadService()
{
    statusChanged = {};
    cancelled_.store(true);
    if (reply_) {
        reply_->disconnect(this);
        reply_->abort();
        reply_->deleteLater();
    }
    if (worker_) worker_->wait();
    cleanStaging();
}

bool ModelDownloadService::busy() const { return reply_ || worker_; }

void ModelDownloadService::setStatus(State state, QString message, int percent,
    qint64 received, qint64 total)
{
    status_ = {state, std::move(message), percent, received, total};
    if (statusChanged) statusChanged();
}

void ModelDownloadService::postProgress(State state, QString message, int percent)
{
    QMetaObject::invokeMethod(this, [this, state, message = std::move(message), percent] {
        if (!cancelled_.load()) setStatus(state, message, percent);
    }, Qt::QueuedConnection);
}

void ModelDownloadService::runWorker(std::function<Result()> work, std::function<void(Result)> done)
{
    worker_.reset(QThread::create([this, work = std::move(work), done = std::move(done)] {
        Result result{Outcome::Failed, {}};
        try { result = work(); }
        catch (const std::exception& error) { result.error = QString::fromUtf8(error.what()); }
        catch (...) { result.error = QStringLiteral("An unexpected model installation error occurred."); }
        QMetaObject::invokeMethod(this, [this, result = std::move(result), done] {
            worker_->wait();
            worker_.reset();
            if (cancelled_.load()) {
                cleanStaging();
                setStatus(State::Cancelled, QStringLiteral("Model setup cancelled. You can continue the jam."));
            } else done(result);
        }, Qt::QueuedConnection);
    }));
    worker_->start(QThread::LowPriority);
}

void ModelDownloadService::check()
{
    if (busy()) return;
    cancelled_.store(false);
    try {
        manifest_ = readManifest(QDir(root_).filePath(QStringLiteral("models.json")));
        // Do not create a model directory merely to check its contents.
        const QString relative = QDir(root_).relativeFilePath(modelsDirectory_);
        require(relative != QStringLiteral("..") && !relative.startsWith(QStringLiteral("../")) &&
            !QDir::isAbsolutePath(relative), QStringLiteral("The model folder must be inside Jam2."));
        setStatus(State::Checking, QStringLiteral("Checking JamTaster models..."), 0);
        runWorker([this] {
            for (qsizetype i = 0; i < manifest_.files.size(); ++i) {
                if (cancelled_.load()) return Result{Outcome::Cancelled, {}};
                const auto& model = manifest_.files[i];
                const QString path = QDir(modelsDirectory_).filePath(model.name);
                const QFileInfo info(path);
                if (!info.isFile() || info.isSymLink() || info.size() != model.bytes)
                    return Result{Outcome::Missing, {}};
                const auto hash = hashFile(path, cancelled_, [this, i, &model](qint64 pos, qint64) {
                    postProgress(State::Checking, QStringLiteral("Checking %1...").arg(model.name),
                        static_cast<int>((i * 100 + pos * 100 / model.bytes) / manifest_.files.size()));
                });
                if (hash != model.sha256) return Result{Outcome::Missing, {}};
            }
            return Result{Outcome::Ready, {}};
        }, [this](Result result) {
            if (result.outcome == Outcome::Ready)
                setStatus(State::Ready, QStringLiteral("JamTaster models are ready."), 100);
            else if (result.outcome == Outcome::Missing) requestMetadata();
            else fail(result.error);
        });
    } catch (const std::exception& error) { fail(QString::fromUtf8(error.what())); }
}

void ModelDownloadService::requestMetadata()
{
    setStatus(State::FetchingMetadata, QStringLiteral("Models are missing or damaged. Checking the download size..."));
    QNetworkRequest request(QUrl(manifest_.archiveUrl.toString() + QStringLiteral(".json")));
    request.setTransferTimeout(30000);
    request.setRawHeader("User-Agent", "Jam2-model-download");
    reply_ = network_->get(request);
    reply_->setReadBufferSize(metadataLimit + 1);
    auto bytes = std::make_shared<QByteArray>();
    auto oversized = std::make_shared<bool>(false);
    connect(reply_, &QNetworkReply::readyRead, this, [this, bytes, oversized] {
        bytes->append(reply_->readAll());
        if (bytes->size() > metadataLimit) { *oversized = true; reply_->abort(); }
    });
    connect(reply_, &QNetworkReply::finished, this, [this, bytes, oversized] {
        auto* reply = reply_.data();
        bytes->append(reply->readAll());
        const auto networkError = reply->error();
        const auto error = reply->errorString();
        const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply_.clear();
        reply->deleteLater();
        if (cancelled_.load()) {
            setStatus(State::Cancelled, QStringLiteral("Model setup cancelled. You can continue the jam."));
            return;
        }
        if (*oversized || bytes->size() > metadataLimit) { fail(QStringLiteral("The model download metadata is too large.")); return; }
        if (networkError != QNetworkReply::NoError || http != 200) {
            fail(QStringLiteral("Could not find or fetch the required model release (HTTP %1): %2. Check your connection and retry.").arg(http).arg(error));
            return;
        }
        try {
            const auto json = jsonObject(*bytes);
            archiveBytes_ = json.value(QStringLiteral("bytes")).toInteger();
            archiveHash_ = json.value(QStringLiteral("sha256")).toString().toLatin1();
            require(json.value(QStringLiteral("schema")).toInt() == 1 &&
                json.value(QStringLiteral("channel")).toString() == QStringLiteral("models") &&
                json.value(QStringLiteral("tag")).toString() == manifest_.tag &&
                json.value(QStringLiteral("models_tag")).toString() == manifest_.tag &&
                json.value(QStringLiteral("asset")).toString() == QStringLiteral("Jam2-models.zip") &&
                json.value(QStringLiteral("url")).toString() == manifest_.archiveUrl.toString() &&
                json.value(QStringLiteral("models_manifest_sha256")).toString().toLatin1() == manifest_.sha256 &&
                archiveBytes_ > 0 && archiveBytes_ < 2LL * 1024 * 1024 * 1024 && validHash(archiveHash_),
                QStringLiteral("The model release metadata does not match this Jam2 build. Rebuild/upload the matching model package or reinstall Jam2."));
            setStatus(State::AwaitingApproval,
                QStringLiteral("JamTaster needs a %1 MB model download. Models stay in this Jam2 folder; normal jamming can continue.")
                    .arg(static_cast<double>(archiveBytes_) / 1'000'000.0, 0, 'f', 1), 0);
        } catch (const std::exception& errorMessage) { fail(QString::fromUtf8(errorMessage.what())); }
    });
}

void ModelDownloadService::download()
{
    if (busy() || status_.state != State::AwaitingApproval) return;
    try {
        confinedPath(root_, QDir(root_).filePath(QStringLiteral("temp")), true);
        confinedPath(root_, QDir(root_).filePath(QStringLiteral("temp/jamtaster-models.lock")), false);
        lock_ = std::make_unique<QLockFile>(QDir(root_).filePath(QStringLiteral("temp/jamtaster-models.lock")));
        require(lock_->tryLock(), QStringLiteral("Another Jam2 instance is already setting up models. Close it and retry."));
        confinedPath(root_, staging_, true);
        require(QDir(staging_).removeRecursively(), QStringLiteral("Could not clear the previous model download."));
        require(QDir().mkpath(staging_), QStringLiteral("Could not create the model download folder."));
        confinedPath(root_, modelsDirectory_, true);
        qint64 required = archiveBytes_ + 32 * 1024 * 1024;
        for (const auto& file : manifest_.files) required += file.bytes;
        const QStorageInfo storage(staging_);
        require(!storage.isValid() || storage.bytesAvailable() < 0 || storage.bytesAvailable() >= required,
            QStringLiteral("Not enough free space to download and unpack the JamTaster models."));
        archive_ = std::make_unique<QFile>(QDir(staging_).filePath(QStringLiteral("models.zip")));
        require(archive_->open(QIODevice::WriteOnly | QIODevice::Truncate),
            QStringLiteral("Could not create the model download: %1").arg(archive_->errorString()));
        setStatus(State::Downloading, QStringLiteral("Downloading JamTaster models..."), 0, 0, archiveBytes_);
        QNetworkRequest request(manifest_.archiveUrl);
        request.setTransferTimeout(60000);
        request.setRawHeader("User-Agent", "Jam2-model-download");
        reply_ = network_->get(request);
        reply_->setReadBufferSize(chunkSize);
        connect(reply_, &QNetworkReply::readyRead, this, [this] { readArchiveData(); });
        connect(reply_, &QNetworkReply::finished, this, [this] {
            if (!readArchiveData()) return;
            auto* reply = reply_.data();
            const auto error = reply->error();
            const auto detail = reply->errorString();
            const int http = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            reply_.clear();
            reply->deleteLater();
            const auto size = archive_->size();
            const bool flushed = archive_->flush();
            archive_->close();
            archive_.reset();
            if (cancelled_.load()) {
                cleanStaging();
                setStatus(State::Cancelled, QStringLiteral("Model download cancelled. You can continue the jam."));
            } else if (error != QNetworkReply::NoError || http != 200 || size != archiveBytes_ || !flushed) {
                fail(QStringLiteral("The model download did not complete (HTTP %1): %2. Retry when your connection is available.").arg(http).arg(detail));
            } else {
                setStatus(State::Installing, QStringLiteral("Verifying the model download..."), 0);
                runWorker([this] { return installArchive(); }, [this](Result result) {
                    cleanStaging();
                    if (result.outcome == Outcome::Installed)
                        setStatus(State::Ready, QStringLiteral("JamTaster models are installed and ready."), 100);
                    else fail(result.error);
                });
            }
        });
    } catch (const std::exception& error) { fail(QString::fromUtf8(error.what())); }
}

bool ModelDownloadService::readArchiveData()
{
    const auto bytes = reply_->readAll();
    if (archive_->size() + bytes.size() > archiveBytes_ || archive_->write(bytes) != bytes.size()) {
        fail(QStringLiteral("The model download exceeded its expected size or could not be saved. Check free space and folder permissions."));
        return false;
    }
    const auto received = archive_->size();
    setStatus(State::Downloading, QStringLiteral("Downloading models: %1 / %2 MB")
        .arg(static_cast<double>(received) / 1'000'000.0, 0, 'f', 1)
        .arg(static_cast<double>(archiveBytes_) / 1'000'000.0, 0, 'f', 1),
        static_cast<int>(received * 100 / archiveBytes_), received, archiveBytes_);
    return true;
}

ModelDownloadService::Result ModelDownloadService::installArchive()
{
    const QString zip = QDir(staging_).filePath(QStringLiteral("models.zip"));
    const auto digest = hashFile(zip, cancelled_, [this](qint64 pos, qint64 total) {
        postProgress(State::Installing, QStringLiteral("Verifying the model download..."),
            static_cast<int>(pos * 20 / total));
    });
    if (cancelled_.load()) return {Outcome::Cancelled, {}};
    require(digest == archiveHash_, QStringLiteral("The downloaded model ZIP failed its SHA-256 check. Retry the download."));
    validateZip(zip, manifest_);
    postProgress(State::Installing, QStringLiteral("Unpacking JamTaster models..."), 20);
#ifdef Q_OS_WIN
    const QString tar = QDir(qEnvironmentVariable("SystemRoot", "C:/Windows"))
        .filePath(QStringLiteral("System32/tar.exe"));
#else
    const QString tar = QStringLiteral("/usr/bin/tar");
#endif
    require(QFileInfo(tar).isFile(), QStringLiteral("The system ZIP extractor is unavailable: %1").arg(tar));
    QProcess process;
    process.setProgram(tar);
    process.setArguments({QStringLiteral("-xf"), zip, QStringLiteral("-C"), staging_});
    process.start();
    require(process.waitForStarted(5000), QStringLiteral("Could not start the system ZIP extractor: %1").arg(process.errorString()));
    QElapsedTimer elapsed;
    elapsed.start();
    while (process.state() != QProcess::NotRunning && !process.waitForFinished(100)) {
        if (cancelled_.load() || elapsed.elapsed() > 300000) {
            process.kill();
            process.waitForFinished(3000);
            if (cancelled_.load()) return {Outcome::Cancelled, {}};
            throw std::runtime_error("Unpacking the JamTaster models timed out. Retry the download.");
        }
    }
    require(process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
        QStringLiteral("Could not unpack the model ZIP: %1")
            .arg(QString::fromUtf8(process.readAllStandardError().left(2048))));
    for (qsizetype i = 0; i < manifest_.files.size(); ++i) {
        const auto& model = manifest_.files[i];
        const QString path = QDir(staging_).filePath(QStringLiteral("models/") + model.name);
        require(QFileInfo(path).isFile() && !QFileInfo(path).isSymLink() && QFileInfo(path).size() == model.bytes,
            QStringLiteral("An unpacked model is missing or damaged: %1").arg(model.name));
        const auto hash = hashFile(path, cancelled_, [this, i, &model](qint64 pos, qint64) {
            postProgress(State::Installing, QStringLiteral("Verifying %1...").arg(model.name),
                25 + static_cast<int>((i * 100 + pos * 100 / model.bytes) * 65 /
                    (manifest_.files.size() * 100)));
        });
        if (cancelled_.load()) return {Outcome::Cancelled, {}};
        require(hash == model.sha256, QStringLiteral("The model %1 failed its SHA-256 check. Retry the download.").arg(model.name));
        const QString sidecarPath = path + QStringLiteral(".json");
        QFile sidecar(sidecarPath);
        require(sidecar.open(QIODevice::ReadOnly) && sidecar.size() <= metadataLimit,
            QStringLiteral("Missing model sidecar: %1").arg(model.name));
        const auto json = jsonObject(sidecar.readAll());
        require(json.value(QStringLiteral("model")).toString() == model.name &&
            json.value(QStringLiteral("bytes")).toInteger() == model.bytes &&
            json.value(QStringLiteral("sha256")).toString().toLatin1() == model.sha256,
            QStringLiteral("Model sidecar does not match %1.").arg(model.name));
    }
    // All weights verify before replacing any installed file. No backups are
    // retained; only declared model files, sidecars and the notice are touched.
    QStringList files{QStringLiteral("MODEL_NOTICES.md")};
    for (const auto& model : manifest_.files) files << model.name << model.name + QStringLiteral(".json");
    confinedPath(root_, modelsDirectory_, true);
    for (const auto& name : files) confinedPath(root_, QDir(modelsDirectory_).filePath(name), false);
    for (qsizetype i = 0; i < files.size(); ++i) {
        if (cancelled_.load()) return {Outcome::Cancelled, {}};
        const QString source = QDir(staging_).filePath(QStringLiteral("models/") + files[i]);
        const QString destination = QDir(modelsDirectory_).filePath(files[i]);
        require(!QFileInfo::exists(destination) || QFile::remove(destination),
            QStringLiteral("Could not replace model file %1. Check folder permissions and close other Jam2 instances.").arg(files[i]));
        require(QFile::rename(source, destination), QStringLiteral("Could not install model file %1. Check folder permissions.").arg(files[i]));
        postProgress(State::Installing, QStringLiteral("Installing JamTaster models..."),
            90 + static_cast<int>((i + 1) * 10 / files.size()));
    }
    return {Outcome::Installed, {}};
}

void ModelDownloadService::cleanStaging()
{
    if (archive_) { archive_->close(); archive_.reset(); }
    if (lock_ && lock_->isLocked()) {
        // This instance owns this staging folder only while holding its lock.
        QDir(staging_).removeRecursively();
    }
    lock_.reset();
}

void ModelDownloadService::fail(const QString& error)
{
    if (reply_) {
        auto* reply = reply_.data();
        reply_.clear();
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    cleanStaging();
    setStatus(State::Failed, error + QStringLiteral(" You can close JamTaster and continue the jam."));
}

void ModelDownloadService::cancel()
{
    cancelled_.store(true);
    if (reply_) reply_->abort();
    else if (!worker_) {
        cleanStaging();
        if (status_.state != State::Ready)
            setStatus(State::Cancelled, QStringLiteral("Model setup cancelled. You can continue the jam."));
    }
}

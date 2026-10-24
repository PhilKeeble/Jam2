#include "UpdatePackage.hpp"
#include "UpdaterPlatform.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include <miniz.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <stdexcept>

namespace jam2::update {
namespace {
constexpr qint64 manifestLimit = 8 * 1024 * 1024;

void require(bool condition, const QString& error)
{
    if (!condition) throw std::runtime_error(error.toStdString());
}

void checkCancelled(const std::atomic_bool& cancelled)
{
    require(!cancelled.load(), QStringLiteral("Update cancelled."));
}

bool hashValid(const QByteArray& hash)
{
    return QRegularExpression(QStringLiteral("^[0-9a-f]{64}$"))
        .match(QString::fromLatin1(hash)).hasMatch();
}

QJsonObject object(const QByteArray& bytes)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    require(error.error == QJsonParseError::NoError && document.isObject(),
        QStringLiteral("Invalid update JSON: %1").arg(error.errorString()));
    return document.object();
}

QByteArray readSmall(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly) && file.size() <= manifestLimit,
        QStringLiteral("Could not read update metadata: %1").arg(path));
    return file.readAll();
}

bool safeRelative(const QString& path)
{
    if (path.isEmpty() || path.size() > 2048 || QDir::isAbsolutePath(path) ||
        path.contains(QRegularExpression(QStringLiteral("[\\\\:\\x00-\\x1f<>\"|?*]")))) return false;
    const auto parts = path.split(QLatin1Char('/'));
    for (const auto& part : parts) {
        if (part.isEmpty() || part == QStringLiteral(".") || part == QStringLiteral("..") ||
            part.endsWith(QLatin1Char('.')) || part.endsWith(QLatin1Char(' ')) ||
            QRegularExpression(QStringLiteral("^(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])(?:\\.|$)"),
                QRegularExpression::CaseInsensitiveOption).match(part).hasMatch()) return false;
    }
    return true;
}

bool ownedPath(const QString& path, const QString& targetPlatform)
{
    if (!safeRelative(path) || path.endsWith(QStringLiteral(".onnx"), Qt::CaseInsensitive)) return false;
    const QString lower = path.toLower();
    const auto parts = lower.split(QLatin1Char('/'));
    static const QSet<QString> mutableFolders{
        QStringLiteral("settings"), QStringLiteral("temp"), QStringLiteral("update"),
        QStringLiteral("tracks"), QStringLiteral("songs"), QStringLiteral("logs"),
        QStringLiteral("captures"), QStringLiteral("cache"), QStringLiteral("jam_recordings"),
        QStringLiteral("received_tracks"), QStringLiteral("models")};
    for (const auto& part : parts) if (mutableFolders.contains(part)) return false;
    if (parts.size() == 1) {
        static const QSet<QString> common{QStringLiteral("license"), QStringLiteral("third_party_notices.md"),
            QStringLiteral("version.txt"), QStringLiteral("models.json"), QStringLiteral("qt.conf")};
        return common.contains(lower) || (targetPlatform == QStringLiteral("windows") &&
            (lower == QStringLiteral("jam2.exe") || lower.endsWith(QStringLiteral(".dll"))));
    }
    if (lower.startsWith(QStringLiteral("licenses/"))) return true;
    if (targetPlatform == QStringLiteral("macos")) return path.startsWith(QStringLiteral("Jam2.app/"));
    return lower.startsWith(QStringLiteral("platforms/")) || lower.startsWith(QStringLiteral("tls/")) ||
        lower.startsWith(QStringLiteral("components/updater/")) ||
        lower.startsWith(QStringLiteral("components/pluginhost/")) ||
        lower.startsWith(QStringLiteral("components/jamtaster/"));
}

void validateLink(const QString& path, const QString& target)
{
    require(!target.isEmpty() && target.size() <= 2048 && !QDir::isAbsolutePath(target) &&
        !target.contains(QLatin1Char('\\')) && !target.contains(QLatin1Char(':')) &&
        !target.contains(QRegularExpression(QStringLiteral("[\\x00-\\x1f]"))),
        QStringLiteral("Unsafe update symlink: %1").arg(path));
    const QString resolved = QDir::cleanPath(QFileInfo(path).path() + QLatin1Char('/') + target);
    require(path.startsWith(QStringLiteral("Jam2.app/")) && resolved.startsWith(QStringLiteral("Jam2.app/")),
        QStringLiteral("An update symlink escapes the app bundle: %1").arg(path));
}

std::filesystem::path nativePath(const QString& value)
{
#ifdef Q_OS_WIN
    return std::filesystem::path(value.toStdWString());
#else
    return std::filesystem::path(value.toUtf8().constData());
#endif
}

void atomicWrite(const QString& path, const QByteArray& bytes)
{
    QSaveFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit(),
        QStringLiteral("Could not commit updated metadata: %1").arg(path));
}

class Archive {
public:
    explicit Archive(const QString& path) : file_(path)
    {
        require(file_.open(QIODevice::ReadOnly), QStringLiteral("Could not open the update ZIP."));
        zip_.m_pIO_opaque = &file_;
        zip_.m_pRead = [](void* opaque, mz_uint64 offset, void* buffer, size_t bytes) -> size_t {
            auto& file = *static_cast<QFile*>(opaque);
            if (offset > static_cast<mz_uint64>(file.size()) || !file.seek(static_cast<qint64>(offset))) return 0;
            const qint64 result = file.read(static_cast<char*>(buffer), static_cast<qint64>(bytes));
            return result < 0 ? 0 : static_cast<size_t>(result);
        };
        if (!mz_zip_reader_init(&zip_, static_cast<mz_uint64>(file_.size()), 0)) {
            const auto detail = error();
            mz_zip_reader_end(&zip_);
            throw std::runtime_error(QStringLiteral("Could not read the update ZIP: %1").arg(detail).toStdString());
        }
    }
    ~Archive() { mz_zip_reader_end(&zip_); }
    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;
    mz_zip_archive& zip() { return zip_; }
    QString error() { return QString::fromLatin1(mz_zip_get_error_string(mz_zip_peek_last_error(&zip_))); }
    QByteArray small(mz_uint index, qint64 limit)
    {
        mz_zip_archive_file_stat stat{};
        require(mz_zip_reader_file_stat(&zip_, index, &stat) && stat.m_uncomp_size <= static_cast<mz_uint64>(limit),
            QStringLiteral("Update ZIP metadata exceeds its size limit."));
        QByteArray result(static_cast<qsizetype>(stat.m_uncomp_size), Qt::Uninitialized);
        require(mz_zip_reader_extract_to_mem(&zip_, index, result.data(), static_cast<size_t>(result.size()), 0),
            QStringLiteral("Could not extract update metadata: %1").arg(error()));
        return result;
    }
private:
    QFile file_;
    mz_zip_archive zip_{};
};
} // namespace

QString platform()
{
#ifdef Q_OS_WIN
    return QStringLiteral("windows");
#else
    return QStringLiteral("macos");
#endif
}
QString assetName() { return QStringLiteral("Jam2-%1.zip").arg(platform()); }
QString helperRelativePath()
{
#ifdef Q_OS_WIN
    return QStringLiteral("components/updater/jam2-updater.exe");
#else
    return QStringLiteral("Jam2.app/Contents/Helpers/jam2-updater");
#endif
}
QString executableRelativePath()
{
#ifdef Q_OS_WIN
    return QStringLiteral("jam2.exe");
#else
    return QStringLiteral("Jam2.app/Contents/MacOS/jam2");
#endif
}
bool validVersion(const QString& version)
{
    if (!QRegularExpression(QStringLiteral("^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)$"))
            .match(version).hasMatch()) return false;
    for (const auto& part : version.split(QLatin1Char('.'))) {
        bool ok = false;
        const auto value = part.toUInt(&ok);
        if (!ok || value > 65535) return false;
    }
    return true;
}

Metadata parseMetadata(const QByteArray& bytes, const QString& version)
{
    require(bytes.size() <= 65536 && validVersion(version), QStringLiteral("Invalid update version or metadata size."));
    const auto json = object(bytes);
    Metadata result{version, QUrl(json.value(QStringLiteral("url")).toString()),
        json.value(QStringLiteral("bytes")).toInteger(),
        json.value(QStringLiteral("sha256")).toString().toLatin1(),
        json.value(QStringLiteral("models_manifest_sha256")).toString().toLatin1()};
    const QString tag = QStringLiteral("v") + version;
    const QUrl expected(QStringLiteral("https://github.com/PhilKeeble/Jam2/releases/download/%1/%2").arg(tag, assetName()));
    require(json.value(QStringLiteral("schema")).toInt() == 1 &&
        json.value(QStringLiteral("version")).toString() == version &&
        json.value(QStringLiteral("tag")).toString() == tag &&
        json.value(QStringLiteral("channel")).toString() == QStringLiteral("stable") &&
        json.value(QStringLiteral("asset")).toString() == assetName() && result.url == expected &&
        result.bytes > 0 && result.bytes <= 2LL * 1024 * 1024 * 1024 &&
        hashValid(result.sha256) && hashValid(result.modelsManifestSha256),
        QStringLiteral("Release metadata does not match the requested Jam2 version and platform."));
    return result;
}

Manifest parseManifest(const QByteArray& bytes, bool requireHelper)
{
    require(bytes.size() <= manifestLimit, QStringLiteral("The package manifest is too large."));
    const auto json = object(bytes);
    Manifest result;
    result.version = json.value(QStringLiteral("version")).toString();
    result.platform = json.value(QStringLiteral("platform")).toString();
    result.encoded = bytes;
    require(json.value(QStringLiteral("schema")).toInt() == 1 && validVersion(result.version) &&
        result.platform == platform() && json.value(QStringLiteral("models_manifest")).toString() == QStringLiteral("models.json"),
        QStringLiteral("Unsupported package manifest version or platform."));
    for (const auto& architecture : json.value(QStringLiteral("architectures")).toArray())
        result.architectures.push_back(architecture.toString());
#ifdef Q_OS_WIN
    require(result.architectures == QStringList{QStringLiteral("x86_64")}, QStringLiteral("The update is not a Windows x64 build."));
#else
    require(result.architectures.size() == 2 && result.architectures.contains(QStringLiteral("arm64")) &&
        result.architectures.contains(QStringLiteral("x86_64")), QStringLiteral("The update is not a universal macOS build."));
#endif
    const auto files = json.value(QStringLiteral("files")).toArray();
    require(!files.isEmpty() && files.size() <= 20000, QStringLiteral("Invalid package file count."));
    QSet<QString> paths;
    qint64 total = 0;
    for (const auto& entry : files) {
        const auto value = entry.toObject();
        File file{value.value(QStringLiteral("path")).toString(), value.value(QStringLiteral("bytes")).toInteger(-1),
            value.value(QStringLiteral("sha256")).toString().toLatin1(), value.value(QStringLiteral("link")).toString()};
        const auto key = file.path.toCaseFolded();
        require(ownedPath(file.path, result.platform) && !paths.contains(key),
            QStringLiteral("Unsafe, duplicate or user-data path in update: %1").arg(file.path));
        paths.insert(key);
        if (value.contains(QStringLiteral("link"))) {
            require(result.platform == QStringLiteral("macos"), QStringLiteral("Windows updates cannot contain symlinks."));
            validateLink(file.path, file.link);
        } else {
            require(file.bytes >= 0 && file.bytes <= 2LL * 1024 * 1024 * 1024 && hashValid(file.sha256),
                QStringLiteral("Invalid file size or SHA-256: %1").arg(file.path));
            total += file.bytes;
            require(total <= 4LL * 1024 * 1024 * 1024, QStringLiteral("The unpacked update is too large."));
        }
        result.files.push_back(std::move(file));
    }
    for (const auto& file : result.files) {
        QString parent = QFileInfo(file.path).path();
        while (parent != QStringLiteral(".")) {
            require(!paths.contains(parent.toCaseFolded()), QStringLiteral("A package file is also used as a parent directory."));
            parent = QFileInfo(parent).path();
        }
    }
    for (const auto& path : {executableRelativePath(), QStringLiteral("version.txt"), QStringLiteral("models.json")})
        require(paths.contains(path.toCaseFolded()),
            QStringLiteral("Update %1 is missing required file: %2. Repackage this release with the current build-release script.")
                .arg(result.version, path));
    require(!requireHelper || paths.contains(helperRelativePath().toCaseFolded()),
        QStringLiteral("Update %1 does not include the updater (%2). Repackage this release with the current build-release script.")
            .arg(result.version, helperRelativePath()));
    return result;
}

QString confinedPath(const QString& root, const QString& relative, bool createParents)
{
    require(QFileInfo(root).isAbsolute() && QFileInfo(root).isDir() &&
        !QFileInfo(root).isSymLink() && !QFileInfo(root).isJunction() && safeRelative(relative),
        QStringLiteral("Invalid Jam2 root or relative update path."));
    QString current = QDir::cleanPath(root);
    const auto parts = relative.split(QLatin1Char('/'));
    for (qsizetype i = 0; i < parts.size(); ++i) {
        current = QDir(current).filePath(parts[i]);
        const QFileInfo info(current);
        require(!info.isSymLink() && !info.isJunction(), QStringLiteral("Update path contains a filesystem link: %1").arg(current));
        if (i + 1 < parts.size()) {
            require(!info.exists() || info.isDir(), QStringLiteral("Update parent is not a directory: %1").arg(current));
            if (createParents && !info.exists()) require(QDir().mkdir(current), QStringLiteral("Could not create %1").arg(current));
        }
    }
    return current;
}

void clearDirectory(const QString& root, const QString& relative)
{
    const QString path = confinedPath(root, relative, true);
    const QFileInfo info(path);
    require(!info.exists() || (info.isDir() && QDir(path).removeRecursively()),
        QStringLiteral("Could not clear %1. Another updater may still be running.").arg(path));
    require(QDir().mkpath(path), QStringLiteral("Could not create %1").arg(path));
}

QByteArray sha256File(const QString& path, const std::atomic_bool* cancelled)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), QStringLiteral("Could not read %1: %2").arg(path, file.errorString()));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        if (cancelled) checkCancelled(*cancelled);
        const auto data = file.read(1024 * 1024);
        require(!data.isEmpty(), QStringLiteral("Could not finish reading %1").arg(path));
        hash.addData(data);
    }
    return hash.result().toHex();
}

Manifest extractPackage(const QString& root, const Metadata& metadata,
    const std::atomic_bool& cancelled, const Progress& progress)
{
    const QString archivePath = confinedPath(root, QStringLiteral("update/package.zip"));
    require(QFileInfo(archivePath).size() == metadata.bytes && sha256File(archivePath, &cancelled) == metadata.sha256,
        QStringLiteral("The update ZIP failed its size or SHA-256 check."));
    Archive archive(archivePath);
    auto& zip = archive.zip();
    const int manifestIndex = mz_zip_reader_locate_file(&zip, "package-manifest.json", nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE);
    require(manifestIndex >= 0, QStringLiteral("The ZIP has no package-manifest.json."));
    const auto manifest = parseManifest(archive.small(static_cast<mz_uint>(manifestIndex), manifestLimit));
    require(manifest.version == metadata.version, QStringLiteral("The ZIP's version does not match the requested update."));
    QHash<QString, const File*> expected;
    QSet<QString> directories;
    for (const auto& file : manifest.files) {
        expected.insert(file.path, &file);
        QString parent = QFileInfo(file.path).path();
        while (parent != QStringLiteral(".")) { directories.insert(parent); parent = QFileInfo(parent).path(); }
    }
    const auto count = mz_zip_reader_get_num_files(&zip);
    require(count <= 40000, QStringLiteral("The ZIP contains too many entries."));
    QSet<QString> seen;
    struct Entry { mz_uint index; QString path; const File* file; mz_uint32 mode; };
    QVector<Entry> entries;
    for (mz_uint index = 0; index < count; ++index) {
        checkCancelled(cancelled);
        mz_zip_archive_file_stat stat{};
        require(mz_zip_reader_file_stat(&zip, index, &stat), QStringLiteral("Damaged ZIP entry."));
        const mz_uint length = mz_zip_reader_get_filename(&zip, index, nullptr, 0);
        require(length > 0 && length <= 2049, QStringLiteral("Invalid ZIP filename length."));
        QByteArray name(static_cast<qsizetype>(length), '\0');
        require(mz_zip_reader_get_filename(&zip, index, name.data(), length) == length, QStringLiteral("Could not read ZIP filename."));
        name.chop(1);
        const QString path = QString::fromUtf8(name);
        require(path.toUtf8() == name && !stat.m_is_encrypted && stat.m_is_supported,
            QStringLiteral("Unsupported ZIP entry: %1").arg(path));
        QString normal = path;
        if (stat.m_is_directory && normal.endsWith(QLatin1Char('/'))) normal.chop(1);
        require(safeRelative(normal), QStringLiteral("Unsafe ZIP path: %1").arg(path));
#ifdef Q_OS_MACOS
        // ditto's AppleDouble metadata is not application payload. Permissions
        // and framework symlinks are reconstructed from the real ZIP entries.
        if (normal == QStringLiteral("__MACOSX") || normal.startsWith(QStringLiteral("__MACOSX/"))) continue;
#endif
        require(!seen.contains(normal.toCaseFolded()), QStringLiteral("Duplicate ZIP path: %1").arg(path));
        seen.insert(normal.toCaseFolded());
        if (stat.m_is_directory) {
            require(directories.contains(normal), QStringLiteral("Unexpected ZIP directory: %1").arg(path));
            continue;
        }
        if (path == QStringLiteral("package-manifest.json")) continue;
        require(expected.contains(path), QStringLiteral("Undeclared file in update ZIP: %1").arg(path));
        const auto* file = expected.value(path);
        const auto kind = (stat.m_external_attr >> 16) & 0170000u;
        require(file->link.isEmpty()
            ? (kind == 0 || kind == 0100000u) && stat.m_uncomp_size == static_cast<mz_uint64>(file->bytes)
            : kind == 0120000u && stat.m_uncomp_size <= 2048,
            QStringLiteral("ZIP file type or size differs from manifest: %1").arg(path));
        entries.push_back({index, path, file, stat.m_external_attr >> 16});
    }
    require(entries.size() == manifest.files.size(), QStringLiteral("The ZIP is missing package files."));
    clearDirectory(root, QStringLiteral("update/package"));
    const QString destination = QDir(root).filePath(QStringLiteral("update/package"));
    for (qsizetype i = 0; i < entries.size(); ++i) {
        checkCancelled(cancelled);
        const auto& entry = entries[i];
        if (!entry.file->link.isEmpty()) continue;
        const QString path = confinedPath(destination, entry.path, true);
        QFile output(path);
        require(output.open(QIODevice::WriteOnly | QIODevice::Truncate), QStringLiteral("Could not create staged file: %1").arg(entry.path));
        struct Writer { QFile* file; const std::atomic_bool* cancelled; qint64 limit; qint64 written = 0; };
        Writer writer{&output, &cancelled, entry.file->bytes};
        const auto callback = [](void* opaque, mz_uint64 offset, const void* data, size_t size) noexcept -> size_t {
            auto& state = *static_cast<Writer*>(opaque);
            try {
                if (state.cancelled->load() || offset != static_cast<mz_uint64>(state.written) ||
                    size > static_cast<size_t>(state.limit - state.written)) return 0;
                const auto written = state.file->write(static_cast<const char*>(data), static_cast<qint64>(size));
                if (written < 0) return 0;
                state.written += written;
                return static_cast<size_t>(written);
            } catch (...) { return 0; }
        };
        require(mz_zip_reader_extract_to_callback(&zip, entry.index, callback, &writer, 0) &&
            writer.written == entry.file->bytes && output.flush(),
            QStringLiteral("Could not unpack %1: %2").arg(entry.path, archive.error()));
        output.close();
#ifndef Q_OS_WIN
        const auto mode = entry.mode & 0777u;
        if (mode) std::filesystem::permissions(nativePath(path), static_cast<std::filesystem::perms>(mode));
#endif
        require(sha256File(path, &cancelled) == entry.file->sha256, QStringLiteral("Staged file failed SHA-256 verification: %1").arg(entry.path));
        if (progress) progress(QStringLiteral("Unpacking %1").arg(entry.path), static_cast<int>((i + 1) * 100 / entries.size()));
    }
    // Links are created after all ordinary files so extraction never writes
    // through a link from the archive.
    for (const auto& entry : entries) {
        if (entry.file->link.isEmpty()) continue;
        checkCancelled(cancelled);
        const auto bytes = archive.small(entry.index, 2048);
        require(bytes == entry.file->link.toUtf8(), QStringLiteral("ZIP symlink differs from manifest: %1").arg(entry.path));
        const auto path = confinedPath(destination, entry.path, true);
        std::filesystem::create_symlink(nativePath(entry.file->link), nativePath(path));
    }
    atomicWrite(QDir(destination).filePath(QStringLiteral("package-manifest.json")), manifest.encoded);
    require(sha256File(QDir(destination).filePath(QStringLiteral("models.json")), &cancelled) == metadata.modelsManifestSha256,
        QStringLiteral("The package's model manifest does not match the release metadata."));
    return verifyPackage(root, metadata.version, cancelled, progress);
}

Manifest verifyPackage(const QString& root, const QString& version,
    const std::atomic_bool& cancelled, const Progress& progress)
{
    const auto package = confinedPath(root, QStringLiteral("update/package"));
    require(QFileInfo(package).isDir(), QStringLiteral("There is no staged update/package folder."));
    const auto manifestPath = confinedPath(package, QStringLiteral("package-manifest.json"));
    const auto manifest = parseManifest(readSmall(manifestPath));
    require(manifest.version == version, QStringLiteral("Staged package version does not match the requested update."));
    for (qsizetype i = 0; i < manifest.files.size(); ++i) {
        checkCancelled(cancelled);
        const auto& file = manifest.files[i];
        const QString path = QDir(package).filePath(file.path);
        if (file.link.isEmpty()) {
            confinedPath(package, file.path);
            require(QFileInfo(path).isFile() && QFileInfo(path).size() == file.bytes && sha256File(path, &cancelled) == file.sha256,
                QStringLiteral("Staged file is missing or failed verification: %1").arg(file.path));
        } else {
            require(QFileInfo(path).isSymLink() && std::filesystem::read_symlink(nativePath(path)) == nativePath(file.link),
                QStringLiteral("Staged symlink does not match: %1").arg(file.path));
            const QString resolved = QFileInfo(path).canonicalFilePath();
            require(resolved.startsWith(QDir(package).filePath(QStringLiteral("Jam2.app/"))),
                QStringLiteral("Staged symlink escapes the app bundle: %1").arg(file.path));
        }
        if (progress) progress(QStringLiteral("Verifying %1").arg(file.path), static_cast<int>((i + 1) * 100 / manifest.files.size()));
    }
    require(QString::fromUtf8(readSmall(QDir(package).filePath(QStringLiteral("version.txt")))).trimmed() == version,
        QStringLiteral("Staged version.txt does not match the package manifest."));
    verifyMacSignature(QDir(package).filePath(QStringLiteral("Jam2.app")));
    return manifest;
}

void installPackage(const QString& root, const Manifest& expected,
    const std::atomic_bool& cancelled, const Progress& progress)
{
    const auto manifest = verifyPackage(root, expected.version, cancelled, progress);
    require(manifest.encoded == expected.encoded, QStringLiteral("The staged package changed while waiting for Jam2 to exit."));
    require(QFileInfo(confinedPath(root, executableRelativePath())).isFile(), QStringLiteral("The selected folder is not a Jam2 installation."));
    QVector<QString> obsolete;
    const auto oldPath = confinedPath(root, QStringLiteral("package-manifest.json"));
    QSet<QString> newPaths;
    for (const auto& file : manifest.files) newPaths.insert(file.path.toCaseFolded());
    if (QFileInfo(oldPath).exists()) {
        const auto old = parseManifest(readSmall(oldPath), false);
        for (const auto& file : old.files) if (!newPaths.contains(file.path.toCaseFolded())) obsolete.push_back(file.path);
    }
    // Preflight every destination before replacing anything. A second Jam2 or
    // a lingering worker on Windows produces an error instead of being killed.
    for (const auto& file : manifest.files) {
#ifdef Q_OS_MACOS
        if (file.path.startsWith(QStringLiteral("Jam2.app/"))) continue;
#endif
        const auto path = confinedPath(root, file.path);
        require(!QFileInfo(path).exists() || QFileInfo(path).isFile(), QStringLiteral("A destination is not a regular file: %1").arg(file.path));
        preflightWritableFile(path);
    }
    for (const auto& relative : obsolete) {
#ifdef Q_OS_MACOS
        if (relative.startsWith(QStringLiteral("Jam2.app/"))) continue;
#endif
        preflightWritableFile(confinedPath(root, relative));
    }
    preflightWritableFile(oldPath);
    const QString package = QDir(root).filePath(QStringLiteral("update/package"));
    checkCancelled(cancelled);
#ifdef Q_OS_MACOS
    const QString oldBundle = confinedPath(root, QStringLiteral("Jam2.app"));
    require(!QFileInfo(oldBundle).exists() || QDir(oldBundle).removeRecursively(), QStringLiteral("Could not replace Jam2.app. Check folder permissions."));
    require(QDir().rename(QDir(package).filePath(QStringLiteral("Jam2.app")), oldBundle), QStringLiteral("Could not install the new Jam2.app."));
#endif
    for (qsizetype i = 0; i < manifest.files.size(); ++i) {
        const auto& file = manifest.files[i];
        if (file.path == QStringLiteral("version.txt")) continue;
#ifdef Q_OS_MACOS
        if (file.path.startsWith(QStringLiteral("Jam2.app/"))) continue;
#endif
        const auto destination = confinedPath(root, file.path, true);
        require(!QFileInfo(destination).exists() || QFile::remove(destination), QStringLiteral("Could not replace %1. Close other Jam2 instances and retry.").arg(file.path));
        require(QFile::copy(QDir(package).filePath(file.path), destination), QStringLiteral("Could not install %1. Check free space and folder permissions.").arg(file.path));
        if (progress) progress(QStringLiteral("Installing %1").arg(file.path), static_cast<int>((i + 1) * 100 / manifest.files.size()));
    }
    for (const auto& relative : obsolete) {
#ifdef Q_OS_MACOS
        if (relative.startsWith(QStringLiteral("Jam2.app/"))) continue;
#endif
        const auto path = confinedPath(root, relative);
        require(!QFileInfo::exists(path) || QFile::remove(path), QStringLiteral("Could not remove obsolete application file: %1").arg(relative));
    }
    atomicWrite(oldPath, manifest.encoded);
    atomicWrite(confinedPath(root, QStringLiteral("version.txt")), expected.version.toUtf8() + '\n');
}

} // namespace jam2::update

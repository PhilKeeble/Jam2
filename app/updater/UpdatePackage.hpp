#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

#include <atomic>
#include <functional>

namespace jam2::update {

struct Metadata {
    QString version;
    QUrl url;
    qint64 bytes = 0;
    QByteArray sha256;
    QByteArray modelsManifestSha256;
};

struct File {
    QString path;
    qint64 bytes = 0;
    QByteArray sha256;
    QString link;
};

struct Manifest {
    QString version;
    QString platform;
    QStringList architectures;
    QVector<File> files;
    QByteArray encoded;
};

using Progress = std::function<void(const QString&, int)>;

QString platform();
QString assetName();
QString helperRelativePath();
QString executableRelativePath();
bool validVersion(const QString& version);
Metadata parseMetadata(const QByteArray& bytes, const QString& version);
Manifest parseManifest(const QByteArray& bytes, bool requireHelper = true);
QByteArray sha256File(const QString& path, const std::atomic_bool* cancelled = nullptr);

// Existing components are checked for links/junctions before any mutation.
QString confinedPath(const QString& root, const QString& relative, bool createParents = false);
void clearDirectory(const QString& root, const QString& relative);
Manifest extractPackage(const QString& root, const Metadata& metadata,
    const std::atomic_bool& cancelled, const Progress& progress = {});
Manifest verifyPackage(const QString& root, const QString& version,
    const std::atomic_bool& cancelled, const Progress& progress = {});
void installPackage(const QString& root, const Manifest& manifest,
    const std::atomic_bool& cancelled, const Progress& progress = {});

} // namespace jam2::update

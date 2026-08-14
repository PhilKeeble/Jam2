#include "LooperAssetMaterializer.hpp"

#include "TrackWorkspaceSupport.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>
#include <exception>
#include <utility>

namespace {

Qt::CaseSensitivity pathCaseSensitivity() noexcept
{
#if defined(_WIN32)
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

QString absoluteCleanPath(const QString& path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(
        canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

bool pathWithin(const QString& path, const QString& folder)
{
    const QString cleanPath = absoluteCleanPath(path);
    const QString cleanFolder = QDir(folder).absolutePath();
    return cleanPath.startsWith(
        cleanFolder + QLatin1Char('/'), pathCaseSensitivity());
}

bool samePath(const QString& first, const QString& second)
{
    return absoluteCleanPath(first).compare(
        absoluteCleanPath(second), pathCaseSensitivity()) == 0;
}

bool isSha256Hex(const QString& value)
{
    static const QRegularExpression expression(QStringLiteral("^[0-9a-f]{64}$"));
    return expression.match(value).hasMatch();
}

QString portableFileStem(QString value, const QString& fallback)
{
    value = value.trimmed();
    value.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]+")),
        QStringLiteral("_"));
    while (value.startsWith(QLatin1Char('.')) ||
           value.startsWith(QLatin1Char('_'))) {
        value.remove(0, 1);
    }
    while (value.endsWith(QLatin1Char('.')) ||
           value.endsWith(QLatin1Char('_'))) {
        value.chop(1);
    }
    if (value.isEmpty()) value = fallback;
    static const QRegularExpression windowsDeviceName(QStringLiteral(
        "^(con|prn|aux|nul|com[1-9]|lpt[1-9])$"),
        QRegularExpression::CaseInsensitiveOption);
    if (windowsDeviceName.match(value).hasMatch()) {
        value.prepend(QLatin1Char('_'));
    }
    return value.left(120);
}

QString sourcePath(
    const QString& lanePath,
    const QString& sourceProjectFolder)
{
    if (QFileInfo(lanePath).isAbsolute() || sourceProjectFolder.isEmpty()) {
        return absoluteCleanPath(lanePath);
    }
    return absoluteCleanPath(
        QDir(sourceProjectFolder).absoluteFilePath(lanePath));
}

bool copyAtomically(const QString& sourcePath, const QString& destinationPath)
{
    QFile input(sourcePath);
    QSaveFile output(destinationPath);
    if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly)) {
        return false;
    }
    constexpr qint64 kBlockBytes = 1024 * 1024;
    while (!input.atEnd()) {
        const QByteArray block = input.read(kBlockBytes);
        if ((block.isEmpty() && input.error() != QFileDevice::NoError) ||
            output.write(block) != block.size()) {
            return false;
        }
    }
    return output.commit();
}

QString existingHash(const QString& path)
{
    try {
        return readWavMetadata(path).sha256;
    } catch (const std::exception&) {
        return {};
    }
}

} // namespace

namespace jam2::gui {

QStringList rollbackLooperAssetMaterialization(
    const QStringList& createdPaths) noexcept
{
    QStringList failures;
    for (auto it = createdPaths.crbegin(); it != createdPaths.crend(); ++it) {
        if (QFileInfo::exists(*it) && !QFile::remove(*it)) failures.append(*it);
    }
    return failures;
}

LooperAssetMaterializationResult materializeLooperAssets(
    const LooperProject& sourceProject,
    const QString& sourceProjectFolder,
    const QString& targetProjectFolder)
{
    LooperAssetMaterializationResult result;
    result.project = sourceProject;
    const QString targetRoot = QDir(targetProjectFolder).absolutePath();
    if (targetProjectFolder.trimmed().isEmpty() ||
        !QFileInfo(targetProjectFolder).isAbsolute()) {
        result.error = QStringLiteral("JamJar asset folder must be an absolute path.");
        return result;
    }
    QDir target(targetRoot);
    if (!target.mkpath(QStringLiteral("imported"))) {
        result.error = QStringLiteral("Could not create the project's imported folder.");
        return result;
    }

    QHash<QString, QString> importedNameByFoldedName;
    const QDir imported(target.absoluteFilePath(QStringLiteral("imported")));
    for (const QFileInfo& entry : imported.entryInfoList(
             QDir::AllEntries | QDir::NoDotAndDotDot, QDir::Name)) {
        importedNameByFoldedName.insert(entry.fileName().toLower(), entry.fileName());
    }
    QHash<QString, QString> relativePathByHash;

    const auto fail = [&result](const QString& error) {
        const QStringList rollbackFailures =
            rollbackLooperAssetMaterialization(result.createdPaths);
        result.createdPaths = rollbackFailures;
        result.error = rollbackFailures.isEmpty()
            ? error
            : error + QStringLiteral(" Cleanup also failed for: ") +
                rollbackFailures.join(QStringLiteral(", "));
    };

    try {
        for (LooperBank& bank : result.project.banks()) {
            for (LooperLane& lane : bank.lanes) {
                if (lane.assetPath.trimmed().isEmpty()) continue;
                const QString source = sourcePath(
                    lane.assetPath, sourceProjectFolder);
                if (!isSha256Hex(lane.assetHash) ||
                    !QFileInfo(source).isFile()) {
                    fail(QStringLiteral(
                        "A lane WAV is missing or has an invalid hash: %1")
                        .arg(lane.name));
                    return result;
                }
                const WavMetadata metadata = readWavMetadata(source);
                if (metadata.sha256 != lane.assetHash) {
                    fail(QStringLiteral(
                        "A lane WAV does not match its content hash: %1")
                        .arg(lane.name));
                    return result;
                }

                QString relativePath = relativePathByHash.value(lane.assetHash);
                if (relativePath.isEmpty() && pathWithin(source, targetRoot)) {
                    relativePath = target.relativeFilePath(source);
                }
                if (relativePath.isEmpty()) {
                    const QString base = portableFileStem(
                        lane.name, QStringLiteral("Track"));
                    int suffix = 1;
                    for (;;) {
                        const QString fileName = suffix == 1
                            ? base + QStringLiteral(".wav")
                            : QStringLiteral("%1-%2.wav").arg(base).arg(suffix);
                        const QString folded = fileName.toLower();
                        const QString occupiedName =
                            importedNameByFoldedName.value(folded);
                        if (!occupiedName.isEmpty()) {
                            const QString occupiedPath = imported.absoluteFilePath(
                                occupiedName);
                            if (existingHash(occupiedPath) == lane.assetHash) {
                                relativePath = QStringLiteral("imported/") + occupiedName;
                                break;
                            }
                            ++suffix;
                            continue;
                        }
                        const QString destination = imported.absoluteFilePath(fileName);
                        if (!samePath(source, destination)) {
                            QFile reservation(destination);
                            if (!reservation.open(
                                    QIODevice::WriteOnly | QIODevice::NewOnly)) {
                                if (QFileInfo::exists(destination)) {
                                    importedNameByFoldedName.insert(folded, fileName);
                                    ++suffix;
                                    continue;
                                }
                                fail(QStringLiteral(
                                    "Could not reserve a portable WAV name for %1.")
                                    .arg(lane.name));
                                return result;
                            }
                            reservation.close();
                            result.createdPaths.append(destination);
                            if (!copyAtomically(source, destination)) {
                                fail(QStringLiteral(
                                    "Could not atomically save WAV %1.")
                                    .arg(lane.name));
                                return result;
                            }
                            if (existingHash(destination) != lane.assetHash) {
                                fail(QStringLiteral(
                                    "Saved WAV changed while it was being copied: %1")
                                    .arg(lane.name));
                                return result;
                            }
                        }
                        relativePath = QStringLiteral("imported/") + fileName;
                        importedNameByFoldedName.insert(folded, fileName);
                        break;
                    }
                }
                if (relativePath.startsWith(QStringLiteral("../")) ||
                    QFileInfo(relativePath).isAbsolute()) {
                    fail(QStringLiteral("A lane WAV resolved outside the JamJar: %1")
                        .arg(lane.name));
                    return result;
                }
                relativePathByHash.insert(lane.assetHash, relativePath);
                lane.assetPath = relativePath;
            }
        }
    } catch (const std::exception& error) {
        fail(QString::fromUtf8(error.what()));
        return result;
    } catch (...) {
        fail(QStringLiteral("Unknown failure while materializing JamJar WAVs."));
        return result;
    }
    return result;
}

} // namespace jam2::gui

#include "JamStorage.hpp"

#include "GuiPresentation.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QRegularExpression>

namespace {

Qt::CaseSensitivity pathCaseSensitivity() noexcept
{
#ifdef Q_OS_WIN
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

bool samePath(const QString& first, const QString& second) noexcept
{
    return QDir::cleanPath(first).compare(
               QDir::cleanPath(second), pathCaseSensitivity()) == 0;
}

const QStringList& adjectives()
{
    static const QStringList values{
        QStringLiteral("Amber"), QStringLiteral("Azure"), QStringLiteral("Blue"),
        QStringLiteral("Bright"), QStringLiteral("Burning"), QStringLiteral("Calm"),
        QStringLiteral("Celestial"), QStringLiteral("Copper"), QStringLiteral("Cosmic"),
        QStringLiteral("Crimson"), QStringLiteral("Dancing"), QStringLiteral("Distant"),
        QStringLiteral("Electric"), QStringLiteral("Emerald"), QStringLiteral("Falling"),
        QStringLiteral("Flying"), QStringLiteral("Frozen"), QStringLiteral("Gentle"),
        QStringLiteral("Golden"), QStringLiteral("Hidden"), QStringLiteral("Indigo"),
        QStringLiteral("Infinite"), QStringLiteral("Ivory"), QStringLiteral("Lunar"),
        QStringLiteral("Midnight"), QStringLiteral("Neon"), QStringLiteral("Obsidian"),
        QStringLiteral("Orbiting"), QStringLiteral("Pink"), QStringLiteral("Purple"),
        QStringLiteral("Quiet"), QStringLiteral("Radiant"), QStringLiteral("Red"),
        QStringLiteral("Rising"), QStringLiteral("Running"), QStringLiteral("Silver"),
        QStringLiteral("Solar"), QStringLiteral("Spinning"), QStringLiteral("Starlit"),
        QStringLiteral("Teal"), QStringLiteral("Ultraviolet"), QStringLiteral("Velvet"),
        QStringLiteral("Violet"), QStringLiteral("Wandering"), QStringLiteral("White"),
        QStringLiteral("Wild"), QStringLiteral("Yellow"), QStringLiteral("Young"),
    };
    return values;
}

const QStringList& nouns()
{
    static const QStringList values{
        QStringLiteral("Asteroid"), QStringLiteral("Aurora"), QStringLiteral("Comet"),
        QStringLiteral("Cosmos"), QStringLiteral("Crater"), QStringLiteral("Eclipse"),
        QStringLiteral("Galaxy"), QStringLiteral("Horizon"), QStringLiteral("Meteor"),
        QStringLiteral("Moon"), QStringLiteral("Nebula"), QStringLiteral("Nova"),
        QStringLiteral("Orbit"), QStringLiteral("Planet"), QStringLiteral("Pulsar"),
        QStringLiteral("Quasar"), QStringLiteral("Rocket"), QStringLiteral("Satellite"),
        QStringLiteral("Sky"), QStringLiteral("Solstice"), QStringLiteral("Star"),
        QStringLiteral("Starlight"), QStringLiteral("Sun"), QStringLiteral("Supernova"),
        QStringLiteral("Telescope"), QStringLiteral("Voyager"), QStringLiteral("Zenith"),
    };
    return values;
}

bool containsArtifact(const QString& folder)
{
    const QFileInfoList entries = QDir(folder).entryInfoList(
        QDir::AllEntries | QDir::Hidden | QDir::System |
            QDir::NoDotAndDotDot,
        QDir::Name);
    for (const QFileInfo& entry : entries) {
        if (entry.isSymLink() || !entry.isDir() ||
            containsArtifact(entry.absoluteFilePath())) {
            return true;
        }
    }
    return false;
}

}

int JamStorage::pruneEmptyUnsavedWorkspaces()
{
    const QDir tracks(appReleaseFolderPath(QStringLiteral("tracks")));
    int removed = 0;
    for (const QString& name : tracks.entryList(
             QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
             QDir::Name)) {
        const QString target = tracks.absoluteFilePath(name);
        const QString tracksRoot = tracks.absolutePath();
        if (QFileInfo(target).isSymLink() ||
            !QDir(target).absolutePath().startsWith(
                tracksRoot + QLatin1Char('/'), Qt::CaseInsensitive) ||
            containsArtifact(target)) {
            continue;
        }
        if (QDir(target).removeRecursively()) {
            ++removed;
        }
    }
    return removed;
}

QString JamStorage::randomDisplayName()
{
    const qsizetype candidateCount = adjectives().size() * nouns().size();
    const qsizetype first = static_cast<qsizetype>(
        QRandomGenerator::system()->bounded(
            static_cast<quint32>(candidateCount)));
    for (qsizetype offset = 0; offset < candidateCount; ++offset) {
        const qsizetype index = (first + offset) % candidateCount;
        const QString candidate =
            adjectives().at(index / nouns().size()) + QLatin1Char(' ') +
            nouns().at(index % nouns().size());
        const QString slug = portableSlug(candidate);
        if (!QFileInfo::exists(rootFor(false, slug)) &&
            !QFileInfo::exists(rootFor(true, slug))) {
            return candidate;
        }
    }
    // The fixed catalog currently provides well over a thousand combinations.
    // Preserve the two-word user-facing contract even if every one is occupied;
    // startNew() remains non-destructive and Save will reject a collision.
    return QStringLiteral("Cosmic Voyager");
}

QString JamStorage::portableSlug(const QString& displayName)
{
    QString value = displayName.trimmed();
    value.replace(QRegularExpression(QStringLiteral(R"(\s+)")), QStringLiteral("_"));
    value.replace(
        QRegularExpression(QStringLiteral(R"([<>:"/\\|?*\x00-\x1f])")),
        QStringLiteral("_"));
    value.replace(QRegularExpression(QStringLiteral("_+")), QStringLiteral("_"));
    value.replace(QRegularExpression(QStringLiteral(R"([. _]+$)")), QString{});
    while (value.startsWith(QLatin1Char('.')) || value.startsWith(QLatin1Char('_'))) {
        value.remove(0, 1);
    }
    if (value.isEmpty()) value = QStringLiteral("Untitled_Jam");
    static const QRegularExpression windowsDeviceName(
        QStringLiteral(R"(^(con|prn|aux|nul|com[1-9]|lpt[1-9])$)"),
        QRegularExpression::CaseInsensitiveOption);
    if (windowsDeviceName.match(value).hasMatch()) value.prepend(QLatin1Char('_'));
    return value.left(120);
}

void JamStorage::startNew(const QString& displayName)
{
    displayName_ = displayName.trimmed();
    slug_ = portableSlug(displayName_);
    saved_ = false;
    rootFolder_ = rootFor(false, slug_);
    projectFilePath_.clear();
    hasArtifacts_ = false;
}

void JamStorage::openSaved(const QString& projectFilePath, const QString& displayName)
{
    const QFileInfo info(projectFilePath);
    displayName_ = displayName.trimmed();
    slug_ = portableSlug(info.completeBaseName());
    saved_ = true;
    rootFolder_ = info.absolutePath();
    projectFilePath_ = info.absoluteFilePath();
    hasArtifacts_ = false;
}

QString JamStorage::assetFolder(AssetKind kind) const
{
    return QDir(rootFolder_).absoluteFilePath(kindFolderName(kind));
}

QString JamStorage::projectFilePath() const
{
    if (!saved_) return {};
    return projectFilePath_;
}

QString JamStorage::nextTakeName() const
{
    const QDir folder(assetFolder(AssetKind::JamRecordings));
    for (int take = 1; take < 1000000; ++take) {
        const QString name = QStringLiteral("Take-%1").arg(take);
        if (!QFileInfo::exists(folder.absoluteFilePath(name))) return name;
    }
    return QStringLiteral("Take-%1").arg(QRandomGenerator::system()->generate());
}

QString JamStorage::uniqueTakeFolder(const QString& requestedName) const
{
    const QString base = portableSlug(
        requestedName.trimmed().isEmpty() ? nextTakeName() : requestedName);
    const QDir folder(assetFolder(AssetKind::JamRecordings));
    QString candidate = base;
    int suffix = 2;
    while (QFileInfo::exists(folder.absoluteFilePath(candidate))) {
        candidate = QStringLiteral("%1-%2").arg(base).arg(suffix++);
    }
    return folder.absoluteFilePath(candidate);
}

bool JamStorage::rename(const QString& displayName, QString& error)
{
    const QString nextDisplay = displayName.trimmed();
    if (nextDisplay.isEmpty()) {
        error = QStringLiteral("The jam name cannot be empty.");
        return false;
    }
    const QString nextSlug = portableSlug(nextDisplay);
    const bool managedSavedRoot = saved_ &&
        samePath(rootFolder_, rootFor(true, slug_));
    if (saved_ && !managedSavedRoot) {
        // A JamJar may be opened from any user-selected folder. Its parent is
        // not a Jam2-owned project directory and must never be moved merely
        // because the title stored inside the project changes.
        displayName_ = nextDisplay;
        return true;
    }
    const QString destination = rootFor(saved_, nextSlug);
    if (!samePath(destination, rootFolder_) &&
        QFileInfo::exists(destination)) {
        error = QStringLiteral("A jam named %1 already exists.").arg(nextDisplay);
        return false;
    }
    const QString oldRoot = rootFolder_;
    const QString oldProjectPath = projectFilePath_;
    if (!moveRoot(destination, error)) return false;
    if (saved_) {
        const QString relocatedProjectPath = oldProjectPath.isEmpty()
            ? QString{}
            : QDir(rootFolder_).absoluteFilePath(
                  QDir(oldRoot).relativeFilePath(oldProjectPath));
        const QString renamedProjectPath = QDir(rootFolder_).absoluteFilePath(
            nextSlug + QStringLiteral(".jamjar"));
        if (!relocatedProjectPath.isEmpty() &&
            !samePath(relocatedProjectPath, renamedProjectPath) &&
            QFileInfo::exists(relocatedProjectPath) &&
            !QFile::rename(relocatedProjectPath, renamedProjectPath)) {
            QString rollbackError;
            (void)moveRoot(oldRoot, rollbackError);
            projectFilePath_ = oldProjectPath;
            error = QStringLiteral("Could not rename the JamJar project file.");
            return false;
        }
        projectFilePath_ = renamedProjectPath;
    }
    displayName_ = nextDisplay;
    slug_ = nextSlug;
    return true;
}

bool JamStorage::moveToSongs(QString& error)
{
    if (saved_) return true;
    const QString destination = rootFor(true, slug_);
    if (QFileInfo::exists(destination)) {
        error = QStringLiteral("A saved jam named %1 already exists.").arg(displayName_);
        return false;
    }
    if (!moveRoot(destination, error)) return false;
    saved_ = true;
    projectFilePath_ = QDir(rootFolder_).absoluteFilePath(
        slug_ + QStringLiteral(".jamjar"));
    hasArtifacts_ = false;
    return true;
}

bool JamStorage::discardUnsaved(QString& error)
{
    if (saved_ || rootFolder_.isEmpty() || !QFileInfo::exists(rootFolder_)) return true;
    const QString tracksRoot = QDir(appReleaseFolderPath(QStringLiteral("tracks"))).absolutePath();
    const QString target = QDir(rootFolder_).absolutePath();
    const QString prefix = tracksRoot + QLatin1Char('/');
    if (!target.startsWith(prefix, Qt::CaseInsensitive) || target == tracksRoot) {
        error = QStringLiteral("Refused to remove an unsafe jam workspace path.");
        return false;
    }
    QDir directory(target);
    if (!directory.removeRecursively()) {
        error = QStringLiteral("Could not remove the unsaved jam workspace: %1").arg(target);
        return false;
    }
    hasArtifacts_ = false;
    return true;
}

QString JamStorage::rootFor(bool saved, const QString& slug)
{
    const QDir parent(appReleaseFolderPath(
        saved ? QStringLiteral("songs") : QStringLiteral("tracks")));
    return parent.absoluteFilePath(slug);
}

QString JamStorage::kindFolderName(AssetKind kind)
{
    switch (kind) {
    case AssetKind::Generated: return QStringLiteral("generated");
    case AssetKind::Received: return QStringLiteral("received");
    case AssetKind::Imported: return QStringLiteral("imported");
    case AssetKind::Recorded: return QStringLiteral("recorded");
    case AssetKind::Prepared: return QStringLiteral("prepared");
    case AssetKind::JamRecordings: return QStringLiteral("jam_recordings");
    }
    return QStringLiteral("recorded");
}

bool JamStorage::moveRoot(const QString& destination, QString& error)
{
    const QString source = QDir(rootFolder_).absolutePath();
    const QString target = QDir(destination).absolutePath();
    if (source == target) return true;
    if (!QFileInfo::exists(source)) {
        rootFolder_ = target;
        return true;
    }
    if (!QDir().mkpath(QFileInfo(target).absolutePath())) {
        error = QStringLiteral("Could not create the jam's parent folder.");
        return false;
    }
    QDir parent(QFileInfo(source).absolutePath());
    if (!parent.rename(QFileInfo(source).fileName(), target)) {
        error = QStringLiteral("Could not move the jam workspace from %1 to %2.")
            .arg(source, target);
        return false;
    }
    rootFolder_ = target;
    return true;
}

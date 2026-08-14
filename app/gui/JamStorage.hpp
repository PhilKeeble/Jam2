#pragma once

#include <QString>

class JamStorage final {
public:
    enum class AssetKind {
        Generated,
        Received,
        Imported,
        Recorded,
        Prepared,
        JamRecordings,
    };

    static QString randomDisplayName();
    static QString portableSlug(const QString& displayName);
    static int pruneEmptyUnsavedWorkspaces();

    void startNew(const QString& displayName);
    void openSaved(const QString& projectFilePath, const QString& displayName);

    const QString& displayName() const noexcept { return displayName_; }
    const QString& rootFolder() const noexcept { return rootFolder_; }
    bool isSaved() const noexcept { return saved_; }
    bool hasArtifacts() const noexcept { return hasArtifacts_; }
    void markArtifactCreated() noexcept { hasArtifacts_ = true; }
    void clearArtifactState() noexcept { hasArtifacts_ = false; }

    QString assetFolder(AssetKind kind) const;
    QString projectFilePath() const;
    QString nextTakeName() const;
    QString uniqueTakeFolder(const QString& requestedName) const;

    bool rename(const QString& displayName, QString& error);
    bool moveToSongs(QString& error);
    bool discardUnsaved(QString& error);

private:
    static QString rootFor(bool saved, const QString& slug);
    static QString kindFolderName(AssetKind kind);
    bool moveRoot(const QString& destination, QString& error);

    QString displayName_;
    QString slug_;
    QString rootFolder_;
    QString projectFilePath_;
    bool saved_ = false;
    bool hasArtifacts_ = false;
};

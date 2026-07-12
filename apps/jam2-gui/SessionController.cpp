#include "SessionController.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

namespace {

QString siblingToolPath(const char* binary)
{
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString sibling = appDir.absoluteFilePath(QString::fromLatin1(binary));
    if (QFileInfo::exists(sibling)) {
        return sibling;
    }

    QDir releaseDir(appDir);
    if (releaseDir.dirName() == QStringLiteral("MacOS") &&
        releaseDir.cdUp() &&
        releaseDir.dirName() == QStringLiteral("Contents") &&
        releaseDir.cdUp() &&
        releaseDir.dirName().endsWith(QStringLiteral(".app")) &&
        releaseDir.cdUp()) {
        const QString besideBundle = releaseDir.absoluteFilePath(QString::fromLatin1(binary));
        if (QFileInfo::exists(besideBundle)) {
            return besideBundle;
        }
    }

    QDir root(appDir);
    if (root.dirName() == QStringLiteral("release")) {
        root.cdUp();
    }
    return root.absoluteFilePath(QStringLiteral("release/%1").arg(QString::fromLatin1(binary)));
}

}

QString SessionController::defaultJam2Path()
{
#if defined(Q_OS_WIN)
    constexpr const char* binary = "jam2.exe";
#else
    constexpr const char* binary = "jam2";
#endif
    return siblingToolPath(binary);
}

QString SessionController::defaultBindHost()
{
    return QStringLiteral("0.0.0.0");
}

QString SessionController::defaultPublicHost()
{
    return QStringLiteral("127.0.0.1");
}

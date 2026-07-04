#include "SessionController.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

QString SessionController::defaultJam2Path()
{
#if defined(Q_OS_WIN)
    constexpr const char* binary = "jam2.exe";
#else
    constexpr const char* binary = "jam2";
#endif
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString sibling = appDir.absoluteFilePath(binary);
    if (QFileInfo::exists(sibling)) {
        return sibling;
    }
    QDir root(appDir);
    if (root.dirName() == "release") {
        root.cdUp();
    }
    return root.absoluteFilePath(QStringLiteral("release/%1").arg(binary));
}

QString SessionController::defaultCapturePath()
{
#if defined(Q_OS_WIN)
    constexpr const char* binary = "jam2-capture.exe";
#else
    constexpr const char* binary = "jam2-capture";
#endif
    const QDir appDir(QCoreApplication::applicationDirPath());
    const QString sibling = appDir.absoluteFilePath(binary);
    if (QFileInfo::exists(sibling)) {
        return sibling;
    }
    QDir root(appDir);
    if (root.dirName() == "release") {
        root.cdUp();
    }
    return root.absoluteFilePath(QStringLiteral("release/%1").arg(binary));
}

QString SessionController::defaultBindHost()
{
    return QStringLiteral("0.0.0.0");
}

QString SessionController::defaultPublicHost()
{
    return QStringLiteral("127.0.0.1");
}

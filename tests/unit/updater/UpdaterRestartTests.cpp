#include "UpdatePackage.hpp"
#include "UpdaterPlatform.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>

#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    try {
        // Explicit manual integration mode uses the installed signed app and
        // production parent-wait/restart code, without replacing its bundle.
        if (argc == 4 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--live-restart")) {
            const QString root = QString::fromLocal8Bit(argv[2]);
            bool valid = false;
            const qint64 pid = QString::fromLocal8Bit(argv[3]).toLongLong(&valid);
            require(valid && pid > 0, "live restart requires the saved app's PID");
            jam2::update::ParentProcess parent(pid, QDir(root).filePath(jam2::update::executableRelativePath()));
            QElapsedTimer timeout;
            timeout.start();
            while (!parent.exited() && timeout.elapsed() < 30000) QThread::msleep(20);
            require(parent.exited(), "app did not quit normally; no restart was attempted");
            jam2::update::restartApplication(root);
            std::cout << "Live restart requested through production launcher\n";
            return 0;
        }
        require(argc == 2, "expected a build-local fixture workspace");
        const QString workspace = QString::fromLocal8Bit(argv[1]);
        require(QDir().mkpath(workspace), "restart fixture workspace exists");
        QTemporaryDir temporary(QDir(workspace).filePath(QStringLiteral("updater restart-XXXXXX")));
        require(temporary.isValid(), "restart fixture has a private installation path with spaces");
        const QString root = temporary.path();
        // A missing installation must fail before calling the native launcher.
        bool missingRejected = false;
        try {
            jam2::update::restartApplication(root);
        } catch (const std::exception& error) {
            missingRejected = QString::fromUtf8(error.what()).contains(QStringLiteral("executable is missing"));
        }
        require(missingRejected, "missing application reports a clear restart error");
        require(QDir(root).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty(),
            "restart failure leaves the installation untouched");
#ifdef Q_OS_MACOS
        const QString executable = QDir(root).filePath(jam2::update::executableRelativePath());
        require(QDir().mkpath(QFileInfo(executable).absolutePath()), "malformed bundle fixture directory exists");
        QFile file(executable);
        require(file.open(QIODevice::WriteOnly) && file.write("invalid application") > 0,
            "malformed bundle fixture exists without a runnable app");
        file.close();
        bool invalidRejected = false;
        try {
            jam2::update::restartApplication(root);
        } catch (const std::exception& error) {
            invalidRejected = QString::fromUtf8(error.what()).contains(QStringLiteral("invalid application metadata"));
        }
        require(invalidRejected, "malformed bundle reports native launch error before starting any process");
        require(file.open(QIODevice::ReadOnly) && file.readAll() == "invalid application",
            "native restart rejection leaves the installed file intact");
#endif
        std::cout << "Updater restart checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Updater restart failure: " << error.what() << '\n';
        return 1;
    }
}

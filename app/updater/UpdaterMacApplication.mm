#include "UpdaterMacApplication.hpp"

#import <AppKit/AppKit.h>

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QTextStream>
#include <QThread>

#include <stdexcept>

namespace jam2::update {
void restartMacApplication(const QString& bundlePath)
{
    @autoreleasepool {
        NSURL* url = [NSURL fileURLWithPath:bundlePath.toNSString() isDirectory:YES];
        NSBundle* bundle = [NSBundle bundleWithURL:url];
        if (!bundle.bundleIdentifier || !bundle.executableURL) {
            throw std::runtime_error("The update was installed, but Jam2.app has invalid application metadata. Reinstall Jam2.");
        }
        const QString root = QFileInfo(bundlePath).absolutePath();
        QProcess launch;
        launch.setProgram(QString::fromNSString(bundle.executablePath));
        launch.setWorkingDirectory(root);
        qint64 pid = 0;
        if (!launch.startDetached(&pid)) {
            throw std::runtime_error(QStringLiteral("The update was installed, but Jam2 could not start: %1. Start it manually.")
                .arg(launch.errorString()).toStdString());
        }
        // A successful spawn is not a completed GUI launch. AppKit's varying
        // process properties refresh on turns of the main common-mode run loop.
        QElapsedTimer timeout;
        timeout.start();
        NSRunningApplication* application = nil;
        while (timeout.elapsed() < 15000) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);
            if (!application) application = [NSRunningApplication runningApplicationWithProcessIdentifier:static_cast<pid_t>(pid)];
            if (application.terminated) {
                throw std::runtime_error("The update was installed, but Jam2 exited before finishing launch. Start it manually.");
            }
            if (application.finishedLaunching) break;
            QThread::msleep(10);
        }
        if (!application.finishedLaunching) {
            throw std::runtime_error("The update was installed, but Jam2 did not finish launching within 15 seconds. Start it manually.");
        }
        const qint64 launchWaitMs = timeout.elapsed();
        // Reopen only after the app is ready. This is the same Launch Services
        // request used when the user opens an already-running app in Finder.
        QProcess activate;
        activate.setProgram(QStringLiteral("/usr/bin/open"));
        activate.setArguments({bundlePath});
        activate.setWorkingDirectory(root);
        activate.start();
        const bool finished = activate.waitForFinished(10000);
        const QString launchError = activate.errorString();
        if (!finished && activate.state() != QProcess::NotRunning) {
            activate.kill();
            activate.waitForFinished(3000);
        }
        const QString detail = QString::fromUtf8(activate.readAllStandardError()).trimmed();
        if (!finished || activate.exitStatus() != QProcess::NormalExit || activate.exitCode() != 0) {
            throw std::runtime_error(QStringLiteral("The update was installed, but macOS could not activate Jam2: %1. Open Jam2.app manually.")
                .arg(detail.isEmpty() ? launchError : detail).toStdString());
        }
        QTextStream(stdout) << "mac_restart_pid=" << pid << " launch_finished=1 launch_wait_ms=" << launchWaitMs
            << " reopen_exit_code=" << activate.exitCode() << '\n';
    }
}
}

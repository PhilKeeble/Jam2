#include "UpdaterPlatform.hpp"
#include "UpdatePackage.hpp"
#ifdef Q_OS_MACOS
#include "UpdaterMacApplication.hpp"
#endif

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>

#include <limits>
#include <iterator>
#include <stdexcept>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <libproc.h>
#include <signal.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace jam2::update {
namespace {
void require(bool condition, const QString& error)
{
    if (!condition) throw std::runtime_error(error.toStdString());
}
bool samePath(const QString& first, const QString& second)
{
#ifdef Q_OS_WIN
    return QDir::cleanPath(QDir::fromNativeSeparators(first)).compare(
        QDir::cleanPath(QDir::fromNativeSeparators(second)), Qt::CaseInsensitive) == 0;
#else
    return QDir::cleanPath(first) == QDir::cleanPath(second);
#endif
}
} // namespace

struct ParentProcess::Impl {
#ifdef Q_OS_WIN
    HANDLE handle = nullptr;
    ~Impl() { if (handle) CloseHandle(handle); }
#else
    pid_t pid = 0;
    QString executable;
#endif
};

ParentProcess::ParentProcess(qint64 pid, const QString& expectedExecutable)
    : impl_(std::make_unique<Impl>())
{
    require(pid >= 0 && pid <= std::numeric_limits<int>::max(), QStringLiteral("Invalid Jam2 process ID."));
    if (pid == 0) return;
#ifdef Q_OS_WIN
    impl_->handle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!impl_->handle) {
        require(GetLastError() == ERROR_INVALID_PARAMETER, QStringLiteral("Could not inspect the Jam2 process (Windows error %1).").arg(GetLastError()));
        return;
    }
    if (WaitForSingleObject(impl_->handle, 0) == WAIT_OBJECT_0) return;
    wchar_t path[32768];
    DWORD length = static_cast<DWORD>(std::size(path));
    require(QueryFullProcessImageNameW(impl_->handle, 0, path, &length) &&
        samePath(QString::fromWCharArray(path, static_cast<qsizetype>(length)), expectedExecutable),
        QStringLiteral("The requested process is not Jam2 in this installation."));
#else
    impl_->pid = static_cast<pid_t>(pid);
    impl_->executable = expectedExecutable;
    if (kill(impl_->pid, 0) != 0) {
        require(errno == ESRCH, QStringLiteral("Could not inspect the Jam2 process."));
        impl_->pid = 0;
        return;
    }
    char path[PROC_PIDPATHINFO_MAXSIZE];
    const int length = proc_pidpath(impl_->pid, path, sizeof(path));
    require(length > 0 && samePath(QString::fromUtf8(path), expectedExecutable),
        QStringLiteral("The requested process is not Jam2 in this installation."));
#endif
}

ParentProcess::~ParentProcess() = default;

void configureHelperProcess(QProcess& process)
{
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* arguments) {
        arguments->flags |= CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS;
    });
#else
    (void)process;
#endif
}

void restartApplication(const QString& root)
{
    require(QFileInfo(QDir(root).filePath(executableRelativePath())).isFile(),
        QStringLiteral("The update was installed, but the Jam2 executable is missing. Reinstall Jam2."));
#ifdef Q_OS_MACOS
    restartMacApplication(QDir(root).filePath(QStringLiteral("Jam2.app")));
#else
    QProcess process;
    process.setProgram(QDir(root).filePath(executableRelativePath()));
    process.setWorkingDirectory(root);
    require(process.startDetached(),
        QStringLiteral("The update was installed, but Jam2 could not restart. Start it manually."));
#endif
}

bool ParentProcess::exited() const
{
#ifdef Q_OS_WIN
    if (!impl_->handle) return true;
    const auto result = WaitForSingleObject(impl_->handle, 0);
    require(result != WAIT_FAILED, QStringLiteral("Could not wait for Jam2 to exit."));
    return result == WAIT_OBJECT_0;
#else
    if (!impl_->pid) return true;
    if (kill(impl_->pid, 0) != 0) {
        require(errno == ESRCH, QStringLiteral("Could not wait for Jam2 to exit."));
        return true;
    }
    char path[PROC_PIDPATHINFO_MAXSIZE];
    const int length = proc_pidpath(impl_->pid, path, sizeof(path));
    if (length <= 0) {
        if (kill(impl_->pid, 0) != 0 && errno == ESRCH) return true;
        throw std::runtime_error("Could not inspect the original Jam2 process while waiting for it to exit.");
    }
    return !samePath(QString::fromUtf8(path), impl_->executable);
#endif
}

void preflightWritableFile(const QString& path)
{
    const QFileInfo info(path);
    if (!info.exists()) return;
    require(info.isFile() && !info.isSymLink() && !info.isJunction(), QStringLiteral("Unsafe destination: %1").arg(path));
#ifdef Q_OS_WIN
    const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_WRITE | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    require(handle != INVALID_HANDLE_VALUE,
        QStringLiteral("Cannot replace %1 (Windows error %2). Close other Jam2 instances and retry; check folder permissions.")
            .arg(path).arg(GetLastError()));
    CloseHandle(handle);
#else
    require(info.isWritable() && QFileInfo(info.absolutePath()).isWritable(), QStringLiteral("The update cannot write %1. Check folder permissions.").arg(path));
#endif
}

void verifyMacSignature(const QString& bundle)
{
#ifdef Q_OS_MACOS
    const auto path = bundle.toUtf8();
    auto* url = CFURLCreateFromFileSystemRepresentation(nullptr,
        reinterpret_cast<const UInt8*>(path.constData()), path.size(), true);
    require(url != nullptr, QStringLiteral("Could not inspect the Mac application signature."));
    SecStaticCodeRef code = nullptr;
    const auto created = SecStaticCodeCreateWithPath(url, kSecCSDefaultFlags, &code);
    CFRelease(url);
    require(created == errSecSuccess && code, QStringLiteral("The staged Mac app has no valid code signature."));
    const auto verified = SecStaticCodeCheckValidity(code,
        kSecCSCheckAllArchitectures | kSecCSCheckNestedCode | kSecCSStrictValidate, nullptr);
    CFRelease(code);
    require(verified == errSecSuccess, QStringLiteral("The staged Mac app failed code-signature verification (%1).").arg(verified));
#else
    (void)bundle;
#endif
}
} // namespace jam2::update

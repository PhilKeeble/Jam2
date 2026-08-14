#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "AutomationProcess.hpp"
#include "TestTiming.hpp"

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cwchar>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

constexpr std::size_t kMaximumFrameBytes = 64 * 1024;

#if defined(_WIN32)

QString windowsError(const char* context, DWORD code = GetLastError())
{
    wchar_t* text = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageW(flags, nullptr, code, 0,
        reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    QString detail = length > 0 && text != nullptr
        ? QString::fromWCharArray(text, static_cast<qsizetype>(length)).trimmed()
        : QStringLiteral("Windows error %1").arg(code);
    if (text != nullptr) LocalFree(text);
    return QString::fromLatin1(context) + QStringLiteral(": ") + detail;
}

std::wstring quoteWindowsArgument(const std::wstring& argument)
{
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) return argument;
    std::wstring quoted(1, L'\"');
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'\"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

std::vector<wchar_t> windowsCommandLine(
    const QString& executable, const QStringList& arguments)
{
    std::wstring line = quoteWindowsArgument(executable.toStdWString());
    for (const QString& argument : arguments) {
        line.push_back(L' ');
        line += quoteWindowsArgument(argument.toStdWString());
    }
    std::vector<wchar_t> result(line.begin(), line.end());
    result.push_back(L'\0');
    return result;
}

bool environmentKeyEquals(const std::wstring& entry, const wchar_t* key)
{
    const std::size_t keyLength = std::wcslen(key);
    return entry.size() > keyLength && entry[keyLength] == L'=' &&
        _wcsnicmp(entry.c_str(), key, keyLength) == 0;
}

std::vector<wchar_t> childEnvironment(HANDLE commandRead, HANDLE eventWrite)
{
    std::vector<std::wstring> entries;
    LPWCH environment = GetEnvironmentStringsW();
    if (environment != nullptr) {
        for (const wchar_t* item = environment; *item != L'\0'; item += std::wcslen(item) + 1) {
            std::wstring entry(item);
            if (!environmentKeyEquals(entry, L"JAM2_AUTOMATION_COMMAND_HANDLE") &&
                !environmentKeyEquals(entry, L"JAM2_AUTOMATION_EVENT_HANDLE")) {
                entries.push_back(std::move(entry));
            }
        }
        FreeEnvironmentStringsW(environment);
    }
    entries.push_back(L"JAM2_AUTOMATION_COMMAND_HANDLE=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(commandRead)));
    entries.push_back(L"JAM2_AUTOMATION_EVENT_HANDLE=" +
        std::to_wstring(reinterpret_cast<std::uintptr_t>(eventWrite)));
    std::sort(entries.begin(), entries.end(), [](const std::wstring& left, const std::wstring& right) {
        return _wcsicmp(left.c_str(), right.c_str()) < 0;
    });

    std::size_t size = 1;
    for (const auto& entry : entries) size += entry.size() + 1;
    std::vector<wchar_t> block;
    block.reserve(size);
    for (const auto& entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

void closeHandle(HANDLE& handle) noexcept
{
    if (handle != nullptr && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    handle = nullptr;
}

#else

QString posixError(const char* context)
{
    return QString::fromLatin1(context) + QStringLiteral(": ") +
        QString::fromLocal8Bit(std::strerror(errno));
}

void closeFd(int& descriptor) noexcept
{
    if (descriptor >= 0) (void)::close(descriptor);
    descriptor = -1;
}

bool setCloseOnExec(int descriptor)
{
    const int flags = ::fcntl(descriptor, F_GETFD);
    return flags >= 0 && ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) == 0;
}

bool clearCloseOnExec(int descriptor)
{
    const int flags = ::fcntl(descriptor, F_GETFD);
    return flags >= 0 && ::fcntl(descriptor, F_SETFD, flags & ~FD_CLOEXEC) == 0;
}

#endif

} // namespace

std::unique_ptr<AutomationProcess> AutomationProcess::launch(
    const QString& executable,
    const QStringList& arguments,
    QString& error)
{
    error.clear();
    if (!QFileInfo::exists(executable)) {
        error = QStringLiteral("Jam2 executable does not exist: ") + executable;
        return nullptr;
    }
    auto process = std::unique_ptr<AutomationProcess>(new AutomationProcess());

#if defined(_WIN32)
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE commandRead = nullptr;
    HANDLE commandWrite = nullptr;
    HANDLE eventRead = nullptr;
    HANDLE eventWrite = nullptr;
    HANDLE nullInput = INVALID_HANDLE_VALUE;
    HANDLE nullOutput = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&commandRead, &commandWrite, &attributes, 0) ||
        !SetHandleInformation(commandWrite, HANDLE_FLAG_INHERIT, 0) ||
        !CreatePipe(&eventRead, &eventWrite, &attributes, 0) ||
        !SetHandleInformation(eventRead, HANDLE_FLAG_INHERIT, 0)) {
        error = windowsError("creating automation pipes failed");
        closeHandle(commandRead);
        closeHandle(commandWrite);
        closeHandle(eventRead);
        closeHandle(eventWrite);
        return nullptr;
    }
    nullInput = CreateFileW(L"NUL", GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_EXISTING, 0, nullptr);
    nullOutput = CreateFileW(L"NUL", GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_EXISTING, 0, nullptr);
    if (nullInput == INVALID_HANDLE_VALUE || nullOutput == INVALID_HANDLE_VALUE) {
        error = windowsError("opening NUL handles failed");
        closeHandle(commandRead);
        closeHandle(commandWrite);
        closeHandle(eventRead);
        closeHandle(eventWrite);
        closeHandle(nullInput);
        closeHandle(nullOutput);
        return nullptr;
    }

    std::array<HANDLE, 4> inherited{commandRead, eventWrite, nullInput, nullOutput};
    SIZE_T attributeBytes = 0;
    (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    std::vector<std::byte> attributeStorage(attributeBytes);
    auto* attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    const bool attributeInitialized =
        InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeBytes) != FALSE;
    const bool attributeReady = attributeInitialized &&
        UpdateProcThreadAttribute(attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited.data(), inherited.size() * sizeof(HANDLE), nullptr, nullptr) != FALSE;
    if (!attributeReady) {
        error = windowsError("restricting inherited handles failed");
        if (attributeInitialized) DeleteProcThreadAttributeList(attributeList);
        closeHandle(commandRead);
        closeHandle(commandWrite);
        closeHandle(eventRead);
        closeHandle(eventWrite);
        closeHandle(nullInput);
        closeHandle(nullOutput);
        return nullptr;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nullInput;
    startup.StartupInfo.hStdOutput = nullOutput;
    startup.StartupInfo.hStdError = nullOutput;
    startup.lpAttributeList = attributeList;
    PROCESS_INFORMATION information{};
    auto commandLine = windowsCommandLine(executable, arguments);
    auto environment = childEnvironment(commandRead, eventWrite);
    const std::wstring application = QDir::toNativeSeparators(executable).toStdWString();
    const std::wstring workingDirectory = QFileInfo(executable).absolutePath().toStdWString();
    const BOOL created = CreateProcessW(
        application.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
        environment.data(), workingDirectory.c_str(), &startup.StartupInfo, &information);
    const DWORD createError = created ? ERROR_SUCCESS : GetLastError();
    DeleteProcThreadAttributeList(attributeList);
    closeHandle(commandRead);
    closeHandle(eventWrite);
    closeHandle(nullInput);
    closeHandle(nullOutput);
    if (!created) {
        error = windowsError("launching Jam2 failed", createError);
        closeHandle(commandWrite);
        closeHandle(eventRead);
        return nullptr;
    }
    CloseHandle(information.hThread);
    process->processHandle_ = reinterpret_cast<std::uintptr_t>(information.hProcess);
    process->commandWrite_ = reinterpret_cast<std::uintptr_t>(commandWrite);
    process->eventRead_ = reinterpret_cast<std::uintptr_t>(eventRead);
#else
    int commandPipe[2]{-1, -1};
    int eventPipe[2]{-1, -1};
    if (::pipe(commandPipe) != 0 || ::pipe(eventPipe) != 0) {
        error = posixError("creating automation pipes failed");
        closeFd(commandPipe[0]);
        closeFd(commandPipe[1]);
        closeFd(eventPipe[0]);
        closeFd(eventPipe[1]);
        return nullptr;
    }
    if (!setCloseOnExec(commandPipe[0]) || !setCloseOnExec(commandPipe[1]) ||
        !setCloseOnExec(eventPipe[0]) || !setCloseOnExec(eventPipe[1])) {
        error = posixError("marking automation pipes close-on-exec failed");
        closeFd(commandPipe[0]);
        closeFd(commandPipe[1]);
        closeFd(eventPipe[0]);
        closeFd(eventPipe[1]);
        return nullptr;
    }
    const pid_t child = ::fork();
    if (child < 0) {
        error = posixError("forking Jam2 failed");
        closeFd(commandPipe[0]);
        closeFd(commandPipe[1]);
        closeFd(eventPipe[0]);
        closeFd(eventPipe[1]);
        return nullptr;
    }
    if (child == 0) {
        if (!clearCloseOnExec(commandPipe[0]) || !clearCloseOnExec(eventPipe[1])) {
            ::_exit(126);
        }
        (void)::setenv("JAM2_AUTOMATION_COMMAND_HANDLE",
            std::to_string(commandPipe[0]).c_str(), 1);
        (void)::setenv("JAM2_AUTOMATION_EVENT_HANDLE",
            std::to_string(eventPipe[1]).c_str(), 1);
        const int nullDescriptor = ::open("/dev/null", O_RDWR);
        if (nullDescriptor >= 0) {
            (void)::dup2(nullDescriptor, STDIN_FILENO);
            (void)::dup2(nullDescriptor, STDOUT_FILENO);
            (void)::dup2(nullDescriptor, STDERR_FILENO);
            if (nullDescriptor > STDERR_FILENO) (void)::close(nullDescriptor);
        }
        const QByteArray workingDirectory = QFileInfo(executable).absolutePath().toUtf8();
        (void)::chdir(workingDirectory.constData());
        (void)::close(commandPipe[1]);
        (void)::close(eventPipe[0]);
        std::vector<QByteArray> storage;
        storage.reserve(static_cast<std::size_t>(arguments.size() + 1));
        storage.push_back(QFileInfo(executable).absoluteFilePath().toUtf8());
        for (const QString& argument : arguments) storage.push_back(argument.toUtf8());
        std::vector<char*> argv;
        argv.reserve(storage.size() + 1);
        for (QByteArray& item : storage) argv.push_back(item.data());
        argv.push_back(nullptr);
        ::execv(argv[0], argv.data());
        ::_exit(127);
    }
    closeFd(commandPipe[0]);
    closeFd(eventPipe[1]);
#if defined(__APPLE__)
    int noSignal = 1;
    (void)::fcntl(commandPipe[1], F_SETNOSIGPIPE, noSignal);
#endif
    process->processId_ = child;
    process->commandWrite_ = commandPipe[1];
    process->eventRead_ = eventPipe[0];
#endif
    return process;
}

AutomationProcess::~AutomationProcess()
{
    stopForCleanup();
    closePipes();
#if defined(_WIN32)
    HANDLE process = reinterpret_cast<HANDLE>(processHandle_);
    if (process != nullptr) CloseHandle(process);
    processHandle_ = 0;
#endif
}

bool AutomationProcess::send(QJsonObject command, QString& error)
{
    error.clear();
    command.insert(QStringLiteral("format"), QStringLiteral("jam2-automation"));
    const QByteArray payload = QJsonDocument(command).toJson(QJsonDocument::Compact);
    if (payload.isEmpty() || static_cast<std::size_t>(payload.size()) > kMaximumFrameBytes) {
        error = QStringLiteral("automation command is outside frame bounds");
        return false;
    }
    std::array<unsigned char, 4> prefix{};
    qToLittleEndian<std::uint32_t>(static_cast<std::uint32_t>(payload.size()), prefix.data());
    const auto writeBytes = [&error](auto descriptor, const void* source, std::size_t bytes) {
        const auto* input = static_cast<const unsigned char*>(source);
        std::size_t offset = 0;
        while (offset < bytes) {
#if defined(_WIN32)
            DWORD written = 0;
            const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
                bytes - offset, std::numeric_limits<DWORD>::max()));
            if (!WriteFile(descriptor, input + offset, requested, &written, nullptr) || written == 0) {
                error = windowsError("writing automation command failed");
                return false;
            }
            offset += written;
#else
            const ssize_t written = ::write(descriptor, input + offset, bytes - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) {
                error = posixError("writing automation command failed");
                return false;
            }
            offset += static_cast<std::size_t>(written);
#endif
        }
        return true;
    };
#if defined(_WIN32)
    HANDLE descriptor = reinterpret_cast<HANDLE>(commandWrite_);
#else
    int descriptor = commandWrite_;
#endif
    return writeBytes(descriptor, prefix.data(), prefix.size()) &&
        writeBytes(descriptor, payload.constData(), static_cast<std::size_t>(payload.size()));
}

bool AutomationProcess::readEvent(
    QJsonObject& event, std::chrono::milliseconds timeout, QString& error)
{
    error.clear();
    timeout = jam2::test::scaledTimeout(timeout);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::array<unsigned char, 4> prefix{};
    if (!readExact(prefix.data(), prefix.size(), deadline, error)) return false;
    const std::uint32_t size = qFromLittleEndian<std::uint32_t>(prefix.data());
    if (size == 0 || size > kMaximumFrameBytes) {
        error = QStringLiteral("automation event frame is outside bounds");
        return false;
    }
    QByteArray payload(static_cast<qsizetype>(size), Qt::Uninitialized);
    if (!readExact(payload.data(), size, deadline, error)) return false;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        error = QStringLiteral("automation event is not a JSON object");
        return false;
    }
    event = document.object();
    return true;
}

bool AutomationProcess::readExact(
    void* output, std::size_t bytes,
    std::chrono::steady_clock::time_point deadline, QString& error)
{
    auto* destination = static_cast<unsigned char*>(output);
    std::size_t offset = 0;
    while (offset < bytes) {
        if (std::chrono::steady_clock::now() >= deadline) {
            error = QStringLiteral("timed out waiting for automation event");
            return false;
        }
#if defined(_WIN32)
        HANDLE descriptor = reinterpret_cast<HANDLE>(eventRead_);
        DWORD available = 0;
        if (!PeekNamedPipe(descriptor, nullptr, 0, nullptr, &available, nullptr)) {
            error = windowsError("checking automation event pipe failed");
            return false;
        }
        if (available == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        DWORD read = 0;
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
            bytes - offset, static_cast<std::size_t>(available)));
        if (!ReadFile(descriptor, destination + offset, requested, &read, nullptr) || read == 0) {
            error = windowsError("reading automation event failed");
            return false;
        }
        offset += read;
#else
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        pollfd descriptor{eventRead_, POLLIN, 0};
        const int polled = ::poll(&descriptor, 1,
            static_cast<int>(std::clamp<long long>(remaining.count(), 1, 50)));
        if (polled < 0 && errno == EINTR) continue;
        if (polled < 0) {
            error = posixError("polling automation event failed");
            return false;
        }
        if (polled == 0) continue;
        const ssize_t read = ::read(eventRead_, destination + offset, bytes - offset);
        if (read < 0 && errno == EINTR) continue;
        if (read <= 0) {
            error = posixError("reading automation event failed");
            return false;
        }
        offset += static_cast<std::size_t>(read);
#endif
    }
    return true;
}

bool AutomationProcess::waitForExit(
    std::chrono::milliseconds timeout, int& exitCode, QString& error)
{
    error.clear();
    timeout = jam2::test::scaledTimeout(timeout);
    if (exited_) {
        exitCode = exitCode_;
        return true;
    }
#if defined(_WIN32)
    HANDLE process = reinterpret_cast<HANDLE>(processHandle_);
    const auto bounded = std::clamp<long long>(timeout.count(), 0,
        static_cast<long long>(std::numeric_limits<DWORD>::max() - 1));
    const DWORD waited = WaitForSingleObject(process, static_cast<DWORD>(bounded));
    if (waited == WAIT_TIMEOUT) {
        error = QStringLiteral("timed out waiting for Jam2 process exit");
        return false;
    }
    if (waited != WAIT_OBJECT_0) {
        error = windowsError("waiting for Jam2 process failed");
        return false;
    }
    DWORD code = 0;
    if (!GetExitCodeProcess(process, &code)) {
        error = windowsError("reading Jam2 exit code failed");
        return false;
    }
    exitCode_ = static_cast<int>(code);
#else
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        int status = 0;
        const pid_t waited = ::waitpid(processId_, &status, WNOHANG);
        if (waited == processId_) {
            exitCode_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            break;
        }
        if (waited < 0) {
            error = posixError("waiting for Jam2 process failed");
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            error = QStringLiteral("timed out waiting for Jam2 process exit");
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
#endif
    exited_ = true;
    exitCode = exitCode_;
    closePipes();
    return true;
}

bool AutomationProcess::isRunning() const noexcept
{
    if (exited_) return false;
#if defined(_WIN32)
    const HANDLE process = reinterpret_cast<HANDLE>(processHandle_);
    return process != nullptr && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
#else
    return processId_ > 0;
#endif
}

void AutomationProcess::stopForCleanup() noexcept
{
    if (!isRunning()) return;
#if defined(_WIN32)
    HANDLE process = reinterpret_cast<HANDLE>(processHandle_);
    (void)TerminateProcess(process, 125);
    (void)WaitForSingleObject(process, 5000);
#else
    (void)::kill(processId_, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        if (::waitpid(processId_, &status, WNOHANG) == processId_) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    (void)::kill(processId_, SIGKILL);
    int status = 0;
    (void)::waitpid(processId_, &status, 0);
#endif
}

void AutomationProcess::closePipes() noexcept
{
#if defined(_WIN32)
    HANDLE command = reinterpret_cast<HANDLE>(commandWrite_);
    HANDLE events = reinterpret_cast<HANDLE>(eventRead_);
    closeHandle(command);
    closeHandle(events);
    commandWrite_ = 0;
    eventRead_ = 0;
#else
    closeFd(commandWrite_);
    closeFd(eventRead_);
#endif
}

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>

#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
QByteArray select(const QString& script, const QString& toolset,
    const QString& installed, bool success = true, const QString& minimum = QStringLiteral("14.40.0.0"))
{
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(QStringLiteral("powershell.exe"), {QStringLiteral("-NoProfile"),
        QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"), QStringLiteral("-File"), script,
        QStringLiteral("-ToolsetDirectory"), toolset, QStringLiteral("-InstalledDirectory"), installed,
        QStringLiteral("-MinimumVersion"), minimum});
    require(process.waitForStarted(), "runtime selector starts");
    if (!process.waitForFinished(15000)) {
        process.kill(); process.waitForFinished();
        throw std::runtime_error("runtime selector timed out");
    }
    const auto output = process.readAll();
    const bool ok = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    if (ok != success) {
        std::cerr << output.constData();
        throw std::runtime_error("unexpected runtime selection result");
    }
    if (!success) require(QString::fromUtf8(output).simplified().contains(
        QStringLiteral("Install the latest Microsoft x64 Visual C++ Redistributable")),
        "invalid runtime reports actionable repair instructions");
    return output;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        require(argc == 4, "expected runtime selector, staged executable and compiler runtime folder");
        const QString script = QString::fromLocal8Bit(argv[1]);
        const QString staged = QFileInfo(QString::fromLocal8Bit(argv[2])).absolutePath();
        const QString toolset = QString::fromLocal8Bit(argv[3]);
        QTemporaryDir empty;
        require(empty.isValid(), "create empty runtime candidate");
        const auto selected = QJsonDocument::fromJson(select(script, toolset, staged)).array();
        require(selected.size() >= 8, "select a complete modern x64 runtime");
        const auto reversed = QJsonDocument::fromJson(select(script, staged, toolset)).array();
        // With equal versions the first candidate wins; their DLLs are equivalent.
        require(reversed.size() == selected.size(), "candidate ordering retains the complete runtime set");
        const auto stagedOnly = QJsonDocument::fromJson(select(script, empty.path(), staged)).array();
        require(stagedOnly.size() >= 8, "ordinary build stages a compatible runtime beside the public executable");
        select(script, empty.path(), empty.path(), false);
        select(script, staged, staged, false, QStringLiteral("99.0.0.0"));

        QTemporaryDir incomplete;
        require(incomplete.isValid(), "create incomplete runtime candidate");
        require(QFile::copy(QDir(staged).filePath(QStringLiteral("msvcp140.dll")),
            incomplete.filePath(QStringLiteral("msvcp140.dll"))), "copy one runtime dependency");
        select(script, incomplete.path(), empty.path(), false);
        require(QJsonDocument::fromJson(select(script, incomplete.path(), staged)).array().size() >= 8,
            "incomplete candidate falls back to the complete installed set");

        QTemporaryDir wrongArchitecture;
        require(wrongArchitecture.isValid(), "create wrong-architecture runtime candidate");
        for (const auto& entry : stagedOnly) {
            const QString path = entry.toString();
            require(QFile::copy(path, wrongArchitecture.filePath(QFileInfo(path).fileName())),
                "copy runtime fixture");
        }
        QFile dll(wrongArchitecture.filePath(QStringLiteral("msvcp140.dll")));
        require(dll.open(QIODevice::ReadWrite), "open architecture fixture");
        const auto bytes = dll.readAll();
        require(bytes.size() > 64, "runtime has a DOS header");
        const auto* data = reinterpret_cast<const unsigned char*>(bytes.constData());
        const auto offset = static_cast<qint64>(data[60]) | (static_cast<qint64>(data[61]) << 8) |
            (static_cast<qint64>(data[62]) << 16) | (static_cast<qint64>(data[63]) << 24);
        require(dll.seek(offset + 4) && dll.write("\x4c\x01", 2) == 2, "mark one DLL as x86");
        dll.close();
        select(script, wrongArchitecture.path(), empty.path(), false);
        std::cout << "Windows runtime selection checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

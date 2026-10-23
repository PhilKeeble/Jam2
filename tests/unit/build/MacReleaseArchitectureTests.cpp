#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>

#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Result { int exitCode; QByteArray output; };

Result run(const QString& program, const QStringList& arguments)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, arguments);
    require(process.waitForStarted(10000), "architecture validation process starts");
    if (!process.waitForFinished(30000)) {
        process.kill();
        process.waitForFinished(5000);
        throw std::runtime_error("architecture validation process exceeded its deadline");
    }
    require(process.exitStatus() == QProcess::NormalExit, "architecture validation process exits normally");
    return {process.exitCode(), process.readAll()};
}

void write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(),
        "architecture fixture can be written");
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        require(argc == 5, "expected CMake, C compiler, architecture helper and workspace");
        const QString cmake = QString::fromLocal8Bit(argv[1]);
        const QString compiler = QString::fromLocal8Bit(argv[2]);
        const QString helper = QString::fromLocal8Bit(argv[3]);
        const QString workspace = QString::fromLocal8Bit(argv[4]);
        require(QDir().mkpath(workspace), "architecture test workspace exists");
        QTemporaryDir temporary(QDir(workspace).filePath(QStringLiteral("mac architecture-XXXXXX")));
        require(temporary.isValid(), "architecture fixtures have a private directory with spaces");
        const QString source = temporary.filePath(QStringLiteral("fixture.c"));
        write(source, "int jam2_architecture_fixture;\n");
        const QString script = temporary.filePath(QStringLiteral("verify.cmake"));
        write(script, "include([==[" + helper.toUtf8() + "]==])\n"
            "jam2_require_universal_macos(\"${JAM2_TEST_BINARY}\")\n");
        QStringList objects;
        for (const QString& architecture : {QStringLiteral("arm64"), QStringLiteral("x86_64")}) {
            const QString object = temporary.filePath(architecture + QStringLiteral(".o"));
            const auto compiled = run(compiler, {QStringLiteral("-arch"), architecture,
                QStringLiteral("-c"), source, QStringLiteral("-o"), object});
            if (compiled.exitCode != 0) std::cerr << compiled.output.constData();
            require(compiled.exitCode == 0, "Apple compiler creates a real Mach-O fixture");
            objects.append(object);
        }
        const QString universal = temporary.filePath(QStringLiteral("universal.o"));
        require(run(QStringLiteral("/usr/bin/lipo"), {objects[0], objects[1],
            QStringLiteral("-create"), QStringLiteral("-output"), universal}).exitCode == 0,
            "Apple lipo creates the universal fixture");
        const auto verify = [&](const QString& binary) {
            return run(cmake, {QStringLiteral("-DJAM2_TEST_BINARY=") + binary,
                QStringLiteral("-P"), script});
        };
        const auto accepted = verify(universal);
        if (accepted.exitCode != 0) std::cerr << accepted.output.constData();
        require(accepted.exitCode == 0, "production architecture guard accepts Intel/Apple Silicon binaries");
        const QString invalid = temporary.filePath(QStringLiteral("invalid.o"));
        write(invalid, "not a Mach-O file\n");
        objects.append(invalid);
        objects.append(temporary.filePath(QStringLiteral("missing.o")));
        for (const QString& binary : objects) {
            const auto rejected = verify(binary);
            require(rejected.exitCode != 0 && rejected.output.contains("require both arm64 and x86_64") &&
                    rejected.output.contains("QT_DIR"),
                "production guard rejects thin, invalid and missing binaries with actionable diagnostics");
        }
        std::cout << "Mac release architecture checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

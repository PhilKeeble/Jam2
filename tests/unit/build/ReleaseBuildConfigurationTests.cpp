#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>

#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
QString cacheValue(const QString& cache, const QString& key) {
    for (const QString& line : cache.split(QLatin1Char('\n'))) {
        if (line.startsWith(key + QLatin1Char(':'))) return line.section(QLatin1Char('='), 1).trimmed();
    }
    return {};
}
struct Result { int exitCode; QString output; };
Result run(const QString& executable, const QStringList& arguments, const QString& directory) {
    QProcess process;
    process.setWorkingDirectory(directory);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(executable, arguments);
    require(process.waitForStarted(10000), "build configuration child process starts");
    if (!process.waitForFinished(30000)) {
        process.kill(); process.waitForFinished(5000);
        throw std::runtime_error("build configuration child process exceeded its deadline");
    }
    require(process.exitStatus() == QProcess::NormalExit, "build configuration child process exits normally");
    return {process.exitCode(), QString::fromLocal8Bit(process.readAll())};
}
}
int main(int argc, char** argv) {
    try {
        QCoreApplication app(argc, argv);
        require(argc == 5, "expected cmake, cache, source directory and generator");
        const QString cmake = QString::fromLocal8Bit(argv[1]);
        const QString source = QString::fromLocal8Bit(argv[3]);
        const QString generator = QString::fromLocal8Bit(argv[4]);
        QFile file(QString::fromLocal8Bit(argv[2]));
        require(file.open(QIODevice::ReadOnly), "configured build cache is readable");
        const QString cache = QString::fromUtf8(file.readAll());
        const QString configurations = cacheValue(cache, QStringLiteral("CMAKE_CONFIGURATION_TYPES"));
        require(configurations.isEmpty()
                ? cacheValue(cache, QStringLiteral("CMAKE_BUILD_TYPE")) == QStringLiteral("Release")
                : configurations == QStringLiteral("Release"),
            "the configured project supports only Release");
        for (const QString& language : {QStringLiteral("C"), QStringLiteral("CXX")}) {
            const QString flags = cacheValue(cache, QStringLiteral("CMAKE_%1_FLAGS_RELEASE").arg(language));
#ifdef Q_OS_WIN
            require(flags.contains(QStringLiteral("/O2")) && !flags.contains(QStringLiteral("/Od")) &&
                !flags.contains(QStringLiteral("/Ob0")), "MSVC Release remains optimized without instrumentation overrides");
#else
            require(flags.contains(QStringLiteral("-O")) && !flags.contains(QStringLiteral("-O0")),
                "Release remains optimized");
#endif
        }
        require(!cacheValue(cache, QStringLiteral("CMAKE_EXE_LINKER_FLAGS_RELEASE")).contains(QStringLiteral("/PROFILE")),
            "Release linking does not enable native coverage instrumentation");
        if (configurations.isEmpty()) {
            QTemporaryDir rejectedBuild;
            require(rejectedBuild.isValid(), "rejected configuration has a private build directory");
            const auto debug = run(cmake, {QStringLiteral("-S"), source, QStringLiteral("-B"), rejectedBuild.path(),
                QStringLiteral("-G"), generator, QStringLiteral("-DCMAKE_BUILD_TYPE=Debug"),
                QStringLiteral("-DBUILD_TESTING=OFF")}, source);
            require(debug.exitCode != 0 && debug.output.contains(QStringLiteral("Jam2 only supports Release builds")),
                "a Debug configuration is rejected before any Jam2 build runs");
        }
#ifdef Q_OS_WIN
        const auto removedOption = run(QStringLiteral("cmd.exe"), {QStringLiteral("/d"), QStringLiteral("/c"),
            QStringLiteral("call compile.cmd --in-dev-shell --coverage")}, source);
#else
        const auto removedOption = run(QStringLiteral("bash"), {QStringLiteral("./compile.sh"), QStringLiteral("--coverage")}, source);
#endif
        require(removedOption.exitCode == 2 && removedOption.output.contains(QStringLiteral("Unknown compile option: --coverage")),
            "the removed coverage option is rejected without building or running tests");
        std::cout << "Release build configuration checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

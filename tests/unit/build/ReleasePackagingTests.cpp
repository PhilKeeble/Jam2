#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>

#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

QByteArray read(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "could not read release artifact");
    return file.readAll();
}

QByteArray run(const QString& program, const QStringList& arguments, bool success = true)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, arguments);
    require(process.waitForStarted(), "release validation process did not start");
    if (!process.waitForFinished(90000)) {
        process.kill();
        process.waitForFinished();
        throw std::runtime_error("release validation process timed out");
    }
    const QByteArray output = process.readAll();
    const bool ok = process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
    if (ok != success) {
        std::cerr << output.constData();
        throw std::runtime_error("unexpected release validation exit status");
    }
    return output;
}
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        require(argc == 6, "expected CMake, config, package script, executable and version");
        const QString cmake = QString::fromLocal8Bit(argv[1]);
        const QString config = QString::fromLocal8Bit(argv[2]);
        const QString script = QString::fromLocal8Bit(argv[3]);
        const QString executable = QString::fromLocal8Bit(argv[4]);
        const QString version = QString::fromLocal8Bit(argv[5]);
        require(QString::fromUtf8(run(executable, {QStringLiteral("--version")})).trimmed()
                == QStringLiteral("Jam2 ") + version,
            "public executable reports the compiled release version");

        const QString workspace = QDir(QFileInfo(config).absolutePath()).filePath(
            QStringLiteral("test-artifacts"));
        require(QDir().mkpath(workspace), "could not create build-local test workspace");
        QTemporaryDir temporary(QDir(workspace).filePath(QStringLiteral("release-package-XXXXXX")));
        require(temporary.isValid(), "could not create isolated package output");
        const QString output = temporary.filePath(QStringLiteral("dist"));
        run(cmake, {QStringLiteral("-DJAM2_PACKAGE_CONFIG=") + config,
            QStringLiteral("-DJAM2_PACKAGE_OUTPUT=") + output,
            QStringLiteral("-DJAM2_INCLUDE_MODELS=OFF"), QStringLiteral("-P"), script});
#ifdef Q_OS_WIN
        const QString name = QStringLiteral("Jam2-windows.zip");
#else
        const QString name = QStringLiteral("Jam2-macos.zip");
#endif
        const QString archive = QDir(output).filePath(name);
        const auto metadata = QJsonDocument::fromJson(read(archive + QStringLiteral(".json"))).object();
        require(metadata.value(QStringLiteral("version")).toString() == version &&
                metadata.value(QStringLiteral("tag")).toString() == QStringLiteral("v") + version,
            "archive metadata pins the compiled version and release tag");
        QFile zip(archive);
        require(zip.open(QIODevice::ReadOnly), "application ZIP exists");
        QCryptographicHash hash(QCryptographicHash::Sha256);
        require(hash.addData(&zip), "application ZIP hash can be computed");
        require(metadata.value(QStringLiteral("bytes")).toVariant().toLongLong() == zip.size() &&
                metadata.value(QStringLiteral("sha256")).toString().toLatin1() == hash.result().toHex(),
            "archive metadata matches the finished ZIP bytes");
        zip.close();
        const auto entries = run(cmake, {QStringLiteral("-E"), QStringLiteral("tar"),
            QStringLiteral("tf"), archive}).split('\n');
        require(entries.contains("version.txt") && entries.contains("package-manifest.json") &&
                entries.contains("models.json"), "application ZIP carries its version and manifests");
        for (const QByteArray& entry : entries) {
            require(!entry.endsWith(".onnx") && !entry.startsWith("settings/") &&
                    !entry.startsWith("tracks/") && !entry.startsWith("songs/") &&
                    !entry.startsWith("update/") && !entry.startsWith("logs/") &&
                    !entry.startsWith("cache/") && !entry.startsWith("temp/") &&
                    !entry.contains("..") && !entry.startsWith('/'),
                "application archive excludes weights, user data and escaping paths");
        }
        require(!QFile::exists(QDir(output).filePath(QStringLiteral("Jam2-models.zip"))),
            "models are not packaged unless requested");

        // Exercise the production packager's rejection before it can include weights.
        const QString rejectedConfig = temporary.filePath(QStringLiteral("rejected.cmake"));
        QFile changed(rejectedConfig);
        require(changed.open(QIODevice::WriteOnly), "could not write invalid package configuration");
        changed.write(read(config));
        changed.write("\nlist(APPEND JAM2_PACKAGE_APP_FILES components/jamtaster/models/beat_this.onnx)\n");
        changed.close();
        const auto rejected = run(cmake, {QStringLiteral("-DJAM2_PACKAGE_CONFIG=") + rejectedConfig,
            QStringLiteral("-DJAM2_PACKAGE_OUTPUT=") + temporary.filePath(QStringLiteral("rejected")),
            QStringLiteral("-P"), script}, false);
        require(rejected.contains("Model weights or user data") || rejected.contains("Missing or nonportable"),
            "a declared model payload cannot enter the app archive");
        require(!QFile::exists(temporary.filePath(QStringLiteral("rejected/") + name)),
            "invalid payload does not produce an application ZIP");
        std::cout << "Release packaging tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Release packaging failure: " << error.what() << '\n';
        return 1;
    }
}

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "write publication fixture");
}
QByteArray read(const QString& path)
{
    QFile file(path); require(file.open(QIODevice::ReadOnly), "read publication trace"); return file.readAll();
}
void testPlatform(const QString& cmake, const QString& script, const QString& workspace, const QString& platform)
{
    QTemporaryDir temp(QDir(workspace).filePath(QStringLiteral("publish %1-XXXXXX").arg(platform)));
    require(temp.isValid() && QDir().mkpath(temp.filePath(QStringLiteral("dist"))), "create private publication fixture");
    const QString asset = platform == QStringLiteral("macos") ? QStringLiteral("Jam2-macos.pkg") : QStringLiteral("Jam2-windows.zip");
    const QByteArray payload("release artifact fixture");
    const QString package = temp.filePath(QStringLiteral("dist/") + asset);
    write(package, payload);
    QJsonObject metadata{{"schema", 1}, {"channel", "stable"}, {"version", "1.2.3"}, {"tag", "v1.2.3"},
        {"asset", asset}, {"bytes", payload.size()},
        {"sha256", QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex())},
        {"models_tag", "models-0123456789abcdef"}, {"models_manifest_sha256", QString(64, 'a')}};
    write(package + QStringLiteral(".json"), QJsonDocument(metadata).toJson());
    const QString config = temp.filePath(QStringLiteral("config.cmake"));
    write(config, QStringLiteral("set(JAM2_PACKAGE_SOURCE [==[%1]==])\nset(JAM2_PACKAGE_RELEASE [==[%1]==])\n"
        "set(JAM2_PACKAGE_OUTPUT [==[%1/dist]==])\nset(JAM2_PACKAGE_VERSION 1.2.3)\nset(JAM2_PACKAGE_PLATFORM %2)\n")
        .arg(temp.path(), platform).toUtf8());
    const QString gh = temp.filePath(QStringLiteral("fake-gh"));
    write(gh, R"sh(#!/bin/bash
printf '%s\n' "$*" >> "$JAM2_PUBLICATION_TRACE"
if [[ "$1" == auth ]]; then exit 0; fi
if [[ "$1" == api ]]; then
    if [[ "$*" == *--paginate* ]]; then
        printf '[[%s]]\n' "$JAM2_PUBLICATION_RELEASE"
    else
        printf '%s\n' "$JAM2_PUBLICATION_RELEASE"
    fi
    exit 0
fi
if [[ "$1" == release && "$2" == upload ]]; then exit 0; fi
exit 99
)sh");
    require(QFile::setPermissions(gh, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner), "make fake GitHub CLI executable");
    const QString trace = temp.filePath(QStringLiteral("trace"));
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("JAM2_PUBLICATION_TRACE"), trace);
    env.insert(QStringLiteral("JAM2_PUBLICATION_RELEASE"), QStringLiteral(
        "{\"id\":1,\"tag_name\":\"v1.2.3\",\"draft\":true,\"prerelease\":false,\"immutable\":false,\"assets\":[]}"));
    auto run = [&] {
        QProcess process; process.setProcessEnvironment(env); process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(cmake, {QStringLiteral("-DJAM2_PACKAGE_CONFIG=") + config,
            QStringLiteral("-DJAM2_GH_EXECUTABLE:FILEPATH=") + gh, QStringLiteral("-P"), script});
        require(process.waitForFinished(30000) && process.exitStatus() == QProcess::NormalExit, "publication finishes using only fake GitHub operations");
        return std::pair{process.exitCode(), process.readAll()};
    };
    const auto [code, output] = run();
    if (code != 0) std::cerr << output.constData();
    require(code == 0, "verified local artifact uploads and waits for the other release assets");
    const QByteArray commands = read(trace);
    require(commands.contains("release upload v1.2.3") && commands.contains(asset.toUtf8()) &&
            commands.contains((asset + QStringLiteral(".json")).toUtf8()) && !commands.contains("Jam2-macos.zip"),
        "publisher uploads the correct platform format and matching metadata");
    require(output.contains("Jam2-macos.pkg and verified metadata") &&
            !commands.contains("release edit") && !commands.contains("release delete"),
        "readiness requires the macOS PKG and never publishes or deletes releases while assets are missing");
    metadata.insert(QStringLiteral("sha256"), QString(64, '0'));
    write(package + QStringLiteral(".json"), QJsonDocument(metadata).toJson());
    write(trace, {});
    const auto [badCode, badOutput] = run();
    require(badCode != 0 && badOutput.contains("does not match") && read(trace).isEmpty(),
        "mismatched local artifact metadata fails before any GitHub operation");
}
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    try {
        require(argc == 4, "expected CMake, publisher and artifact root");
        for (const auto& platform : {QStringLiteral("macos"), QStringLiteral("windows")})
            testPlatform(QString::fromLocal8Bit(argv[1]), QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]), platform);
        std::cout << "Release platform publication checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

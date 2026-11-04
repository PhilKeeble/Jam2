#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
void write(const QString& path, const QByteArray& bytes, bool executable = false)
{
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "create fixture parent");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "write fixture");
    if (executable) require(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner), "make fake tool executable");
}
QByteArray read(const QString& path)
{
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "read fixture artifact");
    return file.readAll();
}

// Run the actual update.sh with deterministic external tools. No real package
// manager, Git checkout or network operation is invoked here. One SDK case
// compiles and loads the real offline probe with the selected Apple toolchain.
void scenario(const QString& script, const QString& artifacts, const QString& name)
{
    QTemporaryDir fixture(QDir(artifacts).filePath(QStringLiteral("mac update %1-XXXXXX").arg(name)));
    require(fixture.isValid(), "create isolated update fixture");
    const QDir root(fixture.path());
    const QString qt = root.filePath(QStringLiteral("qt sdk"));
    require(QDir().mkpath(root.filePath(QStringLiteral("tools/macos"))) &&
            QDir().mkpath(root.filePath(QStringLiteral("bin"))) &&
            QDir().mkpath(root.filePath(QStringLiteral("tmp"))) &&
            QFile::copy(script, root.filePath(QStringLiteral("update.sh"))), "copy actual update script");
    const QString probe = QDir(QFileInfo(script).absolutePath()).filePath(QStringLiteral("tools/macos/QtTlsProbe.cpp"));
    require(QFile::copy(probe, root.filePath(QStringLiteral("tools/macos/QtTlsProbe.cpp"))), "copy actual TLS probe source");
    for (const auto& module : {"Qt6", "Qt6Core", "Qt6Gui", "Qt6Widgets", "Qt6Network"}) {
        write(QDir(qt).filePath(QStringLiteral("lib/cmake/%1/%1Config.cmake").arg(QLatin1String(module))), {});
    }
    require(QDir().mkpath(QDir(qt).filePath(QStringLiteral("plugins/tls"))), "create TLS plugin fixture");
    write(root.filePath(QStringLiteral("qmake-template")), R"sh(#!/bin/bash
case "$2" in
    QT_VERSION) echo 6.11.1 ;;
    QT_INSTALL_LIBS) echo "$JAM2_FIXTURE/qt sdk/lib" ;;
    QT_INSTALL_PLUGINS) echo "$JAM2_FIXTURE/qt sdk/plugins" ;;
    *) exit 1 ;;
esac
)sh", true);
    write(root.filePath(QStringLiteral("bin/uname")), "#!/bin/bash\necho Darwin\n", true);
    write(root.filePath(QStringLiteral("bin/xcode-select")), "#!/bin/bash\nexit 0\n", true);
    write(root.filePath(QStringLiteral("bin/git")), R"sh(#!/bin/bash
printf 'git %s\n' "$*" >> "$JAM2_FIXTURE/trace"
exit 0
)sh", true);
    write(root.filePath(QStringLiteral("bin/brew")), R"sh(#!/bin/bash
printf 'brew %s\n' "$*" >> "$JAM2_FIXTURE/trace"
case "$1" in
    shellenv) exit 0 ;;
    --prefix) echo "$JAM2_FIXTURE/qt sdk" ;;
    list) [[ -f "$JAM2_FIXTURE/installed" ]] ;;
    install|reinstall)
        [[ "$JAM2_SCENARIO" != install-failure ]] || exit 8
        touch "$JAM2_FIXTURE/installed"
        cp "$JAM2_FIXTURE/qmake-template" "$JAM2_FIXTURE/qt sdk/bin/qmake"
        [[ "$JAM2_SCENARIO" == still-broken ]] || touch "$JAM2_FIXTURE/ready"
        ;;
    *) exit 1 ;;
esac
)sh", true);
    write(root.filePath(QStringLiteral("bin/xcrun")), R"sh(#!/bin/bash
printf 'xcrun %s\n' "$*" >> "$JAM2_FIXTURE/trace"
[[ "$JAM2_SCENARIO" != compile-failure ]] || exit 9
while (( $# )); do
    if [[ "$1" == -o ]]; then
        shift
        cat > "$1" <<'PROBE'
#!/bin/bash
printf 'probe %s\n' "$*" >> "$JAM2_FIXTURE/trace"
[[ -f "$JAM2_FIXTURE/ready" ]] || exit 1
echo 'Qt TLS backend: securetransport'
PROBE
        chmod +x "$1"
        exit 0
    fi
    shift
done
exit 1
)sh", true);
    require(QDir().mkpath(QDir(qt).filePath(QStringLiteral("bin"))), "create qmake directory");
    const bool realSdk = name == QStringLiteral("real-sdk");
    if (realSdk) {
        require(QFile::remove(root.filePath(QStringLiteral("bin/xcrun"))) &&
                QFile::link(QStringLiteral("/usr/bin/xcrun"), root.filePath(QStringLiteral("bin/xcrun"))),
            "use the real Apple compiler for the SDK integration check");
    }
    const bool custom = name.startsWith(QStringLiteral("custom-")) || realSdk;
    if (name != QStringLiteral("missing") && name != QStringLiteral("install-failure")) {
        write(root.filePath(QStringLiteral("installed")), {});
        require(QFile::copy(root.filePath(QStringLiteral("qmake-template")),
                QDir(qt).filePath(QStringLiteral("bin/qmake"))), "install fixture qmake");
    }
    if (name == QStringLiteral("satisfied") || name == QStringLiteral("custom-satisfied")) {
        write(root.filePath(QStringLiteral("ready")), {});
    }
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PATH"), root.filePath(QStringLiteral("bin")) + QStringLiteral(":/usr/bin:/bin"));
    environment.insert(QStringLiteral("JAM2_FIXTURE"), root.path());
    environment.insert(QStringLiteral("JAM2_SCENARIO"), name);
    environment.insert(QStringLiteral("TMPDIR"), root.filePath(QStringLiteral("tmp")));
    environment.remove(QStringLiteral("QT_DIR"));
    if (custom) environment.insert(QStringLiteral("QT_DIR"), realSdk
        ? QLibraryInfo::path(QLibraryInfo::PrefixPath) : qt);
    QProcess process;
    process.setProcessEnvironment(environment);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(QStringLiteral("/bin/bash"), {root.filePath(QStringLiteral("update.sh"))});
    require(process.waitForStarted(10000), "start update script");
    if (!process.waitForFinished(30000)) {
        process.kill();
        process.waitForFinished();
        throw std::runtime_error("update fixture timed out");
    }
    const QByteArray output = process.readAll();
    const QByteArray trace = read(root.filePath(QStringLiteral("trace")));
    const bool success = name == QStringLiteral("satisfied") || name == QStringLiteral("missing") ||
        name == QStringLiteral("broken") || name == QStringLiteral("custom-satisfied") || realSdk;
    if ((process.exitCode() == 0) != success) {
        std::cerr << name.toStdString() << ": " << output.constData();
        throw std::runtime_error("update result did not match TLS prerequisite state");
    }
    require(process.exitStatus() == QProcess::NormalExit, "update script exits normally");
    require(output.contains("UPDATE SUCCEEDED") == success, "success is only reported after TLS validation");
    require(trace.contains("pull --ff-only") == success, "TLS failure prevents repository update");
    const bool install = name == QStringLiteral("missing") || name == QStringLiteral("install-failure");
    const bool repair = name == QStringLiteral("broken") || name == QStringLiteral("still-broken") || name == QStringLiteral("compile-failure");
    require(trace.count("brew install qtbase") == (install ? 1 : 0), "install only absent dependencies, once");
    require(trace.count("brew reinstall qtbase") == (repair ? 1 : 0), "repair only failed Homebrew Qt, once");
    if (custom) require(!trace.contains("brew "), "custom SDK never installs unrelated Homebrew Qt");
    if (success) require(output.contains("Qt TLS backend: "), "selected Qt TLS backend is observable");
    if (trace.contains("xcrun")) {
        require(trace.contains("clang++ -std=c++17 -O2") && trace.contains("tools/macos/QtTlsProbe.cpp"),
            "the real offline probe source is compiled with Apple tooling and optimization");
    }
    require(QDir(root.filePath(QStringLiteral("tmp"))).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty(),
        "temporary TLS compiler output is cleaned on success and failure");
}
}

int main(int argc, char** argv)
{
    try {
        QCoreApplication app(argc, argv);
        require(argc == 3, "expected update script and artifact directory");
        for (const QString& name : {QStringLiteral("satisfied"), QStringLiteral("missing"),
                QStringLiteral("broken"), QStringLiteral("still-broken"), QStringLiteral("install-failure"),
                QStringLiteral("compile-failure"), QStringLiteral("custom-satisfied"), QStringLiteral("custom-broken"),
                QStringLiteral("real-sdk")}) {
            scenario(QString::fromLocal8Bit(argv[1]), QString::fromLocal8Bit(argv[2]), name);
        }
        std::cout << "Mac update prerequisite checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}

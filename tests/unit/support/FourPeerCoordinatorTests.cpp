#include "FourPeerCoordinator.hpp"
#include "TestTiming.hpp"

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>

#include <filesystem>
#include <iostream>

namespace {

int failures = 0;

void check(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

bool isWithin(const QString& candidate, const QString& root)
{
    QString normalizedCandidate = QDir::fromNativeSeparators(
        QFileInfo(candidate).absoluteFilePath());
    QString normalizedRoot = QDir::fromNativeSeparators(
        QFileInfo(root).absoluteFilePath());
    normalizedCandidate = QDir::cleanPath(normalizedCandidate);
    normalizedRoot = QDir::cleanPath(normalizedRoot);
#ifdef Q_OS_WIN
    constexpr auto comparison = Qt::CaseInsensitive;
#else
    constexpr auto comparison = Qt::CaseSensitive;
#endif
    return normalizedCandidate.compare(normalizedRoot, comparison) == 0 ||
        normalizedCandidate.startsWith(normalizedRoot + QLatin1Char('/'), comparison);
}

QString exerciseFailedLaunch(bool markSuccessful)
{
    QTemporaryDir missingExecutableParent;
    check(missingExecutableParent.isValid(), "could not create missing-executable parent");

    FourPeerCoordinator coordinator;
    QString error;
    const bool launched = coordinator.launch(
        QDir(missingExecutableParent.path()).absoluteFilePath(
            QStringLiteral("definitely-not-jam2.exe")),
        {},
        error);
    check(!launched, "a nonexistent peer executable unexpectedly launched");
    check(!error.isEmpty(), "failed peer launch omitted its diagnostic");
    const QString root = coordinator.storageRoot(0);
    check(QFileInfo::exists(root), "failed peer storage was absent before cleanup");
    if (markSuccessful) coordinator.markSuccessful();
    return root;
}

} // namespace

int main()
{
    const QString artifactRoot = qEnvironmentVariable("JAM2_TEST_ARTIFACT_ROOT");
    check(!artifactRoot.isEmpty(),
        "CTest omitted the build-local artifact root");
    check(isWithin(QDir::tempPath(), artifactRoot),
        "Qt temporary paths escaped the build-local artifact root");
    check(isWithin(QString::fromStdWString(
            std::filesystem::temp_directory_path().wstring()), artifactRoot),
        "standard-library temporary paths escaped the build-local artifact root");
    QTemporaryDir placementProbe;
    check(placementProbe.isValid(),
        "could not create a build-local temporary placement probe");
    check(isWithin(placementProbe.path(), artifactRoot),
        "QTemporaryDir escaped the build-local artifact root");

    const QByteArray previousScale = qgetenv("JAM2_TEST_TIMEOUT_SCALE");
    const bool hadPreviousScale = qEnvironmentVariableIsSet("JAM2_TEST_TIMEOUT_SCALE");
    qputenv("JAM2_TEST_TIMEOUT_SCALE", "4");
    check(jam2::test::scaledTimeout(std::chrono::milliseconds(7)) ==
            std::chrono::milliseconds(28),
        "coverage timeout scaling did not apply exactly");
    qputenv("JAM2_TEST_TIMEOUT_SCALE", "invalid");
    check(jam2::test::scaledTimeout(std::chrono::milliseconds(7)) ==
            std::chrono::milliseconds(7),
        "invalid timeout scaling did not fail closed to one");
    if (hadPreviousScale) {
        qputenv("JAM2_TEST_TIMEOUT_SCALE", previousScale);
    } else {
        qunsetenv("JAM2_TEST_TIMEOUT_SCALE");
    }

    const QString retainedRoot = exerciseFailedLaunch(false);
    check(QFileInfo(retainedRoot).isDir(),
        "an unmarked coordinator deleted failed-test artifacts");
    if (QFileInfo(retainedRoot).isDir()) {
        check(QDir(retainedRoot).removeRecursively(),
            "could not clean the deliberately retained test fixture");
    }

    const QString successfulRoot = exerciseFailedLaunch(true);
    check(!QFileInfo::exists(successfulRoot),
        "a successful coordinator retained temporary artifacts");

    if (failures != 0) {
        std::cerr << failures << " four-peer coordinator checks failed\n";
        return 1;
    }
    std::cout << "four-peer coordinator artifact checks passed\n";
    return 0;
}

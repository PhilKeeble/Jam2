#include "FourPeerCoordinator.hpp"
#include "LoopbackPortReservations.hpp"
#include "TestTiming.hpp"

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>

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
    using namespace std::chrono_literals;
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

    LoopbackPortReservations loopbackPorts;
    QString reservationError;
    const bool portsReserved = loopbackPorts.reserve(16, reservationError);
    std::set<std::uint16_t> reservedPorts;
    if (!portsReserved) {
        check(false, qPrintable(QStringLiteral(
            "could not reserve 16 ports valid for both loopback TCP and UDP: %1")
                .arg(reservationError)));
    }
    if (portsReserved) {
        for (std::size_t index = 0; index < 16; ++index) {
            reservedPorts.insert(loopbackPorts.port(index));
        }
        check(reservedPorts.size() == 16,
            "TCP/UDP loopback reservations were not unique");
        bool rejectedOutOfRange = false;
        try {
            (void)loopbackPorts.port(16);
        } catch (const std::out_of_range&) {
            rejectedOutOfRange = true;
        }
        check(rejectedOutOfRange,
            "loopback reservation accepted an out-of-range index");
    }
    loopbackPorts.release();
    for (const std::uint16_t port : reservedPorts) {
        QUdpSocket udp;
        QTcpServer tcp;
        const bool udpRebound = udp.bind(
            QHostAddress::LocalHost,
            port,
            QUdpSocket::DontShareAddress);
        const bool tcpRebound = tcp.listen(QHostAddress::LocalHost, port);
        check(udpRebound && tcpRebound,
            "released loopback reservation retained a TCP or UDP port");
    }

    reservationError.clear();
    check(loopbackPorts.reserve(16, reservationError),
        "loopback ports could not be reserved again after release");
    loopbackPorts.release();

    check(jam2::test::deadmanTimeout(7ms) == 7ms,
        "deadman timeout unexpectedly depends on machine speed");

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

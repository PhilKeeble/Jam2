#include "TestTiming.hpp"

#include <QCoreApplication>
#include <QProcess>

#include <chrono>
#include <iostream>

namespace {

int failures = 0;

void checkWorker(
    const QString& executable,
    const QByteArray& expectedDiagnostic,
    const char* label)
{
    QProcess process;
    process.start(executable, {});
    const auto timeout = jam2::test::scaledTimeout(std::chrono::seconds(20));
    const int timeoutMs = static_cast<int>(timeout.count());
    if (!process.waitForStarted(timeoutMs)) {
        ++failures;
        std::cerr << "FAIL: " << label << " did not start: "
                  << process.errorString().toStdString() << '\n';
        return;
    }
    if (!process.waitForFinished(timeoutMs)) {
        ++failures;
        process.kill();
        process.waitForFinished();
        std::cerr << "FAIL: " << label << " did not reject an empty invocation\n";
        return;
    }
    const QByteArray output = process.readAllStandardOutput() +
        process.readAllStandardError();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 2) {
        ++failures;
        std::cerr << "FAIL: " << label << " returned " << process.exitCode()
                  << " instead of the argument-contract code 2\n";
    }
    if (!output.contains(expectedDiagnostic)) {
        ++failures;
        std::cerr << "FAIL: " << label << " omitted its usage diagnostic\n";
    }
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc != 3) {
        std::cerr << "usage: jam2_worker_entrypoint_tests "
                     "<jamtaster-worker> <plugin-worker>\n";
        return 2;
    }

    checkWorker(QString::fromLocal8Bit(argv[1]),
        QByteArrayLiteral("Usage: jamtaster-worker"), "JamTaster worker");
    checkWorker(QString::fromLocal8Bit(argv[2]),
        QByteArrayLiteral("usage: jam2-plugin-worker"), "plugin worker");

    if (failures != 0) {
        std::cerr << failures << " worker entrypoint checks failed\n";
        return 1;
    }
    std::cout << "private worker entrypoint checks passed\n";
    return 0;
}

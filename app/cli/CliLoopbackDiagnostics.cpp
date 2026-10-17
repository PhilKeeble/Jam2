#include "CliLoopbackDiagnostics.hpp"

#include "GuiLoopbackRecorder.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFileInfo>
#include <QTimer>

#include <iostream>
#include <string_view>

int jam2RunLoopbackDiagnostic(int argc, char* argv[])
{
    if (argc == 4 && (std::string_view(argv[3]) == "--help" ||
            std::string_view(argv[3]) == "-h")) {
        std::cout << "Usage: jam2 debug loopback <new-output.wav> <seconds:1..30> [source-id]\n"
            "Records the default or selected output using the same loopback backend as track recording.\n";
        return 0;
    }
    if (argc != 5 && argc != 6) {
        std::cerr << "Usage: jam2 debug loopback <new-output.wav> <seconds:1..30> [source-id]\n";
        return 2;
    }
    bool validSeconds = false;
    const int seconds = QString::fromLocal8Bit(argv[4]).toInt(&validSeconds);
    const QString output = QString::fromLocal8Bit(argv[3]).trimmed();
    if (!validSeconds || seconds < 1 || seconds > 30 || output.isEmpty() ||
        QFileInfo::exists(output)) {
        std::cerr << "Loopback diagnostic requires a new output path and a duration of 1..30 seconds\n";
        return 2;
    }

    QEventLoop loop;
    GuiLoopbackRecorder recorder;
    GuiLoopbackOptions options;
    options.outputPath = QFileInfo(output).absoluteFilePath();
    options.targetSampleRate = 48000;
    options.source = argc == 6 ? QString::fromLocal8Bit(argv[5]) : QStringLiteral("default");
    // Count captured frames, so first-use permission/startup time does not
    // consume the requested recording duration.
    options.durationBars = 1;
    options.beatsPerBar = 1;
    options.bpm = 60.0 / seconds;
    // Keep the complete timeline so the diagnostic can measure silence too.
    options.trimLeadingSilence = false;
    options.trimTrailingSilence = false;
    bool succeeded = false;
    QString error;
    if (!recorder.start(options, [&loop, &succeeded](bool ok, const QString& path,
            const QString& failure, const QString& diagnostics) {
            QMetaObject::invokeMethod(&loop, [&loop, &succeeded, ok, path, failure, diagnostics] {
                succeeded = ok;
                std::cout << diagnostics.toStdString() << '\n';
                if (ok) std::cout << "loopback WAV: " << path.toStdString() << '\n';
                else std::cerr << failure.toStdString() << '\n';
                loop.quit();
            }, Qt::QueuedConnection);
        }, &error)) {
        std::cerr << error.toStdString() << '\n';
        return 1;
    }
    QTimer::singleShot((seconds + 30) * 1000, &loop, [&recorder] { recorder.stop(); });
    std::cout << "loopback diagnostic capturing" << std::endl;
    loop.exec();
    return succeeded ? 0 : 1;
}

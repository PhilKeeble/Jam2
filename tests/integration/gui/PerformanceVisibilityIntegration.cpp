#include "AutomationProcess.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>

#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {
using namespace std::chrono_literals;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

QJsonObject receive(AutomationProcess& process, const QString& expected)
{
    QJsonObject event;
    QString error;
    if (!process.readEvent(event, 15s, error)) throw std::runtime_error(error.toStdString());
    if (event.value(QStringLiteral("event")).toString() != expected)
        throw std::runtime_error(QJsonDocument(event).toJson(QJsonDocument::Compact).toStdString());
    return event;
}

QJsonObject request(AutomationProcess& process, QJsonObject command, const QString& expected)
{
    static int sequence = 0;
    const QString id = QStringLiteral("visibility-%1").arg(++sequence);
    command.insert(QStringLiteral("id"), id);
    QString error;
    if (!process.send(command, error)) throw std::runtime_error(error.toStdString());
    const auto event = receive(process, expected);
    require(event.value(QStringLiteral("id")).toString() == id, "response id matches the navigation request");
    return event;
}

QJsonObject activity(AutomationProcess& process, bool requireRenderer = true)
{
    const auto event = request(process, {{QStringLiteral("type"), QStringLiteral("snapshot")},
        {QStringLiteral("control"), QStringLiteral("performance.home.track-gain")}}, QStringLiteral("snapshot"));
    const auto renderer = event.value(QStringLiteral("performance")).toObject()
        .value(QStringLiteral("home_renderer")).toObject();
    if (requireRenderer)
        require(renderer.contains(QStringLiteral("animation_ticks")), "real public app exposes renderer activity");
    return renderer;
}

void invoke(AutomationProcess& process, const QString& control)
{
    request(process, {{QStringLiteral("type"), QStringLiteral("invoke")},
        {QStringLiteral("control"), control}, {QStringLiteral("operation"), QStringLiteral("click")}},
        QStringLiteral("command_applied"));
}

QJsonObject measureCpu(AutomationProcess& process, const QString& page, unsigned long durationMs,
    bool requireRenderer)
{
    const auto before = process.cpuTimeNs();
    require(before.has_value(), "native process CPU time is available");
    QElapsedTimer elapsed; elapsed.start();
    QThread::msleep(durationMs);
    const auto after = process.cpuTimeNs();
    const double wallSeconds = elapsed.nsecsElapsed() / 1e9;
    require(after.has_value() && *after >= *before && wallSeconds > 0, "native process CPU sample is valid");
    auto result = activity(process, requireRenderer);
    result.insert(QStringLiteral("page"), page);
    result.insert(QStringLiteral("wall_seconds"), wallSeconds);
    result.insert(QStringLiteral("cpu_seconds"), static_cast<double>(*after - *before) / 1e9);
    result.insert(QStringLiteral("cpu_one_core_percent"), static_cast<double>(*after - *before) / 1e7 / wallSeconds);
    return result;
}
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    const bool measureOnly = argc == 3 && QByteArrayView(argv[2]) == "--measure-only";
    if (argc != 2 && !measureOnly) return 2;
    try {
        QString error;
        auto process = AutomationProcess::launch(QString::fromLocal8Bit(argv[1]),
            {QStringLiteral("debug"), QStringLiteral("gui-agent"), QStringLiteral("--instance-id"),
                QStringLiteral("performance-visibility")}, error);
        if (!process) throw std::runtime_error(error.toStdString());
        receive(*process, QStringLiteral("hello"));
        // Allow the actual authored background's asynchronous startup/reveal to finish.
        QThread::msleep(8000);
        const auto initial = request(*process, {{"type", "snapshot"}, {"control", "performance.home.track-gain"}}, "snapshot");
        if (initial.value("window").toObject().value("active_modal_title") == "Start Local Engine")
            invoke(*process, QStringLiteral("application.local-engine.cancel"));
        QJsonArray observations;
        auto visible = activity(*process, !measureOnly);
        if (visible.contains(QStringLiteral("background_layers_ready"))) {
            QElapsedTimer startup; startup.start();
            while ((!visible.value("background_layers_ready").toBool() ||
                    visible.value("background_reveal_active").toBool()) && startup.elapsed() < 20000) {
                QThread::msleep(100);
                visible = activity(*process, !measureOnly);
            }
            require(visible.value("background_layers_ready").toBool() &&
                !visible.value("background_reveal_active").toBool(), "real renderer settles before CPU measurements");
        }
        if (!measureOnly)
            require(visible.value(QStringLiteral("visible")).toBool() &&
                visible.value(QStringLiteral("animation_active")).toBool(), "Jam view starts with animation active");
        observations.append(measureCpu(*process, QStringLiteral("jam-idle"), measureOnly ? 6000 : 350, !measureOnly));
        for (const QString& page : {QStringLiteral("looper"), QStringLiteral("chords"),
                 QStringLiteral("beats"), QStringLiteral("metronome")}) {
            if (page == QStringLiteral("looper"))
                invoke(*process, QStringLiteral("performance.home.open.looper"));
            else
                invoke(*process, QStringLiteral("workspace.open.%1").arg(page));
            const auto before = activity(*process, !measureOnly);
            if (!measureOnly)
                require(!before.value(QStringLiteral("visible")).toBool() &&
                    !before.value(QStringLiteral("animation_active")).toBool(), "detail navigation stops the hidden Jam view timer");
            auto after = measureCpu(*process, page, measureOnly ? 3000 : page == QStringLiteral("looper") ? 2000 : 350, !measureOnly);
            if (!measureOnly)
                for (const QString& field : {QStringLiteral("animation_ticks"), QStringLiteral("paint_events"),
                         QStringLiteral("background_refreshes"), QStringLiteral("foreground_rebuilds")})
                    require(before.value(field) == after.value(field), "hidden Jam view performs zero animation, painting or layer rebuilds");
            observations.append(after);
        }
        invoke(*process, QStringLiteral("workspace.close"));
        const auto resumed = activity(*process, !measureOnly);
        const auto running = measureCpu(*process, QStringLiteral("jam-resumed"), measureOnly ? 3000 : 350, !measureOnly);
        if (!measureOnly)
            require(running.value(QStringLiteral("visible")).toBool() &&
                running.value(QStringLiteral("animation_active")).toBool() &&
                running.value(QStringLiteral("animation_ticks")).toInteger() > resumed.value(QStringLiteral("animation_ticks")).toInteger() &&
                running.value(QStringLiteral("paint_events")).toInteger() > resumed.value(QStringLiteral("paint_events")).toInteger(),
                "closing Track/detail views resumes real Jam view painting and animation");
        observations.append(running);
        const auto bytes = QJsonDocument(observations).toJson(QJsonDocument::Compact);
        std::cout << "METRIC performance_visibility " << bytes.constData() << '\n';
        const QString artifacts = qEnvironmentVariable("JAM2_TEST_ARTIFACT_ROOT");
        if (!artifacts.isEmpty()) {
            QFile file(QDir(artifacts).filePath(QStringLiteral("performance-visibility.json")));
            require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "retain actual page visibility observations");
        }
        QString shutdownError;
        require(process->send({{QStringLiteral("type"), QStringLiteral("shutdown")},
            {QStringLiteral("id"), QStringLiteral("shutdown")}}, shutdownError), "send clean shutdown");
        receive(*process, QStringLiteral("command_applied"));
        receive(*process, QStringLiteral("shutdown"));
        int code = -1;
        require(process->waitForExit(10s, code, shutdownError) && code == 0, "public app exits cleanly after page navigation");
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}

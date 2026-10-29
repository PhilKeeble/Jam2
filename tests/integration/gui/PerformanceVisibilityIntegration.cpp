#include "AutomationProcess.hpp"

#include <QCoreApplication>
#include <QDir>
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

QJsonObject activity(AutomationProcess& process)
{
    const auto event = request(process, {{QStringLiteral("type"), QStringLiteral("snapshot")},
        {QStringLiteral("control"), QStringLiteral("performance.home.track-gain")}}, QStringLiteral("snapshot"));
    const auto renderer = event.value(QStringLiteral("performance")).toObject()
        .value(QStringLiteral("home_renderer")).toObject();
    require(renderer.contains(QStringLiteral("animation_ticks")), "real public app exposes renderer activity");
    return renderer;
}

void invoke(AutomationProcess& process, const QString& control)
{
    request(process, {{QStringLiteral("type"), QStringLiteral("invoke")},
        {QStringLiteral("control"), control}, {QStringLiteral("operation"), QStringLiteral("click")}},
        QStringLiteral("command_applied"));
}
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    if (argc != 2) return 2;
    try {
        QString error;
        auto process = AutomationProcess::launch(QString::fromLocal8Bit(argv[1]),
            {QStringLiteral("debug"), QStringLiteral("gui-agent"), QStringLiteral("--instance-id"),
                QStringLiteral("performance-visibility")}, error);
        if (!process) throw std::runtime_error(error.toStdString());
        receive(*process, QStringLiteral("hello"));
        // Allow the actual authored background's asynchronous startup/reveal to finish.
        QThread::msleep(8000);
        QJsonArray observations;
        auto visible = activity(*process);
        require(visible.value(QStringLiteral("visible")).toBool() &&
            visible.value(QStringLiteral("animation_active")).toBool(), "Jam view starts with animation active");
        for (const QString& page : {QStringLiteral("looper"), QStringLiteral("chords"),
                 QStringLiteral("beats"), QStringLiteral("metronome")}) {
            if (page == QStringLiteral("looper"))
                invoke(*process, QStringLiteral("performance.home.open.looper"));
            else
                invoke(*process, QStringLiteral("workspace.open.%1").arg(page));
            const auto before = activity(*process);
            require(!before.value(QStringLiteral("visible")).toBool() &&
                !before.value(QStringLiteral("animation_active")).toBool(), "detail navigation stops the hidden Jam view timer");
            QThread::msleep(page == QStringLiteral("looper") ? 2000 : 350);
            auto after = activity(*process);
            for (const QString& field : {QStringLiteral("animation_ticks"), QStringLiteral("paint_events"),
                     QStringLiteral("background_refreshes"), QStringLiteral("foreground_rebuilds")})
                require(before.value(field) == after.value(field), "hidden Jam view performs zero animation, painting or layer rebuilds");
            after.insert(QStringLiteral("page"), page);
            observations.append(after);
        }
        invoke(*process, QStringLiteral("workspace.close"));
        const auto resumed = activity(*process);
        QThread::msleep(350);
        const auto running = activity(*process);
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

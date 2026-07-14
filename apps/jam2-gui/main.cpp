#include "MainWindow.hpp"
#include "SessionController.hpp"
#include "cli_entry.hpp"

#include <QApplication>
#include <QFile>
#include <QFont>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

#if defined(_WIN32)
void detachConsoleForGui() noexcept
{
    DWORD processIds[2]{};
    const DWORD attachedProcessCount = GetConsoleProcessList(processIds, 2);
    if (attachedProcessCount == 1) {
        // A console created solely for an Explorer launch can be hidden before
        // detaching. Never hide a console shared with PowerShell or cmd.exe.
        if (const HWND consoleWindow = GetConsoleWindow(); consoleWindow != nullptr) {
            ShowWindow(consoleWindow, SW_HIDE);
        }
    }
    (void)FreeConsole();
}

#endif

constexpr std::string_view kDebugUsage = R"(Usage:
  jam2 debug describe --json
  jam2 debug run <scenario.json>

Subcommands:
  describe    Emit the supported automation schema, operations, and bounds
  run         Validate and execute a bounded declarative scenario

Run `jam2 debug <subcommand> -h` for details.
)";

bool isDebugHelpArgument(std::string_view argument) noexcept
{
    return argument == "-h" || argument == "--help" || argument == "help";
}

bool hasDebugHelpArgument(int argc, char* argv[], int start) noexcept
{
    for (int index = start; index < argc; ++index) {
        if (isDebugHelpArgument(argv[index])) {
            return true;
        }
    }
    return false;
}

void printDebugDescribeHelp()
{
    std::cout << R"(Usage:
  jam2 debug describe --json

Writes one JSON object describing the automation schema version, supported
operations, event/artifact protocol versions, size/count limits, and names of
inherited secret environment variables. It never emits secret values.
)";
}

void printDebugRunHelp()
{
    std::cout << R"(Usage:
  jam2 debug run <scenario.json>

The file must use schema `jam2-debug-scenario-v1` and contain only:
  schema       Required schema name
  run_id       Required non-secret run identity (1..128 UTF-8 bytes)
  operation    local, lifecycle.local-network-local, validate.boundaries,
               network.create, or network.join
  arguments    Array of at most 128 non-secret CLI argument strings

Limits:
  Scenario file             2..262144 bytes
  Arguments                 At most 128
  Bytes per argument        At most 4096

Secrets for join operations are inherited through variables reported by
`jam2 debug describe --json`; they are rejected in scenario arguments.
)";
}

void applyDarkTheme(QApplication& app)
{
    app.setStyleSheet(R"(
        QWidget {
            background: #101418;
            color: #d7dde4;
            font-size: 10pt;
        }
        QLabel#AppTitle {
            color: #f1f5f9;
            font-size: 17pt;
            font-weight: 700;
        }
        QLabel#StatusPill {
            color: #cbd5e1;
            background: #1b242d;
            border: 1px solid #2f3b46;
            border-radius: 4px;
            padding: 5px 8px;
        }
        QGroupBox {
            border: 1px solid #2a3540;
            border-radius: 6px;
            margin-top: 14px;
            padding: 8px;
            background: #151b21;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 8px;
            padding: 0 4px;
            color: #9fb1c1;
        }
        QLineEdit, QSpinBox, QComboBox, QTextEdit, QPlainTextEdit, QTableWidget {
            background: #0c1116;
            border: 1px solid #344250;
            border-radius: 4px;
            color: #e5edf5;
            selection-background-color: #2563eb;
            padding: 4px;
        }
        QComboBox::drop-down {
            border: 0;
            width: 22px;
        }
        QPushButton {
            background: #22303c;
            border: 1px solid #3b4b5b;
            border-radius: 5px;
            padding: 6px 10px;
            color: #edf2f7;
        }
        QPushButton:hover {
            background: #2c3c49;
        }
        QPushButton:pressed {
            background: #1d4ed8;
        }
        QPushButton:disabled, QSlider:disabled, QLineEdit:disabled {
            color: #6b7785;
            background: #151a20;
            border-color: #27313a;
        }
        QTabWidget::pane {
            border: 1px solid #2a3540;
            border-radius: 6px;
            background: #111820;
        }
        QTabBar::tab {
            background: #17202a;
            border: 1px solid #2a3540;
            padding: 7px 12px;
            margin-right: 2px;
        }
        QTabBar::tab:selected {
            background: #243241;
            color: #ffffff;
        }
        QHeaderView::section {
            background: #17202a;
            color: #d7dde4;
            border: 1px solid #2a3540;
            padding: 4px;
        }
        QTableWidget::item {
            padding: 8px;
        }
        QSlider::groove:horizontal {
            height: 6px;
            background: #27313a;
            border-radius: 3px;
        }
        QSlider::handle:horizontal {
            background: #60a5fa;
            width: 14px;
            margin: -5px 0;
            border-radius: 7px;
        }
    )");
}

int runDebugCommand(int argc, char* argv[])
{
    if (argc < 3 || isDebugHelpArgument(argv[2])) {
        std::cout << kDebugUsage;
        return 0;
    }
    const std::string_view subcommand{argv[2]};
    if (subcommand == "describe" && hasDebugHelpArgument(argc, argv, 3)) {
        printDebugDescribeHelp();
        return 0;
    }
    if (subcommand == "run" && hasDebugHelpArgument(argc, argv, 3)) {
        printDebugRunHelp();
        return 0;
    }
    if (argc == 4 && std::string_view(argv[2]) == "describe" &&
        std::string_view(argv[3]) == "--json") {
        const QJsonObject description{
            {QStringLiteral("schema"), QStringLiteral("jam2-debug-description-v1")},
            {QStringLiteral("scenario_schema"), QStringLiteral("jam2-debug-scenario-v1")},
            {QStringLiteral("automation_protocol"), 1},
            {QStringLiteral("event_schema"), 1},
            {QStringLiteral("artifact_manifest_schema"), 1},
            {QStringLiteral("max_scenario_bytes"), 256 * 1024},
            {QStringLiteral("max_arguments"), 128},
            {QStringLiteral("max_argument_bytes"), 4096},
            {QStringLiteral("operations"), QJsonArray{
                QStringLiteral("local"),
                QStringLiteral("lifecycle.local-network-local"),
                QStringLiteral("validate.boundaries"),
                QStringLiteral("network.create"),
                QStringLiteral("network.join")}},
            {QStringLiteral("secret_environment"), QJsonArray{
                QStringLiteral("JAM2_DEBUG_JOIN_URL")}},
        };
        std::cout << QJsonDocument(description).toJson(QJsonDocument::Compact).constData() << '\n';
        return 0;
    }
    if (argc != 4 || std::string_view(argv[2]) != "run") {
        std::cerr << kDebugUsage;
        return 2;
    }

    QFile file(QString::fromLocal8Bit(argv[3]));
    if (!file.open(QIODevice::ReadOnly) || file.size() < 2 || file.size() > 256 * 1024) {
        std::cerr << "debug scenario must be a readable file of 2..262144 bytes\n";
        return 2;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!document.isObject() || parseError.error != QJsonParseError::NoError) {
        std::cerr << "debug scenario is not valid JSON object data\n";
        return 2;
    }
    const QJsonObject scenario = document.object();
    const std::set<QString> allowed{
        QStringLiteral("schema"), QStringLiteral("run_id"),
        QStringLiteral("operation"), QStringLiteral("arguments")};
    for (auto it = scenario.begin(); it != scenario.end(); ++it) {
        if (!allowed.contains(it.key())) {
            std::cerr << "debug scenario contains unknown field\n";
            return 2;
        }
    }
    if (scenario.value(QStringLiteral("schema")).toString() != QStringLiteral("jam2-debug-scenario-v1")) {
        std::cerr << "unsupported debug scenario schema\n";
        return 2;
    }
    const QString runId = scenario.value(QStringLiteral("run_id")).toString();
    if (runId.isEmpty() || runId.toUtf8().size() > 128) {
        std::cerr << "debug scenario run_id must be 1..128 UTF-8 bytes\n";
        return 2;
    }
    const QString operation = scenario.value(QStringLiteral("operation")).toString();
    const QJsonValue argumentsValue = scenario.value(QStringLiteral("arguments"));
    if (!argumentsValue.isArray() || argumentsValue.toArray().size() > 128) {
        std::cerr << "debug scenario arguments must be an array of at most 128 strings\n";
        return 2;
    }

    std::vector<std::string> storage;
    storage.push_back(argv[0]);
    const bool lifecycleSmoke = operation == QStringLiteral("lifecycle.local-network-local");
    const bool boundaryValidation = operation == QStringLiteral("validate.boundaries");
    if (operation == QStringLiteral("local") || lifecycleSmoke) {
        storage.emplace_back("local");
    } else if (boundaryValidation) {
    } else if (operation == QStringLiteral("network.create")) {
        storage.insert(storage.end(), {"network", "create"});
    } else if (operation == QStringLiteral("network.join")) {
        const QByteArray url = qgetenv("JAM2_DEBUG_JOIN_URL");
        if (url.isEmpty() || url.size() > 4096 || !url.startsWith("jam2://")) {
            std::cerr << "network.join requires JAM2_DEBUG_JOIN_URL through the inherited environment\n";
            return 2;
        }
        storage.insert(storage.end(), {"network", "join", url.toStdString()});
    } else {
        std::cerr << "unsupported debug scenario operation\n";
        return 2;
    }

    std::vector<std::string> scenarioArguments;
    scenarioArguments.reserve(static_cast<std::size_t>(argumentsValue.toArray().size()));
    for (const QJsonValue value : argumentsValue.toArray()) {
        if (!value.isString()) {
            std::cerr << "debug scenario arguments must contain only strings\n";
            return 2;
        }
        const QByteArray argument = value.toString().toUtf8();
        if (argument.isEmpty() || argument.size() > 4096 ||
            argument == "--session-key" || argument.startsWith("--session-key=") ||
            argument.contains("jam2://")) {
            std::cerr << "debug scenario argument is empty, too large, or contains secret material\n";
            return 2;
        }
        scenarioArguments.push_back(argument.toStdString());
        storage.push_back(scenarioArguments.back());
    }

    if (boundaryValidation) {
        QStringList fixtures;
        fixtures.reserve(static_cast<qsizetype>(scenarioArguments.size()));
        for (const std::string& argument : scenarioArguments) {
            fixtures.push_back(QString::fromUtf8(argument));
        }
        const QJsonObject result = jam2RunBoundaryValidation(fixtures);
        std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << '\n';
        return result.value(QStringLiteral("ok")).toBool(false) ? 0 : 3;
    }

    if (lifecycleSmoke) {
        Jam2CliHost host;
        jam2_cli_set_host(&host);
        const auto invoke = [&](std::vector<std::string> phase) {
            phase.insert(phase.end(), scenarioArguments.begin(), scenarioArguments.end());
            phase.insert(phase.end(), {"--stream-ms", "750"});
            std::vector<char*> phaseArgv;
            phaseArgv.reserve(phase.size());
            for (std::string& value : phase) phaseArgv.push_back(value.data());
            return jam2_cli_main(static_cast<int>(phaseArgv.size()), phaseArgv.data());
        };
        const std::string executable = argv[0];
        const int localBefore = invoke({executable, "local"});
        const std::uint64_t frameBefore = jam2_cli_embedded_engine_stats().engine_frame;
        const int network = invoke({
            executable, "network", "_run",
            "--bind", "127.0.0.1:0", "--session-id", "1",
            "--session-key", "00000000000000000000000000000001",
            "--no-stun",
            "--bootstrap-role", "creator",
            "--peers", ""});
        const std::uint64_t frameNetwork = jam2_cli_embedded_engine_stats().engine_frame;
        const int localAfter = invoke({executable, "local"});
        const Jam2EmbeddedEngineStats stats = jam2_cli_embedded_engine_stats();
        jam2_cli_set_host(nullptr);
        jam2_cli_shutdown_embedded_engine();
        const bool ok = localBefore == 0 && network == 0 && localAfter == 0 &&
            stats.starts == 1 && stats.restarts == 0 && stats.reuses == 2 &&
            frameBefore > 0 && frameNetwork > frameBefore && stats.engine_frame > frameNetwork;
        const QJsonObject result{
            {QStringLiteral("event"), QStringLiteral("debug_lifecycle_result")},
            {QStringLiteral("schema"), 1},
            {QStringLiteral("ok"), ok},
            {QStringLiteral("engine_starts"), static_cast<qint64>(stats.starts)},
            {QStringLiteral("engine_restarts"), static_cast<qint64>(stats.restarts)},
            {QStringLiteral("engine_reuses"), static_cast<qint64>(stats.reuses)},
            {QStringLiteral("frame_before_network"), static_cast<qint64>(frameBefore)},
            {QStringLiteral("frame_after_network"), static_cast<qint64>(frameNetwork)},
            {QStringLiteral("frame_after_return_local"), static_cast<qint64>(stats.engine_frame)},
        };
        std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << '\n';
        return ok ? 0 : 3;
    }

    std::vector<char*> forwarded;
    forwarded.reserve(storage.size());
    for (std::string& value : storage) forwarded.push_back(value.data());
    const QJsonObject startEvent{
        {QStringLiteral("event"), QStringLiteral("debug_start")},
        {QStringLiteral("schema"), 1},
        {QStringLiteral("run_id"), runId},
        {QStringLiteral("operation"), operation},
    };
    std::cout << QJsonDocument(startEvent).toJson(QJsonDocument::Compact).constData() << '\n';
    if (operation == QStringLiteral("network.create") || operation == QStringLiteral("network.join")) {
        int forwardedArgc = static_cast<int>(forwarded.size());
        QCoreApplication app(forwardedArgc, forwarded.data());
        QCoreApplication::setApplicationName("Jam2");
        QCoreApplication::setOrganizationName("Jam2");
        return SessionController::runNetworkCommand(forwardedArgc, forwarded.data());
    }
    return jam2_cli_main(static_cast<int>(forwarded.size()), forwarded.data());
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc > 1) {
        if (std::string_view(argv[1]) == "debug") {
            return runDebugCommand(argc, argv);
        }
        if (SessionController::handlesNetworkCommand(argc, argv)) {
            QCoreApplication app(argc, argv);
            QCoreApplication::setApplicationName("Jam2");
            QCoreApplication::setOrganizationName("Jam2");
            return SessionController::runNetworkCommand(argc, argv);
        }
        return jam2_cli_main(argc, argv);
    }

#if defined(_WIN32)
    detachConsoleForGui();
    // GUI/UI/file workers inherit a high-priority process. The UDP/audio packet
    // worker independently requests time-critical/MMCSS scheduling.
    (void)SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#endif
    QApplication app(argc, argv);
    QApplication::setApplicationName("Jam2");
    QApplication::setOrganizationName("Jam2");
    applyDarkTheme(app);

    MainWindow window;
    window.resize(1920, 1080);
    window.show();

    return app.exec();
}

#include "DebugAutomation.hpp"
#include "MainWindow.hpp"
#include "SessionController.hpp"
#include "CliEntrypoint.hpp"

#include <QApplication>
#include <QCoreApplication>

#include <iostream>
#include <string_view>

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
        if (const HWND consoleWindow = GetConsoleWindow(); consoleWindow != nullptr) {
            ShowWindow(consoleWindow, SW_HIDE);
        }
    }
    (void)FreeConsole();
}
#endif

void applyDarkTheme(QApplication& app)
{
    app.setStyleSheet(R"(
        QWidget { background: #101418; color: #d7dde4; font-size: 10pt; }
        QLabel#AppTitle { color: #f1f5f9; font-size: 17pt; font-weight: 700; }
        QLabel#StatusPill {
            color: #cbd5e1; background: #1b242d; border: 1px solid #2f3b46;
            border-radius: 4px; padding: 5px 8px;
        }
        QGroupBox {
            border: 1px solid #2a3540; border-radius: 6px; margin-top: 14px;
            padding: 8px; background: #151b21;
        }
        QGroupBox::title {
            subcontrol-origin: margin; left: 8px; padding: 0 4px; color: #9fb1c1;
        }
        QLineEdit, QSpinBox, QComboBox, QTextEdit, QPlainTextEdit, QTableWidget {
            background: #0c1116; border: 1px solid #344250; border-radius: 4px;
            color: #e5edf5; selection-background-color: #2563eb; padding: 4px;
        }
        QComboBox::drop-down { border: 0; width: 22px; }
        QPushButton {
            background: #22303c; border: 1px solid #3b4b5b; border-radius: 5px;
            padding: 6px 10px; color: #edf2f7;
        }
        QPushButton:hover { background: #2c3c49; }
        QPushButton:pressed { background: #1d4ed8; }
        QPushButton:disabled, QSlider:disabled, QLineEdit:disabled {
            color: #6b7785; background: #151a20; border-color: #27313a;
        }
        QTabWidget::pane {
            border: 1px solid #2a3540; border-radius: 6px; background: #111820;
        }
        QTabBar::tab {
            background: #17202a; border: 1px solid #2a3540; padding: 7px 12px;
            margin-right: 2px;
        }
        QTabBar::tab:selected { background: #243241; color: #ffffff; }
        QHeaderView::section {
            background: #17202a; color: #d7dde4; border: 1px solid #2a3540;
            padding: 4px;
        }
        QTableWidget::item { padding: 8px; }
        QSlider::groove:horizontal {
            height: 6px; background: #27313a; border-radius: 3px;
        }
        QSlider::handle:horizontal {
            background: #60a5fa; width: 14px; margin: -5px 0; border-radius: 7px;
        }
    )");
}

void setApplicationIdentity()
{
    QCoreApplication::setApplicationName(QStringLiteral("Jam2"));
    QCoreApplication::setOrganizationName(QStringLiteral("Jam2"));
}

} // namespace

int jam2ApplicationMain(int argc, char* argv[])
{
    const bool hasAutomationHandle =
        qEnvironmentVariableIsSet("JAM2_AUTOMATION_COMMAND_HANDLE") ||
        qEnvironmentVariableIsSet("JAM2_AUTOMATION_EVENT_HANDLE");
    const bool debugRun = argc > 2 && std::string_view(argv[1]) == "debug" &&
        std::string_view(argv[2]) == "run";
    if (hasAutomationHandle && !debugRun) {
        std::cerr << "automation handles are accepted only by an explicitly reactive debug run\n";
        return 2;
    }
    if (argc > 1) {
        if (std::string_view(argv[1]) == "debug") {
            QCoreApplication app(argc, argv);
            setApplicationIdentity();
            return jam2RunDebugCommand(argc, argv);
        }
        if (SessionController::handlesNetworkCommand(argc, argv)) {
            QCoreApplication app(argc, argv);
            setApplicationIdentity();
            return SessionController::runNetworkCommand(argc, argv);
        }
        return jam2::cli::runFrontend(argc, argv);
    }

#if defined(_WIN32)
    detachConsoleForGui();
    (void)SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#endif
    QApplication app(argc, argv);
    setApplicationIdentity();
    applyDarkTheme(app);

    MainWindow window;
    window.resize(1920, 1080);
    window.show();
    return app.exec();
}

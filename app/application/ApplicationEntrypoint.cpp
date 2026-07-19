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

void applyCustomTheme(QApplication& app)
{
    app.setStyleSheet(R"(
        QWidget { background: #050607; color: #e4e9ec; font-size: 10pt; }
        QLabel#AppTitle { color: #ffffff; font-size: 17pt; font-weight: 700; }
        QLabel#StatusPill {
            color: #e4e9ec; background: #171a1c; border: 1px solid #596269;
            border-radius: 4px; padding: 5px 8px;
        }
        QGroupBox {
            border: 1px solid #596269; border-radius: 6px; margin-top: 14px;
            padding: 8px; background: #0d0f10;
        }
        QGroupBox::title {
            subcontrol-origin: margin; left: 8px; padding: 0 4px; color: #a4afb5;
        }
        QLineEdit, QAbstractSpinBox, QComboBox, QTextEdit, QPlainTextEdit, QTableWidget {
            background: #000000; border: 1px solid #89959c; border-radius: 4px;
            color: #e4e9ec; selection-background-color: #255f72; padding: 4px;
        }
        QComboBox::drop-down { border: 0; width: 22px; }
        QPushButton, QToolButton {
            background: #22282c; border: 1px solid #89959c; border-radius: 5px;
            padding: 6px 10px; color: #ffffff;
        }
        QPushButton:hover, QToolButton:hover { background: #30383d; }
        QPushButton:pressed, QToolButton:pressed {
            background: #123e32; border-color: #419f81;
        }
        QToolButton#SettingsButton { padding: 0; }
        QPushButton:disabled, QToolButton:disabled, QSlider:disabled,
        QLineEdit:disabled, QAbstractSpinBox:disabled, QComboBox:disabled {
            color: #a4afb5; background: #171a1c; border-color: #596269;
        }
        QTabWidget::pane {
            border: 1px solid #596269; border-radius: 6px; background: #0d0f10;
        }
        QTabBar::tab {
            background: #171a1c; border: 1px solid #596269; padding: 7px 12px;
            margin-right: 2px;
        }
        QTabBar::tab:selected {
            background: #255f72; color: #ffffff; border-color: #419f81;
        }
        QHeaderView::section {
            background: #171a1c; color: #e4e9ec; border: 1px solid #596269;
            padding: 4px;
        }
        QTableWidget::item { padding: 8px; }
        QSlider::groove:horizontal {
            height: 6px; background: #121516; border-radius: 3px;
        }
        QSlider::handle:horizontal {
            background: #419f81; width: 14px; margin: -5px 0; border-radius: 7px;
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
    applyCustomTheme(app);

    MainWindow window;
    window.resize(1920, 1080);
    window.show();
    return app.exec();
}

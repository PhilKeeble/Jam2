#include "DebugAutomation.hpp"
#include "GuiPresentation.hpp"
#include "MainWindow.hpp"
#include "SessionController.hpp"
#include "CliEntrypoint.hpp"

#include <QApplication>
#include <QCoreApplication>
#include <QIcon>

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
        QWidget {
            background: #050809; color: #e9e6dc;
            font-family: "Segoe UI Variable", "Segoe UI"; font-size: 10.5pt;
        }
        QLineEdit#SongTitle {
            color: #e9e6dc; background: transparent; border: 0;
            font-family: Georgia; font-size: 16pt; padding: 3px 8px;
        }
        QLabel#StatusPill {
            color: #c6d0d0; background: #101719; border: 1px solid #354247;
            border-radius: 3px; padding: 5px 8px;
            font-family: Bahnschrift; font-size: 8.5pt;
        }
        QLabel#StatusPill[issue="true"] {
            color: #ffd68e; background: #211714; border-color: #e8a44a;
        }
        QGroupBox {
            border: 1px solid #354247; border-radius: 4px; margin-top: 16px;
            padding: 10px; background: #0b1011;
        }
        QGroupBox::title {
            subcontrol-origin: margin; left: 8px; padding: 0 5px;
            color: #ccd4d2; font-family: Bahnschrift; font-size: 9pt;
        }
        QLineEdit, QAbstractSpinBox, QComboBox, QTextEdit, QPlainTextEdit, QTableWidget {
            background: #070b0c; border: 1px solid #526368; border-radius: 3px;
            color: #f1eee5; selection-background-color: #74435e; padding: 6px;
        }
        QComboBox::drop-down { border: 0; width: 22px; }
        QPushButton, QToolButton {
            background: #121b1d; border: 1px solid #526368; border-radius: 3px;
            padding: 7px 11px; color: #f5f2e9;
        }
        QPushButton:hover, QToolButton:hover {
            background: #1c282b; border-color: #66d4cf;
        }
        QPushButton:pressed, QToolButton:pressed {
            background: #241c20; border-color: #e8a44a;
        }
        QPushButton[active="true"] {
            color: #160e07; background: #e8a44a; border-color: #ffd68e;
        }
        QPushButton#SessionAction, QPushButton#DataButton, QPushButton#DetailTool {
            font-family: Bahnschrift; font-size: 8.5pt; padding: 6px 9px;
        }
        QPushButton#DataButton { color: #ffd68e; border-color: #986d36; }
        QFrame#DetailPanel {
            background: #0b1011; border: 1px solid #354247; border-radius: 4px;
        }
        QLabel#MicroHeading, QLabel#DrawerSection {
            color: #9ca9ab; font-family: Bahnschrift; font-size: 8.5pt;
            letter-spacing: 1px;
        }
        QLabel#DetailPosition {
            color: #e9e6dc; font-family: Georgia; font-size: 16pt;
        }
        QPushButton#DetailTab {
            background: transparent; color: #9ca9ab; border: 0;
            border-bottom: 2px solid #354247; border-radius: 0;
            font-family: Bahnschrift; font-size: 9pt; padding: 8px 13px;
        }
        QPushButton#DetailTab:hover { color: #e9e6dc; border-bottom-color: #66d4cf; }
        QPushButton#DetailTab[active="true"] {
            color: #ffd68e; background: transparent; border-bottom-color: #e8a44a;
        }
        QPushButton#CloseDetailButton {
            color: #c8d1d0; background: #0b1011; padding: 6px 10px;
        }
        QPushButton#MainTransportButton {
            color: #160e07; background: #e8a44a; border: 1px solid #ffd68e;
            border-radius: 32px; font-size: 19pt; font-weight: 700; padding: 0;
        }
        QPushButton#MainTransportButton[active="true"] {
            color: #fff8f2; background: #c92f58; border-color: #ff7d86;
        }
        QFrame#TempoCard {
            background: #050809; border: 1px solid #526368; border-radius: 4px;
        }
        QPushButton#MetronomeToggle, QPushButton#TempoButton {
            background: transparent; border: 0; border-radius: 0;
            font-family: Bahnschrift;
        }
        QPushButton#MetronomeToggle {
            color: #d7dedb; font-size: 8.5pt; padding: 5px 8px;
            border: 1px solid #526368; border-radius: 3px;
        }
        QPushButton#MetronomeToggle[active="true"] {
            color: #160e07; background: #e8a44a; border-color: #ffd68e;
        }
        QPushButton#TempoButton {
            color: #f0ede4; font-family: Georgia; font-size: 12pt; padding: 2px 8px;
        }
        QLabel#PerformancePosition {
            color: #e8a44a; font-family: Bahnschrift; font-size: 12pt;
            font-weight: 600; padding: 0 8px;
        }
        QFrame#PerformanceTransport {
            background: #050809; border: 1px solid #354247; border-radius: 4px;
        }
        QLabel#StripTitle {
            color: #9ca9ab; font-family: Bahnschrift; font-size: 8.5pt;
            letter-spacing: 1px;
        }
        QWidget#DataOverlay { background: rgba(3, 5, 6, 150); }
        QFrame#DataDrawer {
            background: #0b1011; border-left: 1px solid #708287; border-radius: 0;
        }
        QFrame#DataDrawer QLabel { color: #dfe4df; font-size: 11pt; }
        QLabel#DrawerTitle {
            color: #ffd68e; font-family: Georgia; font-size: 20pt;
            font-weight: 500; padding-top: 2px;
        }
        QFrame#MetricCard {
            background: #101719; border: 1px solid #354247; border-radius: 3px;
        }
        QFrame#MetricCard QLabel#MetricValue {
            color: #f3efe4; font-family: Bahnschrift; font-size: 11.5pt;
        }
        QFrame#MetricCard QLabel#MetricCaption {
            color: #9ca9ab; font-family: Bahnschrift; font-size: 7.5pt;
        }
        QLabel#DiagnosisDetail, QLabel#ArtifactGuide {
            color: #d8dfdc; font-size: 11.5pt; line-height: 1.35;
        }
        QFrame#GuideSection { background: #0b1011; border: 1px solid #354247; }
        QToolButton#GuideToggle {
            color: #e9e6dc; background: #101719; border: 0; border-radius: 0;
            font-family: Bahnschrift; font-size: 10pt; text-align: left;
            padding: 9px 11px;
        }
        QToolButton#SettingsButton { padding: 0; }
        QPushButton:disabled, QToolButton:disabled, QSlider:disabled,
        QLineEdit:disabled, QAbstractSpinBox:disabled, QComboBox:disabled {
            color: #758184; background: #101719; border-color: #354247;
        }
        QTabWidget::pane {
            border: 1px solid #354247; border-radius: 3px; background: #0b1011;
        }
        QTabBar::tab {
            background: #101719; border: 1px solid #354247; padding: 8px 13px;
            margin-right: 2px;
        }
        QTabBar::tab:selected {
            background: #30202a; color: #ffffff; border-color: #e8a44a;
        }
        QHeaderView::section {
            background: #101719; color: #dce3df; border: 1px solid #354247;
            padding: 7px; font-family: Bahnschrift; font-size: 9pt;
        }
        QTableWidget::item { padding: 8px; }
        QSlider::groove:horizontal {
            height: 6px; background: #172023; border: 1px solid #354247;
            border-radius: 3px;
        }
        QSlider::sub-page:horizontal {
            background: #e8a44a; border: 1px solid #e8a44a; border-radius: 3px;
        }
        QSlider::handle:horizontal {
            background: #e8a44a; border: 1px solid #ffd68e;
            width: 16px; margin: -5px 0; border-radius: 8px;
        }
        QScrollBar:vertical { background: #0b1011; width: 10px; margin: 0; }
        QScrollBar::handle:vertical {
            background: #526368; min-height: 28px; border-radius: 5px;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
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
    app.setWindowIcon(QIcon(QStringLiteral(":/jam2/assets/logo-nebula.png")));
    applyCustomTheme(app);
    installCompactDialogPolicy(app);

    MainWindow window;
    window.resize(1920, 1080);
    window.show();
    return app.exec();
}

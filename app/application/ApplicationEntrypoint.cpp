#include "DebugAutomation.hpp"
#include "GuiPresentation.hpp"
#include "GuiTestAgent.hpp"
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
    app.setStyleSheet(QString::fromLatin1(R"(
        QWidget {
            background: #050809; color: #e9e6dc;
            font-family: "Segoe UI Variable", "Segoe UI"; font-size: 10.5pt;
        }
        QLabel {
            background: transparent;
        }
        QLineEdit#SongTitle, QSpinBox#MetronomeBpm {
            font-family: Georgia;
        }
        QLineEdit#SongTitle {
            color: #e9e6dc; background: transparent; border: 1px solid transparent;
            border-radius: 3px;
            font-size: 16pt; padding: 3px 8px;
        }
        QLineEdit#SongTitle[editing="true"] {
            background: #11191a; border-color: #66d4cf;
            selection-background-color: #315f60;
        }
        QLabel#StatusPill {
            color: #c6d0d0; background: #101719; border: 1px solid #354247;
            border-radius: 3px; padding: 5px 8px;
            font-family: Bahnschrift; font-size: 8.5pt;
        }
        QLabel#StatusPill[issue="true"] {
            color: #ffd68e; background: #211714; border-color: #e8a44a;
        }
        QLabel#StatusPill[jamtaster="running"] {
            color: #ffe3aa; background: #2a2112; border-color: #e8a44a;
        }
        QLabel#StatusPill[jamtaster="complete"] {
            color: #b9f2df; background: #10241f; border-color: #58b99b;
        }
        QLabel#StatusPill[jamtaster="cancelled"] {
            color: #c6d0d0; background: #171d1e; border-color: #526368;
        }
        QLabel#StatusPill[jamtaster="error"] {
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
        QAbstractSpinBox[jam2MutedEditor="true"] {
            border: 1px solid #89959c; background: #000000; color: #ffffff;
            padding: 2px 28px 2px 6px;
        }
        QComboBox[jam2MutedEditor="true"], QLineEdit[jam2MutedEditor="true"] {
            border: 1px solid #89959c; background: #000000; color: #ffffff;
            padding: 2px 6px;
        }
        QAbstractSpinBox[jam2MutedEditor="true"]:focus,
        QComboBox[jam2MutedEditor="true"]:focus,
        QLineEdit[jam2MutedEditor="true"]:focus {
            border-color: #e8a44a;
        }
        QAbstractSpinBox[jam2MutedEditor="true"]:disabled,
        QComboBox[jam2MutedEditor="true"]:disabled,
        QLineEdit[jam2MutedEditor="true"]:disabled {
            border-color: #535270; background: #161727; color: #aaa5ba;
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
        QWidget#DetailIdentityPanel,
        QWidget#ChordReferenceSection,
        QWidget#ChordReferenceControls {
            background: transparent;
        }
        QLabel#MicroHeading, QLabel#DrawerSection {
            color: #9ca9ab; font-family: Bahnschrift; font-size: 8.5pt;
            letter-spacing: 1px;
        }
        QLineEdit#DetailPosition {
            color: #e9e6dc; background: transparent; border: 1px solid transparent;
            border-radius: 3px; font-family: Georgia; font-size: 16pt; padding: 2px 6px;
        }
        QLineEdit#DetailPosition[editing="true"] {
            background: #11191a; border-color: #66d4cf; selection-background-color: #315f60;
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
        QGroupBox#MetronomeTempoCard {
            background: #04060c; border-color: #526368;
        }
        QWidget#MetronomeTempoControls, QWidget#MetronomeTempoPanel {
            background: transparent;
        }
        QWidget#MetronomeTempoControls QLabel {
            background: transparent;
        }
        QFrame#MetronomeCard {
            background: #0b1011; border: 1px solid #354247; border-radius: 4px;
        }
        QWidget#MetronomeCardHeader {
            background: #101719; border-bottom: 1px solid #354247;
        }
        QSpinBox#MetronomeBpm {
            color: #ffd68e; background: transparent; border: 0;
            font-size: 30pt; padding: 0;
        }
        QPushButton#MetronomeTap {
            color: #f5f2e9; font-family: Bahnschrift; font-size: 11pt;
            padding: 9px 12px; text-align: center;
        }
        QPushButton#MetronomeBpmAdjust {
            color: #d7dedb; background: #070b0c; border: 1px solid #526368;
            font-family: Bahnschrift; font-size: 17pt; padding: 0;
        }
        QPushButton#MetronomeBpmAdjust:hover {
            color: #ffd68e; border-color: #e8a44a;
        }
        QLabel#MetronomeFact {
            color: #f0ede4; font-family: Bahnschrift; font-size: 11pt;
            font-weight: 600;
        }
        QLabel#MetronomeSecondary {
            color: #9ca9ab; font-family: Bahnschrift; font-size: 9pt;
        }
        QLabel#MetronomeLegend {
            color: #f5f2e9; font-family: Bahnschrift; font-size: 10pt;
        }
        QLabel#MetronomeModeDescription {
            color: #f0ede4; font-size: 10pt; padding-top: 5px;
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
        QCheckBox#PlaybackCountIn {
            color: #c7cfcd; background: transparent; border: 0;
            font-family: Bahnschrift; font-size: 7.5pt; padding: 0; spacing: 5px;
            min-height: 13px; max-height: 15px;
        }
        QCheckBox#PlaybackCountIn::indicator {
            width: 15px; height: 15px;
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
        QPushButton#LocalAudioTag, QPushButton#LocalMidiTag,
        QPushButton#LocalPluginsTag, QPushButton#LocalBypassTag {
            background: transparent; border-radius: 3px; font-family: Bahnschrift;
            font-size: 7.5pt; font-weight: 600; padding: 2px 7px;
            min-height: 16px; max-height: 20px;
        }
        QPushButton#LocalAudioTag {
            color: #ffd68e; border: 1px solid #b98036;
        }
        QPushButton#LocalAudioTag:hover {
            color: #fff4df; background: #302317; border-color: #e8a44a;
        }
        QPushButton#LocalMidiTag {
            color: #8ce7e1; border: 1px solid #438c89;
        }
        QPushButton#LocalMidiTag:hover {
            color: #e9fffc; background: #193031; border-color: #66d4cf;
        }
        QPushButton#LocalPluginsTag {
            color: #d5b4f1; border: 1px solid #76539a;
        }
        QPushButton#LocalPluginsTag:hover {
            color: #f5eaff; background: #2c2137; border-color: #a46fda;
        }
        QPushButton#LocalBypassTag {
            color: #c8d1cf; border: 1px solid #526368;
        }
        QPushButton#LocalBypassTag:hover {
            color: #ffffff; background: #252e30; border-color: #86999d;
        }
        QPushButton#LocalBypassTag:checked {
            color: #ffd8df; background: #441827; border-color: #e14975;
        }
        QPushButton#LocalBypassTag:disabled {
            color: #596568; background: transparent; border-color: #2f3a3c;
        }
        QLabel#BankStripLabel {
            color: #9ca9ab; background: transparent; font-family: Bahnschrift;
            font-size: 8.5pt; letter-spacing: 1px;
        }
        QGroupBox#TrackSharingCard QLabel#TrackSharingStatus {
            background: transparent;
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

    )") + QString::fromLatin1(R"(
        /* One popup language throughout Jam2. These rules intentionally sit
           after the workspace theme so every QDialog, including message and
           file dialogs, inherits the Groove Library's warmer, softer visual
           hierarchy without changing the controls each workflow uses. */
        QDialog {
            background: #101516; color: #eee4d1;
        }
        /* Layout-only QWidget containers otherwise inherit the application's
           near-black workspace background and appear as bars behind dialog
           checkboxes and labels. Concrete dialog controls below restore their
           own surfaces where they need one. */
        QDialog QWidget {
            background: transparent;
        }
        QDialog QGroupBox {
            background: #0b1011;
        }
        QDialog QLabel, QDialog QCheckBox, QDialog QRadioButton {
            color: #eee4d1;
        }
        QDialog QLineEdit, QDialog QAbstractSpinBox, QDialog QComboBox,
        QDialog QTextEdit, QDialog QPlainTextEdit, QDialog QTableWidget {
            background: #182022; color: #f4ead8;
            border: 1px solid #3b4a4c; border-radius: 3px;
            selection-background-color: #315153; selection-color: #ffffff;
        }
        QDialog QLineEdit, QDialog QAbstractSpinBox, QDialog QComboBox {
            padding: 6px 8px;
        }
        QDialog QLineEdit:focus, QDialog QAbstractSpinBox:focus,
        QDialog QComboBox:focus, QDialog QTextEdit:focus,
        QDialog QPlainTextEdit:focus, QDialog QTableWidget:focus,
        QDialog QListWidget:focus, QDialog QTreeWidget:focus,
        QDialog QTableView:focus {
            background: #202a2c; color: #fff4df;
            border: 2px solid #d8bf91;
        }
        QDialog QPushButton:focus, QDialog QToolButton:focus {
            background: #383426; color: #fff4df;
            border: 2px solid #ffd68e;
        }
        QDialog QCheckBox:focus {
            background: transparent; color: #fff4df;
            border: 1px dotted #d8bf91;
        }
        QDialog QRadioButton:focus {
            background: transparent; color: #fff4df; border: 0;
        }
        QDialog QCheckBox {
            background: transparent; border: 1px dotted transparent;
            padding: 1px 3px; spacing: 7px; min-height: 24px;
        }
        QDialog QRadioButton {
            background: transparent; border: 0; padding: 2px 0; spacing: 7px;
            min-height: 24px;
        }
        QDialog QRadioButton::indicator {
            width: 16px; height: 16px; border-radius: 8px;
            background: #182022; border: 2px solid #71878a;
        }
        QDialog QRadioButton::indicator:hover {
            background: #202a2c; border-color: #d8bf91;
        }
        QDialog QRadioButton::indicator:checked {
            width: 6px; height: 6px; border-radius: 8px;
            background: #e8a44a; border: 5px solid #182022;
        }
        QDialog QRadioButton::indicator:checked:hover {
            background: #ffd68e; border-color: #202a2c;
        }
        QDialog QRadioButton::indicator:disabled {
            background: #171e1f; border-color: #3b4749;
        }
        QProgressBar#JamTasterProgress {
            background: #182022; color: #f4ead8;
            border: 1px solid #465658; border-radius: 4px;
            text-align: center; min-height: 18px;
        }
        QProgressBar#JamTasterProgress::chunk {
            background: #e8a44a; border-radius: 3px;
        }
        QDialog QSlider:focus {
            border: 1px solid #d8bf91; border-radius: 3px;
        }
        QDialog QAbstractSpinBox[jam2MutedEditor="true"],
        QDialog QComboBox[jam2MutedEditor="true"],
        QDialog QLineEdit[jam2MutedEditor="true"] {
            background: #182022; color: #f4ead8;
            border: 1px solid #3b4a4c; padding: 6px 8px;
        }
        QDialog QAbstractSpinBox[jam2MutedEditor="true"]:focus,
        QDialog QComboBox[jam2MutedEditor="true"]:focus,
        QDialog QLineEdit[jam2MutedEditor="true"]:focus {
            border-color: #b99b67;
        }
        QDialog QComboBox QAbstractItemView {
            background: #182022; color: #f4ead8;
            border: 1px solid #465658;
            selection-background-color: #315153; selection-color: #ffffff;
            outline: 0;
        }
        QDialog QListWidget, QDialog QTreeWidget, QDialog QTableView {
            background: #12191a; color: #e7ddca;
            border: 1px solid #344244; border-radius: 4px; outline: 0;
            selection-background-color: #29484b; selection-color: #ffffff;
        }
        QDialog QListWidget::item, QDialog QTreeWidget::item {
            border-bottom: 1px solid #253133; padding: 7px 9px;
        }
        QDialog QListWidget::item:hover, QDialog QTreeWidget::item:hover {
            background: #1b292b;
        }
        QDialog QListWidget::item:selected, QDialog QTreeWidget::item:selected {
            background: #29484b; color: #ffffff;
            border-left: 3px solid #b99b67;
        }
        QDialog QGroupBox {
            background: #0b1011; border: 1px solid #344244;
            border-radius: 4px; margin-top: 16px; padding: 10px;
        }
        QDialog QGroupBox::title {
            subcontrol-origin: margin; left: 10px; padding: 0 5px;
            color: #d8bf91; font-family: Bahnschrift; font-size: 9pt;
            font-weight: 600;
        }
        QDialog QPushButton, QDialog QToolButton {
            background: #202b2d; color: #eee4d1;
            border: 1px solid #465658; border-radius: 3px;
            padding: 7px 12px;
        }
        QDialog QPushButton:hover, QDialog QToolButton:hover {
            background: #2a3a3c; border-color: #71878a;
        }
        QDialog QPushButton:pressed, QDialog QToolButton:pressed {
            background: #30251c; border-color: #b99b67;
        }
        QDialog QPushButton:default {
            color: #ffe3ad; background: #29281f; border-color: #b99b67;
        }
        QDialog QPushButton:default:hover {
            background: #383426; border-color: #d8bf91;
        }
        QDialog QPushButton:disabled, QDialog QToolButton:disabled,
        QDialog QLineEdit:disabled, QDialog QAbstractSpinBox:disabled,
        QDialog QComboBox:disabled {
            color: #718082; border-color: #303b3d; background: #171e1f;
        }
        QDialog QPushButton#PluginBypassAction:checked {
            color: #fff4df; background: #7a4d11;
            border: 2px solid #e8a44a;
        }
        QDialog QPushButton#PluginBypassAction:focus:!checked {
            color: #eee4d1; background: #202b2d;
            border: 2px solid #ffd68e;
        }
        QDialog QPushButton#PluginRemoveAction {
            color: #ffd8df; background: #35141f; border-color: #9f294b;
        }
        QDialog QPushButton#PluginRemoveAction:hover {
            color: #ffffff; background: #571b30; border-color: #e14975;
        }
        QDialog QPushButton#PluginRemoveAction:disabled {
            color: #775d65; background: #24171b; border-color: #483039;
        }
        QDialog QDialogButtonBox {
            border-top: 1px solid #253133; padding-top: 10px;
        }
        QDialog QTabWidget::pane {
            background: #0b1011; border: 1px solid #344244;
            border-radius: 4px;
        }
        QDialog QTabBar::tab {
            background: #182022; color: #c8d1cf;
            border: 1px solid #344244; border-bottom: 0;
            border-top-left-radius: 3px; border-top-right-radius: 3px;
            padding: 8px 13px; margin-right: 2px;
        }
        QDialog QTabBar::tab:selected {
            background: #29484b; color: #ffffff; border-color: #b99b67;
        }
        QDialog QHeaderView::section {
            background: #182022; color: #d8bf91;
            border: 0; border-right: 1px solid #344244;
            border-bottom: 1px solid #344244; padding: 7px;
        }
        QDialog QTableWidget::item:selected, QDialog QTableView::item:selected {
            background: #29484b; color: #ffffff;
        }
        QDialog QScrollBar:vertical { background: #101516; }
        QDialog QScrollBar::handle:vertical { background: #465658; }

        QMenu {
            background: #12191a; color: #e7ddca;
            border: 1px solid #344244; border-radius: 4px; padding: 4px;
        }
        QMenu::item {
            border-radius: 3px; padding: 7px 24px 7px 10px;
        }
        QMenu::item:selected {
            background: #29484b; color: #ffffff;
            border-left: 3px solid #b99b67;
        }
        QMenu::item:disabled { color: #718082; }
        QMenu::separator {
            height: 1px; background: #344244; margin: 5px 7px;
        }
        QToolTip {
            background: #202b2d; color: #f4ead8;
            border: 1px solid #b99b67; border-radius: 3px; padding: 5px 7px;
        }
    )"));
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
    const bool debugGuiAgent = argc > 2 && std::string_view(argv[1]) == "debug" &&
        std::string_view(argv[2]) == "gui-agent";
    if (hasAutomationHandle && !debugRun && !debugGuiAgent) {
        std::cerr << "automation handles are accepted only by an explicitly reactive debug command\n";
        return 2;
    }
    if (argc > 1) {
        if (debugGuiAgent) {
            QApplication app(argc, argv);
            setApplicationIdentity();
            app.setWindowIcon(QIcon(QStringLiteral(":/jam2/assets/logo-nebula.png")));
            applyCustomTheme(app);
            installCompactDialogPolicy(app);
            return jam2RunGuiTestAgent(app, argc, argv);
        }
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

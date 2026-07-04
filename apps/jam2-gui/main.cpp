#include "MainWindow.hpp"

#include <QApplication>
#include <QFont>

namespace {

void applyDarkTheme(QApplication& app)
{
    app.setStyleSheet(R"(
        QWidget {
            background: #101418;
            color: #d7dde4;
            font-size: 13px;
        }
        QLabel#AppTitle {
            color: #f1f5f9;
            font-size: 22px;
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

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("Jam2 GUI");
    QApplication::setOrganizationName("Jam2");
    applyDarkTheme(app);

    MainWindow window;
    window.resize(1920, 1080);
    window.show();

    return app.exec();
}

#pragma once
#include <QDialog>
#include <functional>
class QLabel;
class QProgressBar;
class QPushButton;
class QCloseEvent;
namespace jam2::update { class ApplicationUpdateService; }

class UpdateDialog final : public QDialog {
public:
    UpdateDialog(jam2::update::ApplicationUpdateService& service,
        std::function<bool()> closeApplication, QWidget* parent = nullptr);
    void refresh();
private:
    void closeEvent(QCloseEvent* event) override;
    jam2::update::ApplicationUpdateService& service_;
    std::function<bool()> closeApplication_;
    QLabel* heading_;
    QLabel* message_;
    QLabel* explanation_;
    QProgressBar* progress_;
    QPushButton* download_;
    QPushButton* restart_;
    QPushButton* later_;
    QPushButton* cancel_;
    QPushButton* retry_;
    QPushButton* close_;
};

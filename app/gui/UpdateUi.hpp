#pragma once
#include "ApplicationUpdateService.hpp"
#include <QObject>
#include <QTimer>
#include <functional>
#include <memory>
class QWidget;
class UpdateDialog;

class UpdateUi final : public QObject {
public:
    UpdateUi(QWidget* window, QString root, QString installedVersion, std::function<bool()> closeWindow,
        jam2::update::ApplicationUpdateService* service = nullptr);
    ~UpdateUi() override;
    void startAutomaticCheck();
    void showDialog();
    void windowClosing();
    std::function<void(const QString&, const QString&)> presentationChanged;
private:
    void refresh();
    QWidget* window_;
    std::unique_ptr<jam2::update::ApplicationUpdateService> ownedService_;
    jam2::update::ApplicationUpdateService* service_;
    std::unique_ptr<UpdateDialog> dialog_;
    QTimer promptTimer_;
    jam2::update::State lastState_ = jam2::update::State::Idle;
    bool closing_ = false;
};

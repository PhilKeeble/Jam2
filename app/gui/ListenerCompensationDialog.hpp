#pragma once

#include "RuntimeContracts.hpp"

#include <QDialog>

class QDoubleSpinBox;
class QWidget;

class ListenerCompensationDialog final : public QDialog {
public:
    explicit ListenerCompensationDialog(
        const Jam2MetronomeCompensationSettings& settings,
        QWidget* parent = nullptr);

    Jam2MetronomeCompensationSettings settings() const noexcept;

private:
    QDoubleSpinBox* maximum_ = nullptr;
    QDoubleSpinBox* smoothing_ = nullptr;
    QDoubleSpinBox* deadband_ = nullptr;
    QDoubleSpinBox* slew_ = nullptr;
};

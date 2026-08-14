#include "ListenerCompensationDialog.hpp"

#include "GuiControlContract.hpp"
#include "GuiPresentation.hpp"

#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

QDoubleSpinBox* makeSpin(
    QWidget* parent,
    double value,
    double maximum,
    const QString& suffix)
{
    auto* spin = new QDoubleSpinBox(parent);
    spin->setRange(0.0, maximum);
    spin->setDecimals(1);
    spin->setSuffix(suffix);
    spin->setValue(value);
    applyMutedEditorStyle(spin);
    return spin;
}

void registerModalControl(QObject& control, const char* id)
{
    jam2::gui::registerGuiControl(
        control,
        QStringLiteral("metronome.compensation-dialog.") +
            QString::fromLatin1(id),
        QStringLiteral("metronome.listener-compensation"),
        jam2::gui::GuiControlAvailability::Modal);
}

} // namespace

ListenerCompensationDialog::ListenerCompensationDialog(
    const Jam2MetronomeCompensationSettings& settings,
    QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Listener Compensation"));
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    maximum_ = makeSpin(this, settings.maximum_ms, 1000.0, QStringLiteral(" ms"));
    smoothing_ = makeSpin(this, settings.smoothing_ms, 10000.0, QStringLiteral(" ms"));
    deadband_ = makeSpin(this, settings.deadband_ms, 1000.0, QStringLiteral(" ms"));
    slew_ = makeSpin(this, settings.slew_ms_per_second, 10000.0, QStringLiteral(" ms/s"));

    form->addRow(QStringLiteral("Max offset"), maximum_);
    form->addRow(QStringLiteral("Smoothing"), smoothing_);
    form->addRow(QStringLiteral("Deadband"), deadband_);
    form->addRow(QStringLiteral("Slew limit"), slew_);
    layout->addLayout(form);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    registerModalControl(*maximum_, "maximum");
    registerModalControl(*smoothing_, "smoothing");
    registerModalControl(*deadband_, "deadband");
    registerModalControl(*slew_, "slew");
    if (QPushButton* apply = buttons->button(QDialogButtonBox::Ok)) {
        registerModalControl(*apply, "apply");
    }
    if (QPushButton* cancel = buttons->button(QDialogButtonBox::Cancel)) {
        registerModalControl(*cancel, "cancel");
    }
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

Jam2MetronomeCompensationSettings ListenerCompensationDialog::settings() const noexcept
{
    return {
        maximum_->value(),
        smoothing_->value(),
        deadband_->value(),
        slew_->value(),
    };
}

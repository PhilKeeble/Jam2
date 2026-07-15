#include "GuiPresentation.hpp"

#include <QCheckBox>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QProxyStyle>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStyleOption>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>

namespace {

class Jam2Style final : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;

    void drawPrimitive(
        PrimitiveElement element,
        const QStyleOption* option,
        QPainter* painter,
        const QWidget* widget = nullptr) const override
    {
        if (element != PE_IndicatorCheckBox) {
            QProxyStyle::drawPrimitive(element, option, painter, widget);
            return;
        }

        const QRect box = option->rect.adjusted(1, 1, -1, -1);
        const bool enabled = (option->state & State_Enabled) != 0;
        const bool checked = (option->state & State_On) != 0;
        const bool hovered = (option->state & State_MouseOver) != 0;
        const bool focused = (option->state & State_HasFocus) != 0;
        const QColor border = enabled
            ? (hovered || focused ? QColor(QStringLiteral("#66c6a6"))
                                 : QColor(QStringLiteral("#8a97a1")))
            : QColor(QStringLiteral("#4d565d"));
        const QColor fill = checked ? QColor(QStringLiteral("#1b3b33"))
                                    : QColor(QStringLiteral("#101214"));
        const QColor tick = enabled ? QColor(QStringLiteral("#f2f5f7"))
                                    : QColor(QStringLiteral("#7a858c"));

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(QPen(border, 1.25));
        painter->setBrush(fill);
        painter->drawRect(box);
        if (checked) {
            painter->setPen(QPen(tick, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            QPainterPath path;
            path.moveTo(box.left() + box.width() * 0.24, box.top() + box.height() * 0.54);
            path.lineTo(box.left() + box.width() * 0.43, box.top() + box.height() * 0.72);
            path.lineTo(box.left() + box.width() * 0.77, box.top() + box.height() * 0.30);
            painter->drawPath(path);
        }
        painter->restore();
    }
};

}

void installJam2Style()
{
    static bool installed = false;
    if (!installed) {
        QApplication::setStyle(new Jam2Style(QApplication::style()));
        installed = true;
    }
}

void showJamReadyInviteDialog(QWidget* parent, const QString& inviteUrl)
{
    QApplication::clipboard()->setText(inviteUrl);

    auto* dialog = new QDialog(parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(QStringLiteral("Jam Ready"));
    dialog->setWindowModality(Qt::NonModal);
    dialog->resize(680, 150);
    auto* layout = new QVBoxLayout(dialog);
    auto* instruction = new QLabel(
        QStringLiteral("Send this URL to the people joining your jam."), dialog);
    auto* urlEdit = new QLineEdit(inviteUrl, dialog);
    urlEdit->setReadOnly(true);
    urlEdit->setCursorPosition(0);
    auto* copiedLabel = new QLabel(QStringLiteral("Copied to clipboard."), dialog);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    auto* copyButton = buttons->addButton(
        QStringLiteral("Copy URL"), QDialogButtonBox::ActionRole);
    layout->addWidget(instruction);
    layout->addWidget(urlEdit);
    layout->addWidget(copiedLabel);
    layout->addWidget(buttons);
    QObject::connect(copyButton, &QPushButton::clicked, dialog, [inviteUrl, copiedLabel] {
        QApplication::clipboard()->setText(inviteUrl);
        copiedLabel->setText(QStringLiteral("Copied to clipboard."));
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

FocusPreset focusPresetForKey(const QString& key)
{
    if (key == QStringLiteral("bass")) {
        return {95.0, 14.0, 7.0};
    }
    if (key == QStringLiteral("guitar")) {
        return {850.0, 12.0, 5.0};
    }
    if (key == QStringLiteral("vocals")) {
        return {1800.0, 12.0, 4.5};
    }
    if (key == QStringLiteral("drums")) {
        return {3200.0, 10.0, 3.5};
    }
    return {120.0, 12.0, 6.0};
}

QDir appReleaseDir()
{
    QDir appDir(QCoreApplication::applicationDirPath());

    QDir bundleDir(appDir);
    if (bundleDir.dirName() == QStringLiteral("MacOS") &&
        bundleDir.cdUp() &&
        bundleDir.dirName() == QStringLiteral("Contents") &&
        bundleDir.cdUp() &&
        bundleDir.dirName().endsWith(QStringLiteral(".app")) &&
        bundleDir.cdUp()) {
        return bundleDir;
    }

    if (appDir.dirName() == QStringLiteral("release")) {
        return appDir;
    }

    QDir probe(appDir);
    while (true) {
        if (probe.exists(QStringLiteral("release"))) {
            return QDir(probe.absoluteFilePath(QStringLiteral("release")));
        }
        if (!probe.cdUp()) {
            break;
        }
    }

    return appDir;
}

QString appReleaseFilePath(const QString& folder, const QString& fileName)
{
    QDir dir = appReleaseDir();
    dir.mkpath(folder);
    return dir.absoluteFilePath(folder + QLatin1Char('/') + fileName);
}

QString appReleaseFolderPath(const QString& folder)
{
    QDir dir = appReleaseDir();
    dir.mkpath(folder);
    return dir.absoluteFilePath(folder);
}

bool isCustomFocusPreset(const QString& key)
{
    return key.isEmpty() || key == QStringLiteral("custom");
}

QString trackValueEditorStyle()
{
    return QStringLiteral(
        "QAbstractSpinBox { border: 1px solid #52616c; background: #101214; color: #f2f5f7; padding: 2px 28px 2px 6px; }"
        "QComboBox, QLineEdit { border: 1px solid #52616c; background: #101214; color: #f2f5f7; padding: 2px 6px; }"
        "QAbstractSpinBox:focus, QComboBox:focus, QLineEdit:focus { border: 1px solid #66c6a6; }"
        "QAbstractSpinBox:disabled, QComboBox:disabled, QLineEdit:disabled { border: 1px solid #343c42; background: #171a1d; color: #6f7a82; }");
}

void applyMutedEditorStyle(QWidget* widget)
{
    if (widget == nullptr) {
        return;
    }
    widget->setAttribute(Qt::WA_MacShowFocusRect, false);
    widget->setStyleSheet(trackValueEditorStyle());
    if (auto* combo = qobject_cast<QComboBox*>(widget); combo != nullptr && combo->lineEdit() != nullptr) {
        combo->lineEdit()->setAttribute(Qt::WA_MacShowFocusRect, false);
        combo->lineEdit()->setStyleSheet(trackValueEditorStyle());
    }
}

void updateCaptureDurationControl(QCheckBox* manualStopCheck, QSpinBox* durationSpin)
{
    if (manualStopCheck == nullptr || durationSpin == nullptr) {
        return;
    }
    durationSpin->setEnabled(!manualStopCheck->isChecked());
}

void updateCaptureDurationControl(QCheckBox* manualStopCheck, QSpinBox* durationSpin, QLabel* durationLabel)
{
    updateCaptureDurationControl(manualStopCheck, durationSpin);
    if (durationLabel != nullptr && manualStopCheck != nullptr) {
        durationLabel->setEnabled(!manualStopCheck->isChecked());
    }
}

QString jamSliderStyle()
{
    return QStringLiteral(
        "QSlider::groove:horizontal { height: 6px; background: #2a3035; border: 1px solid #3d464d; }"
        "QSlider::sub-page:horizontal { background: #66c6a6; border: 1px solid #66c6a6; }"
        "QSlider::add-page:horizontal { background: #1b1f23; border: 1px solid #3d464d; }"
        "QSlider::handle:horizontal { width: 16px; height: 16px; margin: -6px 0; background: #f2f5f7; border: 1px solid #66c6a6; }"
        "QSlider::groove:horizontal:disabled { background: #2e3235; border: 1px solid #454b50; }"
        "QSlider::sub-page:horizontal:disabled { background: #5e666c; border: 1px solid #5e666c; }"
        "QSlider::add-page:horizontal:disabled { background: #25282b; border: 1px solid #454b50; }"
        "QSlider::handle:horizontal:disabled { background: #8a9298; border: 1px solid #6b7379; }");
}

void applyJamSliderStyle(QSlider* slider)
{
    if (slider != nullptr) {
        slider->setStyleSheet(jamSliderStyle());
    }
}

QString dbText(double db)
{
    return QStringLiteral("%1%2 dB")
        .arg(db >= 0.0 ? QStringLiteral("+") : QString())
        .arg(db, 0, 'f', 1);
}

double gainFromDb(double db)
{
    if (db <= -60.0) {
        return 0.0;
    }
    return std::pow(10.0, db / 20.0);
}

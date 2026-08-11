#include "GuiPresentation.hpp"
#include "GuiTheme.hpp"

#include <QCheckBox>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QGuiApplication>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QProxyStyle>
#include <QPushButton>
#include <QScreen>
#include <QSlider>
#include <QSpinBox>
#include <QStyleOption>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace {

namespace theme = jam2::gui::theme;

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
        const QColor fill = checked
            ? theme::withAlpha(theme::playhead, 70)
            : theme::editorBg;
        const QColor tick = enabled ? theme::textStrong : theme::textMuted;

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setPen(QPen(enabled ? theme::playhead : theme::border, 1.0));
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

class CompactDialogEventFilter final : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        auto* dialog = qobject_cast<QDialog*>(watched);
        if (dialog == nullptr ||
            (event->type() != QEvent::Polish && event->type() != QEvent::Show)) {
            return QObject::eventFilter(watched, event);
        }

        if (event->type() == QEvent::Polish) {
            prepareDialog(*dialog);
            placeDialog(*dialog);
            return QObject::eventFilter(watched, event);
        }

        // Polish already prepared the final size before the window became
        // visible. Re-centre synchronously on Show, but do not hide and resize
        // the live window again: that left stale child-widget pixels in the
        // backing store on Windows, visibly duplicating dialog controls.
        placeDialog(*dialog);
        return QObject::eventFilter(watched, event);
    }

private:
    static constexpr auto kPreparedSizeProperty = "jam2PreparedDialogSize";

    static void prepareDialog(QDialog& dialog)
    {
        if (dialog.layout() != nullptr) dialog.layout()->activate();
        QSize prepared = dialog.testAttribute(Qt::WA_Resized)
            ? dialog.size()
            : dialog.sizeHint();
        if (!prepared.isValid()) prepared = QSize(480, 320);
        dialog.setProperty(kPreparedSizeProperty, prepared);
    }

    static void placeDialog(QDialog& dialog)
    {
        QWidget* owner = dialog.parentWidget() != nullptr
            ? dialog.parentWidget()->window()
            : QApplication::activeWindow();
        if (owner == &dialog) owner = nullptr;
        QScreen* screen = owner != nullptr ? owner->screen() : dialog.screen();
        if (screen == nullptr) screen = QGuiApplication::primaryScreen();
        if (screen == nullptr) return;

        const QRect available = screen->availableGeometry();
        const QSize maximum(
            std::min(900, std::max(320, available.width() - 96)),
            std::min(720, std::max(240, available.height() - 96)));
        if (dialog.isMaximized() || dialog.isFullScreen()) {
            dialog.setWindowState(dialog.windowState() &
                ~(Qt::WindowMaximized | Qt::WindowFullScreen));
        }
        dialog.setMaximumSize(maximum);

        QSize target = dialog.property(kPreparedSizeProperty).toSize();
        if (!target.isValid()) {
            if (dialog.layout() != nullptr) dialog.layout()->activate();
            target = dialog.sizeHint();
        }
        if (qobject_cast<QFileDialog*>(&dialog) != nullptr && !target.isValid()) {
            target = QSize(800, 560);
        }
        if (!target.isValid()) target = QSize(480, 320);
        target = target.expandedTo(dialog.minimumSizeHint()).boundedTo(maximum);
        dialog.resize(target);

        const QPoint centre = owner != nullptr
            ? owner->frameGeometry().center()
            : available.center();
        QRect frame(QPoint(0, 0), dialog.frameGeometry().size());
        frame.moveCenter(centre);
        frame.moveLeft(std::clamp(
            frame.left(),
            available.left(),
            std::max(available.left(), available.right() - frame.width() + 1)));
        frame.moveTop(std::clamp(
            frame.top(),
            available.top(),
            std::max(available.top(), available.bottom() - frame.height() + 1)));
        dialog.move(frame.topLeft());
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

void installCompactDialogPolicy(QApplication& app)
{
    auto* filter = new CompactDialogEventFilter(&app);
    app.installEventFilter(filter);
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

void applyMutedEditorStyle(QWidget* widget)
{
    if (widget == nullptr) {
        return;
    }
    widget->setAttribute(Qt::WA_MacShowFocusRect, false);
    widget->setProperty("jam2MutedEditor", true);
    if (auto* combo = qobject_cast<QComboBox*>(widget); combo != nullptr && combo->lineEdit() != nullptr) {
        combo->lineEdit()->setAttribute(Qt::WA_MacShowFocusRect, false);
        combo->lineEdit()->setProperty("jam2MutedEditor", true);
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
        "QSlider::groove:horizontal { height: 6px; background: #172023; border: 1px solid #354247; border-radius: 3px; }"
        "QSlider::sub-page:horizontal { background: #e8a44a; border: 1px solid #e8a44a; border-radius: 3px; }"
        "QSlider::add-page:horizontal { background: #090d0e; border: 1px solid #354247; border-radius: 3px; }"
        "QSlider::handle:horizontal { width: 17px; height: 17px; margin: -6px 0; background: #e8a44a; border: 1px solid #ffd68e; border-radius: 8px; }"
        "QSlider::groove:horizontal:disabled { background: #101719; border: 1px solid #354247; }"
        "QSlider::sub-page:horizontal:disabled { background: #59676a; border: 1px solid #59676a; }"
        "QSlider::add-page:horizontal:disabled { background: #0b1011; border: 1px solid #354247; }"
        "QSlider::handle:horizontal:disabled { background: #657275; border: 1px solid #354247; }");
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

QString metronomeStepLabel(int step, int division)
{
    const int safeDivision = qMax(1, division);
    const int safeStep = qMax(0, step);
    const int beat = safeStep / safeDivision + 1;
    const int subdivision = safeDivision == 2
        ? (safeStep % safeDivision) * 2 + 1
        : safeStep % safeDivision + 1;
    return QStringLiteral("%1.%2").arg(beat).arg(subdivision);
}

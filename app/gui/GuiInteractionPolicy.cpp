#include "GuiInteractionPolicy.hpp"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTabBar>
#include <QTextEdit>
#include <QWheelEvent>
#include <QWidget>

namespace jam2::gui {

bool explicitValueEditorHasFocus(QWidget* focus)
{
    for (QWidget* widget = focus; widget != nullptr; widget = widget->parentWidget()) {
        if (qobject_cast<QLineEdit*>(widget) ||
            qobject_cast<QPlainTextEdit*>(widget) ||
            qobject_cast<QTextEdit*>(widget) ||
            qobject_cast<QAbstractSpinBox*>(widget) ||
            qobject_cast<QComboBox*>(widget)) {
            return true;
        }
    }
    return false;
}

bool blocksIncidentalNavigationKey(QWidget* focus)
{
    for (QWidget* widget = focus; widget != nullptr; widget = widget->parentWidget()) {
        if (qobject_cast<QAbstractSlider*>(widget) ||
            qobject_cast<QAbstractButton*>(widget) ||
            qobject_cast<QAbstractItemView*>(widget) ||
            qobject_cast<QAbstractScrollArea*>(widget) ||
            qobject_cast<QTabBar*>(widget)) {
            return true;
        }
    }
    return false;
}

bool isWheelValueEditor(QObject* object)
{
    for (QObject* current = object; current != nullptr; current = current->parent()) {
        const QString className = QString::fromLatin1(current->metaObject()->className());
        if (qobject_cast<QAbstractSpinBox*>(current) ||
            qobject_cast<QAbstractSlider*>(current) ||
            qobject_cast<QComboBox*>(current) ||
            className.contains(QStringLiteral("QComboBox"))) {
            return true;
        }
    }
    return false;
}

bool isComboBoxPopupObject(QObject* object)
{
    for (QObject* current = object; current != nullptr; current = current->parent()) {
        const QString className = QString::fromLatin1(current->metaObject()->className());
        if (className.contains(QStringLiteral("QComboBoxListView")) ||
            className.contains(QStringLiteral("QComboBoxPrivateContainer"))) {
            return true;
        }
    }
    return false;
}

QAbstractScrollArea* parentScrollArea(
    QObject* object,
    Qt::Orientation orientation)
{
    auto* widget = qobject_cast<QWidget*>(object);
    while (widget != nullptr) {
        if (auto* scrollArea = qobject_cast<QAbstractScrollArea*>(widget)) {
            QScrollBar* bar = orientation == Qt::Horizontal
                ? scrollArea->horizontalScrollBar()
                : scrollArea->verticalScrollBar();
            if (bar != nullptr && bar->maximum() > bar->minimum()) return scrollArea;
        }
        widget = widget->parentWidget();
    }
    return nullptr;
}

bool scrollAreaByWheel(
    QAbstractScrollArea& scrollArea,
    QWheelEvent& wheel,
    Qt::Orientation orientation,
    bool useVerticalAxis)
{
    QScrollBar* bar = orientation == Qt::Horizontal
        ? scrollArea.horizontalScrollBar()
        : scrollArea.verticalScrollBar();
    if (bar == nullptr || bar->maximum() <= bar->minimum()) return false;
    const QPoint pixelDelta = wheel.pixelDelta();
    const QPoint angleDelta = wheel.angleDelta();
    int delta = orientation == Qt::Horizontal && !useVerticalAxis
        ? pixelDelta.x() : pixelDelta.y();
    if (delta == 0) {
        delta = (orientation == Qt::Horizontal && !useVerticalAxis
            ? angleDelta.x() : angleDelta.y()) / 8;
    }
    if (delta == 0) return false;
    bar->setValue(bar->value() - delta);
    return true;
}

} // namespace jam2::gui

#pragma once

#include <Qt>

class QAbstractScrollArea;
class QObject;
class QWheelEvent;
class QWidget;

namespace jam2::gui {

bool explicitValueEditorHasFocus(QWidget* focus);
bool blocksIncidentalNavigationKey(QWidget* focus);
bool isWheelValueEditor(QObject* object);
bool isComboBoxPopupObject(QObject* object);
QAbstractScrollArea* parentScrollArea(
    QObject* object,
    Qt::Orientation orientation);
bool scrollAreaByWheel(
    QAbstractScrollArea& scrollArea,
    QWheelEvent& wheel,
    Qt::Orientation orientation,
    bool useVerticalAxis = false);

} // namespace jam2::gui

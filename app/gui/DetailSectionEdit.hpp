#pragma once

#include <QLineEdit>

#include <functional>

class QFocusEvent;
class QKeyEvent;
class QMouseEvent;

namespace jam2::gui {

// A deliberately small, reusable inline-name editor. Read-only display is the
// default; an explicitly editable instance enters editing on double-click and
// commits on Enter/focus loss or restores its original value on Escape.
class DetailSectionEdit final : public QLineEdit {
public:
    explicit DetailSectionEdit(QWidget* parent = nullptr);

    std::function<void(const QString&)> onCommitted;

protected:
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;

private:
    void finishEditing(bool commit);

    QString original_;
};

} // namespace jam2::gui

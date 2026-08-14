#include "DetailSectionEdit.hpp"

#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QSizePolicy>
#include <QStyle>

namespace jam2::gui {

DetailSectionEdit::DetailSectionEdit(QWidget* parent)
    : QLineEdit(parent)
{
    setReadOnly(true);
    setFocusPolicy(Qt::NoFocus);
    setFrame(false);
    setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    setMinimumWidth(460);
    setMaximumWidth(760);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setToolTip(QStringLiteral("Double-click to rename this section"));
}

void DetailSectionEdit::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (property("sectionEditable").toBool()) {
        original_ = text();
        setFocusPolicy(Qt::StrongFocus);
        setReadOnly(false);
        setFocus(Qt::MouseFocusReason);
        setProperty("editing", true);
        style()->unpolish(this);
        style()->polish(this);
        selectAll();
        event->accept();
        return;
    }
    QLineEdit::mouseDoubleClickEvent(event);
}

void DetailSectionEdit::keyPressEvent(QKeyEvent* event)
{
    if (!isReadOnly() && event->key() == Qt::Key_Escape) {
        setText(original_);
        finishEditing(false);
        event->accept();
        return;
    }
    if (!isReadOnly() &&
        (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
        finishEditing(true);
        event->accept();
        return;
    }
    QLineEdit::keyPressEvent(event);
}

void DetailSectionEdit::focusOutEvent(QFocusEvent* event)
{
    if (!isReadOnly()) finishEditing(true);
    QLineEdit::focusOutEvent(event);
}

void DetailSectionEdit::finishEditing(bool commit)
{
    QString value = text().trimmed();
    if (value.isEmpty()) value = original_;
    setText(value);
    setCursorPosition(0);
    setReadOnly(true);
    deselect();
    setFocusPolicy(Qt::NoFocus);
    clearFocus();
    setProperty("editing", false);
    style()->unpolish(this);
    style()->polish(this);
    if (commit && value != original_ && onCommitted) onCommitted(value);
}

} // namespace jam2::gui

#include "DetailSectionEdit.hpp"

#include <QApplication>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>

#include <iostream>
#include <stdexcept>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

void doubleClick(jam2::gui::DetailSectionEdit& edit)
{
    QMouseEvent event(
        QEvent::MouseButtonDblClick,
        QPointF(4.0, 4.0), QPointF(4.0, 4.0), QPointF(4.0, 4.0),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&edit, &event);
}

void press(jam2::gui::DetailSectionEdit& edit, int key)
{
    QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(&edit, &event);
}

} // namespace

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    try {
        jam2::gui::DetailSectionEdit edit;
        edit.setText(QStringLiteral("Original"));
        edit.resize(500, 40);

        doubleClick(edit);
        require(edit.isReadOnly(),
            "non-editable section label entered edit mode");

        QStringList committed;
        edit.onCommitted = [&committed](const QString& value) {
            committed.push_back(value);
        };
        edit.setProperty("sectionEditable", true);

        doubleClick(edit);
        require(!edit.isReadOnly() && edit.property("editing").toBool(),
            "editable section label did not enter edit mode");
        edit.setText(QStringLiteral("  Renamed  "));
        press(edit, Qt::Key_Return);
        require(edit.isReadOnly() && !edit.property("editing").toBool() &&
                edit.text() == QStringLiteral("Renamed") &&
                committed == QStringList{QStringLiteral("Renamed")},
            "Return did not commit one trimmed section name");

        doubleClick(edit);
        edit.setText(QStringLiteral("Cancelled"));
        press(edit, Qt::Key_Escape);
        require(edit.text() == QStringLiteral("Renamed") && committed.size() == 1,
            "Escape did not restore the original section name");

        doubleClick(edit);
        edit.setText(QStringLiteral("   "));
        press(edit, Qt::Key_Enter);
        require(edit.text() == QStringLiteral("Renamed") && committed.size() == 1,
            "empty section name did not restore the original without committing");

        doubleClick(edit);
        edit.setText(QStringLiteral("Focus Commit"));
        QFocusEvent focusOut(QEvent::FocusOut, Qt::OtherFocusReason);
        QApplication::sendEvent(&edit, &focusOut);
        require(edit.isReadOnly() && edit.text() == QStringLiteral("Focus Commit") &&
                committed == QStringList{
                    QStringLiteral("Renamed"), QStringLiteral("Focus Commit")},
            "focus loss did not commit the edited section name");

        jam2::gui::DetailSectionEdit noCallback;
        noCallback.setText(QStringLiteral("Before"));
        noCallback.setProperty("sectionEditable", true);
        doubleClick(noCallback);
        noCallback.setText(QStringLiteral("After"));
        press(noCallback, Qt::Key_Return);
        require(noCallback.text() == QStringLiteral("After"),
            "section edit required an optional commit callback");

        std::cout << "Jam2 detail section edit tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Detail section edit test failure: " << error.what() << '\n';
        return 1;
    }
}

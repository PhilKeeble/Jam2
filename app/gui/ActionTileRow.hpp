#pragma once

#include <QFont>
#include <QRect>
#include <QString>
#include <QVector>
#include <QWidget>

class QHBoxLayout;
class QPushButton;

namespace jam2::gui {

struct ActionTileSpec {
    QString label;
    int preferredWidth = 116;
};

struct ActionTileLayout {
    QFont font;
    int padding = 9;
    int spacing = 8;
    int height = 31;
    QVector<QRect> bounds;
};

QFont actionTileFont();
ActionTileLayout layoutActionTiles(
    const QRect& available, const QVector<ActionTileSpec>& actions,
    const QFont& font = actionTileFont(), const QPaintDevice* device = nullptr);

// One row at every width. Owns existing semantic buttons without changing
// their actions or control ids.
class ActionTileRow : public QWidget {
public:
    explicit ActionTileRow(QWidget* parent = nullptr);
    void addButton(QPushButton* button, int preferredWidth);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void updateButtons();
    QHBoxLayout* layout_;
    QVector<QPushButton*> buttons_;
    QVector<ActionTileSpec> actions_;
    QVector<QString> styles_;
    bool updating_ = false;
};

} // namespace jam2::gui

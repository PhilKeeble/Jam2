#include "ActionTileRow.hpp"

#include <QEvent>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QPushButton>
#include <QResizeEvent>

#include <cmath>

namespace jam2::gui {

QFont actionTileFont()
{
    QFont font(QStringLiteral("Bahnschrift"));
    font.setPointSizeF(9.0);
    font.setWeight(QFont::DemiBold);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0.45);
    return font;
}

ActionTileLayout layoutActionTiles(
    const QRect& available, const QVector<ActionTileSpec>& actions, const QFont& font,
    const QPaintDevice* device)
{
    ActionTileLayout result;
    result.font = font;
    if (actions.isEmpty()) return result;
    const int width = qMax(0, available.width());
    QVector<int> widths;
    const auto measure = [&] {
        const QFontMetricsF metrics(result.font, device);
        widths.clear();
        int total = qMax(0, static_cast<int>(actions.size()) - 1) * result.spacing;
        const double scale = result.font.pointSizeF() / qMax(1.0, font.pointSizeF());
        for (const auto& action : actions) {
            const int preferred = qRound(action.preferredWidth * scale) - 2 * (9 - result.padding);
            const int measured = static_cast<int>(std::ceil(metrics.horizontalAdvance(action.label))) +
                result.padding * 2 + 4;
            widths.push_back(qMax(preferred, measured));
            total += widths.back();
        }
        return total;
    };
    int total = measure();
    // Spend decorative space before reducing the readable label size.
    while (total > width && result.padding > 3) {
        --result.padding;
        result.spacing = qMax(3, result.spacing - 1);
        total = measure();
    }
    const double minimumPoints = qMin(8.0, font.pointSizeF());
    while (total > width && result.font.pointSizeF() > minimumPoints + 0.01) {
        result.font.setPointSizeF(qMax(minimumPoints, result.font.pointSizeF() - 0.1));
        total = measure();
    }
    result.height = qMax(27, qMax(qRound(31.0 * result.font.pointSizeF() /
        qMax(1.0, font.pointSizeF())), QFontMetrics(result.font, device).height() + 10));
    const int count = static_cast<int>(actions.size());
    result.spacing = count > 1 ? qMin(result.spacing, width / (count - 1)) : 0;
    const int tileSpace = qMax(0, width - result.spacing * (count - 1));
    int naturalSpace = 0;
    for (int tileWidth : widths) naturalSpace += tileWidth;
    const int usedSpace = qMin(tileSpace, naturalSpace);
    int x = available.left();
    int accumulated = 0;
    int assigned = 0;
    for (int tileWidth : widths) {
        accumulated += tileWidth;
        const int next = naturalSpace > 0
            ? static_cast<int>(static_cast<qint64>(accumulated) * usedSpace / naturalSpace) : 0;
        result.bounds.push_back(QRect(x, available.top(), next - assigned, result.height));
        x += next - assigned + result.spacing;
        assigned = next;
    }
    return result;
}

ActionTileRow::ActionTileRow(QWidget* parent)
    : QWidget(parent), layout_(new QHBoxLayout(this))
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSizeConstraint(QLayout::SetNoConstraint);
    layout_->addStretch(1);
}

void ActionTileRow::addButton(QPushButton* button, int preferredWidth)
{
    buttons_.push_back(button);
    actions_.push_back({button->text(), preferredWidth});
    styles_.push_back(button->styleSheet());
    button->setToolTip(button->text());
    button->setAccessibleName(button->text());
    layout_->insertWidget(layout_->count() - 1, button);
    updateButtons();
    updateGeometry();
}

QSize ActionTileRow::sizeHint() const
{
    const auto sizing = layoutActionTiles(QRect(0, 0, 100000, 31), actions_, actionTileFont(), this);
    return QSize(sizing.bounds.isEmpty() ? 0 : sizing.bounds.back().right() + 1, sizing.height);
}

QSize ActionTileRow::minimumSizeHint() const { return QSize(0, 27); }

void ActionTileRow::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateButtons();
}

void ActionTileRow::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
        updateButtons();
}

void ActionTileRow::updateButtons()
{
    if (updating_) return;
    updating_ = true;
    const auto sizing = layoutActionTiles(rect(), actions_, actionTileFont(), this);
    layout_->setSpacing(sizing.spacing);
    const QFontMetrics metrics(sizing.font, this);
    for (qsizetype i = 0; i < buttons_.size(); ++i) {
        auto* button = buttons_[i];
        button->setFont(sizing.font);
        const QString style = styles_[i] + QStringLiteral(
            "\nQPushButton { padding:0 %1px;font-size:%2pt;font-family:\"%3\";"
            "font-weight:600;letter-spacing:0.45px; }")
            .arg(sizing.padding).arg(sizing.font.pointSizeF(), 0, 'f', 1)
            .arg(sizing.font.family());
        if (button->styleSheet() != style) button->setStyleSheet(style);
        button->setFixedSize(sizing.bounds[i].width(), sizing.height);
        button->setText(metrics.elidedText(actions_[i].label, Qt::ElideRight,
            qMax(0, sizing.bounds[i].width() - 2 * sizing.padding - 2)));
    }
    setFixedHeight(sizing.height);
    updating_ = false;
}

} // namespace jam2::gui

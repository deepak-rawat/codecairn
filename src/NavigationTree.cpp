#include "NavigationTree.h"
#include <QKeyEvent>
#include <QMouseEvent>
#include <QStyle>
#include <QStyleOptionViewItem>

QRect NavigationTree::itemIconRect(const QTreeWidgetItem *item, int column) const {
    if (!item || item->icon(column).isNull())
        return {};
    QStyleOptionViewItem option;
    initViewItemOption(&option);
    option.index = indexFromItem(item, column);
    option.rect = visualRect(option.index);
    option.icon = item->icon(column);
    option.text = item->text(column);
    option.features |= QStyleOptionViewItem::HasDecoration;
    if (!option.text.isEmpty())
        option.features |= QStyleOptionViewItem::HasDisplay;
    return style()->subElementRect(QStyle::SE_ItemViewItemDecoration, &option, this);
}

void NavigationTree::mouseReleaseEvent(QMouseEvent *event) {
    auto *item = itemAt(event->position().toPoint());
    const int column = columnAt(event->position().toPoint().x());
    const bool icon = event->button() == Qt::LeftButton && column >= 0 &&
                      itemIconRect(item, column).contains(event->position().toPoint());
    QTreeWidget::mouseReleaseEvent(event);
    if (icon)
        emit iconClicked(item, column);
}

void NavigationTree::keyPressEvent(QKeyEvent *event) {
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) &&
        (event->modifiers() == Qt::NoModifier || event->modifiers() == Qt::KeypadModifier)) {
        if (auto *item = currentItem(); item && (item->flags() & Qt::ItemIsEnabled)) {
            emit itemActivated(item, currentColumn());
        }
        event->accept();
        return;
    }
    const auto *previous = currentItem();
    QTreeWidget::keyPressEvent(event);
    if (previous != currentItem() && event->key() != Qt::Key_Return &&
        event->key() != Qt::Key_Enter)
        emit keyboardSelectionChanged();
}

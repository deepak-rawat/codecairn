#pragma once
#include <QTreeWidget>

class NavigationTree : public QTreeWidget {
    Q_OBJECT
public:
    using QTreeWidget::QTreeWidget;
    QRect itemIconRect(const QTreeWidgetItem *item, int column = 0) const;
signals:
    void keyboardSelectionChanged();
    void iconClicked(QTreeWidgetItem *item, int column);

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
};

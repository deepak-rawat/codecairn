#pragma once

#include "NavigationTree.h"
#include <QHash>
#include <QPersistentModelIndex>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>

class ReferenceResultsView final : public NavigationTree {
    Q_OBJECT
public:
    explicit ReferenceResultsView(QWidget *parent = nullptr);
    ~ReferenceResultsView() override;
    void rebuildTextDocument();
    QTextCursor textCursor() const { return cursor_; }
    void copySelection() const;
    void selectAllText();
    QList<QTreeWidgetItem *> selectedTextItems() const;
    bool iconHovered(const QModelIndex &index) const { return hoveredIcon_ == index; }
    void paintText(QPainter *painter, const QPointF &origin, const QModelIndex &index,
                   const QColor &foreground, const QColor &heading) const;
signals:
    void textSelectionChanged();
protected:
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
private:
    QTextDocument text_;
    QTextCursor cursor_;
    QVector<QTreeWidgetItem *> lines_;
    QHash<QTreeWidgetItem *, int> lineNumbers_;
    QPersistentModelIndex hoveredIcon_;
    QTimer blink_;
    QTimer rebuild_;
    bool cursorVisible_ = true;
    bool changingCursor_ = false;
    bool draggingText_ = false;
    void applyCursor(const QTextCursor &cursor);
    QTextCursor cursorAt(const QPoint &point) const;
};

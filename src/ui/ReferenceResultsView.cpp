#include "ReferenceResultsView.h"
#include "ReferenceSearch.h"
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QSignalBlocker>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextLayout>

ReferenceResultsView::ReferenceResultsView(QWidget *parent)
    : NavigationTree(parent), cursor_(&text_) {
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    viewport()->setCursor(Qt::IBeamCursor);
    text_.setDocumentMargin(0);
    auto options = text_.defaultTextOption();
    options.setWrapMode(QTextOption::NoWrap);
    text_.setDefaultTextOption(options);
    rebuild_.setSingleShot(true);
    connect(&rebuild_, &QTimer::timeout, this, &ReferenceResultsView::rebuildTextDocument);
    connect(this, &QTreeWidget::itemChanged, this, [this] { rebuild_.start(0); });
    connect(this, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (changingCursor_ || !lineNumbers_.contains(item))
            return;
        if (rebuild_.isActive())
            rebuildTextDocument();
        auto cursor = QTextCursor(text_.findBlockByNumber(lineNumbers_.value(item)));
        applyCursor(cursor);
    });
    connect(&blink_, &QTimer::timeout, this, [this] {
        cursorVisible_ = !cursorVisible_;
        viewport()->update();
    });
}

ReferenceResultsView::~ReferenceResultsView() {
    disconnect(this, nullptr, this, nullptr);
}

void ReferenceResultsView::rebuildTextDocument() {
    rebuild_.stop();
    const int oldPosition = cursor_.position(), oldAnchor = cursor_.anchor();
    lines_.clear();
    lineNumbers_.clear();
    QStringList text;
    for (int group = 0; group < topLevelItemCount(); ++group) {
        auto *heading = topLevelItem(group);
        lines_.append(heading);
        for (int row = 0; row < heading->childCount(); ++row)
            lines_.append(heading->child(row));
    }
    for (int line = 0; line < lines_.size(); ++line) {
        lineNumbers_.insert(lines_[line], line);
        auto display = lines_[line]->data(0, ReferenceItemRole::DisplayText).toString();
        // Context blocks remain multi-line in stored hits, but occupy one visual result row.
        display.replace('\r', ' ').replace('\n', ' ')
            .replace(QChar::ParagraphSeparator, ' ').replace(QChar::LineSeparator, ' ');
        text << display;
    }
    text_.setDefaultFont(font());
    text_.setPlainText(text.join('\n'));
    text_.documentLayout()->documentSize();
    const int end = text_.characterCount() - 1;
    cursor_ = QTextCursor(&text_);
    cursor_.setPosition(qBound(0, oldAnchor, end));
    cursor_.setPosition(qBound(0, oldPosition, end), QTextCursor::KeepAnchor);
    viewport()->update();
}

void ReferenceResultsView::applyCursor(const QTextCursor &cursor) {
    cursor_ = cursor;
    const int line = cursor_.blockNumber();
    if (line >= 0 && line < lines_.size()) {
        changingCursor_ = true;
        setCurrentItem(lines_[line]);
        scrollToItem(lines_[line]);
        changingCursor_ = false;
        const auto block = cursor_.block();
        const int x = qRound(block.layout()->lineAt(0).cursorToX(cursor_.positionInBlock())) +
                      visualItemRect(lines_[line]).left() + iconSize().width() + 10;
        if (x > viewport()->width() - 4)
            horizontalScrollBar()->setValue(horizontalScrollBar()->value() + x - viewport()->width() + 4);
        else if (x < iconSize().width() + 10)
            horizontalScrollBar()->setValue(horizontalScrollBar()->value() + x - iconSize().width() - 10);
    }
    cursorVisible_ = true;
    viewport()->update();
    emit textSelectionChanged();
}

QTextCursor ReferenceResultsView::cursorAt(const QPoint &point) const {
    auto *item = itemAt(point);
    if (!item || !lineNumbers_.contains(item))
        return cursor_;
    const auto block = text_.findBlockByNumber(lineNumbers_.value(item));
    const auto line = block.layout()->lineAt(0);
    const int x = point.x() - visualItemRect(item).left() - iconSize().width() - 10;
    QTextCursor cursor(block);
    cursor.setPosition(block.position() + line.xToCursor(x));
    return cursor;
}

void ReferenceResultsView::copySelection() const {
    if (!cursor_.hasSelection())
        return;
    auto selected = cursor_.selectedText();
    selected.replace(QChar::ParagraphSeparator, '\n');
    QApplication::clipboard()->setText(selected);
}

void ReferenceResultsView::selectAllText() {
    auto cursor = cursor_;
    cursor.select(QTextCursor::Document);
    applyCursor(cursor);
}

QList<QTreeWidgetItem *> ReferenceResultsView::selectedTextItems() const {
    QList<QTreeWidgetItem *> result;
    if (!cursor_.hasSelection())
        return result;
    const int first = text_.findBlock(cursor_.selectionStart()).blockNumber();
    const int last = text_.findBlock(cursor_.selectionEnd() - 1).blockNumber();
    for (int line = qMax(0, first); line <= last && line < lines_.size(); ++line)
        result.append(lines_[line]);
    return result;
}

void ReferenceResultsView::keyPressEvent(QKeyEvent *event) {
    if (rebuild_.isActive())
        rebuildTextDocument();
    if (event->matches(QKeySequence::Copy)) {
        copySelection();
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::SelectAll)) {
        selectAllText();
        event->accept();
        return;
    }
    const bool control = event->modifiers().testFlag(Qt::ControlModifier);
    QTextCursor::MoveOperation operation = QTextCursor::NoMove;
    int count = 1;
    switch (event->key()) {
    case Qt::Key_Left: operation = control ? QTextCursor::PreviousWord : QTextCursor::PreviousCharacter; break;
    case Qt::Key_Right: operation = control ? QTextCursor::NextWord : QTextCursor::NextCharacter; break;
    case Qt::Key_Up: operation = QTextCursor::Up; break;
    case Qt::Key_Down: operation = QTextCursor::Down; break;
    case Qt::Key_Home: operation = control ? QTextCursor::Start : QTextCursor::StartOfBlock; break;
    case Qt::Key_End: operation = control ? QTextCursor::End : QTextCursor::EndOfBlock; break;
    case Qt::Key_PageUp: operation = QTextCursor::Up; count = qMax(1, viewport()->height() / qMax(1, fontMetrics().height())); break;
    case Qt::Key_PageDown: operation = QTextCursor::Down; count = qMax(1, viewport()->height() / qMax(1, fontMetrics().height())); break;
    default: break;
    }
    if (operation != QTextCursor::NoMove) {
        auto cursor = cursor_;
        cursor.movePosition(operation, event->modifiers().testFlag(Qt::ShiftModifier)
                                           ? QTextCursor::KeepAnchor : QTextCursor::MoveAnchor, count);
        applyCursor(cursor);
        event->accept();
    } else if (!event->text().isEmpty() || event->key() == Qt::Key_Delete ||
               event->key() == Qt::Key_Backspace) {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
            emit itemActivated(currentItem(), 0);
        event->accept();
    } else
        NavigationTree::keyPressEvent(event);
}

void ReferenceResultsView::mousePressEvent(QMouseEvent *event) {
    if (rebuild_.isActive())
        rebuildTextDocument();
    const auto point = event->position().toPoint();
    if (event->button() == Qt::LeftButton && itemAt(point) &&
        !itemIconRect(itemAt(point)).contains(point)) {
        setFocus();
        auto cursor = cursorAt(point);
        if (event->modifiers().testFlag(Qt::ShiftModifier)) {
            const auto end = cursor.position();
            cursor = cursor_;
            cursor.setPosition(end, QTextCursor::KeepAnchor);
        }
        applyCursor(cursor);
        draggingText_ = true;
        event->accept();
    } else
        NavigationTree::mousePressEvent(event);
}

void ReferenceResultsView::mouseMoveEvent(QMouseEvent *event) {
    const auto point = event->position().toPoint();
    if (draggingText_ && event->buttons().testFlag(Qt::LeftButton)) {
        auto cursor = cursor_;
        cursor.setPosition(cursorAt(point).position(), QTextCursor::KeepAnchor);
        applyCursor(cursor);
    } else
        NavigationTree::mouseMoveEvent(event);
    auto *item = itemAt(point);
    const auto hovered = item && itemIconRect(item).contains(point)
                             ? QPersistentModelIndex(indexFromItem(item)) : QPersistentModelIndex{};
    if (hovered != hoveredIcon_) {
        hoveredIcon_ = hovered;
        viewport()->update();
    }
    viewport()->setCursor(hovered.isValid() ? Qt::PointingHandCursor : Qt::IBeamCursor);
}

void ReferenceResultsView::mouseReleaseEvent(QMouseEvent *event) {
    if (draggingText_ && event->button() == Qt::LeftButton) {
        draggingText_ = false;
        emit itemClicked(currentItem(), 0);
        event->accept();
    } else
        NavigationTree::mouseReleaseEvent(event);
}

void ReferenceResultsView::mouseDoubleClickEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && itemAt(event->position().toPoint()) &&
        !itemIconRect(itemAt(event->position().toPoint())).contains(event->position().toPoint())) {
        auto cursor = cursorAt(event->position().toPoint());
        cursor.select(QTextCursor::WordUnderCursor);
        applyCursor(cursor);
        event->accept();
    } else
        NavigationTree::mouseDoubleClickEvent(event);
}

void ReferenceResultsView::leaveEvent(QEvent *event) {
    hoveredIcon_ = QPersistentModelIndex{};
    viewport()->setCursor(Qt::IBeamCursor);
    viewport()->update();
    NavigationTree::leaveEvent(event);
}

void ReferenceResultsView::focusInEvent(QFocusEvent *event) {
    NavigationTree::focusInEvent(event);
    cursorVisible_ = true;
    if (QApplication::cursorFlashTime() > 0)
        blink_.start(qMax(100, QApplication::cursorFlashTime() / 2));
    viewport()->update();
    emit textSelectionChanged();
}

void ReferenceResultsView::focusOutEvent(QFocusEvent *event) {
    blink_.stop();
    NavigationTree::focusOutEvent(event);
    viewport()->update();
    emit textSelectionChanged();
}

void ReferenceResultsView::paintText(QPainter *painter, const QPointF &origin,
                                   const QModelIndex &index, const QColor &foreground,
                                   const QColor &heading) const {
    auto *item = itemFromIndex(index);
    if (!lineNumbers_.contains(item))
        return;
    const auto block = text_.findBlockByNumber(lineNumbers_.value(item));
    auto *layout = block.layout();
    QVector<QTextLayout::FormatRange> formats;
    if (index.data(ReferenceItemRole::Heading).toBool()) {
        QTextCharFormat format;
        format.setForeground(heading);
        format.setFontUnderline(true);
        formats.append({5, static_cast<int>(index.data(ReferenceItemRole::Query).toString().size()), format});
    }
    const int start = qMax(cursor_.selectionStart(), block.position()) - block.position();
    const int end = qMin(cursor_.selectionEnd(), block.position() + block.length() - 1) - block.position();
    if (end > start) {
        QTextCharFormat selection;
        selection.setBackground(palette().highlight());
        selection.setForeground(palette().highlightedText());
        formats.append({start, end - start, selection});
    }
    painter->setPen(foreground);
    // Document layouts carry document-space positions; rows supply their own vertical placement.
    const auto offset = origin - layout->position();
    layout->draw(painter, offset, formats);
    if (hasFocus() && cursorVisible_ && cursor_.block() == block)
        layout->drawCursor(painter, offset, cursor_.positionInBlock(), 1);
}

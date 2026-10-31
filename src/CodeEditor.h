#pragma once

#include "AppSettings.h"
#include <ScintillaEditBase.h>
#include <QFont>
#include <QMetaObject>
#include <QPoint>

class QMouseEvent;
class QKeyEvent;

class CodeEditor final : public ScintillaEditBase {
    Q_OBJECT
public:
    explicit CodeEditor(QWidget *parent = nullptr);
    QByteArray bytes() const;
    void loadBytes(const QByteArray &text);
    void applyStyles(const QByteArray &styles);
    void applyTypography(const Typography &typography);
    void applyConfiguration(const AppSettings &settings);
    void selectRange(int start, int end);
    void selectLineAt(int position);
    int caretPosition() const;
    int lineHeight() const;
    bool isModified() const;
    void markSaved();
    void setReadOnly(bool value);
    void showLineNumbers(bool value);
    int lookupPosition() const;
    void setLinkBrowsing(bool enabled) { linkBrowsing_ = enabled; }
    void setLinkAvailable(int position, bool available);
    bool linkGestureActive() const;
    void changeZoom(int delta);
    void resetZoom();
    void setReferenceLines(const QVector<int> &lines);
signals:
    void contentEdited();
    void rangeEdited(int position, int removed, int inserted, const QByteArray &beforeHash);
    void caretMoved();
    void selectionActivated();
    void linkHovered(int position);
    void definitionActivated(int position);
    void referencesRequested();
    void historyRequested(int direction);
    void referenceLineClicked(int line);

protected:
    bool event(QEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void enterEvent(QEnterEvent *event) override;

private:
    AppSettings settings_;
    QMetaObject::Connection screenDpiConnection_;
    void trackScreen();
    bool loading_ = false;
    QByteArray beforeEditHash_;
    bool referenceLineSelected_ = false;
    void setReferenceSelection(bool enabled);
    sptr_t lastCaret_ = -1;
    sptr_t lastAnchor_ = -1;
    bool linkBrowsing_ = false;
    bool controlDown_ = false;
    bool linkAvailable_ = false;
    bool pressedLink_ = false;
    bool linkPressConsumed_ = false;
    int hoveredPosition_ = -1;
    QPoint pressedPoint_;
    int pointPosition(const QPoint &point) const;
    void updatePointer(const QPoint &point);
    void refreshPointer();
    QRect referenceIconRect(const QPoint &point, int *line = nullptr) const;
    int hoveredReferenceLine_ = -1;
};

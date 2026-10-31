#include "CodeEditor.h"
#include <QCryptographicHash>
#include "UiAppearance.h"

#include <QColor>
#include <QApplication>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QScreen>
#include <QTimer>
#include <QImage>

namespace {
int rgb(const QColor &c) {
    return c.red() | (c.green() << 8) | (c.blue() << 16);
}
constexpr int ReferenceMarker = 24;
constexpr int HoveredReferenceMarker = 23;
constexpr int ReferenceMargin = 1;
}

CodeEditor::CodeEditor(QWidget *parent)
    : ScintillaEditBase(parent), settings_(defaultAppSettings()) {
    setFrameShape(QFrame::NoFrame);
    setMouseTracking(true);
    viewport()->setMouseTracking(true);
    send(SCI_SETCODEPAGE, SC_CP_UTF8);
    send(SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
    send(SCI_SETMARGINWIDTHN, 0, 0);
    send(SCI_SETMARGINWIDTHN, 1, 0);
    send(SCI_SETMARGINTYPEN, ReferenceMargin, SC_MARGIN_SYMBOL);
    send(SCI_SETMARGINMASKN, ReferenceMargin, (1 << ReferenceMarker) | (1 << HoveredReferenceMarker));
    send(SCI_SETMARGINSENSITIVEN, ReferenceMargin, true);
    send(SCI_SETMARGINCURSORN, ReferenceMargin, SC_CURSORARROW);
    connect(this, &ScintillaEditBase::marginClicked, this,
            [this](Scintilla::Position position, Scintilla::KeyMod, int margin) {
                const int line = static_cast<int>(send(SCI_LINEFROMPOSITION, position));
                if (margin == ReferenceMargin && (send(SCI_MARKERGET, line) & (1 << ReferenceMarker)))
                    emit referenceLineClicked(line);
            });
    send(SCI_SETSCROLLWIDTHTRACKING, true);
    send(SCI_SETSCROLLWIDTH, 1);
    sends(SCI_SETWORDCHARS, 0, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_$");
    send(SCI_SETSELFORE, false);
    send(SCI_SETMARGINLEFT, 0, 5);
    send(SCI_SETMARGINRIGHT, 0, 2);
    send(SCI_SETEOLMODE, SC_EOL_LF);
    applyConfiguration(settings_);
    trackScreen();
    connect(this, &ScintillaEditBase::modified, this,
            [this](Scintilla::ModificationFlags flags, Scintilla::Position position,
                   Scintilla::Position length, auto...) {
                const auto bits = static_cast<int>(flags);
                if (loading_)
                    return;
                if (bits & (SC_MOD_BEFOREINSERT | SC_MOD_BEFOREDELETE))
                    beforeEditHash_ = QCryptographicHash::hash(bytes(), QCryptographicHash::Sha256).toHex();
                if (bits & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)) {
                    emit rangeEdited(static_cast<int>(position),
                                     bits & SC_MOD_DELETETEXT ? static_cast<int>(length) : 0,
                                     bits & SC_MOD_INSERTTEXT ? static_cast<int>(length) : 0,
                                     beforeEditHash_);
                    emit contentEdited();
                }
            });
    connect(this, &ScintillaEditBase::updateUi, this, [this](Scintilla::Update flags) {
        if (!loading_ && (static_cast<int>(flags) & (SC_UPDATE_V_SCROLL | SC_UPDATE_H_SCROLL)))
            refreshPointer();
        if (!loading_ && (static_cast<int>(flags) & SC_UPDATE_SELECTION)) {
            const auto caret = send(SCI_GETCURRENTPOS), anchor = send(SCI_GETANCHOR);
            if (referenceLineSelected_) {
                const auto first = qMin(caret, anchor);
                const auto line = send(SCI_LINEFROMPOSITION, first);
                if (first != send(SCI_POSITIONFROMLINE, line) ||
                    qMax(caret, anchor) != send(SCI_GETLINEENDPOSITION, line)) {
                    setReferenceSelection(false);
                }
            }
            if (caret != lastCaret_ || anchor != lastAnchor_) {
                lastCaret_ = caret;
                lastAnchor_ = anchor;
                emit caretMoved();
            }
        }
    });
}

QByteArray CodeEditor::bytes() const {
    const auto length = static_cast<qsizetype>(send(SCI_GETLENGTH));
    QByteArray text(length + 1, '\0');
    send(SCI_GETTEXT, static_cast<uptr_t>(text.size()), reinterpret_cast<sptr_t>(text.data()));
    text.resize(length);
    return text;
}

void CodeEditor::loadBytes(const QByteArray &text) {
    loading_ = true;
    setReferenceSelection(false);
    const bool readOnly = send(SCI_GETREADONLY);
    send(SCI_SETREADONLY, false);
    send(SCI_CLEARALL);
    sends(SCI_ADDTEXT, static_cast<uptr_t>(text.size()), text.constData());
    send(SCI_EMPTYUNDOBUFFER);
    send(SCI_SETSAVEPOINT);
    send(SCI_SETREADONLY, readOnly);
    send(SCI_SETEOLMODE, text.contains("\r\n") ? SC_EOL_CRLF : SC_EOL_LF);
    loading_ = false;
    pressedLink_ = false;
    refreshPointer();
}

void CodeEditor::applyStyles(const QByteArray &styles) {
    if (styles.size() != send(SCI_GETLENGTH)) {
        qWarning("Ignoring stale source styles: document length changed.");
        return;
    }
    send(SCI_STARTSTYLING, 0);
    sends(SCI_SETSTYLINGEX, static_cast<uptr_t>(styles.size()), styles.constData());
}

void CodeEditor::applyTypography(const Typography &typography) {
    settings_.editor.typography = typography;
    applyConfiguration(settings_);
}

void CodeEditor::applyConfiguration(const AppSettings &settings) {
    settings_ = settings;
    const auto &editor = settings_.editor;
    const auto &typography = editor.typography;
    const double pointSize = UiAppearance::nativeSourcePointSize(typography.pointSize, *this);
    const auto sourceFamily =
        typography.family.isEmpty() ? UiAppearance::sourceFont() : typography.family;
    const auto family = sourceFamily.toUtf8();
    const auto color = [this](const char *role) {
        return rgb(settings_.color(role));
    };
    // Match the viewport underpaint to text-run backgrounds at fractional DPI.
    viewport()->setStyleSheet("background-color: " + settings_.color("source.background").name() +
                              ';');
    send(SCI_SETTABWIDTH, editor.tabWidth);
    send(SCI_SETINDENT, editor.indentWidth);
    send(SCI_SETUSETABS, editor.useTabs);
    send(SCI_SETCARETLINEVISIBLE, editor.currentLine);
    send(SCI_SETCARETLINEBACK, color("source.currentLine"));
    send(SCI_SETCARETFORE, color("source.caret"));
    setReferenceSelection(referenceLineSelected_);
    send(SCI_SETADDITIONALSELBACK, color("source.additionalSelection"));
    send(SCI_SETEXTRAASCENT, editor.extraAscent);
    send(SCI_SETEXTRADESCENT, editor.extraDescent);
    sends(SCI_STYLESETFONT, STYLE_DEFAULT, family.constData());
    send(SCI_STYLESETSIZEFRACTIONAL, STYLE_DEFAULT, qRound(pointSize * SC_FONT_SIZE_MULTIPLIER));
    send(SCI_STYLESETFORE, STYLE_DEFAULT, color("source.foreground"));
    send(SCI_STYLESETBACK, STYLE_DEFAULT, color("source.background"));
    send(SCI_STYLECLEARALL);
    for (size_t i = 0; i < settings_.syntax.size(); ++i) {
        const auto &style = settings_.syntax[i];
        const bool declaration = i == static_cast<size_t>(TextStyle::Type) ||
                                 i == static_cast<size_t>(TextStyle::Function);
        const double sizeScale =
            typography.uniform ? 1.0
                               : style.scale * (declaration ? typography.declarationScale : 1.0);
        send(SCI_STYLESETFORE, i, rgb(style.foreground));
        send(SCI_STYLESETBACK, i,
             style.background.isValid() ? rgb(style.background) : color("source.background"));
        send(SCI_STYLESETBOLD, i, style.bold);
        send(SCI_STYLESETITALIC, i, style.italic);
        const auto styleFamily =
            (style.fontFamily.isEmpty() ? sourceFamily : style.fontFamily).toUtf8();
        sends(SCI_STYLESETFONT, i, styleFamily.constData());
        send(SCI_STYLESETSIZEFRACTIONAL, i,
             qRound(pointSize * sizeScale * SC_FONT_SIZE_MULTIPLIER));
    }
    send(SCI_STYLESETFORE, STYLE_LINENUMBER, color("source.lineNumbers"));
    send(SCI_STYLESETBACK, STYLE_LINENUMBER, color("source.background"));
    send(SCI_SETVIEWWS, typography.whitespace ? SCWS_VISIBLEALWAYS : SCWS_INVISIBLE);
    showLineNumbers(editor.lineNumbers);
    const int iconSize = settings_.ui.iconSize;
    for (const bool hovered : {false, true}) {
        const auto image = UiAppearance::referenceLinkImage(iconSize, hovered);
        send(SCI_RGBAIMAGESETWIDTH, image.width());
        send(SCI_RGBAIMAGESETHEIGHT, image.height());
        send(SCI_RGBAIMAGESETSCALE, 100);
        sends(SCI_MARKERDEFINERGBAIMAGE, hovered ? HoveredReferenceMarker : ReferenceMarker,
              reinterpret_cast<const char *>(image.constBits()));
    }
    if (send(SCI_GETMARGINWIDTHN, ReferenceMargin) > 0)
        send(SCI_SETMARGINWIDTHN, ReferenceMargin, iconSize + 4);
}

void CodeEditor::setReferenceLines(const QVector<int> &lines) {
    send(SCI_MARKERDELETEALL, HoveredReferenceMarker);
    hoveredReferenceLine_ = -1;
    send(SCI_MARKERDELETEALL, ReferenceMarker);
    for (const int line : lines)
        if (line >= 0 && line < send(SCI_GETLINECOUNT))
            send(SCI_MARKERADD, line, ReferenceMarker);
    send(SCI_SETMARGINWIDTHN, ReferenceMargin, lines.isEmpty() ? 0 : settings_.ui.iconSize + 4);
}

void CodeEditor::selectLineAt(int position) {
    const auto line = send(SCI_LINEFROMPOSITION, position);
    selectRange(static_cast<int>(send(SCI_POSITIONFROMLINE, line)),
                static_cast<int>(send(SCI_GETLINEENDPOSITION, line)));
    setReferenceSelection(true);
}

void CodeEditor::selectRange(int start, int end) {
    setReferenceSelection(false);
    send(SCI_SETSEL, static_cast<uptr_t>(start), end);
    send(SCI_SCROLLCARET);
}

void CodeEditor::setReferenceSelection(bool enabled) {
    referenceLineSelected_ = enabled;
    const auto foreground = rgb(settings_.color("source.foreground"));
    const auto background = rgb(settings_.color(enabled ? "source.referenceSelection"
                                                       : "source.selectionBackground"));
    send(SCI_SETSELBACK, true, background);
    send(SCI_SETSELFORE, enabled, foreground);
    if (enabled) {
        send(SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_INACTIVE_BACK, 0xff000000u | background);
        send(SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_INACTIVE_TEXT, 0xff000000u | foreground);
    } else {
        send(SCI_RESETELEMENTCOLOUR, SC_ELEMENT_SELECTION_INACTIVE_BACK);
        send(SCI_RESETELEMENTCOLOUR, SC_ELEMENT_SELECTION_INACTIVE_TEXT);
    }
}

int CodeEditor::caretPosition() const {
    return static_cast<int>(send(SCI_GETCURRENTPOS));
}
int CodeEditor::lineHeight() const {
    return static_cast<int>(send(SCI_TEXTHEIGHT, 0));
}
bool CodeEditor::isModified() const {
    return send(SCI_GETMODIFY);
}
void CodeEditor::markSaved() {
    send(SCI_SETSAVEPOINT);
}
void CodeEditor::setReadOnly(bool value) {
    send(SCI_SETREADONLY, value);
}

void CodeEditor::showLineNumbers(bool value) {
    settings_.editor.lineNumbers = value;
    send(SCI_SETMARGINWIDTHN, 0,
         value ? send(SCI_TEXTWIDTH, STYLE_LINENUMBER, reinterpret_cast<sptr_t>("99999 ")) + 8 : 0);
}

int CodeEditor::lookupPosition() const {
    const auto start = send(SCI_GETSELECTIONSTART), end = send(SCI_GETSELECTIONEND);
    const auto position = start != end ? start : send(SCI_GETCURRENTPOS);
    return static_cast<int>(send(SCI_WORDSTARTPOSITION, static_cast<uptr_t>(position), true));
}

bool CodeEditor::linkGestureActive() const {
    return linkBrowsing_ || controlDown_;
}

int CodeEditor::pointPosition(const QPoint &point) const {
    return static_cast<int>(send(SCI_POSITIONFROMPOINTCLOSE, point.x(), point.y()));
}

void CodeEditor::setLinkAvailable(int position, bool available) {
    if (position != hoveredPosition_ || !linkGestureActive())
        return;
    linkAvailable_ = available;
    viewport()->setCursor(available ? Qt::PointingHandCursor : Qt::IBeamCursor);
}

void CodeEditor::updatePointer(const QPoint &point) {
    int line = -1;
    const bool reference = referenceIconRect(point, &line).contains(point);
    if (hoveredReferenceLine_ != (reference ? line : -1)) {
        send(SCI_MARKERDELETEALL, HoveredReferenceMarker);
        hoveredReferenceLine_ = reference ? line : -1;
        if (reference)
            send(SCI_MARKERADD, line, HoveredReferenceMarker);
    }
    if (reference) {
        viewport()->setCursor(Qt::PointingHandCursor);
        return;
    }
    const int position = linkGestureActive() ? pointPosition(point) : -1;
    if (position == hoveredPosition_)
        return;
    hoveredPosition_ = position;
    linkAvailable_ = false;
    viewport()->setCursor(Qt::IBeamCursor);
    emit linkHovered(position);
}

QRect CodeEditor::referenceIconRect(const QPoint &point, int *line) const {
    const int left = static_cast<int>(send(SCI_GETMARGINWIDTHN, 0));
    const int width = static_cast<int>(send(SCI_GETMARGINWIDTHN, ReferenceMargin));
    if (!width || point.x() < left || point.x() >= left + width)
        return {};
    const int position = static_cast<int>(send(SCI_POSITIONFROMPOINTCLOSE, left + width + 6, point.y()));
    if (position < 0)
        return {};
    const int sourceLine = static_cast<int>(send(SCI_LINEFROMPOSITION, position));
    if (!(send(SCI_MARKERGET, sourceLine) & (1 << ReferenceMarker)))
        return {};
    if (line)
        *line = sourceLine;
    const int y = static_cast<int>(send(SCI_POINTYFROMPOSITION, 0, send(SCI_POSITIONFROMLINE, sourceLine)));
    const int size = settings_.ui.iconSize;
    return {left + (width - size) / 2, y + (lineHeight() - size) / 2, size, size};
}

void CodeEditor::refreshPointer() {
    send(SCI_MARKERDELETEALL, HoveredReferenceMarker);
    hoveredReferenceLine_ = -1;
    hoveredPosition_ = -1;
    linkAvailable_ = false;
    viewport()->setCursor(Qt::IBeamCursor);
    emit linkHovered(-1);
    QTimer::singleShot(0, this, [this] {
        const auto point = viewport()->mapFromGlobal(QCursor::pos());
        if (viewport()->isVisible() && viewport()->rect().contains(point) &&
            QApplication::activeWindow() == window())
            updatePointer(point);
    });
}

void CodeEditor::trackScreen() {
    disconnect(screenDpiConnection_);
    auto *currentScreen = screen();
    if (!currentScreen) {
        qWarning("Cannot track source font DPI: no screen is available.");
        return;
    }
    screenDpiConnection_ = connect(currentScreen, &QScreen::logicalDotsPerInchChanged, this,
                                   [this] { applyConfiguration(settings_); });
}

bool CodeEditor::event(QEvent *event) {
    const bool handled = ScintillaEditBase::event(event);
    if (event->type() == QEvent::ScreenChangeInternal || event->type() == QEvent::Show) {
        trackScreen();
        applyConfiguration(settings_);
    }
    return handled;
}

void CodeEditor::enterEvent(QEnterEvent *event) {
    ScintillaEditBase::enterEvent(event);
    updatePointer(viewport()->mapFromGlobal(QCursor::pos()));
}

void CodeEditor::mouseMoveEvent(QMouseEvent *event) {
    controlDown_ = event->modifiers().testFlag(Qt::ControlModifier);
    if (event->buttons() == Qt::NoButton)
        updatePointer(event->position().toPoint());
    ScintillaEditBase::mouseMoveEvent(event);
    if (hoveredReferenceLine_ >= 0)
        viewport()->setCursor(Qt::PointingHandCursor);
    if (linkAvailable_ && linkGestureActive())
        viewport()->setCursor(Qt::PointingHandCursor);
}

void CodeEditor::mousePressEvent(QMouseEvent *event) {
    controlDown_ = event->modifiers().testFlag(Qt::ControlModifier);
    pressedPoint_ = event->position().toPoint();
    pressedLink_ =
        event->button() == Qt::LeftButton &&
        (controlDown_ || (linkBrowsing_ && linkAvailable_ && event->modifiers() == Qt::NoModifier));
    linkPressConsumed_ = pressedLink_ && controlDown_;
    if (linkPressConsumed_) {
        event->accept();
        return;
    }
    ScintillaEditBase::mousePressEvent(event);
}

void CodeEditor::mouseReleaseEvent(QMouseEvent *event) {
    const bool activate = pressedLink_ && event->button() == Qt::LeftButton &&
                          (event->position().toPoint() - pressedPoint_).manhattanLength() <
                              QApplication::startDragDistance();
    const bool consumed = linkPressConsumed_;
    pressedLink_ = false;
    linkPressConsumed_ = false;
    // End Scintilla's selection gesture before navigation can change focus.
    if (!consumed)
        ScintillaEditBase::mouseReleaseEvent(event);
    if (activate) {
        emit definitionActivated(pointPosition(event->position().toPoint()));
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton)
        emit selectionActivated();
}

void CodeEditor::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_0 && event->modifiers() == Qt::ControlModifier) {
        resetZoom();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Control) {
        controlDown_ = true;
        hoveredPosition_ = -1;
        updatePointer(viewport()->mapFromGlobal(QCursor::pos()));
    }
    if (event->modifiers().testFlag(Qt::AltModifier) &&
        (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right)) {
        emit historyRequested(event->key() == Qt::Key_Left ? -1 : 1);
        event->accept();
        return;
    }
    ScintillaEditBase::keyPressEvent(event);
}

void CodeEditor::keyReleaseEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Control) {
        controlDown_ = false;
        if (!linkBrowsing_) {
            hoveredPosition_ = -1;
            linkAvailable_ = false;
            viewport()->setCursor(Qt::IBeamCursor);
            emit linkHovered(-1);
        }
    }
    ScintillaEditBase::keyReleaseEvent(event);
}

void CodeEditor::leaveEvent(QEvent *event) {
    send(SCI_MARKERDELETEALL, HoveredReferenceMarker);
    hoveredReferenceLine_ = -1;
    hoveredPosition_ = -1;
    linkAvailable_ = false;
    viewport()->setCursor(Qt::IBeamCursor);
    emit linkHovered(-1);
    ScintillaEditBase::leaveEvent(event);
}

void CodeEditor::contextMenuEvent(QContextMenuEvent *event) {
    QMenu menu(this);
    auto *definition = menu.addAction("Jump to Definition");
    auto *references = menu.addAction("Lookup References");
    menu.addSeparator();
    auto *copy = menu.addAction("Copy");
    copy->setEnabled(send(SCI_GETSELECTIONSTART) != send(SCI_GETSELECTIONEND));
    auto *paste = menu.addAction("Paste");
    paste->setEnabled(!send(SCI_GETREADONLY));
    const auto *selected = menu.exec(event->globalPos());
    if (selected == definition)
        emit definitionActivated(lookupPosition());
    else if (selected == references)
        emit referencesRequested();
    else if (selected == copy)
        send(SCI_COPY);
    else if (selected == paste)
        send(SCI_PASTE);
}

void CodeEditor::changeZoom(int delta) {
    send(SCI_SETZOOM,
         static_cast<uptr_t>(qBound(-4, static_cast<int>(send(SCI_GETZOOM)) + delta, 20)));
    hoveredPosition_ = -1;
    linkAvailable_ = false;
    viewport()->setCursor(Qt::IBeamCursor);
    emit linkHovered(-1);
}
void CodeEditor::resetZoom() {
    send(SCI_SETZOOM, 0);
    hoveredPosition_ = -1;
    linkAvailable_ = false;
    viewport()->setCursor(Qt::IBeamCursor);
    emit linkHovered(-1);
}

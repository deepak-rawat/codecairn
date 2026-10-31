#include "UiAppearance.h"

#include <QApplication>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QHeaderView>
#include <QHash>
#include <QIconEngine>
#include <QPainter>
#include <QPaintDevice>
#include <QImage>
#include <QStyledItemDelegate>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QTreeWidget>

namespace {
// Profile point sizes retain the original Windows sizing, independent of monitor DPI.
constexpr double SourceReferenceDpi = 96.0;

class TreeBranches final : public QProxyStyle {
public:
    TreeBranches() : QProxyStyle(QStyleFactory::create("Fusion")) {}
    void setColors(const AppSettings &settings) {
        line_ = settings.color("ui.branchLine");
        border_ = settings.color("ui.branchBorder");
        background_ = settings.color("ui.branchBackground");
        glyph_ = settings.color("ui.branchGlyph");
    }
    void drawPrimitive(PrimitiveElement element, const QStyleOption *option, QPainter *painter,
                       const QWidget *widget) const override {
        if (element != PE_IndicatorBranch) {
            QProxyStyle::drawPrimitive(element, option, painter, widget);
            return;
        }
        painter->save();
        const auto r = option->rect;
        const auto c = r.center();
        painter->setPen(QPen(line_, 1, Qt::DotLine));
        if (option->state & State_Item) {
            painter->drawLine(c.x(), r.top(), c.x(), c.y());
            painter->drawLine(c.x(), c.y(), r.right(), c.y());
        }
        if (option->state & State_Sibling)
            painter->drawLine(c.x(), r.top(), c.x(), r.bottom());
        if (option->state & State_Children) {
            painter->setPen(border_);
            painter->setBrush(background_);
            painter->drawRect(c.x() - 3, c.y() - 3, 6, 6);
            painter->setPen(glyph_);
            painter->drawLine(c.x() - 2, c.y(), c.x() + 2, c.y());
            if (!(option->state & State_Open))
                painter->drawLine(c.x(), c.y() - 2, c.x(), c.y() + 2);
        }
        painter->restore();
    }

private:
    QColor line_, border_, background_, glyph_;
};

class CompactRows final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void setMinimumHeight(int height) { minimumHeight_ = height; }
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        auto size = QStyledItemDelegate::sizeHint(option, index);
        size.setHeight(qMax(minimumHeight_, option.fontMetrics.height()));
        return size;
    }

private:
    int minimumHeight_ = 0;
};

// Original vector glyphs; drawn in logical coordinates so they remain crisp at high DPI.
class GlyphEngine final : public QIconEngine {
public:
    explicit GlyphEngine(QString kind) : kind_(std::move(kind)) {}
    QIconEngine *clone() const override { return new GlyphEngine(kind_); }
    QPixmap pixmap(const QSize &size, QIcon::Mode mode, QIcon::State state) override {
        QPixmap image(size);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        paint(&painter, image.rect(), mode, state);
        return image;
    }
    void paint(QPainter *p, const QRect &rect, QIcon::Mode mode, QIcon::State state) override {
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->translate(rect.topLeft());
        p->scale(rect.width() / 16.0, rect.height() / 16.0);
        if (mode == QIcon::Disabled)
            p->setOpacity(0.35);
        const QColor cyan("#38b9d9"), yellow("#e4da52"), green("#59dc65"), white("#eeeeee");
        p->setPen(QPen(white, 1));
        if (kind_ == "searchSymbol") {
            p->setPen(QPen(QColor("#1515d0"), 1));
            p->setBrush(QColor("#7700d0"));
            p->drawRect(QRectF(3, 3, 8, 10));
            p->setPen(white);
            auto font = p->font();
            font.setPixelSize(11);
            font.setBold(true);
            p->setFont(font);
            p->drawText(QRectF(3, 1, 8, 13), Qt::AlignCenter, "?");
        } else if (kind_ == "referenceLink" || kind_ == "referencePrevious" || kind_ == "referenceNext") {
            p->setPen(QPen(QColor("#727272"), 1));
            p->setBrush(QColor("#dedede"));
            p->drawRect(QRectF(1, 3, 14, 10));
            p->setPen(Qt::NoPen);
            p->setBrush(QColor("#ed3030"));
            if (kind_ != "referenceNext")
                p->drawPolygon(QPolygonF{{3, 8}, {7, 5}, {7, 11}});
            if (kind_ != "referencePrevious")
                p->drawPolygon(QPolygonF{{13, 8}, {9, 5}, {9, 11}});
        } else if (kind_ == "back" || kind_ == "forward") {
            if (kind_ == "forward") {
                p->translate(16, 0);
                p->scale(-1, 1);
            }
            p->setBrush(cyan);
            p->setPen(QPen(QColor("#147797"), 1));
            p->drawPolygon(QPolygonF{{1, 8}, {7, 2}, {7, 5}, {14, 5}, {14, 11}, {7, 11}, {7, 14}});
        } else if (kind_ == "Folder" || kind_ == "openFolder") {
            p->setPen(QPen(QColor("#97842f"), 1));
            p->setBrush(yellow);
            p->drawPolygon(QPolygonF{{1, 3}, {6, 3}, {8, 5}, {14, 5}, {14, 13}, {1, 13}});
            p->setBrush(QColor("#fff2a0"));
            p->drawPolygon(QPolygonF{{2, 7}, {15, 7}, {13, 13}, {1, 13}});
        } else if (kind_ == "undo" || kind_ == "redo") {
            if (kind_ == "redo") {
                p->translate(16, 0);
                p->scale(-1, 1);
            }
            p->setPen(QPen(yellow, 2));
            p->drawArc(QRectF(4, 5, 10, 9), -30 * 16, 210 * 16);
            p->setBrush(yellow);
            p->drawPolygon(QPolygonF{{1, 6}, {6, 2}, {6, 10}});
        } else if (kind_ == "cut") {
            p->setPen(QPen(cyan, 1.5));
            p->drawEllipse(QRectF(1, 9, 5, 5));
            p->drawEllipse(QRectF(9, 9, 5, 5));
            p->drawLine(4, 11, 12, 1);
            p->drawLine(12, 11, 4, 1);
        } else if (kind_ == "copy" || kind_ == "paste") {
            p->setBrush(kind_ == "paste" ? yellow : cyan);
            p->drawRect(QRectF(2, 1, 9, 11));
            p->setBrush(white);
            p->drawRect(QRectF(5, 4, 9, 11));
            p->setPen(QColor("#555555"));
            for (int y : {7, 10, 12})
                p->drawLine(7, y, 12, y);
        } else if (kind_ == "find") {
            p->setPen(QPen(cyan, 2));
            p->drawEllipse(QRectF(2, 1, 8, 8));
            p->drawLine(9, 9, 14, 14);
        } else if (kind_ == "settings") {
            p->translate(8, 8);
            p->setPen(QPen(cyan, 2));
            for (int i = 0; i < 8; ++i) {
                p->drawLine(0, -4, 0, -7);
                p->rotate(45);
            }
            p->setBrush(cyan);
            p->drawEllipse(QRectF(-4, -4, 8, 8));
            p->setBrush(QColor("#3c3c3c"));
            p->setPen(Qt::NoPen);
            p->drawEllipse(QRectF(-2, -2, 4, 4));
        } else if (kind_ == "save") {
            p->setBrush(cyan);
            p->drawRect(QRectF(2, 2, 12, 12));
            p->setBrush(QColor("#333333"));
            p->drawRect(QRectF(5, 2, 6, 4));
            p->setBrush(white);
            p->drawRect(QRectF(4, 9, 8, 5));
        } else if (kind_ == "refresh") {
            p->setPen(QPen(cyan, 2));
            p->drawArc(QRectF(3, 3, 10, 10), 40 * 16, 290 * 16);
            p->setBrush(cyan);
            p->drawPolygon(QPolygonF{{10, 1}, {14, 2}, {13, 6}});
        } else if (kind_ == "lock") {
            p->setPen(QPen(cyan, 2));
            p->drawArc(QRectF(state == QIcon::On ? 4 : 7, 1, 7, 9), 0, 180 * 16);
            p->setBrush(cyan);
            p->drawRect(QRectF(3, 7, 10, 7));
            p->setPen(QPen(QColor("#333333"), 2));
            p->drawLine(8, 10, 8, 12);
        } else if (kind_ == "Variable") {
            p->setBrush(QColor("#385aef"));
            p->setPen(QPen(QColor("#203baf"), 1));
            p->drawEllipse(QRectF(1, 3, 14, 10));
            p->setPen(white);
            p->drawLine(4, 6, 8, 6);
        } else if (kind_ == "Field") {
            p->setBrush(green);
            p->setPen(QPen(QColor("#248f30"), 1));
            p->drawPolygon(QPolygonF{{8, 2}, {13, 8}, {8, 14}, {3, 8}});
        } else if (kind_ == "Function" || kind_ == "Prototype") {
            p->setBrush(cyan);
            p->drawRect(QRectF(2, 3, 11, 10));
            p->setPen(QPen(QColor("#155061"), 1));
            p->drawLine(4, 5, 11, 5);
            p->setPen(white);
            p->drawLine(5, 8, 10, 8);
            if (kind_ == "Function")
                p->drawLine(5, 10, 8, 10);
        } else if (kind_ == "file" || kind_ == "Include" || kind_ == "open") {
            p->setBrush(white);
            p->setPen(QPen(QColor("#a0a0a0"), 1));
            p->drawPolygon(QPolygonF{{3, 1}, {10, 1}, {13, 4}, {13, 14}, {3, 14}});
            p->setPen(QPen(cyan, 1));
            for (int y : {6, 9, 12})
                p->drawLine(5, y, 11, y);
        } else {
            const QString glyph = kind_ == "Type"                                 ? "t"
                                  : kind_ == "Struct"                             ? "S"
                                  : kind_ == "Enum"                               ? "E"
                                  : kind_ == "EnumConstant"                       ? "Em"
                                  : kind_ == "Constant" || kind_ == "Conditional" ? "#"
                                  : kind_ == "Macro"                              ? "M"
                                  : kind_ == "Namespace"                          ? "{}"
                                  : kind_ == "sortName"                           ? "AZ"
                                  : kind_ == "sortLine"                           ? "1"
                                  : kind_ == "sortKind"                           ? "T"
                                  : kind_ == "expand"                             ? "+"
                                  : kind_ == "collapse"                           ? "-"
                                  : kind_ == "find"                               ? "?"
                                  : kind_ == "undo"                               ? "U"
                                  : kind_ == "redo"                               ? "R"
                                  : kind_ == "cut"                                ? "X"
                                  : kind_ == "copy"                               ? "C"
                                  : kind_ == "paste"                              ? "P"
                                  : kind_ == "zoomIn"                             ? "A+"
                                  : kind_ == "zoomOut"                            ? "A-"
                                                                                  : "*";
            p->setPen(kind_ == "Enum" || kind_ == "EnumConstant" || kind_ == "Constant"
                          ? QColor("#ee6868")
                      : kind_ == "Type" || kind_ == "Struct" || kind_ == "Conditional" ? yellow
                      : kind_ == "Macro"                                               ? green
                                                                                       : cyan);
            QFont font("Sans Serif");
            font.setPixelSize(glyph.size() > 1 ? 10 : 14);
            font.setBold(true);
            p->setFont(font);
            p->drawText(QRectF(0, 0, 16, 16), Qt::AlignCenter, glyph);
        }
        p->restore();
    }

private:
    QString kind_;
};
}

namespace UiAppearance {
QFont uiFont(const AppSettings &settings) {
    QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
#ifdef Q_OS_WIN
    font.setFamily("Segoe UI");
#endif
    if (!settings.ui.fontFamily.isEmpty())
        font.setFamily(settings.ui.fontFamily);
    font.setPixelSize(settings.ui.fontSizePx);
    return font;
}

int bandHeight(const AppSettings &settings) {
    return qMax(settings.ui.bandHeight,
                qMax(QFontMetrics(uiFont(settings)).height() + 6, settings.ui.iconSize + 4));
}

QString sourceFont() {
    const auto families = QFontDatabase::families();
    for (const auto &family : {"Consolas", "DejaVu Sans Mono", "Menlo", "Liberation Mono"})
        if (families.contains(family, Qt::CaseInsensitive))
            return family;
    return QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
}

double nativeSourcePointSize(double referencePoints, const QPaintDevice &device) {
    return referencePoints * SourceReferenceDpi / device.logicalDpiY();
}

QIcon applicationIcon() {
    QIcon result;
    for (const int size : {16, 24, 32, 48, 64, 128, 256, 512})
        result.addFile(QString(":/icons/codecairn-%1.png").arg(size), QSize(size, size));
    return result;
}

QIcon icon(const QString &kind) {
    static const QHash<QString, QString> categories{{"Structures", "Struct"},
                                                    {"Type Definitions", "Type"},
                                                    {"Enumerations", "Enum"},
                                                    {"Constants", "Constant"},
                                                    {"Enum Constants", "EnumConstant"},
                                                    {"Variables", "Variable"},
                                                    {"Functions", "Function"},
                                                    {"Macros", "Macro"},
                                                    {"Function Prototypes", "Prototype"},
                                                    {"Data Members", "Field"},
                                                    {"Namespaces", "Namespace"}};
    if (const auto found = categories.constFind(kind); found != categories.cend())
        return QIcon(new GlyphEngine(found.value()));
    return QIcon(new GlyphEngine(kind));
}

QImage referenceLinkImage(int size, bool hovered) {
    QImage image(size, size, QImage::Format_RGBA8888);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    icon("referenceLink").paint(&painter, image.rect());
    if (hovered) {
        painter.setPen(QPen(QColor("#ed3030"), 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(image.rect().adjusted(0, 0, -1, -1));
    }
    return image;
}

void configureTree(QTreeWidget *tree, const QStringList &headers, const AppSettings &settings) {
    auto *style = new TreeBranches;
    style->setObjectName("codecairnBranches");
    style->setParent(tree);
    tree->setStyle(style);
    tree->setHeaderLabels(headers);
    tree->setAlternatingRowColors(false);
    tree->setUniformRowHeights(true);
    tree->setItemDelegate(new CompactRows(tree));
    tree->setSelectionMode(QAbstractItemView::SingleSelection);
    tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree->header()->setStretchLastSection(true);
    tree->setTextElideMode(Qt::ElideRight);
    applyTreeAppearance(tree, settings);
}

void applyTreeAppearance(QTreeWidget *tree, const AppSettings &settings) {
    auto *branches =
        dynamic_cast<TreeBranches *>(tree->findChild<QProxyStyle *>("codecairnBranches"));
    auto *rows = dynamic_cast<CompactRows *>(tree->itemDelegate());
    if (!branches || !rows) {
        qWarning("Cannot apply tree appearance: missing reference-style components.");
        return;
    }
    branches->setColors(settings);
    rows->setMinimumHeight(qMax(settings.ui.rowHeight, settings.ui.symbolIconSize + 2));
    tree->setFont(uiFont(settings));
    tree->setIndentation(settings.ui.treeIndent);
    tree->setIconSize(QSize(settings.ui.symbolIconSize, settings.ui.symbolIconSize));
    tree->header()->setFixedHeight(bandHeight(settings));
    tree->doItemsLayout();
    tree->viewport()->update();
}

QString styleSheet(const AppSettings &settings) {
    QString result = R"(
        QMainWindow, QDialog, QWidget { background: @ui.background@; color: @ui.foreground@; }
        QMenuBar { background: @menu.background@; color: @menu.foreground@; }
        QMenuBar::item { padding: 1px 6px; }
        QMenuBar::item:selected { background: @menu.selection@; }
        QMenu { background: @menu.popupBackground@; color: @menu.foreground@; border: 1px solid @menu.border@; }
        QMenu::item:selected { background: @ui.selectionBackground@; color: @ui.selectionForeground@; }
        QMenu::item:disabled { color: @menu.disabled@; }
        QToolBar { background: @ui.background@; spacing: 1px; padding: 0px; border: none; }
        QToolButton { padding: 1px; border: 1px solid transparent; background: transparent; }
        QToolButton:hover { border-color: @ui.hoverBorder@; background: @ui.hoverBackground@; }
        QToolButton:checked { border-color: @ui.checkedBorder@; background: @ui.inputBackground@; }
        QToolBar::separator { background: @ui.strongBorder@; width: 1px; margin: 3px; }
        QLineEdit, QComboBox, QDoubleSpinBox, QSpinBox { background: @ui.inputBackground@; color: @ui.brightForeground@;
            border: 1px solid @ui.strongBorder@; border-radius: 0; padding: 1px 2px;
            selection-background-color: @ui.selectionBackground@; }
        QLineEdit:focus { border-color: @ui.activeBorder@; }
        QTreeWidget { background: @ui.panelBackground@; border: 1px solid @ui.border@; outline: none; }
        QTreeWidget::item { padding: 0; margin: 0; border: none; }
        QTreeWidget::item:selected { background: @ui.selectionBackground@; color: @ui.selectionForeground@; }
        QTreeWidget::branch:selected { background: @ui.selectionBackground@; }
        QHeaderView::section { background: @ui.headerBackground@; color: @ui.foreground@;
            border: none; border-right: 1px solid @ui.headerBorder@; padding: 1px 3px; }
        QTabWidget::pane { border: none; }
        QTabBar::tab { background: @ui.tabBackground@; color: @ui.tabForeground@; min-height: @bandContent@px;
            border: 1px solid @ui.tabBorder@; padding: 0 5px; margin: 0; }
        QTabBar::tab:selected { background: @ui.selectedTab@; color: @ui.selectionForeground@; border-color: @ui.activeBorder@; }
        QTabBar::close-button { width: 10px; height: 10px; }
        QSplitter::handle { background: @ui.scrollbarTrack@; }
        QSplitter::handle:hover { background: @ui.strongBorder@; }
        QLabel#outlineTitle, QLabel#contextTitle { font-weight: bold; color: @ui.brightForeground@;
            padding-left: 3px; background: @ui.titleBackground@; }
        QStatusBar { background: @ui.tabBackground@; color: @ui.tabForeground@; }
        QStatusBar::item { border: none; }
        QToolTip { color: @tooltip.foreground@; background: @tooltip.background@; border: 1px solid @ui.strongBorder@; }
        QScrollBar:vertical { background: @ui.scrollbarTrack@; width: @scrollbarWidth@px; margin: 0; }
        QScrollBar:horizontal { background: @ui.scrollbarTrack@; height: @scrollbarWidth@px; margin: 0; }
        QScrollBar::handle { background: @ui.scrollbarHandle@; border: 1px solid @ui.scrollbarBorder@;
            border-radius: 0; min-width: 20px; min-height: 20px; }
        QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
        QScrollBar::add-page, QScrollBar::sub-page { background: @ui.scrollbarTrack@; }
    )";
    for (auto it = settings.colors.begin(); it != settings.colors.end(); ++it)
        result.replace('@' + it.key() + '@', it.value().name());
    result.replace("@bandContent@", QString::number(bandHeight(settings) - 4));
    result.replace("@scrollbarWidth@", QString::number(settings.ui.scrollbarWidth));
    return result;
}
}

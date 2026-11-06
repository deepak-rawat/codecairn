#include "MainWindow.h"
#include "NavigationTree.h"
#include "ReferenceResultsView.h"
#include "SymbolBrowser.h"
#include "UiAppearance.h"
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QHeaderView>
#include <QMenu>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <algorithm>
#include <utility>

namespace {
QByteArray hash(const QByteArray &bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

QString queryFor(const ReferenceGroup &group) {
    return group.query.isEmpty() ? group.title.section(" - ", 0, 0) : group.query;
}

QString resultText(const ReferenceHit &hit, const SearchOutputOptions &output,
                   const QString &root) {
    QString result;
    if (output.container && !hit.container.isEmpty())
        result = hit.container;
    if (output.file) {
        QString file = hit.location.path;
        if (output.relativePaths && !root.isEmpty()) {
            const QFileInfo relative(QDir(root).relativeFilePath(file));
            file = relative.fileName();
            if (relative.path() != ".")
                file += " (" + QDir::toNativeSeparators(relative.path()) + ')';
        }
        result += (result.isEmpty() ? "" : " in ") + file;
    }
    if (output.line)
        result += " line " + QString::number(hit.line + 1);
    if (output.text)
        result += (result.isEmpty() ? "" : " : ") + hit.text;
    return result;
}

class ReferenceRows final : public QStyledItemDelegate {
public:
    explicit ReferenceRows(QObject *parent) : QStyledItemDelegate(parent) {}
    void setAppearance(const AppSettings &settings) {
        iconSize_ = settings.ui.iconSize;
        background_ = settings.color("source.background");
        foreground_ = settings.color("source.foreground");
        gutter_ = settings.color("ui.background");
        heading_ = settings.syntax[static_cast<size_t>(TextStyle::Field)].foreground;
    }
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const QFontMetrics metrics(option.font);
        return {metrics.horizontalAdvance(index.data(ReferenceItemRole::DisplayText).toString()) +
                    iconSize_ + 12,
                qMax(metrics.height(), iconSize_)};
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        painter->save();
        painter->fillRect(option.rect, background_);
        painter->fillRect(
            QRect(option.rect.left(), option.rect.top(), iconSize_ + 6, option.rect.height()),
            gutter_);
        QStyleOptionViewItem item(option);
        initStyleOption(&item, index);
        item.text = index.data(ReferenceItemRole::DisplayText).toString();
        item.features |= QStyleOptionViewItem::HasDisplay;
        item.decorationSize = QSize(iconSize_, iconSize_);
        if (!item.icon.isNull()) {
            const auto rect = option.widget->style()->subElementRect(
                QStyle::SE_ItemViewItemDecoration, &item, option.widget);
            item.icon.paint(painter, rect);
            const auto *view = qobject_cast<const ReferenceResultsView *>(option.widget);
            if (view && view->iconHovered(index)) {
                painter->setPen(QPen(QColor("#ed3030"), 1));
                painter->setBrush(Qt::NoBrush);
                painter->drawRect(rect.adjusted(0, 0, -1, -1));
            }
        }
        const QPointF origin(option.rect.left() + iconSize_ + 10,
                             option.rect.top() +
                                 (option.rect.height() - option.fontMetrics.height()) / 2);
        painter->setFont(option.font);
        if (const auto *view = qobject_cast<const ReferenceResultsView *>(option.widget))
            view->paintText(painter, origin, index, foreground_, heading_);
        painter->restore();
    }

private:
    int iconSize_ = 16;
    QColor background_, foreground_, gutter_, heading_;
};

void outputDialog(QWidget *parent, const std::shared_ptr<SearchOutputOptions> &options) {
    QDialog dialog(parent);
    dialog.setWindowTitle("Include in Results");
    auto *layout = new QVBoxLayout(&dialog);
    QVector<QCheckBox *> boxes;
    const QStringList labels{"Container function or class",
                             "File names",
                             "Line numbers",
                             "Source text",
                             "Create source links",
                             "Project-relative file names"};
    const auto &o = *options;
    const bool values[]{o.container, o.file, o.line, o.text, o.links, o.relativePaths};
    for (int i = 0; i < labels.size(); ++i) {
        auto *box = new QCheckBox(labels[i], &dialog);
        box->setObjectName("referenceOutput" + QString::number(i));
        box->setChecked(values[i]);
        boxes.append(box);
        layout->addWidget(box);
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted)
        *options = {boxes[0]->isChecked(), boxes[1]->isChecked(), boxes[2]->isChecked(),
                    boxes[3]->isChecked(), boxes[4]->isChecked(), boxes[5]->isChecked()};
}
}

void MainWindow::cancelReferenceSearch() {
    const bool wasBusy =
        referenceSearchBusy_ || (replaceReferenceResults_ && !pendingReferenceGroups_.isEmpty());
    referenceSearchBusy_ = false;
    ++referenceToken_;
    touchReferenceResults_ = false;
    referenceTouchHits_.clear();
    replaceReferenceResults_ = false;
    pendingReferenceGroups_.clear();
    if (referenceService_)
        referenceService_->cancelLocations("references");
    if (referenceCancelled_)
        referenceCancelled_->store(true);
    if (cancelReferenceAction_)
        cancelReferenceAction_->setEnabled(false);
    if (moreReferencesAction_)
        moreReferencesAction_->setEnabled(false);
    if (wasBusy && referencesStatus_)
        referencesStatus_->setText("Search cancelled. Previous results remain available.");
}

void MainWindow::ensureReferenceResults() {
    if (referenceResultsPage_)
        return;
    referenceResultsPage_ = new QWidget(documentsTabs_);
    referenceResultsPage_->setObjectName("searchResultsPage");
    auto *layout = new QVBoxLayout(referenceResultsPage_);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *bar = new QToolBar(referenceResultsPage_);
    bar->setIconSize(QSize(configuration_.ui.iconSize, configuration_.ui.iconSize));
    cancelReferenceAction_ =
        bar->addAction("Cancel Search", this, &MainWindow::cancelReferenceSearch);
    cancelReferenceAction_->setObjectName("cancelReferenceSearch");
    cancelReferenceAction_->setEnabled(referenceSearchBusy_);
    moreReferencesAction_ = bar->addAction("Load More", this, &MainWindow::continueReferenceSearch);
    moreReferencesAction_->setObjectName("moreReferenceResults");
    moreReferencesAction_->setEnabled(false);
    auto *remove = bar->addAction("Remove Selected", this, &MainWindow::removeReferenceResults);
    remove->setObjectName("removeReferenceResults");
    auto *exportAction = bar->addAction("Export...", this, &MainWindow::exportReferenceResults);
    exportAction->setObjectName("exportReferenceResults");
    auto *touch =
        bar->addAction("Touch Files...", this, [this] { touchReferenceHits(referenceHits_); });
    touch->setObjectName("touchReferenceFiles");
    auto *clear = bar->addAction("Clear Results", this, [this] {
        cancelReferenceSearch();
        referenceGroups_.clear();
        rebuildReferenceResults();
        referencesStatus_->setText("No search results.");
    });
    clear->setObjectName("clearReferenceResults");
    for (const auto &name :
         {"firstReference", "previousReference", "nextReference", "jumpReference"})
        bar->addAction(actions_.value(name));
    layout->addWidget(bar);
    bar->hide();
    referencesList_ = new ReferenceResultsView(referenceResultsPage_);
    connect(qobject_cast<ReferenceResultsView *>(referencesList_),
            &ReferenceResultsView::textSelectionChanged, this, &MainWindow::updateActions);
    referencesList_->setObjectName("referenceResults");
    referencesList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    UiAppearance::configureTree(referencesList_, {"Function / class", "File", "Line", "Source"},
                                configuration_);
    auto *oldDelegate = referencesList_->itemDelegate();
    referencesList_->setItemDelegate(new ReferenceRows(referencesList_));
    delete oldDelegate;
    applyReferenceAppearance();
    referencesList_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(referencesList_, &QWidget::customContextMenuRequested, this,
            [this, bar](const QPoint &point) {
                auto *menu = new QMenu(referencesList_);
                menu->setObjectName("referenceResultsMenu");
                connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
                auto *view = qobject_cast<ReferenceResultsView *>(referencesList_);
                auto *copy = menu->addAction("Copy", view, &ReferenceResultsView::copySelection);
                copy->setEnabled(view->textCursor().hasSelection());
                menu->addSeparator();
                menu->addActions(bar->actions());
                menu->popup(referencesList_->viewport()->mapToGlobal(point));
            });
    layout->addWidget(referencesList_, 1);
    referencesStatus_ = new QLabel(referenceResultsPage_);
    referencesStatus_->setObjectName("referenceResultsStatus");
    referencesStatus_->setWordWrap(true);
    layout->addWidget(referencesStatus_);
    connect(referencesList_, &QTreeWidget::currentItemChanged, this,
            [this] { activateReferenceResult(false); });
    connect(referencesList_, &QTreeWidget::itemClicked, this,
            [this] { activateReferenceResult(false); });
    connect(referencesList_, &QTreeWidget::itemActivated, this,
            [this] { activateReferenceResult(true); });
    connect(static_cast<NavigationTree *>(referencesList_), &NavigationTree::iconClicked, this,
            [this](QTreeWidgetItem *item, int) {
                if (!item->data(0, Qt::UserRole).isValid())
                    return;
                referencesList_->setCurrentItem(item);
                activateReferenceResult(true);
            });
    const int index = documentsTabs_->addTab(referenceResultsPage_, "Search Results");
    documentBar_->insertTab(index, "Search Results");
    documentsTabs_->setTabToolTip(index, "Search history. Enter or Ctrl+L follows a source link.");
    documentBar_->setTabToolTip(index, documentsTabs_->tabToolTip(index));
    rebuildReferenceResults();
    if (selectedReferenceIndex_ >= 0 && selectedReferenceIndex_ < referenceRows_.size())
        referencesList_->setCurrentItem(referenceRows_[selectedReferenceIndex_]);
}

void MainWindow::touchReferenceHits(const QVector<ReferenceHit> &hits) {
    if (hits.isEmpty())
        return;
    const auto snapshot = hits;
    const QPointer<QWidget> page = referenceResultsPage_;
    const auto token = referenceToken_;
    QSet<QString> paths;
    for (const auto &hit : snapshot) {
        if (hit.stale) {
            reportError("Results contain stale targets. Rerun the lookup before touching files.");
            return;
        }
        if (auto *doc = findDocument(hit.location.path); doc && doc->editor->isModified()) {
            reportError("Matching files have unsaved edits. Save and rerun before touching files.");
            return;
        }
        paths.insert(hit.location.path);
    }
    if (QMessageBox::warning(
            this, "Touch matching files",
            QString("Set the modification timestamp of %1 matching files to now?\n"
                    "This can trigger recompilation. File contents will not be changed.")
                .arg(paths.size()),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    if (!page || page != referenceResultsPage_ || token != referenceToken_)
        return;
    const auto errors = touchReferenceFiles(snapshot);
    if (!errors.isEmpty())
        reportError("Some files could not be touched:\n" + errors.join('\n'));
    referencesStatus_->setText(errors.isEmpty() ? "Matching file timestamps updated."
                                                : "File touch completed with errors; see status.");
}

void MainWindow::applyReferenceAppearance() {
    if (!referencesList_)
        return;
    auto *rows = dynamic_cast<ReferenceRows *>(referencesList_->itemDelegate());
    if (!rows) {
        reportError("Search Results renderer is unavailable.");
        return;
    }
    rows->setAppearance(configuration_);
    auto font = QFont(configuration_.editor.typography.family.isEmpty()
                          ? UiAppearance::sourceFont()
                          : configuration_.editor.typography.family);
    font.setPointSizeF(configuration_.editor.typography.pointSize);
    referencesList_->setFont(font);
    referencesList_->setIconSize(QSize(configuration_.ui.iconSize, configuration_.ui.iconSize));
    referencesList_->setHeaderHidden(true);
    referencesList_->setRootIsDecorated(false);
    referencesList_->setItemsExpandable(false);
    referencesList_->setExpandsOnDoubleClick(false);
    referencesList_->setIndentation(0);
    referencesList_->setTextElideMode(Qt::ElideNone);
    referencesList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    for (int column = 1; column < 4; ++column)
        referencesList_->setColumnHidden(column, true);
    referencesList_->header()->setStretchLastSection(false);
    referencesList_->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    referencesList_->setStyleSheet(
        "QTreeWidget { background:" + configuration_.color("source.background").name() +
        "; border:none; }");
    referencesList_->doItemsLayout();
    qobject_cast<ReferenceResultsView *>(referencesList_)->rebuildTextDocument();
    referencesList_->viewport()->update();
}

void MainWindow::rebuildReferenceResults() {
    const QSignalBlocker blocker(referencesList_);
    if (referencesList_)
        referencesList_->clear();
    referenceHits_.clear();
    referenceRows_.clear();
    for (const auto &group : referenceGroups_) {
        const auto query = queryFor(group);
        auto *heading =
            referencesList_ ? new QTreeWidgetItem(referencesList_, {group.title}) : nullptr;
        if (heading) {
            QSet<QString> files;
            for (const auto &hit : group.hits)
                files.insert(workspacePath(hit.location.path));
            heading->setData(0, ReferenceItemRole::Heading, true);
            heading->setData(0, ReferenceItemRole::Query, query);
            heading->setData(0, ReferenceItemRole::DisplayText,
                             QString("---- %1 Matches (%2 in %3 files) ----")
                                 .arg(query)
                                 .arg(group.hits.size())
                                 .arg(files.size()));
        }
        const auto &o = group.output;
        for (const auto &hit : group.hits) {
            referenceHits_.append(hit);
            if (!heading)
                continue;
            const auto path = o.relativePaths && !projectPath_.isEmpty()
                                  ? QDir(projectPath_).relativeFilePath(hit.location.path)
                                  : hit.location.path;
            auto *row = new QTreeWidgetItem(
                heading, {resultText(hit, o, projectPath_), o.file ? path : QString{},
                          o.line ? QString::number(hit.line + 1) : QString{},
                          o.text ? hit.text : QString{}});
            row->setData(0, Qt::UserRole, static_cast<int>(referenceHits_.size()) - 1);
            row->setData(0, ReferenceItemRole::Query, query);
            row->setData(0, ReferenceItemRole::DisplayText, resultText(hit, o, projectPath_));
            if (hit.linked)
                row->setIcon(0, UiAppearance::icon(hit.stale ? "warning" : "referenceLink"));
            const auto tip = hit.location.path + ':' + QString::number(hit.line + 1) +
                             (hit.stale    ? "\nStale target; rerun the search."
                              : hit.linked ? "\nClick the link icon to open; click text to preview."
                                           : "\nSource links were disabled for this search.");
            for (int column = 0; column < 4; ++column)
                row->setToolTip(column, tip);
            referenceRows_.append(row);
        }
        if (heading)
            heading->setExpanded(true);
    }
    if (selectedReferenceIndex_ >= referenceHits_.size())
        selectedReferenceIndex_ = -1;
    if (auto *view = qobject_cast<ReferenceResultsView *>(referencesList_))
        view->rebuildTextDocument();
    updateReferenceMarkers();
    if (showingReferenceOutline_)
        updateSearchOutline();
    updateActions();
}

void MainWindow::updateSearchOutline() {
    const QSignalBlocker block(outline_);
    outline_->clear();
    outlineDocument_.clear();
    outlineTitle_->setText("Search Results");
    outline_->setContextMenuPolicy(Qt::ActionsContextMenu);
    QSet<QString> queries;
    for (const auto &group : referenceGroups_) {
        const auto query = queryFor(group);
        if (queries.contains(query))
            continue;
        queries.insert(query);
        auto *item = new QTreeWidgetItem(outline_, {query});
        item->setData(0, ReferenceItemRole::Query, query);
        item->setIcon(0, UiAppearance::icon("searchSymbol"));
        item->setToolTip(0, query + "\nDelete removes this symbol's search results.");
        if (query == selectedSearchQuery_)
            outline_->setCurrentItem(item);
    }
    if (!outline_->currentItem() && outline_->topLevelItemCount())
        outline_->setCurrentItem(outline_->topLevelItem(0));
    outlineState_->setText(
        QString("%1 searched symbols - Delete removes selected results").arg(queries.size()));
    filterOutline();
    updateActions();
}

void MainWindow::selectSearchQuery() {
    const auto *item = outline_->currentItem();
    if (!showingReferenceOutline_ || !item || !referencesList_)
        return;
    const auto query = item->data(0, ReferenceItemRole::Query).toString();
    selectedSearchQuery_ = query;
    for (int g = 0; g < referenceGroups_.size(); ++g) {
        if (queryFor(referenceGroups_[g]) != query)
            continue;
        auto *heading = referencesList_->topLevelItem(g);
        referencesList_->scrollToItem(heading, QAbstractItemView::PositionAtTop);
        if (heading->childCount()) {
            referencesList_->setCurrentItem(heading->child(0));
            activateReferenceResult(false);
        } else if (!pinContextAction_->isChecked()) {
            contextTimer_->stop();
            contextFollowsEditor_ = false;
            contextPath_.clear();
            showContext("No matches for " + query, {}, {}, 0, 0);
        }
        return;
    }
}

void MainWindow::removeSearchQuery() {
    const auto *item = outline_->currentItem();
    if (!showingReferenceOutline_ || !item)
        return;
    const auto query = item->data(0, ReferenceItemRole::Query).toString();
    if ((referenceSearchBusy_ || replaceReferenceResults_) && referenceIdentifier_ == query)
        cancelReferenceSearch();
    referenceGroups_.removeIf([&](const auto &group) { return queryFor(group) == query; });
    selectedReferenceIndex_ = -1;
    selectedSearchQuery_.clear();
    rebuildReferenceResults();
    clearPanelFilter(outlineFilter_);
    if (outline_->currentItem())
        selectSearchQuery();
    else if (!pinContextAction_->isChecked()) {
        contextTimer_->stop();
        contextFollowsEditor_ = false;
        contextPath_.clear();
        showContext("No search results", {}, {}, 0, 0);
    }
    saveProjectSession();
}

void MainWindow::showReferenceResults(int index) {
    if (!referenceResultsPage_) {
        ensureReferenceResults();
        referencesStatus_->setText(
            "Saved search history. Source links are validated before navigation.");
    }
    if (index < 0)
        index = selectedReferenceIndex_;
    documentsTabs_->setCurrentWidget(referenceResultsPage_);
    if (index >= 0 && index < referenceRows_.size()) {
        referencesList_->setCurrentItem(referenceRows_[index]);
        referencesList_->scrollToItem(referenceRows_[index]);
        activateReferenceResult(false);
    }
    referencesList_->setFocus();
}

void MainWindow::updateReferenceMarkers() {
    for (const auto &doc : documents_) {
        const auto identity = workspacePath(doc->path);
        QByteArray sourceHash;
        QSet<int> lines;
        for (const auto &hit : referenceHits_) {
            if (!hit.linked || hit.stale || workspacePath(hit.location.path) != identity)
                continue;
            if (sourceHash.isEmpty())
                sourceHash = hash(snapshot(doc.get()));
            if (sourceHash == hit.location.expectedHash)
                lines.insert(hit.line);
        }
        doc->editor->setReferenceLines(lines.values());
    }
}

void MainWindow::returnToReferenceResults(const QString &path, int line) {
    const auto identity = workspacePath(path);
    auto *doc = findDocument(path);
    if (!doc)
        return;
    const auto sourceHash = hash(snapshot(doc));
    const auto matches = [&](int i) {
        const auto &hit = referenceHits_[i];
        return hit.linked && !hit.stale && hit.line == line &&
               workspacePath(hit.location.path) == identity &&
               hit.location.expectedHash == sourceHash;
    };
    if (selectedReferenceIndex_ >= 0 && selectedReferenceIndex_ < referenceHits_.size() &&
        matches(selectedReferenceIndex_)) {
        showReferenceResults(selectedReferenceIndex_);
        return;
    }
    for (int i = 0; i < referenceHits_.size(); ++i)
        if (matches(i)) {
            showReferenceResults(i);
            return;
        }
    updateReferenceMarkers();
    reportError("There is no valid reference source link on this line. Rerun the search.");
}

void MainWindow::activateReferenceResult(bool open) {
    auto *item = referencesList_ ? referencesList_->currentItem() : nullptr;
    if (item && !item->data(0, Qt::UserRole).isValid() && item->childCount()) {
        item = item->child(0);
        if (open)
            referencesList_->setCurrentItem(item);
    }
    if (!item || !item->data(0, Qt::UserRole).isValid())
        return;
    const int index = item->data(0, Qt::UserRole).toInt();
    if (index < 0 || index >= referenceHits_.size())
        return;
    selectedReferenceIndex_ = index;
    selectedSearchQuery_ = item->data(0, ReferenceItemRole::Query).toString();
    if (showingReferenceOutline_) {
        const QSignalBlocker block(outline_);
        for (int i = 0; i < outline_->topLevelItemCount(); ++i)
            if (outline_->topLevelItem(i)->data(0, ReferenceItemRole::Query).toString() ==
                selectedSearchQuery_)
                outline_->setCurrentItem(outline_->topLevelItem(i));
    }
    const auto &hit = referenceHits_[index];
    if (!hit.linked || hit.stale) {
        if (open)
            reportError(hit.stale ? "Source link is stale. Rerun the search."
                                  : "Source links were disabled for this search.");
        return;
    }
    if (open) {
        auto location = hit.location;
        location.wholeLine = true;
        navigateTo(location, true, true);
    } else {
        SourceSymbol symbol;
        symbol.name =
            hit.container.isEmpty() ? QFileInfo(hit.location.path).fileName() : hit.container;
        symbol.start = symbol.nameStart = hit.location.start;
        symbol.end = symbol.nameEnd = hit.location.end;
        previewTarget({hit.location.path, hit.location.expectedHash, symbol, false}, "Reference");
    }
}

void MainWindow::stepReferenceResult(int direction, bool first) {
    if (referenceHits_.isEmpty())
        return;
    if (!referenceResultsPage_) {
        ensureReferenceResults();
    }
    int index = selectedReferenceIndex_;
    if (const auto *item = referencesList_->currentItem();
        item && item->data(0, Qt::UserRole).isValid())
        index = item->data(0, Qt::UserRole).toInt();
    index = first ? 0 : qBound(0, index + direction, static_cast<int>(referenceRows_.size()) - 1);
    while (index >= 0 && index < referenceHits_.size()) {
        if (referenceHits_[index].linked && !referenceHits_[index].stale) {
            referencesList_->setCurrentItem(referenceRows_[index]);
            referencesList_->scrollToItem(referenceRows_[index]);
            activateReferenceResult(true);
            return;
        }
        index += direction;
    }
}

void MainWindow::jumpReferenceLink() {
    if (referenceResultsPage_ && documentsTabs_->currentWidget() == referenceResultsPage_) {
        activateReferenceResult(true);
        return;
    }
    auto *doc = currentDocument();
    if (!doc)
        return;
    const int position = static_cast<int>(doc->editor->send(SCI_GETSELECTIONSTART));
    const int line = static_cast<int>(doc->editor->send(SCI_LINEFROMPOSITION, position));
    returnToReferenceResults(doc->path, line);
}

void MainWindow::lookupSymbolReferences() {
    const auto *focus = QApplication::focusWidget();
    if (focus && (focus == symbolList_ || symbolList_->isAncestorOf(focus) ||
                  focus == categoryTree_ || categoryTree_->isAncestorOf(focus))) {
        const bool category = focus == categoryTree_ || categoryTree_->isAncestorOf(focus);
        const auto *item = category ? categoryTree_->currentItem() : symbolList_->currentItem();
        const auto &symbols = category ? symbolPage_.categorySymbols : symbolPage_.symbols;
        if (item && item->data(0, SymbolItemRole::Index).isValid()) {
            const int index = item->data(0, SymbolItemRole::Index).toInt();
            if (index >= 0 && index < symbols.size()) {
                const auto &entry = symbols[index];
                lookupReferences(nullptr, false, entry.path, entry.symbol.nameStart);
                return;
            }
        }
    }
    if (focus && (focus == outline_ || outline_->isAncestorOf(focus))) {
        auto *doc = currentDocument();
        const auto *item = outline_->currentItem();
        if (doc && item && item->data(0, Qt::UserRole).isValid()) {
            const int index = item->data(0, Qt::UserRole).toInt();
            if (doc->revision != doc->analyzedRevision || index < 0 ||
                index >= doc->analysis.symbols.size()) {
                reportError("Symbols are updating; retry after analysis finishes.");
                return;
            }
            lookupReferences(nullptr, false, doc->path, doc->analysis.symbols[index].nameStart);
            return;
        }
    }
    lookupReferences();
}

void MainWindow::lookupReferences(CodeEditor *editor, bool projectSearch, const QString &symbolPath,
                                  int symbolPosition) {
    if (!editor)
        editor = contextPreviewEditor_->hasFocus() ? contextPreviewEditor_ : activeEditor();
    auto *doc = currentDocument();
    QString path = symbolPath;
    QByteArray bytes;
    int position = symbolPosition;
    QString word;
    if (!path.isEmpty()) {
        auto *live = findDocument(path);
        const auto file =
            live ? FileContents{snapshot(live), {}, live->hasUtf8Bom} : readSourceFile(path);
        if (!file.error.isEmpty()) {
            reportError(file.error);
            return;
        }
        bytes = file.bytes;
    } else if (editor && (doc || editor == contextPreviewEditor_)) {
        path = editor == contextPreviewEditor_ ? contextPath_ : doc->path;
        bytes = editor == contextPreviewEditor_ ? editor->bytes() : snapshot(doc);
        position = editor->lookupPosition();
    }
    if (position >= 0 && position < bytes.size()) {
        const auto identifierByte = [](char c) {
            const auto u = static_cast<unsigned char>(c);
            return u >= 128 || c == '_' || c == '$' || (c >= 'a' && c <= 'z') ||
                   (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        };
        int start = position, end = position;
        while (start > 0 && identifierByte(bytes[start - 1]))
            --start;
        while (end < bytes.size() && identifierByte(bytes[end]))
            ++end;
        position = start;
        word = QString::fromUtf8(bytes.mid(start, end - start));
    }
    if (referencesDialog_)
        referencesDialog_->close();
    auto *dialog = new QDialog(this);
    referencesDialog_ = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(projectSearch ? "searchProjectDialog" : "lookupReferencesDialog");
    dialog->setWindowTitle(projectSearch ? "Search Project" : "Lookup References");
    dialog->resize(670, 540);
    const auto settings = projectSearch ? configuration_.searchProject : configuration_.references;
    const auto &initial = settings.search;
    auto *layout = new QGridLayout(dialog);
    layout->setColumnStretch(0, 1);
    layout->setHorizontalSpacing(12);
    auto *content = new QWidget(dialog);
    auto *mainLayout = new QVBoxLayout(content);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(10);
    auto *history = new QComboBox(dialog);
    history->setObjectName("referenceQueryHistory");
    history->setEditable(true);
    history->setInsertPolicy(QComboBox::NoInsert);
    history->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    history->setMinimumContentsLength(20);
    QStringList previousQueries;
    for (auto it = referenceGroups_.crbegin(); it != referenceGroups_.crend(); ++it) {
        const auto previous = queryFor(*it);
        if (!previousQueries.contains(previous))
            previousQueries.append(previous);
        if (previousQueries.size() == 20)
            break;
    }
    history->addItems(previousQueries);
    history->setEditText(word);
    auto *query = history->lineEdit();
    query->setObjectName("referenceQuery");
    auto *findLabel = new QLabel("&Find:", dialog);
    findLabel->setBuddy(history);
    mainLayout->addWidget(findLabel);
    mainLayout->addWidget(history);
    mainLayout->addSpacing(8);
    auto *mode = new QComboBox(dialog);
    mode->setObjectName("referenceMethod");
    mode->addItem("Simple String", static_cast<int>(SearchMethod::Literal));
    mode->addItem("Regular Expression", static_cast<int>(SearchMethod::RegularExpression));
    mode->addItem("Keyword Expression", static_cast<int>(SearchMethod::KeywordExpression));
    mode->addItem("Look Up Reference", static_cast<int>(SearchMethod::References));
    mode->addItem("Text Occurrences (Whole Identifiers)",
                  static_cast<int>(SearchMethod::Occurrences));
    mode->setCurrentIndex(mode->findData(static_cast<int>(initial.method)));
    auto *scope = new QComboBox(dialog);
    scope->setObjectName("referenceScope");
    scope->addItems({"All Project Files", "Current File", "Selected Project Files"});
    scope->setCurrentIndex(settings.scope);
    auto *scopeLabel = new QLabel("Search &In:", dialog);
    scopeLabel->setBuddy(scope);
    mainLayout->addWidget(scopeLabel);
    mainLayout->addWidget(scope);
    auto *patterns = new QLineEdit(initial.filePatterns, dialog);
    patterns->setObjectName("referenceFilePatterns");
    patterns->setPlaceholderText("All file types; e.g. *.cpp;*.h");
    auto *patternLabel = new QLabel("File &Types:", dialog);
    patternLabel->setBuddy(patterns);
    mainLayout->addWidget(patternLabel);
    mainLayout->addWidget(patterns);
    auto *optionsGroup = new QGroupBox("Options", dialog);
    optionsGroup->setObjectName("referenceOptions");
    auto *optionsLayout = new QGridLayout(optionsGroup);
    optionsLayout->setColumnStretch(0, 11);
    optionsLayout->setColumnStretch(1, 10);
    optionsLayout->setHorizontalSpacing(16);
    auto *leftOptions = new QVBoxLayout;
    auto *rightOptions = new QVBoxLayout;
    auto *declarations = new QCheckBox("Include Declarations", dialog);
    declarations->setObjectName("referenceDeclarations");
    declarations->setChecked(settings.declarations);
    declarations->setToolTip(
        "Include declarations as well as uses when smart reference matching is enabled.");
    auto *skipComments = new QCheckBox("Skip Co&mments", dialog);
    skipComments->setObjectName("referenceSkipComments");
    skipComments->setChecked(initial.comments == CommentSearch::Exclude);
    auto *onlyComments = new QCheckBox("Search &Only Comments", dialog);
    onlyComments->setObjectName("referenceOnlyComments");
    onlyComments->setChecked(initial.comments == CommentSearch::Only);
    connect(skipComments, &QCheckBox::toggled, onlyComments, [onlyComments](bool checked) {
        if (checked)
            onlyComments->setChecked(false);
    });
    connect(onlyComments, &QCheckBox::toggled, skipComments, [skipComments](bool checked) {
        if (checked)
            skipComments->setChecked(false);
    });
    auto *strings = new QCheckBox("Include Strings", dialog);
    strings->setObjectName("referenceStrings");
    strings->setChecked(initial.strings);
    auto *sensitive = new QCheckBox("Case Se&nsitive", dialog);
    sensitive->setObjectName("referenceCaseSensitive");
    sensitive->setChecked(initial.caseSensitive);
    auto *whole = new QCheckBox("&Whole Words Only", dialog);
    whole->setObjectName("referenceWholeWords");
    whole->setChecked(initial.wholeWords);
    auto *smart = new QCheckBox("Smart &Reference Matching", dialog);
    smart->setObjectName("referenceSmartMatching");
    smart->setChecked(initial.smartMatching);
    smart->setToolTip(
        "Find uses of the exact selected symbol through the language server, not unrelated names.");
    auto *inactive = new QCheckBox("Skip Inacti&ve Code", dialog);
    inactive->setObjectName("referenceSkipInactive");
    inactive->setChecked(initial.skipInactive);
    inactive->setToolTip("Use C/C++ compiler configuration to skip inactive code; uncertain "
                         "regions are retained with warnings.");
    auto *touch = new QCheckBox("Touch files and cause recompile", dialog);
    touch->setObjectName("referenceTouchFiles");
    touch->setToolTip("After all result pages are loaded, confirm before updating matching file "
                      "timestamps. Contents are never rewritten.");
    auto *context = new QSpinBox(dialog);
    context->setObjectName("referenceContextLines");
    context->setRange(0, 1000);
    context->setValue(initial.contextLines);
    auto *variations = new QCheckBox("Find Word Variations", dialog);
    variations->setObjectName("referenceVariations");
    variations->setChecked(initial.variations);
    auto *fragments = new QCheckBox("Find Word Fragments", dialog);
    fragments->setObjectName("referenceFragments");
    fragments->setChecked(initial.fragments);
    for (auto *box : {sensitive, whole, inactive, skipComments, onlyComments, smart, touch})
        leftOptions->addWidget(box);
    leftOptions->addStretch();
    auto *methodLabel = new QLabel("Search Metho&d:", dialog);
    methodLabel->setBuddy(mode);
    rightOptions->addWidget(methodLabel);
    rightOptions->addWidget(mode);
    rightOptions->addSpacing(6);
    rightOptions->addWidget(declarations);
    rightOptions->addWidget(strings);
    auto *replaceResults = new QCheckBox("Replace all results", dialog);
    replaceResults->setObjectName("referenceReplaceResults");
    replaceResults->setToolTip(
        "Unchecked appends to existing searches. Checked replaces all searches only after every "
        "result page "
        "completes successfully. Failed or cancelled searches keep the previous results.");
    rightOptions->addWidget(replaceResults);
    auto *contextControls = new QWidget(dialog);
    auto *contextForm = new QFormLayout(contextControls);
    contextForm->setContentsMargins(0, 0, 0, 0);
    contextForm->addRow("Lines of Context:", context);
    contextForm->addRow(variations);
    contextForm->addRow(fragments);
    rightOptions->addWidget(contextControls);
    rightOptions->addStretch();
    optionsLayout->addLayout(leftOptions, 0, 0);
    optionsLayout->addLayout(rightOptions, 0, 1);
    auto output = std::make_shared<SearchOutputOptions>(initial.output);
    auto *include = new QPushButton("Include in Res&ults...", dialog);
    include->setObjectName("referenceOutputOptions");
    include->setAutoDefault(false);
    include->setMinimumSize(158, 28);
    connect(include, &QPushButton::clicked, dialog,
            [dialog, output] { outputDialog(dialog, output); });
    optionsLayout->addWidget(include, 1, 0, Qt::AlignLeft);
    mainLayout->addWidget(optionsGroup, 1);
    auto *status = new QLabel(dialog);
    status->setObjectName("referenceDialogStatus");
    status->setWordWrap(true);
    mainLayout->addWidget(status);
    layout->addWidget(content, 0, 0);
    auto *buttonColumn = new QVBoxLayout;
    auto *search = new QPushButton("&Search", dialog);
    search->setObjectName("runReferenceLookup");
    search->setDefault(true);
    auto *close = new QPushButton("&Close", dialog);
    close->setObjectName("closeReferenceLookup");
    close->setAutoDefault(false);
    for (auto *button : {search, close}) {
        button->setMinimumSize(118, 30);
        buttonColumn->addWidget(button);
        buttonColumn->addSpacing(12);
    }
    buttonColumn->addStretch();
    layout->addLayout(buttonColumn, 0, 1);
    connect(close, &QPushButton::clicked, dialog, &QDialog::reject);
    auto wholePreference = std::make_shared<bool>(initial.wholeWords);
    connect(whole, &QCheckBox::toggled, dialog, [mode, wholePreference](bool checked) {
        const auto method = static_cast<SearchMethod>(mode->currentData().toInt());
        if (method == SearchMethod::Literal || method == SearchMethod::RegularExpression)
            *wholePreference = checked;
    });
    const auto updateMethod = [=] {
        const auto method = static_cast<SearchMethod>(mode->currentData().toInt());
        const bool references = method == SearchMethod::References;
        const bool semantic = references && smart->isChecked();
        const bool keywords = method == SearchMethod::KeywordExpression;
        declarations->setEnabled(semantic);
        smart->setEnabled(references);
        skipComments->setEnabled(!semantic);
        onlyComments->setEnabled(!semantic);
        strings->setEnabled(!semantic);
        sensitive->setEnabled(!semantic);
        const bool forcedWhole = references || method == SearchMethod::Occurrences;
        whole->setEnabled(!forcedWhole && !keywords);
        whole->setChecked(forcedWhole || *wholePreference);
        contextControls->setVisible(keywords);
        context->setEnabled(keywords);
        variations->setEnabled(keywords);
        fragments->setEnabled(keywords);
        status->setText(
            semantic
                ? "Exact symbol references require a language server. Comments, strings, inactive "
                  "branches "
                  "and project coverage depend on that provider; no text fallback is performed."
                : "Text searches are not semantic. Keyword operators: AND, OR, NOT, +, !, -, "
                  "=term, ? \"regex\".");
    };
    connect(mode, &QComboBox::currentIndexChanged, dialog, updateMethod);
    connect(smart, &QCheckBox::toggled, dialog, updateMethod);
    updateMethod();
    connect(search, &QPushButton::clicked, dialog, [=, this] {
        SearchOptions options;
        options.method = static_cast<SearchMethod>(mode->currentData().toInt());
        options.caseSensitive = sensitive->isChecked();
        options.wholeWords = whole->isChecked();
        options.comments = onlyComments->isChecked()   ? CommentSearch::Only
                           : skipComments->isChecked() ? CommentSearch::Exclude
                                                       : CommentSearch::Include;
        options.strings = strings->isChecked();
        options.skipInactive = inactive->isChecked();
        options.smartMatching = smart->isChecked();
        options.contextLines = context->value();
        options.variations = variations->isChecked();
        options.fragments = fragments->isChecked();
        options.filePatterns = patterns->text().trimmed();
        options.output = *output;
        const auto text = query->text();
        const auto validation = validateReferenceSearch(text, options);
        if (!validation.isEmpty()) {
            status->setText(validation);
            return;
        }
        const bool semantic = options.method == SearchMethod::References && options.smartMatching;
        if (semantic && (path.isEmpty() || word.isEmpty() || text != word)) {
            status->setText(
                "Select the intended symbol in source or a symbol window for smart matching. "
                "For a typed name, disable Smart reference matching.");
            return;
        }
        if (scope->currentIndex() == 1 && path.isEmpty()) {
            status->setText("Current-file scope requires an open source file.");
            return;
        }
        if (semantic) {
            auto *live = findDocument(path);
            const auto latest =
                live ? FileContents{snapshot(live), {}, live->hasUtf8Bom} : readSourceFile(path);
            if (!latest.error.isEmpty() || latest.bytes != bytes) {
                status->setText(
                    latest.error.isEmpty()
                        ? "Source changed. Select the symbol again before smart matching."
                        : latest.error);
                return;
            }
        }
        auto &saved = projectSearch ? configuration_.searchProject : configuration_.references;
        saved.semantic = semantic;
        saved.scope = scope->currentIndex();
        saved.declarations = declarations->isChecked();
        saved.comments = options.comments != CommentSearch::Exclude;
        saved.strings = options.strings;
        saved.search = options;
        configurationDirty_ = true;
        if (persistSettings_ && !saveConfiguration()) {
            status->setText("Could not save search settings; see status.");
            return;
        }
        cancelReferenceSearch();
        referenceCancelled_ = std::make_shared<std::atomic_bool>(false);
        const auto token = ++referenceToken_;
        referenceIdentifier_ = text;
        referenceOverlays_ = liveOverlays();
        if (auto *origin = findDocument(path);
            origin && std::none_of(referenceOverlays_.cbegin(), referenceOverlays_.cend(),
                                   [&](const auto &overlay) {
                                       return workspacePath(overlay.path) == workspacePath(path);
                                   }))
            referenceOverlays_.append(
                {path, snapshot(origin),
                 origin->analyzedRevision == origin->revision ? origin->analysis : SourceAnalysis{},
                 origin->snapshotHash});
        referencesFiles_.clear();
        referencesScope_ = scope->currentText();
        if (scope->currentIndex() == 1)
            referencesFiles_ << path;
        else if (scope->currentIndex() == 2) {
            for (const auto *item : fileList_->selectedItems())
                referencesFiles_ << item->data(0, Qt::UserRole + 1).toString();
        } else {
            referencesFiles_ = workspaceIndex_->files();
            for (const auto &overlay : referenceOverlays_)
                if (!referencesFiles_.contains(overlay.path))
                    referencesFiles_ << overlay.path;
        }
        const auto filters =
            options.filePatterns.split(QRegularExpression("[;\\s]+"), Qt::SkipEmptyParts);
        referencesFiles_.removeDuplicates();
        if (!filters.isEmpty())
            referencesFiles_.removeIf([&](const auto &file) {
                return !QDir::match(filters, QFileInfo(file).fileName());
            });
        if (referencesFiles_.isEmpty()) {
            status->setText("No files in the selected scope and file types.");
            return;
        }
        referenceRequest_ = {text,
                             referencesFiles_,
                             referenceOverlays_,
                             options,
                             projectPath_,
                             0,
                             RuntimeLimits::ReferenceResults,
                             {}};
        referenceSemantic_ = semantic;
        referenceTargets_.clear();
        referenceOriginPath_ = path;
        referenceOriginHash_ = hash(bytes);
        referencePageOffset_ = 0;
        referenceListedFiles_.clear();
        touchReferenceResults_ = touch->isChecked();
        replaceReferenceResults_ = replaceResults->isChecked();
        referenceSearchBusy_ = true;
        ensureReferenceResults();
        cancelReferenceAction_->setEnabled(true);
        documentsTabs_->setCurrentWidget(referenceResultsPage_);
        referencesStatus_->setText("Searching for " + text + "...");
        dialog->hide();
        dialog->deleteLater();
        if (semantic) {
            if (auto *service = languageService(path, bytes)) {
                referenceService_ = service;
                for (const auto &overlay : referenceOverlays_)
                    if (auto *live = findDocument(overlay.path);
                        live && languageService(overlay.path, overlay.source) == service)
                        service->syncDocument(overlay.path, overlay.source,
                                              static_cast<int>(live->revision));
                auto *live = findDocument(path);
                service->locations(path, bytes, live ? static_cast<int>(live->revision) : 0,
                                   position, token, "references", true, saved.declarations);
            } else
                displayReferences(
                    {}, "No semantic provider is available. Select a text search explicitly.");
        } else {
            // continueReferenceSearch creates its own generation and watcher.
            referenceSearchBusy_ = false;
            continueReferenceSearch();
        }
    });
    dialog->show();
    query->selectAll();
    query->setFocus();
}

void MainWindow::continueReferenceSearch() {
    if (referenceSearchBusy_)
        return;
    for (const auto &overlay : referenceRequest_.overlays)
        if (auto *doc = findDocument(overlay.path); doc && snapshot(doc) != overlay.source) {
            cancelReferenceSearch();
            referencesStatus_->setText("Source changed since the search started. Previous results "
                                       "were kept; rerun the search.");
            reportError(referencesStatus_->text());
            return;
        }
    const auto token = ++referenceToken_;
    referenceCancelled_ = std::make_shared<std::atomic_bool>(false);
    referenceSearchBusy_ = true;
    cancelReferenceAction_->setEnabled(true);
    moreReferencesAction_->setEnabled(false);
    referencesStatus_->setText("Searching for " + referenceIdentifier_ + "...");
    auto *job = new QFutureWatcher<ReferenceResults>(this);
    connect(job, &QFutureWatcher<ReferenceResults>::finished, this, [this, job, token] {
        const auto results = job->result();
        job->deleteLater();
        if (token == referenceToken_ && referenceResultsPage_)
            displayReferenceHits(results, referenceSemantic_);
    });
    auto request = referenceRequest_;
    request.offset = referencePageOffset_;
    if (referenceSemantic_) {
        const auto targets = referenceTargets_;
        const auto cancel = referenceCancelled_;
        job->setFuture(QtConcurrent::run([request, targets, cancel] {
            return resolveSemanticReferences(
                targets, request.paths, request.query, request.overlays, cancel, request.offset,
                request.limit, request.options.skipInactive, request.expectedHashes);
        }));
    } else
        job->setFuture(QtConcurrent::run(searchReferences, request, referenceCancelled_));
}

void MainWindow::displayReferences(const QVector<LspLocation> &targets, const QString &error) {
    if (!referenceResultsPage_)
        return;
    referenceSearchBusy_ = false;
    if (!error.isEmpty()) {
        cancelReferenceSearch();
        referencesStatus_->setText(error);
        reportError(error);
        return;
    }
    if (auto *doc = findDocument(referenceOriginPath_);
        doc && hash(snapshot(doc)) != referenceOriginHash_) {
        cancelReferenceSearch();
        referencesStatus_->setText(
            "Source changed during semantic lookup. Select the symbol again.");
        reportError(referencesStatus_->text());
        return;
    }
    if (!findDocument(referenceOriginPath_)) {
        const auto source = readSourceFile(referenceOriginPath_);
        if (!source.error.isEmpty() || hash(source.bytes) != referenceOriginHash_) {
            cancelReferenceSearch();
            referencesStatus_->setText(
                "Source changed or became unreadable during semantic lookup. Rerun it.");
            reportError(referencesStatus_->text());
            return;
        }
    }
    referenceTargets_ = targets;
    continueReferenceSearch();
}

void MainWindow::displayReferenceHits(const ReferenceResults &results, bool semantic) {
    referenceSearchBusy_ = false;
    cancelReferenceAction_->setEnabled(false);
    if (!results.error.isEmpty()) {
        cancelReferenceSearch();
        referencesStatus_->setText(results.error);
        reportError(results.error);
        return;
    }
    if (replaceReferenceResults_ && results.cancelled) {
        cancelReferenceSearch();
        referencesStatus_->setText("Replacement cancelled. Previous search results were kept.");
        return;
    }
    QSet<QString> files;
    for (const auto &hit : results.hits)
        files.insert(hit.location.path);
    const auto summary = QString("%1 - %2 matches in %3 files (%4; %5)%6%7")
                             .arg(referenceIdentifier_)
                             .arg(results.hits.size())
                             .arg(files.size())
                             .arg(semantic ? "semantic references; provider coverage applies"
                                           : "text matches, not semantic",
                                  referencesScope_)
                             .arg(results.cancelled ? " - cancelled" : "")
                             .arg(results.truncated ? " - more available" : "");
    ReferenceGroup group{summary, referenceRequest_.options.output, {}, referenceIdentifier_};
    auto listed = referenceListedFiles_;
    for (auto hit : results.hits) {
        if (auto *doc = findDocument(hit.location.path)) {
            snapshot(doc);
            if (doc->snapshotHash != hit.location.expectedHash)
                hit.stale = true;
        }
        hit.linked = group.output.links;
        if (group.output.file && !group.output.line && !group.output.text) {
            const auto identity = workspacePath(hit.location.path);
            if (listed.contains(identity))
                continue;
            listed.insert(identity);
        }
        group.hits.append(hit);
    }
    int previousHits = referenceHits_.size();
    if (replaceReferenceResults_) {
        previousHits = 0;
        for (const auto &pending : pendingReferenceGroups_)
            previousHits += pending.hits.size();
        if (std::any_of(group.hits.cbegin(), group.hits.cend(),
                        [](const auto &hit) { return hit.stale; })) {
            cancelReferenceSearch();
            referencesStatus_->setText(
                "Source changed during replacement. Previous results were kept; rerun the search.");
            reportError(referencesStatus_->text());
            return;
        }
    }
    const auto previousGroups =
        replaceReferenceResults_ ? pendingReferenceGroups_.size() : referenceGroups_.size();
    if (previousGroups >= 1000 || previousHits + group.hits.size() > 100000) {
        cancelReferenceSearch();
        referencesStatus_->setText("Search history limit reached (1,000 groups / 100,000 rows). "
                                   "Export and remove older results before rerunning.");
        reportError(referencesStatus_->text());
        return;
    }
    referenceListedFiles_ = listed;
    referencePageOffset_ += results.hits.size();
    for (auto it = results.sourceHashes.cbegin(); it != results.sourceHashes.cend(); ++it)
        referenceRequest_.expectedHashes.insert(it.key(), it.value());
    int first = referenceHits_.size();
    if (replaceReferenceResults_) {
        pendingReferenceGroups_.append(group);
        if (results.truncated) {
            moreReferencesAction_->setEnabled(true);
            cancelReferenceAction_->setEnabled(true);
            referencesStatus_->setText(
                QString("%1 - %2 matches pending replacement. Previous searches are unchanged. "
                        "Load More to finish, or Cancel Search to discard the pending results.")
                    .arg(referenceIdentifier_)
                    .arg(referencePageOffset_));
            referencesStatus_->setToolTip(summary + '\n' + results.warnings.join('\n'));
            for (const auto &warning : results.warnings)
                qWarning().noquote() << warning;
            if (touchReferenceResults_)
                referenceTouchHits_ += results.hits;
            return;
        }
        auto previous = std::move(referenceGroups_);
        referenceGroups_ = std::exchange(pendingReferenceGroups_, {});
        if (!saveProjectSession()) {
            referenceGroups_ = std::move(previous);
            cancelReferenceSearch();
            referencesStatus_->setText(
                "Could not save the replacement. Previous search results were kept.");
            return;
        }
        replaceReferenceResults_ = false;
        selectedReferenceIndex_ = -1;
        selectedSearchQuery_.clear();
        first = 0;
    } else
        referenceGroups_.append(group);
    rebuildReferenceResults();
    moreReferencesAction_->setEnabled(results.truncated && !results.cancelled);
    referencesStatus_->setText(summary + QString(". %1 warning(s).").arg(results.warnings.size()));
    referencesStatus_->setToolTip(results.warnings.join('\n'));
    if (!referenceGroups_.isEmpty())
        referencesList_->topLevelItem(referencesList_->topLevelItemCount() - 1)
            ->setToolTip(0, summary + '\n' + results.warnings.join('\n'));
    for (const auto &warning : results.warnings)
        qWarning().noquote() << warning;
    documentsTabs_->setCurrentWidget(referenceResultsPage_);
    if (first < referenceRows_.size())
        referencesList_->setCurrentItem(referenceRows_[first]);
    referencesList_->setFocus();
    updateActions();
    if (touchReferenceResults_ && !results.cancelled) {
        referenceTouchHits_ += results.hits;
        if (results.truncated) {
            referencesStatus_->setText(
                referencesStatus_->text() +
                " Load all remaining pages before confirming timestamp updates.");
        } else {
            touchReferenceResults_ = false;
            const auto hits = std::exchange(referenceTouchHits_, {});
            touchReferenceHits(hits);
        }
    }
}

void MainWindow::removeReferenceResults() {
    if (!referencesList_)
        return;
    auto selected = referencesList_->selectedItems();
    if (auto *view = qobject_cast<ReferenceResultsView *>(referencesList_);
        view && view->textCursor().hasSelection())
        selected = view->selectedTextItems();
    for (int g = referenceGroups_.size() - 1; g >= 0; --g) {
        auto *heading = referencesList_->topLevelItem(g);
        if (selected.contains(heading)) {
            referenceGroups_.removeAt(g);
            continue;
        }
        auto &hits = referenceGroups_[g].hits;
        for (int h = hits.size() - 1; h >= 0; --h)
            if (selected.contains(heading->child(h)))
                hits.removeAt(h);
    }
    rebuildReferenceResults();
    referencesStatus_->setText(
        "Selected results removed. Group headings describe the original search.");
}

void MainWindow::exportReferenceResults() {
    const QPointer<QWidget> page = referenceResultsPage_;
    if (!page)
        return;
    const auto path = QFileDialog::getSaveFileName(this, "Export Search Results", {},
                                                   "Text files (*.txt);;All files (*)");
    if (path.isEmpty() || !page || page != referenceResultsPage_)
        return;
    const auto error = exportReferenceHistory(path, referenceGroups_, projectPath_);
    if (!error.isEmpty()) {
        reportError("Could not export search results: " + error);
        return;
    }
    referencesStatus_->setText("Search results exported to " + path);
}

void MainWindow::trackReferenceEdit(const QString &path, int position, int removed, int inserted,
                                    const QByteArray &beforeHash, const QByteArray &source) {
    if (referenceGroups_.isEmpty())
        return;
    const auto identity = workspacePath(path);
    const auto afterHash = hash(source);
    int row = 0;
    for (auto &group : referenceGroups_)
        for (auto &hit : group.hits) {
            if (workspacePath(hit.location.path) == identity) {
                adjustReferenceHit(hit, position, removed, inserted, beforeHash, source, afterHash);
                referenceHits_[row] = hit;
                if (referencesList_) {
                    auto *item = referenceRows_[row];
                    if (group.output.line)
                        item->setText(2, QString::number(hit.line + 1));
                    if (group.output.text)
                        item->setText(3, hit.text);
                    item->setText(0, resultText(hit, group.output, projectPath_));
                    item->setData(0, ReferenceItemRole::DisplayText,
                                  resultText(hit, group.output, projectPath_));
                    if (hit.stale)
                        item->setToolTip(0, "Source link is stale; rerun the search.");
                }
            }
            ++row;
        }
}

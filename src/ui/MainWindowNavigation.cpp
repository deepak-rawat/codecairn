#include "MainWindow.h"
#include <codecairn/LanguageRegistry.h>
#include "NavigationTree.h"
#include <codecairn/Diagnostics.h>
#include <codecairn/NavigationTarget.h>
#include "SymbolCatalogModel.h"
#include <QAction>
#include <QApplication>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLineEdit>
#include <QScrollBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QtConcurrentRun>
#include <utility>

const QByteArray &MainWindow::snapshot(Document *document) const {
    return document->snapshotBytes();
}

void MainWindow::initializeNavigation() {
    connect(engine_, &codecairn::Engine::sourceReady, this,
            [this](codecairn::RequestId requestId, const codecairn::SourceRead &result) {
                if (!pendingOpen_ || requestId != sourceReadRequest_ ||
                    pendingOpen_->generation != navigationGeneration_)
                    return;
                const auto request = *std::exchange(pendingOpen_, {});
                if (!result.file.error.isEmpty())
                    reportError("Cannot navigate to " + result.location.path + ": " +
                                result.file.error);
                else if (!result.range.error.isEmpty()) {
                    reportError(result.range.error);
                    if (result.range.stale)
                        project_->refreshIndex();
                } else
                    commitNavigation(result.location, result.file, request.pin, request.focus,
                                     request.record, request.origin, request.focusWidget,
                                     request.indexedTarget, result.range);
            });
    hoverTimer_ = new QTimer(this);
    hoverTimer_->setSingleShot(true);
    hoverTimer_->setInterval(configuration_.behavior.contextDebounceMs);
    connect(qobject_cast<NavigationTree *>(outline_), &NavigationTree::keyboardSelectionChanged,
            this, [this] { navigateOutline(false); });
    connect(qobject_cast<NavigationTree *>(fileList_), &NavigationTree::keyboardSelectionChanged,
            this, [this] { navigateProjectFile(false); });
    connect(qobject_cast<NavigationTree *>(symbolList_), &NavigationTree::keyboardSelectionChanged,
            this, [this] { navigateProjectSymbol(false); });
    for (auto *entry : {outlineFilter_, fileFilter_, symbolFilter_})
        entry->installEventFilter(this);
    for (auto *tree : {outline_, fileList_, symbolList_})
        tree->installEventFilter(this);
    virtualSymbols_->installEventFilter(this);
    for (auto *entry : {outlineFilter_, fileFilter_, symbolFilter_}) {
        auto *tree = entry == outlineFilter_ ? outline_
                     : entry == fileFilter_  ? fileList_
                                             : symbolList_;
        connect(qApp, &QApplication::focusChanged, this,
                [this, entry, tree](QWidget *old, QWidget *now) {
                    const auto inPanel = [entry, tree](QWidget *widget) {
                        return widget && (widget == entry || entry->isAncestorOf(widget) ||
                                          widget == tree || tree->isAncestorOf(widget));
                    };
                    const auto inSymbols = [this, entry](QWidget *widget) {
                        return entry == symbolFilter_ && widget &&
                               (widget == virtualSymbols_ || virtualSymbols_->isAncestorOf(widget));
                    };
                    if ((inPanel(old) || inSymbols(old)) && !inPanel(now) && !inSymbols(now))
                        clearPanelFilter(entry);
                });
    }
    connect(outline_, &QTreeWidget::itemClicked, this, [this] { navigateOutline(false); });
    connect(fileList_, &QTreeWidget::itemClicked, this, [this] { navigateProjectFile(false); });
    connect(symbolList_, &QTreeWidget::itemClicked, this, [this] { navigateProjectSymbol(false); });
    connect(documentBar_, &QTabBar::tabBarDoubleClicked, this, [this](int index) {
        for (const auto &doc : documents_)
            if (documentsTabs_->indexOf(doc->editor) == index)
                pinDocument(doc.get());
    });
    contextPreviewEditor_->setLinkBrowsing(true);
    connect(contextPreviewEditor_, &CodeEditor::linkHovered, this,
            [this](int position) { requestHover(contextPreviewEditor_, position); });
    connect(contextPreviewEditor_, &CodeEditor::definitionActivated, this,
            [this](int position) { jumpToDefinition(contextPreviewEditor_, position); });
    connect(contextPreviewEditor_, &CodeEditor::referencesRequested, this,
            [this] { lookupReferences(contextPreviewEditor_); });
    connect(hoverTimer_, &QTimer::timeout, this, [this] {
        if (hoverEditor_ && hoverPosition_ >= 0)
            lookupLink(hoverEditor_, hoverPosition_, false);
    });
}

NavigationLocation MainWindow::currentLocation() const {
    const auto *doc = currentDocument();
    if (!doc)
        return {};
    return {doc->path(),
            static_cast<int>(doc->editor->send(SCI_GETSELECTIONSTART)),
            static_cast<int>(doc->editor->send(SCI_GETSELECTIONEND)),
            static_cast<int>(doc->editor->send(SCI_GETFIRSTVISIBLELINE)),
            {}};
}

void MainWindow::pinDocument(Document *document) {
    if (!document)
        return;
    document->isPreview = false;
    updateTab(document);
}

void MainWindow::navigateTo(const NavigationLocation &location, bool pin, bool focus, bool record,
                            const std::optional<IndexedSymbol> &indexedTarget) {
    if (location.path.isEmpty())
        return;
    qCDebug(navigationLog) << "navigation.requested" << "path=" << location.path
                           << "start=" << location.start << "end=" << location.end << "pin=" << pin;
    const auto origin = currentLocation();
    QPointer<QWidget> previousFocus = QApplication::focusWidget();
    ++navigationGeneration_;
    engine_->cancel(sourceReadRequest_);
    pendingOpen_.reset();
    if (auto *document = findDocument(location.path)) {
        commitNavigation(location, {snapshot(document), {}, document->hasUtf8Bom()}, pin, focus,
                         record, origin, previousFocus, indexedTarget);
        return;
    }
    pendingOpen_ = OpenRequest{location, origin, previousFocus,         pin,
                               focus,    record, navigationGeneration_, indexedTarget};
    sourceReadRequest_ = engine_->readSource(location, indexedTarget);
}

void MainWindow::commitNavigation(const NavigationLocation &location, const FileContents &file,
                                  bool pin, bool focus, bool record,
                                  const NavigationLocation &origin,
                                  const QPointer<QWidget> &previousFocus,
                                  const std::optional<IndexedSymbol> &indexedTarget,
                                  const std::optional<ResolvedNavigation> &resolvedRange) {
    const auto resolved = resolvedRange
                              ? *resolvedRange
                              : resolveNavigationTarget(location, file.bytes, indexedTarget);
    if (!resolved.error.isEmpty()) {
        reportError(resolved.error);
        if (resolved.stale)
            project_->refreshIndex();
        return;
    }
    const int requestedStart = resolved.start;
    const int requestedEnd = resolved.end;
    auto *document = findDocument(location.path);
    if (!document) {
        int reuse = -1;
        for (const auto &candidate : documents_) {
            if (candidate->isPreview && !candidate->editor->isModified()) {
                reuse = documentsTabs_->indexOf(candidate->editor);
                break;
            }
        }
        if (reuse >= 0)
            closeTab(reuse);
        addDocument(location.path, file.bytes, file.utf8Bom, false, !pin, reuse);
        document = currentDocument();
    } else {
        documentsTabs_->setCurrentWidget(document->editor);
        if (pin)
            pinDocument(document);
    }
    const int start = qBound(0, requestedStart, static_cast<int>(file.bytes.size()));
    const int end = qBound(start, requestedEnd, static_cast<int>(file.bytes.size()));
    if (location.wholeLine)
        document->editor->selectLineAt(start);
    else
        document->editor->selectRange(start, end, !indexedTarget.has_value());
    if (indexedTarget) {
        syncOutlineToEditor();
        updateStatus();
    }
    if (location.firstVisibleLine >= 0)
        document->editor->send(SCI_SETFIRSTVISIBLELINE, location.firstVisibleLine);
    if (record)
        navigationHistory_.record(origin,
                                  {document->path(), start, end, location.firstVisibleLine, {}});
    historyBack_->setEnabled(navigationHistory_.canBack());
    historyForward_->setEnabled(navigationHistory_.canForward());
    if (focus)
        document->editor->setFocus();
    else if (previousFocus)
        previousFocus->setFocus();
    if (indexedTarget && !pinContextAction_->isChecked()) {
        contextTimer_->stop();
        contextFollowsEditor_ = false;
        contextPath_ = document->path();
        const auto &symbol = indexedTarget->symbol;
        showContext(symbol.name + " - Project index", file.bytes, document->analysis, symbol.start,
                    symbol.end);
        contextPreviewEditor_->selectRange(symbol.nameStart, symbol.nameEnd);
    }
    if (location.wholeLine && !pinContextAction_->isChecked()) {
        contextTimer_->stop();
        contextFollowsEditor_ = false;
        contextPath_ = document->path();
        showContext(QFileInfo(document->path()).fileName() + " - Reference", file.bytes,
                    document->analysis, start, end, true);
    }
}

void MainWindow::navigateHistory(int direction) {
    for (auto *filter : {outlineFilter_, fileFilter_, symbolFilter_})
        clearPanelFilter(filter);
    const auto location = navigationHistory_.move(direction);
    if (!location.path.isEmpty())
        navigateTo(location, true, true, false);
    historyBack_->setEnabled(navigationHistory_.canBack());
    historyForward_->setEnabled(navigationHistory_.canForward());
}

void MainWindow::navigateOutline(bool pin) {
    if (showingReferenceOutline_) {
        selectSearchQuery();
        if (pin)
            clearPanelFilter(outlineFilter_);
        return;
    }
    auto *doc = currentDocument();
    const auto *item = outline_->currentItem();
    if (!doc || !item)
        return;
    const int index = item->data(0, Qt::UserRole).toInt();
    if (doc->revision() != doc->analyzedRevision || index < 0 ||
        index >= doc->analysis.symbols.size()) {
        reportError("Symbols are updating; retry after analysis finishes.");
        return;
    }
    const auto &symbol = doc->analysis.symbols[index];
    navigateTo({doc->path(), symbol.nameStart, symbol.nameEnd, -1, {}}, pin, pin);
    previewSymbol(index);
    if (pin)
        clearPanelFilter(outlineFilter_);
}

void MainWindow::navigateProjectFile(bool pin) {
    const auto *item = fileList_->currentItem();
    if (!item)
        return;
    navigateTo({item->data(0, Qt::UserRole + 1).toString()}, pin, pin);
    if (pin)
        clearPanelFilter(fileFilter_);
}

void MainWindow::navigateProjectSymbol(bool pin) {
    if (symbolFilterPending_) {
        symbolNavigationAfterFilter_ = pin;
        if (symbolQueryTimer_->isActive()) {
            symbolQueryTimer_->stop();
            updateOpenSymbols();
        }
        return;
    }
    if (virtualSymbolMode_) {
        const auto entry = symbolModel_->entry(virtualSymbols_->currentIndex());
        if (!entry) {
            virtualSymbolActivation_ = pin;
            virtualSymbolActivationIndex_ = virtualSymbols_->currentIndex();
            return;
        }
        virtualSymbolActivation_.reset();
        selectedProjectSymbol_ = *entry;
        navigateTo({entry->path, entry->symbol.nameStart, entry->symbol.nameEnd, -1, entry->hash},
                   pin, pin, true, *entry);
        if (pin)
            clearPanelFilter(symbolFilter_);
        return;
    }
    const auto *item = symbolList_->currentItem();
    if (!item)
        return;
    const int row = item->data(0, Qt::UserRole).toInt();
    if (row < 0 || row >= symbolPage_.symbols.size())
        return;
    const auto &entry = symbolPage_.symbols[row];
    selectedProjectSymbol_ = entry;
    navigateTo({entry.path, entry.symbol.nameStart, entry.symbol.nameEnd, -1, entry.hash}, pin, pin,
               true, entry);
    if (pin)
        clearPanelFilter(symbolFilter_);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        QLineEdit *panelFilter =
            watched == outlineFilter_ || watched == outline_ ? outlineFilter_
            : watched == fileFilter_ || watched == fileList_ ? fileFilter_
            : watched == symbolFilter_ || watched == symbolList_ || watched == virtualSymbols_
                ? symbolFilter_
                : nullptr;
        if (panelFilter && key->key() == Qt::Key_Escape) {
            clearPanelFilter(panelFilter);
            return true;
        }
        if (panelFilter && watched != panelFilter &&
            !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))) {
            if (!key->text().isEmpty() && key->text().front().isPrint()) {
                panelFilter->setFocus();
                panelFilter->insert(key->text());
                return true;
            }
            if (key->key() == Qt::Key_Backspace && !panelFilter->text().isEmpty()) {
                panelFilter->setFocus();
                panelFilter->backspace();
                return true;
            }
        }
        if (watched == outline_ && showingReferenceOutline_ && key->key() == Qt::Key_Delete &&
            key->modifiers() == Qt::NoModifier) {
            removeSearchQuery();
            return true;
        }
        if (watched == symbolFilter_ && virtualSymbolMode_) {
            if (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up) {
                const int current = virtualSymbols_->currentIndex().row();
                const int row = qBound(0, current + (key->key() == Qt::Key_Down ? 1 : -1),
                                       qMax(0, symbolModel_->rowCount() - 1));
                virtualSymbols_->setCurrentIndex(symbolModel_->index(row, 0));
                navigateProjectSymbol(false);
                return true;
            }
            if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
                if (!virtualSymbols_->currentIndex().isValid())
                    virtualSymbols_->setCurrentIndex(symbolModel_->index(0, 0));
                navigateProjectSymbol(true);
                return true;
            }
        }
        QTreeWidget *tree = watched == outlineFilter_  ? outline_
                            : watched == fileFilter_   ? fileList_
                            : watched == symbolFilter_ ? symbolList_
                                                       : nullptr;
        if (tree && (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up)) {
            auto *item = tree->currentItem();
            do {
                item = item ? (key->key() == Qt::Key_Down ? tree->itemBelow(item)
                                                          : tree->itemAbove(item))
                            : tree->topLevelItem(0);
            } while (item && item->isHidden());
            if (item) {
                tree->setCurrentItem(item);
                if (tree == outline_)
                    navigateOutline(false);
                else if (tree == fileList_)
                    navigateProjectFile(false);
                else
                    navigateProjectSymbol(false);
            }
            return true;
        }
        if (tree && (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)) {
            const auto visible = [](QTreeWidgetItem *item) {
                if (!item)
                    return false;
                for (auto *ancestor = item; ancestor; ancestor = ancestor->parent())
                    if (ancestor->isHidden() || (ancestor != item && !ancestor->isExpanded()))
                        return false;
                return true;
            };
            if (!visible(tree->currentItem())) {
                for (QTreeWidgetItemIterator it(tree); *it; ++it)
                    if (visible(*it)) {
                        tree->setCurrentItem(*it);
                        break;
                    }
            }
            if (!visible(tree->currentItem()))
                return true;
            if (tree == outline_)
                navigateOutline(true);
            else if (tree == fileList_)
                navigateProjectFile(true);
            else
                navigateProjectSymbol(true);
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

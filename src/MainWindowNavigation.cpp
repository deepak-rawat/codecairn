#include "MainWindow.h"
#include "LanguageRegistry.h"
#include "NavigationTree.h"
#include "Diagnostics.h"
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

const QByteArray &MainWindow::snapshot(Document *document) const {
    return document->snapshotBytes();
}

void MainWindow::initializeNavigation() {
    navigationJob_ = new QFutureWatcher<FileContents>(this);
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
    for (auto *entry : {outlineFilter_, fileFilter_, symbolFilter_}) {
        auto *tree = entry == outlineFilter_ ? outline_ : entry == fileFilter_ ? fileList_ : symbolList_;
        connect(qApp, &QApplication::focusChanged, this, [this, entry, tree](QWidget *old, QWidget *now) {
            const auto inPanel = [entry, tree](QWidget *widget) {
                return widget && (widget == entry || entry->isAncestorOf(widget) ||
                                  widget == tree || tree->isAncestorOf(widget));
            };
            if (inPanel(old) && !inPanel(now))
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
    connectLanguageService(cppLanguageService_);
}

NavigationLocation MainWindow::currentLocation() const {
    const auto *doc = currentDocument();
    if (!doc)
        return {};
    return {doc->path,
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

void MainWindow::navigateTo(const NavigationLocation &location, bool pin, bool focus, bool record) {
    if (location.path.isEmpty())
        return;
    qCDebug(navigationLog) << "navigation.requested" << "path=" << location.path
                           << "start=" << location.start << "end=" << location.end << "pin=" << pin;
    const auto origin = currentLocation();
    QPointer<QWidget> previousFocus = QApplication::focusWidget();
    ++navigationGeneration_;
    pendingOpen_.reset();
    if (auto *document = findDocument(location.path)) {
        commitNavigation(location, {snapshot(document), {}, document->hasUtf8Bom}, pin, focus,
                         record, origin, previousFocus);
        return;
    }
    pendingOpen_ =
        OpenRequest{location, origin, previousFocus, pin, focus, record, navigationGeneration_};
    startNavigationRead();
}

void MainWindow::startNavigationRead() {
    if (navigationBusy_ || !pendingOpen_)
        return;
    navigationBusy_ = true;
    const auto request = *pendingOpen_;
    pendingOpen_.reset();
    disconnect(navigationJob_, &QFutureWatcher<FileContents>::finished, this, nullptr);
    connect(navigationJob_, &QFutureWatcher<FileContents>::finished, this, [this, request] {
        navigationBusy_ = false;
        if (request.generation == navigationGeneration_) {
            const auto file = navigationJob_->result();
            if (!file.error.isEmpty())
                reportError("Cannot navigate to " + request.location.path + ": " + file.error);
            else
                commitNavigation(request.location, file, request.pin, request.focus, request.record,
                                 request.origin, request.focusWidget);
        }
        startNavigationRead();
    });
    navigationJob_->setFuture(
        QtConcurrent::run([path = request.location.path] { return readSourceFile(path); }));
}

void MainWindow::commitNavigation(const NavigationLocation &location, const FileContents &file,
                                  bool pin, bool focus, bool record,
                                  const NavigationLocation &origin,
                                  const QPointer<QWidget> &previousFocus) {
    if (!location.expectedHash.isEmpty() &&
        QCryptographicHash::hash(file.bytes, QCryptographicHash::Sha256).toHex() !=
            location.expectedHash) {
        reportError("Source changed since this target was indexed; refreshing instead of using "
                    "stale offsets.");
        workspaceIndex_->refresh();
        return;
    }
    const int requestedStart = location.line >= 0
                                   ? sourceByteOffset(file.bytes, location.line, location.character)
                                   : location.start;
    const int requestedEnd =
        location.line >= 0 ? sourceByteOffset(file.bytes, location.endLine, location.endCharacter)
                           : location.end;
    if (requestedStart < 0 || requestedEnd < requestedStart || requestedEnd > file.bytes.size()) {
        reportError("Definition range is no longer valid. Retry the lookup.");
        return;
    }
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
        document->editor->selectRange(start, end);
    if (location.firstVisibleLine >= 0)
        document->editor->send(SCI_SETFIRSTVISIBLELINE, location.firstVisibleLine);
    if (record)
        navigationHistory_.record(origin,
                                  {document->path, start, end, location.firstVisibleLine, {}});
    historyBack_->setEnabled(navigationHistory_.canBack());
    historyForward_->setEnabled(navigationHistory_.canForward());
    if (focus)
        document->editor->setFocus();
    else if (previousFocus)
        previousFocus->setFocus();
    if (location.wholeLine && !pinContextAction_->isChecked()) {
        contextTimer_->stop();
        contextFollowsEditor_ = false;
        contextPath_ = document->path;
        showContext(QFileInfo(document->path).fileName() + " - Reference", file.bytes,
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
    if (doc->revision != doc->analyzedRevision || index < 0 ||
        index >= doc->analysis.symbols.size()) {
        reportError("Symbols are updating; retry after analysis finishes.");
        return;
    }
    const auto &symbol = doc->analysis.symbols[index];
    navigateTo({doc->path, symbol.nameStart, symbol.nameEnd, -1, {}}, pin, pin);
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
    const auto *item = symbolList_->currentItem();
    if (!item)
        return;
    const int row = item->data(0, Qt::UserRole).toInt();
    if (row < 0 || row >= symbolPage_.symbols.size())
        return;
    const auto &entry = symbolPage_.symbols[row];
    navigateTo({entry.path, entry.symbol.nameStart, entry.symbol.nameEnd, -1, entry.hash}, pin,
               pin);
    if (pin)
        clearPanelFilter(symbolFilter_);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        QLineEdit *panelFilter = watched == outlineFilter_ || watched == outline_ ? outlineFilter_
                                : watched == fileFilter_ || watched == fileList_ ? fileFilter_
                                : watched == symbolFilter_ || watched == symbolList_ ? symbolFilter_
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

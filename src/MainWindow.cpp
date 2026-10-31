#include "MainWindow.h"
#include "FileIO.h"
#include "UiAppearance.h"
#include "RuntimeLimits.h"
#include "LanguageRegistry.h"
#include "SymbolBrowser.h"
#include "CompilationDatabase.h"
#include "WorkspaceOptionsStore.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontComboBox>
#include <QFontDatabase>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QSet>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
#include <QTabBar>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QScrollBar>
#include <QComboBox>
#include <QCryptographicHash>
#include <QVBoxLayout>
#include <QtConcurrentRun>
#include <algorithm>
#include <utility>

namespace {
constexpr int SymbolRole = SymbolItemRole::Index;
constexpr int PathRole = SymbolItemRole::Path;
constexpr int KeyRole = SymbolItemRole::Key;
constexpr int KindRole = SymbolItemRole::Kind;
constexpr int LineRole = SymbolItemRole::Line;

QString pathIdentity(const QString &path) {
    if (path.startsWith(":/"))
        return path;
    const QFileInfo info(path);
    const auto canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

bool fragmentsMatch(const QString &name, const QString &filter) {
    const auto fragments = filter.split(' ', Qt::SkipEmptyParts);
    for (const auto &fragment : fragments)
        if (!name.contains(fragment, Qt::CaseInsensitive))
            return false;
    return true;
}

}

MainWindow::MainWindow(bool persistSettings, QWidget *parent, QString configPath,
                       QString stateDirectory)
    : QMainWindow(parent), persistSettings_(persistSettings),
      stateStore_(stateDirectory.isEmpty() ? defaultStateDirectory() : stateDirectory),
      configurationFile_(configPath.isEmpty() && persistSettings ? defaultSettingsPath()
                                                                 : configPath),
      configuration_(defaultAppSettings()) {
    Q_INIT_RESOURCE(resources);
    QString startupError;
    const auto loaded = configurationFile_.load();
    if (loaded.settings) {
        configuration_ = *loaded.settings;
        if (!configurationFile_.existed() && !configurationFile_.path().isEmpty()) {
            const auto error = configurationFile_.save(configuration_);
            if (!error.isEmpty())
                startupError = error;
        }
    } else
        startupError = loaded.error;
    buildUi();
    contextTimer_ = new QTimer(this);
    contextTimer_->setSingleShot(true);
    contextTimer_->setInterval(configuration_.behavior.contextDebounceMs);
    initializeWorkspace();
    initializeNavigation();
    connect(contextTimer_, &QTimer::timeout, this, &MainWindow::updateContext);
    connect(documentsTabs_, &QTabWidget::currentChanged, this, [this] {
        contextFollowsEditor_ = true;
        ++contextGeneration_;
        definitionChoices_->hide();
        rememberOutline();
        updateOutline();
        updateStatus();
        contextTimer_->start();
    });
    connect(documentsTabs_, &QTabWidget::tabCloseRequested, this,
            [this](int index) { closeTab(index); });
    connect(sourceFontComboBox_, &QFontComboBox::currentFontChanged, this,
            &MainWindow::applyTypography);
    connect(sourceFontSizeSpinBox_, &QDoubleSpinBox::valueChanged, this,
            &MainWindow::applyTypography);
    connect(declarationScaleSpinBox_, &QDoubleSpinBox::valueChanged, this,
            &MainWindow::applyTypography);
    connect(uniformSizesCheckBox_, &QCheckBox::toggled, this, &MainWindow::applyTypography);
    connect(whitespaceCheckBox_, &QCheckBox::toggled, this, &MainWindow::applyTypography);
    connect(currentLine_, &QCheckBox::toggled, this, &MainWindow::applyTypography);
    connect(pinContextAction_, &QAction::toggled, this, [this](bool pinned) {
        ++contextGeneration_;
        pendingPreviewPath_.clear();
        definitionChoices_->hide();
        contextBack_->setEnabled(!pinned && contextHistoryIndex_ > 0);
        contextForward_->setEnabled(!pinned && contextHistoryIndex_ + 1 < contextHistory_.size());
        if (!pinned)
            updateContext();
    });
    connect(outlineFilter_, &QLineEdit::textChanged, this, &MainWindow::filterOutline);
    connect(outline_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (item) {
            if (showingReferenceOutline_) {
                selectSearchQuery();
                return;
            }
            rememberOutline();
            previewSymbol(item->data(0, SymbolRole).toInt());
        }
    });
    connect(outline_, &QTreeWidget::itemCollapsed, this, [this] { rememberOutline(); });
    connect(outline_, &QTreeWidget::itemExpanded, this, [this] { rememberOutline(); });
    connect(outline_, &QTreeWidget::itemActivated, this, [this] { navigateOutline(true); });
    connect(fileFilter_, &QLineEdit::textChanged, this, &MainWindow::filterFiles);
    connect(fileList_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (item)
            previewFile(item->data(0, PathRole).toString());
    });
    connect(fileList_, &QTreeWidget::itemActivated, this, [this] { navigateProjectFile(true); });
    connect(symbolFilter_, &QLineEdit::textChanged, this, &MainWindow::filterSymbols);
    connect(symbolList_, &QTreeWidget::currentItemChanged, this,
            [this] { activateProjectSymbol(false); });
    connect(symbolList_, &QTreeWidget::itemActivated, this,
            [this] { navigateProjectSymbol(true); });
    diskPreview_ = new QFutureWatcher<DiskPreview>(this);
    if (persistSettings_)
        restoreApplicationState();
    applyConfiguration();
    if (!startupError.isEmpty()) {
        configurationError_ = startupError;
        QTimer::singleShot(0, this, [this, startupError] {
            reportError("UI configuration: " + startupError);
            auto *warning = new QMessageBox(
                QMessageBox::Warning, "UI configuration",
                startupError + "\n\nThe application is using the available validated settings. "
                               "Existing invalid configuration files have not been overwritten.",
                QMessageBox::Ok, this);
            warning->setAttribute(Qt::WA_DeleteOnClose);
            warning->open();
        });
    }
}

MainWindow::~MainWindow() {
    disconnect(qApp, nullptr, this, nullptr);
    for (auto *object : findChildren<QObject *>())
        disconnect(object, nullptr, this, nullptr);
    for (auto *timer : findChildren<QTimer *>())
        timer->stop();
    if (referenceCancelled_)
        referenceCancelled_->store(true);
    workspaceIndex_->cancel();
    workspaceIndex_->invalidateSearch();
    const auto watchers = findChildren<QFutureWatcherBase *>();
    for (auto *watcher : watchers)
        disconnect(watcher, nullptr, nullptr, nullptr);
    workspaceServices_->stopLanguageServices();
    for (auto *watcher : watchers)
        watcher->waitForFinished();
    delete workspaceServices_;
    workspaceServices_ = nullptr;
    workspaceIndex_ = nullptr;
    cppLanguageService_ = nullptr;
}

MainWindow::Document *MainWindow::currentDocument() const {
    for (const auto &doc : documents_)
        if (doc->editor == documentsTabs_->currentWidget())
            return doc.get();
    return nullptr;
}

MainWindow::Document *MainWindow::findDocument(const QString &path) const {
    const auto key = pathIdentity(path);
    for (const auto &doc : documents_) {
#ifdef Q_OS_WIN
        if (doc->path.compare(key, Qt::CaseInsensitive) == 0)
#else
        if (doc->path == key)
#endif
            return doc.get();
    }
    return nullptr;
}

CodeEditor *MainWindow::activeEditor() const {
    auto *doc = currentDocument();
    return doc ? doc->editor : nullptr;
}

int MainWindow::documentCount() const {
    return static_cast<int>(documents_.size());
}

bool MainWindow::openFile(const QString &path) {
    if (auto *doc = findDocument(path)) {
        pinDocument(doc);
        documentsTabs_->setCurrentWidget(doc->editor);
        return true;
    }
    const auto file = readSourceFile(path);
    if (!file.error.isEmpty()) {
        QMessageBox::warning(this, "Cannot open file", path + "\n\n" + file.error);
        return false;
    }
    if (!path.startsWith(":/")) {
        if (projectPath_.isEmpty()) {
            openProject(QFileInfo(path).absolutePath());
        } else if (!pathInWorkspace(path, projectPath_)) {
            const auto choice = QMessageBox::question(
                this, "File outside workspace",
                "Use this file's containing folder as the workspace?\nNo keeps the current "
                "workspace and opens an external file.",
                QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, QMessageBox::No);
            if (choice == QMessageBox::Cancel)
                return false;
            if (choice == QMessageBox::Yes)
                openProject(QFileInfo(path).absolutePath());
        }
    }
    if (auto *doc = findDocument(path)) {
        pinDocument(doc);
        documentsTabs_->setCurrentWidget(doc->editor);
        return true;
    }
    addDocument(pathIdentity(path), file.bytes, file.utf8Bom);
    return true;
}

void MainWindow::openStartupPaths(const QStringList &paths) {
    if (!paths.isEmpty()) {
        for (const auto &path : paths) {
            if (QFileInfo(path).isDir())
                openProject(path);
            else
                openFile(path);
        }
        return;
    }
    if (!persistSettings_)
        return;
    if (!stateError_.isEmpty()) {
        reportError(stateError_);
        return;
    }
    const auto previous = applicationState_.lastWorkspace;
    if (previous.isEmpty())
        return;
    const QFileInfo folder(previous);
    if (!folder.isDir() || !folder.isReadable()) {
        reportError("Last workspace is unavailable: " + previous +
                    ". Open another folder to continue.");
        return;
    }
    openProject(previous);
}

void MainWindow::addDocument(const QString &path, const QByteArray &bytes, bool bom, bool focus,
                             bool preview, int index) {
    auto document =
        std::make_unique<Document>(path, bytes, bom, configuration_.behavior.parseDebounceMs, this);
    auto *doc = document.get();
    doc->isPreview = preview;
    connect(doc->editor, &CodeEditor::rangeEdited, this,
            [this, doc](int position, int removed, int inserted, const QByteArray &beforeHash) {
                trackReferenceEdit(doc->path, position, removed, inserted, beforeHash, doc->editor->bytes());
            });
    connect(doc->editor, &CodeEditor::contentEdited, this, [this, doc] {
        if (doc == currentDocument())
            contextFollowsEditor_ = true;
        pinDocument(doc);
        requestHover(doc->editor, -1);
        ++contextGeneration_;
        definitionChoices_->hide();
        doc->scheduleAnalysis();
        updateTab(doc);
        if (doc == currentDocument()) {
            rememberOutline();
            outlineState_->setText("Parsing unsaved buffer...");
            outline_->setToolTip(outlineState_->text());
            contextTimer_->start();
        }
        updateOpenSymbols();
        updateReferenceMarkers();
    });
    connect(doc->editor, &ScintillaEditBase::savePointChanged, this,
            [this, doc] { updateTab(doc); });
    const auto followSelection = [this, doc] {
        if (doc == currentDocument()) {
            contextFollowsEditor_ = true;
            ++contextGeneration_;
            definitionChoices_->hide();
            syncOutlineToEditor();
            updateStatus();
            contextTimer_->start();
        }
    };
    connect(doc->editor, &CodeEditor::caretMoved, this, followSelection);
    connect(doc->editor, &CodeEditor::selectionActivated, this, followSelection);
    connect(doc->editor, &CodeEditor::linkHovered, this,
            [this, doc](int position) { requestHover(doc->editor, position); });
    connect(doc->editor, &CodeEditor::definitionActivated, this,
            [this, doc](int position) { jumpToDefinition(doc->editor, position); });
    connect(doc->editor, &CodeEditor::referencesRequested, this,
            [this, doc] { lookupReferences(doc->editor); });
    connect(doc->editor, &CodeEditor::referenceLineClicked, this,
            [this, doc](int line) { returnToReferenceResults(doc->path, line); });
    connect(doc->editor, &CodeEditor::historyRequested, this, &MainWindow::navigateHistory);
    connect(doc->editor, &ScintillaEditBase::focusChanged, this, [this, doc](bool focused) {
        if (focused && doc == currentDocument()) {
            contextFollowsEditor_ = true;
            ++contextGeneration_;
            syncOutlineToEditor(false);
            contextTimer_->start();
        }
    });
    connect(doc, &Document::analysisRejected, doc->editor, [this](const QString &error) {
        reportError(error);
        updateOutline();
        updateOpenSymbols();
    });
    connect(doc, &Document::backgroundError, this, &MainWindow::reportError);
    connect(doc, &Document::statusMessage, this, [this](const QString &message, int durationMs) {
        statusBar()->showMessage(message, durationMs);
    });
    connect(doc, &Document::diskReloaded, this, [this] {
        ++contextGeneration_;
        updateReferenceMarkers();
    });
    connect(doc, &Document::analysisCompleted, doc->editor, [this, doc] {
        if (!doc->analysis.error.isEmpty())
            reportError(doc->analysis.error);
        else
            doc->editor->applyStyles(doc->analysis.styles);
        if (auto *service = languageService(doc->path, snapshot(doc)))
            service->syncDocument(doc->path, snapshot(doc), static_cast<int>(doc->revision),
                                  languageById(doc->analysis.language).lspId);
        if (doc == currentDocument()) {
            updateOutline();
            updateContext();
        }
        updateOpenSymbols();
        emit analysisReady();
    });
    documents_.push_back(std::move(document));
    documentsTabs_->insertTab(index, doc->editor, QFileInfo(path).fileName());
    documentBar_->insertTab(index, QFileInfo(path).fileName());
    documentsTabs_->setCurrentWidget(doc->editor);
    documentsTabs_->setTabToolTip(documentsTabs_->indexOf(doc->editor), path);
    documentBar_->setCurrentIndex(documentsTabs_->currentIndex());
    updateTab(doc);
    doc->editor->applyConfiguration(configuration_);
    updateReferenceMarkers();
    startAnalysis(doc);
    if (focus)
        doc->editor->setFocus();
}

void MainWindow::startAnalysis(Document *doc) {
    doc->startAnalysis();
}

void MainWindow::updateOutline() {
    const bool results = referenceResultsPage_ && documentsTabs_->currentWidget() == referenceResultsPage_;
    if (results) {
        if (!showingReferenceOutline_) {
            rememberOutline();
            sourceOutlineFilter_ = outlineFilter_->text();
            const QSignalBlocker filterBlock(outlineFilter_);
            outlineFilter_->setText(referenceOutlineFilter_);
            showingReferenceOutline_ = true;
        }
        updateSearchOutline();
        return;
    }
    if (showingReferenceOutline_) {
        referenceOutlineFilter_ = outlineFilter_->text();
        const QSignalBlocker filterBlock(outlineFilter_);
        outlineFilter_->setText(sourceOutlineFilter_);
        showingReferenceOutline_ = false;
        outline_->setContextMenuPolicy(Qt::DefaultContextMenu);
    }
    rememberOutline();
    const QSignalBlocker block(outline_);
    auto *doc = currentDocument();
    if (!doc) {
        outline_->clear();
        outlineDocument_.clear();
        outlineTitle_->setText("Source");
        outlineState_->setText("Open a source file");
        return;
    }
    outlineTitle_->setText(QFileInfo(doc->path).fileName() +
                           (doc->editor->isModified() ? " *" : ""));
    if (doc->revision != doc->analyzedRevision || doc->isAnalysisRunning()) {
        if (outlineDocument_ != doc->path)
            outline_->clear();
        outlineState_->setText("Parsing unsaved buffer...");
        outline_->setToolTip(outlineState_->text());
        return;
    }
    if (!doc->analysis.error.isEmpty()) {
        outlineState_->setText(doc->analysis.error);
        return;
    }
    updatingOutline_ = true;
    outline_->clear();
    outlineDocument_ = doc->path;
    QVector<QTreeWidgetItem *> items;
    QHash<QString, int> duplicates;
    for (int i = 0; i < doc->analysis.symbols.size(); ++i) {
        const auto &symbol = doc->analysis.symbols[i];
        auto *item = new QTreeWidgetItem({symbol.name});
        item->setData(0, SymbolRole, i);
        item->setData(0, KindRole, symbol.kind);
        item->setData(0, LineRole, symbol.line);
        item->setIcon(0, UiAppearance::icon(symbol.kind));
        item->setToolTip(0, QString("%1 - line %2").arg(symbol.kind).arg(symbol.line + 1));
        QString key;
        if (symbol.parent >= 0 && symbol.parent < items.size()) {
            items[symbol.parent]->addChild(item);
            key = items[symbol.parent]->data(0, KeyRole).toString() + '/';
        } else {
            outline_->addTopLevelItem(item);
        }
        key += symbol.kind + ':' + symbol.name;
        key += ':' + QString::number(duplicates[key]++);
        item->setData(0, KeyRole, key);
        const auto source = QByteArrayView(snapshot(doc));
        const int lines =
            symbol.kind == "Function"
                ? static_cast<int>(
                      source.mid(symbol.start, symbol.end - symbol.start).count('\n')) +
                      1
                : 0;
        if (symbol.kind == "Type" ||
            (symbol.kind == "Function" && configuration_.outline.longFunctionLines > 0 &&
             lines >= configuration_.outline.longFunctionLines)) {
            auto font = item->font(0);
            font.setBold(true);
            item->setFont(0, font);
        }
        items.push_back(item);
        item->setExpanded(!doc->collapsedOutlineKeys.contains(key));
        if (key == doc->selectedOutlineKey)
            outline_->setCurrentItem(item);
    }
    sortOutline(outlineSort_);
    outlineState_->setText(
        !doc->analysis.supported     ? "Plain text: no outline grammar for this file."
        : doc->analysis.syntaxErrors ? "Tree-sitter: incomplete syntax; outline is best effort."
                                     : QString("%1 symbols - syntax only").arg(items.size()));
    outline_->setToolTip(outlineState_->text());
    filterOutline();
    outline_->verticalScrollBar()->setValue(doc->outlineScroll);
    updatingOutline_ = false;
    syncOutlineToEditor(false);
}

void MainWindow::syncOutlineToEditor(bool clearWhenOutside) {
    auto *doc = currentDocument();
    if (!doc || showingReferenceOutline_ || outline_->hasFocus() || outlineFilter_->hasFocus())
        return;
    doc->outlineSelectionPending = doc->outlineSelectionPending || clearWhenOutside;
    if (outlineDocument_ != doc->path || doc->revision != doc->analyzedRevision ||
        doc->isAnalysisRunning() || !doc->analysis.error.isEmpty())
        return;
    clearWhenOutside = doc->outlineSelectionPending;
    doc->outlineSelectionPending = false;
    const int position = static_cast<int>(doc->editor->send(SCI_GETSELECTIONSTART));
    QTreeWidgetItem *best = nullptr;
    const SourceSymbol *bestSymbol = nullptr;
    bool bestName = false;
    for (QTreeWidgetItemIterator it(outline_); *it; ++it) {
        auto *item = *it;
        bool hidden = false;
        for (auto *ancestor = item; ancestor; ancestor = ancestor->parent())
            hidden = hidden || ancestor->isHidden();
        const int index = item->data(0, SymbolRole).toInt();
        if (hidden || index < 0 || index >= doc->analysis.symbols.size())
            continue;
        const auto &symbol = doc->analysis.symbols[index];
        if (position < symbol.start || position >= symbol.end)
            continue;
        const bool name = position >= symbol.nameStart && position < symbol.nameEnd;
        if (!bestSymbol || (name && !bestName) ||
            (name == bestName && symbol.end - symbol.start < bestSymbol->end - bestSymbol->start)) {
            best = item;
            bestSymbol = &symbol;
            bestName = name;
        }
    }
    if (!best && !clearWhenOutside)
        return;
    const QSignalBlocker block(outline_);
    outline_->clearSelection();
    outline_->setCurrentItem(best);
    doc->selectedOutlineKey = best ? best->data(0, KeyRole).toString() : QString{};
    if (best) {
        for (auto *parent = best->parent(); parent; parent = parent->parent()) {
            parent->setExpanded(true);
            doc->collapsedOutlineKeys.remove(parent->data(0, KeyRole).toString());
        }
        outline_->scrollToItem(best);
        doc->outlineScroll = outline_->verticalScrollBar()->value();
    }
}

void MainWindow::clearPanelFilter(QLineEdit *filter) {
    if (filter == symbolFilter_)
        symbolNavigationAfterFilter_.reset();
    if (filter == outlineFilter_) {
        sourceOutlineFilter_.clear();
        referenceOutlineFilter_.clear();
    }
    if (filter->text().isEmpty())
        return;
    if (filter == symbolFilter_)
        revealSelectedSymbol_ = true;
    filter->clear();
    auto *tree = filter == outlineFilter_ ? outline_ : filter == fileFilter_ ? fileList_ : symbolList_;
    if (auto *selected = tree->currentItem(); selected && !selected->isHidden())
        tree->scrollToItem(selected);
    if (filter == outlineFilter_ && activeEditor() && activeEditor()->hasFocus())
        syncOutlineToEditor();
}

void MainWindow::rememberOutline() {
    if (showingReferenceOutline_ || updatingOutline_ || outlineDocument_.isEmpty())
        return;
    auto *doc = findDocument(outlineDocument_);
    if (!doc)
        return;
    for (QTreeWidgetItemIterator it(outline_); *it; ++it) {
        const auto key = (*it)->data(0, KeyRole).toString();
        if ((*it)->childCount() > 0) {
            if ((*it)->isExpanded())
                doc->collapsedOutlineKeys.remove(key);
            else
                doc->collapsedOutlineKeys.insert(key);
        }
    }
    if (auto *item = outline_->currentItem())
        doc->selectedOutlineKey = item->data(0, KeyRole).toString();
    doc->outlineScroll = outline_->verticalScrollBar()->value();
}

void MainWindow::sortOutline(int mode) {
    if (showingReferenceOutline_)
        return;
    outlineSort_ = mode;
    const auto sort = [&](auto &&self, QTreeWidgetItem *parent) -> void {
        auto children = parent->takeChildren();
        std::stable_sort(children.begin(), children.end(), [mode](auto *a, auto *b) {
            if (mode == 1)
                return a->text(0).compare(b->text(0), Qt::CaseInsensitive) < 0;
            if (mode == 2 && a->data(0, KindRole) != b->data(0, KindRole))
                return a->data(0, KindRole).toString() < b->data(0, KindRole).toString();
            if (a->data(0, LineRole) != b->data(0, LineRole))
                return a->data(0, LineRole).toInt() < b->data(0, LineRole).toInt();
            return a->data(0, SymbolRole).toInt() < b->data(0, SymbolRole).toInt();
        });
        parent->addChildren(children);
        for (auto *child : children)
            self(self, child);
    };
    const QSignalBlocker block(outline_);
    // Detaching items loses view expansion/selection; restore both after sorting.
    QSet<QString> expanded;
    const auto selected =
        outline_->currentItem() ? outline_->currentItem()->data(0, KeyRole).toString() : QString{};
    for (QTreeWidgetItemIterator it(outline_); *it; ++it)
        if ((*it)->isExpanded())
            expanded.insert((*it)->data(0, KeyRole).toString());
    sort(sort, outline_->invisibleRootItem());
    for (QTreeWidgetItemIterator it(outline_); *it; ++it) {
        const auto key = (*it)->data(0, KeyRole).toString();
        (*it)->setExpanded(expanded.contains(key));
        if (key == selected)
            outline_->setCurrentItem(*it);
    }
}

void MainWindow::filterOutline() {
    const auto filter = outlineFilter_->text();
    const auto visit = [&](auto &&self, QTreeWidgetItem *item) -> bool {
        bool matches = fragmentsMatch(item->text(0), filter);
        bool allowed = true;
        const auto *document = currentDocument();
        const int index = item->data(0, SymbolRole).toInt();
        if (document && index >= 0 && index < document->analysis.symbols.size()) {
            const auto &symbol = document->analysis.symbols[index];
            allowed = (configuration_.outline.locals || !symbol.local) &&
                      (configuration_.outline.enumerators || symbol.kind != "Constant") &&
                      (configuration_.outline.members ||
                       (symbol.kind != "Field" &&
                        !(symbol.parent >= 0 &&
                          document->analysis.symbols[symbol.parent].kind == "Type")));
        }
        for (int i = 0; i < item->childCount(); ++i)
            matches = self(self, item->child(i)) || matches;
        matches = matches && allowed;
        item->setHidden(!matches);
        return matches;
    };
    for (int i = 0; i < outline_->topLevelItemCount(); ++i)
        visit(visit, outline_->topLevelItem(i));
}

void MainWindow::updateOpenSymbols() {
    SymbolQuery query;
    query.text = symbolFilter_->text();
    query.category = category_;
    query.overlays = liveOverlays();
    query.limit = std::numeric_limits<int>::max();
    query.includeCategorySymbols = true;
    workspaceIndex_->search(query);
}

void MainWindow::filterSymbols() {
    symbolFilterPending_ = true;
    symbolNavigationAfterFilter_.reset();
    workspaceIndex_->invalidateSearch();
    symbolQueryTimer_->start();
}

void MainWindow::selectCategory(const QString &kind) {
    category_ = kind;
    symbolFilter_->setToolTip(kind.isEmpty() ? "All workspace symbol kinds."
                                             : "Filtering workspace " + kind + " symbols.");
    filterSymbols();
    browserTabs_->setCurrentIndex(1);
}

void MainWindow::previewSymbol(int index) {
    auto *doc = currentDocument();
    if (pinContextAction_->isChecked() || !doc || doc->revision != doc->analyzedRevision ||
        index < 0 || index >= doc->analysis.symbols.size())
        return;
    contextTimer_->stop();
    contextFollowsEditor_ = false;
    const auto &symbol = doc->analysis.symbols[index];
    if (symbol.kind == "Include" && !doc->path.startsWith(":/")) {
        const auto source = doc->editor->bytes();
        const auto text = source.mid(symbol.start, symbol.end - symbol.start);
        auto delimiter = text.indexOf('"');
        if (delimiter < 0)
            delimiter = text.indexOf('<');
        if (delimiter >= 0) {
            resolveDefinition(doc->path, source, static_cast<int>(doc->revision),
                              symbol.start + static_cast<int>(delimiter) + 1);
            return;
        }
    }
    contextPath_ = doc->path;
    showContext(symbol.name + "   " + symbol.kind + " in " + QFileInfo(doc->path).fileName() +
                    " at line " + QString::number(symbol.line + 1),
                doc->editor->bytes(), doc->analysis, symbol.start, symbol.end);
}

void MainWindow::updateContext() {
    if (pinContextAction_->isChecked() || !contextFollowsEditor_ ||
        configuration_.context.tracking == "off")
        return;
    if (referenceResultsPage_ && documentsTabs_->currentWidget() == referenceResultsPage_) {
        if (referencesList_->currentItem())
            activateReferenceResult(false);
        else
            selectSearchQuery();
        return;
    }
    auto *doc = currentDocument();
    if (!doc) {
        contextPath_.clear();
        showContext("Context", {}, {}, 0, 0);
        return;
    }
    const int position = doc->editor->lookupPosition();
    if (doc->analyzedRevision == doc->revision) {
        if (configuration_.context.tracking == "enclosing") {
            for (int i = static_cast<int>(doc->analysis.symbols.size()) - 1; i >= 0; --i) {
                const auto &symbol = doc->analysis.symbols[i];
                if (symbol.kind == "Function" && symbol.start <= position &&
                    symbol.end >= position) {
                    contextPath_ = doc->path;
                    showContext(symbol.name + " - enclosing function", snapshot(doc), doc->analysis,
                                symbol.start, symbol.end);
                    return;
                }
            }
        }
        if (position >= 0 && position < doc->analysis.styles.size()) {
            const auto style = static_cast<TextStyle>(doc->analysis.styles[position]);
            if ((!configuration_.context.comments && style == TextStyle::Comment) ||
                (!configuration_.context.strings && style == TextStyle::String))
                return;
        }
    }
    resolveDefinition(doc->path, snapshot(doc), static_cast<int>(doc->revision), position);
}

void MainWindow::showContext(const QString &title, const QByteArray &bytes,
                             const SourceAnalysis &analysis, int start, int end, bool referenceLine) {
    ++contextGeneration_;
    pendingPreviewPath_.clear();
    definitionChoices_->hide();
    contextTitle_->setText(title);
    contextTitle_->setToolTip(title);
    contextStart_ = start;
    contextEnd_ = end;
    contextReferenceLine_ = referenceLine;
    contextOpen_->setEnabled(!contextPath_.isEmpty());
    if (!navigatingContext_ && !contextPath_.isEmpty()) {
        const bool duplicate = contextHistoryIndex_ >= 0 &&
                               contextHistory_[contextHistoryIndex_].path == contextPath_ &&
                               contextHistory_[contextHistoryIndex_].start == start &&
                               contextHistory_[contextHistoryIndex_].end == end &&
                               contextHistory_[contextHistoryIndex_].referenceLine == referenceLine &&
                               contextHistory_[contextHistoryIndex_].bytes == bytes;
        if (!duplicate) {
            contextHistory_.resize(contextHistoryIndex_ + 1);
            contextHistory_.push_back({title, contextPath_, bytes, analysis, start, end, referenceLine});
            qsizetype historyBytes = 0;
            for (const auto &entry : contextHistory_)
                historyBytes += entry.bytes.size() + entry.analysis.styles.size();
            while (contextHistory_.size() > 1 &&
                   (historyBytes > RuntimeLimits::ContextHistoryBytes ||
                    contextHistory_.size() > RuntimeLimits::ContextHistoryEntries)) {
                historyBytes -= contextHistory_.front().bytes.size() +
                                contextHistory_.front().analysis.styles.size();
                contextHistory_.removeFirst();
            }
            contextHistoryIndex_ = static_cast<int>(contextHistory_.size()) - 1;
        }
    }
    contextBack_->setEnabled(!pinContextAction_->isChecked() && contextHistoryIndex_ > 0);
    contextForward_->setEnabled(!pinContextAction_->isChecked() &&
                                contextHistoryIndex_ + 1 < contextHistory_.size());
    if (contextPreviewEditor_->bytes() != bytes)
        contextPreviewEditor_->loadBytes(bytes);
    if (analysis.styles.size() == bytes.size())
        contextPreviewEditor_->applyStyles(analysis.styles);
    contextPreviewEditor_->selectRange(start, start);
    for (const auto &symbol : analysis.symbols) {
        if (symbol.start == start && symbol.end == end && !symbol.outlineOnly) {
            contextPreviewEditor_->selectRange(symbol.nameStart, symbol.nameEnd);
            break;
        }
    }
    if (referenceLine)
        contextPreviewEditor_->selectLineAt(start);
    const auto firstLine =
        contextPreviewEditor_->send(SCI_LINEFROMPOSITION, static_cast<uptr_t>(start));
    contextPreviewEditor_->send(SCI_SETFIRSTVISIBLELINE,
                                static_cast<uptr_t>(std::max<sptr_t>(0, firstLine - 2)));
}

void MainWindow::navigateContext(int direction) {
    const int index = contextHistoryIndex_ + direction;
    if (pinContextAction_->isChecked() || index < 0 || index >= contextHistory_.size())
        return;
    contextTimer_->stop();
    contextHistoryIndex_ = index;
    const auto entry = contextHistory_[index];
    contextPath_ = entry.path;
    navigatingContext_ = true;
    showContext(entry.title, entry.bytes, entry.analysis, entry.start, entry.end, entry.referenceLine);
    navigatingContext_ = false;
}

void MainWindow::openContext() {
    if (contextPath_.isEmpty())
        return;
    const auto path = contextPath_;
    const auto bytes = contextPreviewEditor_->bytes();
    const int start = contextStart_, end = contextEnd_;
    NavigationLocation location{path, start, end, -1,
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()};
    location.wholeLine = contextReferenceLine_;
    navigateTo(location, true, true);
}

void MainWindow::previewFile(const QString &path) {
    if (pinContextAction_->isChecked())
        return;
    contextTimer_->stop();
    contextFollowsEditor_ = false;
    hasPreviewTarget_ = false;
    pendingSemantic_ = false;
    openAfterPreview_ = false;
    definitionChoices_->hide();
    if (auto *doc = findDocument(path)) {
        contextPath_ = doc->path;
        showContext(QFileInfo(path).fileName() + " - live buffer", doc->editor->bytes(),
                    doc->analyzedRevision == doc->revision ? doc->analysis : SourceAnalysis{}, 0,
                    0);
        return;
    }
    ++contextGeneration_;
    contextPath_.clear();
    contextOpen_->setEnabled(false);
    pendingPreviewPath_ = path;
    contextTitle_->setText("Loading " + QFileInfo(path).fileName() + "...");
    contextPreviewEditor_->loadBytes({});
    startDiskPreview();
}

void MainWindow::startDiskPreview() {
    if (diskPreviewBusy_ || pendingPreviewPath_.isEmpty() || pinContextAction_->isChecked())
        return;
    diskPath_ = std::exchange(pendingPreviewPath_, {});
    diskGeneration_ = contextGeneration_;
    diskPreviewBusy_ = true;
    const auto path = diskPath_;
    const bool hasTarget = std::exchange(hasPreviewTarget_, false);
    const auto target = previewTarget_;
    const bool semantic = std::exchange(pendingSemantic_, false);
    const auto definition = pendingDefinition_;
    const auto provenance = pendingProvenance_;
    pendingProvenance_.clear();
    const bool open = std::exchange(openAfterPreview_, false);
    diskPreview_->setFuture(QtConcurrent::run([path] {
        const auto file = readSourceFile(path);
        DiskPreview result{file.bytes, {}, file.error};
        if (result.error.isEmpty()) {
            result.analysis = analyzeSource(file.bytes, path);
            result.error = result.analysis.error;
        }
        return result;
    }));
    // Captured target metadata belongs to this job, not the next queued selection.
    disconnect(diskPreview_, &QFutureWatcher<DiskPreview>::finished, this, nullptr);
    connect(
        diskPreview_, &QFutureWatcher<DiskPreview>::finished, this,
        [this, hasTarget, target, semantic, definition, provenance, open] {
            diskPreviewBusy_ = false;
            if (!pinContextAction_->isChecked() && diskGeneration_ == contextGeneration_) {
                auto result = diskPreview_->result();
                int start = 0, end = 0;
                if (result.error.isEmpty() && hasTarget) {
                    if (QCryptographicHash::hash(result.bytes, QCryptographicHash::Sha256)
                            .toHex() != target.hash)
                        result.error = "Indexed file changed; refresh the index before navigating.";
                    else {
                        start = target.symbol.start;
                        end = target.symbol.end;
                    }
                }
                if (result.error.isEmpty() && semantic) {
                    start = sourceByteOffset(result.bytes, definition.line, definition.character);
                    end =
                        sourceByteOffset(result.bytes, definition.endLine, definition.endCharacter);
                    if (start < 0 || end < start)
                        result.error = "Definition range is no longer valid; retry lookup.";
                    else {
                        for (const auto &symbol : result.analysis.symbols)
                            if (!symbol.outlineOnly && symbol.nameStart == start) {
                                start = symbol.start;
                                end = symbol.end;
                                break;
                            }
                    }
                }
                if (result.error.isEmpty()) {
                    contextPath_ = diskPath_;
                    showContext(QFileInfo(diskPath_).fileName() +
                                    (provenance.isEmpty() ? "" : " - " + provenance),
                                result.bytes, result.analysis, start, end, provenance == "Reference");
                    if (open)
                        openContext();
                } else {
                    contextPath_.clear();
                    contextOpen_->setEnabled(false);
                    contextPreviewEditor_->loadBytes({});
                    contextTitle_->setText(result.error);
                    reportError(result.error);
                    if (hasTarget)
                        workspaceIndex_->refresh();
                }
            }
            startDiskPreview();
        });
}

void MainWindow::openProject(const QString &path) {
    const auto rootPath = workspacePath(path);
    const QFileInfo folder(rootPath);
    if (!folder.isDir() || !folder.isReadable()) {
        reportError("Workspace folder is missing or unreadable: " + path);
        return;
    }
    const auto storageError = WorkspaceOptionsStore::initialize(rootPath);
    if (!storageError.isEmpty()) {
        reportError(storageError);
        return;
    }
    const auto options = WorkspaceOptionsStore::loadOptions(rootPath);
    if (!options.error.isEmpty()) {
        reportError(options.error);
        return;
    }
    const auto database = prepareCompilationDatabase(rootPath, options.compilationDirectory);
    if (!database.error.isEmpty()) {
        reportError(database.error);
        return;
    }
    const bool switchingRoot = projectPath_ != rootPath;
    if (switchingRoot && !saveProjectSession())
        return;
    projectPath_ = rootPath;
    if (persistSettings_) {
        applicationState_.lastWorkspace = projectPath_;
        const auto error = stateStore_.saveState(applicationState_);
        if (!error.isEmpty())
            reportError("Could not remember the workspace: " + error);
    }
    setWindowTitle(QFileInfo(projectPath_).fileName() + " - CodeCairn");
    if (switchingRoot) {
        cancelReferenceSearch();
        if (referencesDialog_)
            referencesDialog_->close();
        referenceGroups_.clear();
        if (referenceResultsPage_)
            rebuildReferenceResults();
        indexWarnings_.clear();
        ++contextGeneration_;
        pendingPreviewPath_.clear();
        definitionChoices_->hide();
        fileList_->clear();
        folderTree_->clear();
        symbolList_->clear();
        categoryTree_->clear();
        symbolPage_ = {};
        workspaceIndex_->open(projectPath_);
        readWorkspaceOptions();
        restartLanguageService();
        restoreProjectSession();
    } else {
        workspaceIndex_->refresh();
    }
    updateOpenSymbols();
}

void MainWindow::populateFiles(const QStringList &paths) {
    const QSignalBlocker fileBlock(fileList_), folderBlock(folderTree_);
    QMap<QString, QTreeWidgetItem *> existing;
    for (int i = 0; i < fileList_->topLevelItemCount(); ++i) {
        auto *item = fileList_->topLevelItem(i);
        existing.insert(item->data(0, PathRole).toString(), item);
    }
    if (existing.size() == paths.size() &&
        std::all_of(paths.cbegin(), paths.cend(),
                    [&](const auto &path) { return existing.contains(path); })) {
        for (const auto &path : paths)
            existing[path]->setText(2, QString::number(QFileInfo(path).size()));
        return;
    }
    const int fileScroll = fileList_->verticalScrollBar()->value();
    const int folderScroll = folderTree_->verticalScrollBar()->value();
    QSet<QString> selectedPaths, expandedPaths;
    for (auto *item : fileList_->selectedItems())
        selectedPaths.insert(item->data(0, PathRole).toString());
    for (QTreeWidgetItemIterator it(folderTree_); *it; ++it)
        if ((*it)->isExpanded())
            expandedPaths.insert((*it)->data(0, PathRole).toString());
    const auto folderSelection = folderTree_->currentItem()
                                     ? folderTree_->currentItem()->data(0, PathRole).toString()
                                     : QString{};
    const QString selected = fileList_->currentItem()
                                 ? fileList_->currentItem()->data(0, PathRole).toString()
                                 : QString{};
    fileList_->clear();
    folderTree_->clear();
    QMap<QString, QTreeWidgetItem *> folders;
    auto *root = new QTreeWidgetItem(folderTree_, {QFileInfo(projectPath_).fileName()});
    root->setIcon(0, UiAppearance::icon("Folder"));
    root->setToolTip(0, projectPath_);
    root->setData(0, PathRole, projectPath_);
    folders.insert(".", root);
    for (const auto &path : paths) {
        const QFileInfo info(path);
        const auto relative = QDir(projectPath_).relativeFilePath(info.absolutePath());
        auto *item = new QTreeWidgetItem(fileList_,
                                         {info.fileName(), relative, QString::number(info.size())});
        item->setData(0, PathRole, path);
        item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        item->setToolTip(0, path);
        if (path == selected)
            fileList_->setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
        item->setSelected(selectedPaths.contains(path));
        auto *parent = root;
        QString current;
        if (relative != ".") {
            for (const auto &part : relative.split('/')) {
                current = current.isEmpty() ? part : current + '/' + part;
                if (!folders.contains(current)) {
                    auto *directory = new QTreeWidgetItem(parent, {part});
                    directory->setIcon(0, UiAppearance::icon("Folder"));
                    directory->setData(0, PathRole, QDir(projectPath_).filePath(current));
                    folders.insert(current, directory);
                }
                parent = folders[current];
            }
        }
        auto *leaf = new QTreeWidgetItem(parent, {info.fileName()});
        leaf->setIcon(0, UiAppearance::icon("file"));
        leaf->setData(0, PathRole, path);
        leaf->setToolTip(0, path);
    }
    root->setExpanded(existing.isEmpty() || expandedPaths.contains(projectPath_));
    for (QTreeWidgetItemIterator it(folderTree_); *it; ++it) {
        const auto path = (*it)->data(0, PathRole).toString();
        if (expandedPaths.contains(path))
            (*it)->setExpanded(true);
        if (path == folderSelection)
            folderTree_->setCurrentItem(*it);
    }
    filterFiles();
    fileList_->doItemsLayout();
    folderTree_->doItemsLayout();
    fileList_->verticalScrollBar()->setValue(fileScroll);
    folderTree_->verticalScrollBar()->setValue(folderScroll);
}

void MainWindow::filterFiles() {
    for (int i = 0; i < fileList_->topLevelItemCount(); ++i) {
        auto *item = fileList_->topLevelItem(i);
        item->setHidden(!fragmentsMatch(item->text(0) + ' ' + item->text(1), fileFilter_->text()));
    }
}

bool MainWindow::saveCurrent(bool saveAs) {
    auto *doc = currentDocument();
    if (!doc)
        return false;
    QString path = doc->path;
    if (saveAs || path.startsWith(":/")) {
        path = QFileDialog::getSaveFileName(this, "Save source file", QFileInfo(path).fileName());
        if (path.isEmpty())
            return false;
        if (auto *other = findDocument(path); other && other != doc) {
            qWarning().noquote() << "file.save_conflict path=" << path
                                 << "reason=Already open in another tab.";
            QMessageBox::warning(this, "Already open", "This path is open in another tab.");
            return false;
        }
    }
    if (pathIdentity(path) == doc->path && !path.startsWith(":/")) {
        const auto current = readSourceFile(path);
        if (!current.error.isEmpty() || current.bytes != doc->savedBytes ||
            current.utf8Bom != doc->hasUtf8Bom) {
            qWarning().noquote() << "file.save_conflict path=" << path
                                 << "reason=Disk contents changed or are unreadable.";
            if (QMessageBox::warning(
                    this, "File changed externally",
                    "The disk file changed or cannot be read. Overwrite it with this buffer?",
                    QMessageBox::Save | QMessageBox::Cancel,
                    QMessageBox::Cancel) != QMessageBox::Save)
                return false;
        }
    }
    const auto bytes = doc->editor->bytes();
    const auto error = writeSourceFile(path, bytes, doc->hasUtf8Bom);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, "Cannot save file", error);
        return false;
    }
    const bool pathChanged = doc->path != pathIdentity(path);
    if (pathIdentity(path) != doc->path)
        cppLanguageService_->closeDocument(doc->path);
    doc->path = pathIdentity(path);
    doc->setSavedContents(bytes, doc->hasUtf8Bom);
    doc->editor->markSaved();
    updateTab(doc);
    if (pathChanged) {
        ++doc->revision;
        startAnalysis(doc);
    }
    workspaceIndex_->refresh();
    statusBar()->showMessage("Saved " + QDir::toNativeSeparators(path), 5000);
    return true;
}

void MainWindow::updateTab(Document *doc) {
    const int index = documentsTabs_->indexOf(doc->editor);
    if (index >= 0) {
        documentsTabs_->setTabText(index, QFileInfo(doc->path).fileName() +
                                              (doc->editor->isModified() ? " *" : ""));
        documentsTabs_->setTabToolTip(index, doc->path);
        documentBar_->setTabText(index, QFileInfo(doc->path).fileName() +
                                            (doc->editor->isModified() ? " *" : ""));
        documentBar_->setTabToolTip(
            index,
            doc->path + (doc->isPreview ? "\nPreview - double-click or edit to keep open" : ""));
        documentBar_->setTabTextColor(index, doc->isPreview
                                                 ? configuration_.color("source.lineNumbers")
                                                 : configuration_.color("ui.tabForeground"));
        if (doc == currentDocument())
            outlineTitle_->setText(QFileInfo(doc->path).fileName() +
                                   (doc->editor->isModified() ? " *" : ""));
    }
}

bool MainWindow::confirmClose(Document *doc) {
    if (!doc->editor->isModified())
        return true;
    documentsTabs_->setCurrentWidget(doc->editor);
    const auto answer = QMessageBox::question(
        this, "Unsaved changes", "Save changes to " + QFileInfo(doc->path).fileName() + "?",
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    return answer == QMessageBox::Discard || (answer == QMessageBox::Save && saveCurrent());
}

void MainWindow::closeTab(int index, bool confirm) {
    if (index < 0)
        return;
    auto *widget = documentsTabs_->widget(index);
    if (widget && widget == referenceResultsPage_) {
        cancelReferenceSearch();
        auto previousGroups = std::move(referenceGroups_);
        referenceGroups_.clear();
        if (confirm && !saveProjectSession()) {
            referenceGroups_ = std::move(previousGroups);
            return;
        }
        referenceHits_.clear();
        referenceRows_.clear();
        selectedReferenceIndex_ = -1;
        selectedSearchQuery_.clear();
        referenceOutlineFilter_.clear();
        referenceIdentifier_.clear();
        referenceRequest_ = {};
        referenceOverlays_.clear();
        referenceTargets_.clear();
        referenceListedFiles_.clear();
        referencesFiles_.clear();
        referencesScope_.clear();
        referenceOriginPath_.clear();
        referenceOriginHash_.clear();
        referencePageOffset_ = 0;
        referenceSemantic_ = false;
        referenceService_.clear();
        referenceCancelled_.reset();
        updateReferenceMarkers();
        moreReferencesAction_ = nullptr;
        referenceResultsPage_ = nullptr;
        referencesList_ = nullptr;
        referencesStatus_ = nullptr;
        cancelReferenceAction_ = nullptr;
        {
            const QSignalBlocker block(documentBar_);
            documentBar_->removeTab(index);
        }
        documentsTabs_->removeTab(index);
        delete widget;
        updateStatus();
        return;
    }
    const auto it = std::find_if(documents_.begin(), documents_.end(),
                                 [widget](const auto &doc) { return doc->editor == widget; });
    if (it == documents_.end() || (confirm && !confirmClose(it->get())))
        return;
    rememberOutline();
    cppLanguageService_->closeDocument((*it)->path);
    disconnect(widget, nullptr, this, nullptr);
    (*it)->stopBackgroundWork();
    {
        const QSignalBlocker block(documentBar_);
        documentBar_->removeTab(index);
    }
    documentsTabs_->removeTab(index);
    const auto watchers = widget->findChildren<QFutureWatcherBase *>();
    for (auto *watcher : watchers)
        disconnect(watcher, nullptr, nullptr, nullptr);
    for (auto *watcher : watchers)
        watcher->waitForFinished();
    delete widget;
    documents_.erase(it);
    documentBar_->setCurrentIndex(documentsTabs_->currentIndex());
    updateOpenSymbols();
    updateContext();
}

void MainWindow::setTypography(const Typography &value) {
    const QSignalBlocker b1(sourceFontComboBox_), b2(sourceFontSizeSpinBox_),
        b3(declarationScaleSpinBox_), b4(uniformSizesCheckBox_), b5(whitespaceCheckBox_);
    sourceFontComboBox_->setCurrentFont(
        QFont(value.family.isEmpty() ? UiAppearance::sourceFont() : value.family));
    sourceFontSizeSpinBox_->setValue(value.pointSize);
    declarationScaleSpinBox_->setValue(value.declarationScale);
    uniformSizesCheckBox_->setChecked(value.uniform);
    whitespaceCheckBox_->setChecked(value.whitespace);
    applyTypography();
}

void MainWindow::applyTypography() {
    auto previous = configuration_.editor;
    auto &editor = configuration_.editor;
    QString family = sourceFontComboBox_->currentFont().family();
    if (editor.typography.family.isEmpty() && family == UiAppearance::sourceFont())
        family.clear();
    const Typography value{family, sourceFontSizeSpinBox_->value(),
                           declarationScaleSpinBox_->value(), uniformSizesCheckBox_->isChecked(),
                           whitespaceCheckBox_->isChecked()};
    editor.typography = value;
    editor.lineNumbers = lineNumbers_->isChecked();
    editor.currentLine = currentLine_->isChecked();
    configurationDirty_ = configurationDirty_ || editor != previous;
    for (const auto &doc : documents_)
        doc->editor->applyConfiguration(configuration_);
    auto contextSettings = configuration_;
    contextSettings.editor.typography.pointSize *= configuration_.context.scale;
    contextPreviewEditor_->applyConfiguration(contextSettings);
    applyReferenceAppearance();
    declarationScaleSpinBox_->setEnabled(!value.uniform);
    updateStatus();
}

void MainWindow::updateStatus() {
    updateActions();
    if (const auto *editor = activeEditor()) {
        const int position = editor->caretPosition();
        const auto eol = editor->send(SCI_GETEOLMODE) == SC_EOL_CRLF ? "CRLF" : "LF";
        metrics_->setText(QString("Line %1  Col %2    [UTF-8]")
                              .arg(editor->send(SCI_LINEFROMPOSITION, position) + 1)
                              .arg(editor->send(SCI_GETCOLUMN, position) + 1));
        formatStatus_->setText(QString("%1                  %2   ")
                                   .arg(eol)
                                   .arg(editor->send(SCI_GETOVERTYPE) ? "OVR" : "INS"));
    } else {
        metrics_->setText("CodeCairn");
        formatStatus_->clear();
    }
}

void MainWindow::reportError(const QString &message) {
    qWarning().noquote() << message;
    statusBar()->showMessage(message);
}

void MainWindow::restoreApplicationState() {
    const auto loaded = stateStore_.loadState();
    if (!loaded.state) {
        stateError_ = loaded.error;
        reportError(stateError_);
        return;
    }
    applicationState_ = *loaded.state;
    QStringList errors;
    if (!applicationState_.geometry.isEmpty() && !restoreGeometry(applicationState_.geometry))
        errors.append("Invalid saved window geometry.");
    if (!applicationState_.mainSplitter.isEmpty() &&
        !mainSplitter_->restoreState(applicationState_.mainSplitter))
        errors.append("Invalid saved main splitter state.");
    if (!applicationState_.rightSplitter.isEmpty() &&
        !rightSplitter_->restoreState(applicationState_.rightSplitter))
        errors.append("Invalid saved right splitter state.");
    browserTabs_->setCurrentIndex(applicationState_.browserTab);
    if (!errors.isEmpty()) {
        stateStore_.invalidateState();
        stateError_ = stateStore_.statePath() + ": " + errors.join(' ');
        reportError(stateError_);
    }
}

bool MainWindow::saveApplicationState() {
    if (!saveProjectSession())
        return false;
    applicationState_.geometry = saveGeometry();
    applicationState_.mainSplitter = mainSplitter_->saveState();
    applicationState_.rightSplitter = rightSplitter_->saveState();
    applicationState_.browserTab = browserTabs_->currentIndex();
    const auto error = stateStore_.saveState(applicationState_);
    if (!error.isEmpty()) {
        reportError("Could not persist application state: " + error);
        return false;
    }
    return true;
}

void MainWindow::closeEvent(QCloseEvent *event) {
    for (const auto &doc : documents_) {
        if (!confirmClose(doc.get())) {
            event->ignore();
            return;
        }
    }
    if (persistSettings_ && !saveApplicationState() &&
        QMessageBox::warning(this, "Application state",
                             "The application state could not be saved. Close without saving it?",
                             QMessageBox::Discard | QMessageBox::Cancel,
                             QMessageBox::Cancel) != QMessageBox::Discard) {
        event->ignore();
        return;
    }
    if (configurationDirty_ && !configurationFile_.path().isEmpty() && !saveConfiguration()) {
        if (QMessageBox::warning(this, "Unsaved UI configuration",
                                 configurationError_ +
                                     "\n\nClose without saving appearance changes?",
                                 QMessageBox::Discard | QMessageBox::Cancel,
                                 QMessageBox::Cancel) != QMessageBox::Discard) {
            event->ignore();
            return;
        }
    }
    event->accept();
}

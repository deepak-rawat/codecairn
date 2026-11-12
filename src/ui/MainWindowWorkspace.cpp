#include "MainWindow.h"
#include <codecairn/FileIO.h>
#include <codecairn/RuntimeLimits.h>
#include <codecairn/WorkspaceOptionsStore.h>
#include "SymbolBrowser.h"
#include "SymbolCatalogModel.h"
#include <codecairn/Diagnostics.h>

#include <QAction>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTreeWidget>
#include <QTabWidget>
#include <utility>
#include <algorithm>

void MainWindow::initializeWorkspace() {
    coreSession_ = new CoreSession(this);
    workspaceServices_ = &coreSession_->projects();
    navigationService_ = &coreSession_->navigation();
    referenceSession_ = &coreSession_->references();
    coreSession_->setSnapshotProvider([this](const QString &path) -> std::optional<QByteArray> {
        if (auto *document = findDocument(path))
            return snapshot(document);
        return {};
    });
    connect(navigationService_, &NavigationService::definitionReady, this,
            &MainWindow::handleDefinitionResult);
    connect(referenceSession_, &ReferenceSession::pageReady, this,
            [this](quint64 token, const ReferenceResults &results, bool semantic) {
                if (token == referenceToken_ && referenceResultsPage_)
                    displayReferenceHits(results, semantic);
            });
    workspaceIndex_ = workspaceServices_->index();
    workspaceIndex_->setRefreshIntervalMinutes(configuration_.behavior.workspaceRefreshMinutes);
    cppLanguageService_ = workspaceServices_->cppService();
    connect(workspaceServices_, &WorkspaceServices::serviceCreated, this,
            &MainWindow::connectLanguageService);
    symbolQueryTimer_ = new QTimer(this);
    symbolQueryTimer_->setInterval(configuration_.behavior.searchDebounceMs);
    symbolQueryTimer_->setSingleShot(true);
    connect(symbolQueryTimer_, &QTimer::timeout, this, &MainWindow::updateOpenSymbols);
    connect(workspaceIndex_, &WorkspaceIndex::symbolsReady, this, &MainWindow::displaySymbols);
    connect(workspaceIndex_, &WorkspaceIndex::failed, this, [this](const QString &message) {
        reportError(message);
        projectState_->setText(message);
        symbolScope_->setText("Index error - see status");
        offerIndexRecreation(message);
    });
    connect(workspaceIndex_, &WorkspaceIndex::catalogReady, this, [this](const QStringList &files) {
        populateFiles(files);
        projectState_->setText(QString("%1 files; indexing symbols...").arg(files.size()));
        updateOpenSymbols();
    });
    connect(workspaceIndex_, &WorkspaceIndex::batchReady, this, [this] {
        if (!workspaceIndex_->busy()) {
            symbolModel_->invalidatePages();
            categoryModel_->invalidatePages();
        }
        if (!symbolQueryTimer_->isActive())
            symbolQueryTimer_->start(qMax(1500, configuration_.behavior.searchDebounceMs));
    });
    connect(workspaceIndex_, &WorkspaceIndex::status, this,
            [this](const QString &message) { statusBar()->showMessage(message, 5000); });
    connect(workspaceIndex_, &WorkspaceIndex::busyChanged, this, [this](bool busy) {
        cancelIndex_->setEnabled(busy);
        refreshFiles_->setEnabled(!projectPath_.isEmpty());
        rebuildSymbols_->setEnabled(!projectPath_.isEmpty());
        rebuildAll_->setEnabled(!projectPath_.isEmpty());
    });
    connect(workspaceIndex_, &WorkspaceIndex::ready, this, [this](const IndexResult &result) {
        populateFiles(result.files);
        const auto text =
            QString("%1 files; %2 parsed, %3 cached%4")
                .arg(result.files.size())
                .arg(result.parsed)
                .arg(result.reused)
                .arg(result.warnings.isEmpty()
                         ? ""
                         : QString("; %1 indexing warnings").arg(result.warnings.size()));
        projectState_->setText(text);
        const auto details = result.warnings.isEmpty() ? text : result.warnings.join('\n');
        fileList_->setToolTip(largeFileCatalog_.isEmpty() ? details
                                                          : fileList_->toolTip() + '\n' + details);
        if (!result.automatic || result.parsed > 0 || result.warnings != indexWarnings_) {
            statusBar()->showMessage(text, 8000);
            for (const auto &warning : result.warnings)
                qWarning().noquote() << warning;
        }
        indexWarnings_ = result.warnings;
        reconcileOpenDocuments(result);
        if (result.changed) {
            symbolModel_->invalidatePages();
            categoryModel_->invalidatePages();
            updateOpenSymbols();
        }
        emit indexReady();
    });
    connect(cppLanguageService_, &LanguageServerClient::status, this,
            [this](const QString &message) {
                statusBar()->showMessage(message, 8000);
                contextTitle_->setToolTip(message);
            });
    connect(cppLanguageService_, &LanguageServerClient::ready, this, [this] {
        if (contextFollowsEditor_)
            contextTimer_->start();
    });
    connect(definitionChoices_, &QComboBox::activated, this, &MainWindow::chooseDefinition);
}

void MainWindow::displayDefinitionCandidates(quint64 token,
                                             const DefinitionCandidates &candidates) {
    if (token == contextGeneration_ && !pinContextAction_->isChecked()) {
        if (!candidates.error.isEmpty()) {
            reportError(candidates.error);
            contextTitle_->setText("Lookup failed: " + candidates.error);
        } else {
            fallbackTargets_ = candidates.targets;
            if (fallbackTargets_.size() == 1 && candidates.complete) {
                previewTarget(fallbackTargets_.first(),
                              "Syntax-index candidate (not compiler verified)");
            } else if (fallbackTargets_.isEmpty()) {
                contextTitle_->setText("Unresolved: " + lookupName_ +
                                       " - check project scope and compilation settings");
                contextTitle_->setToolTip(
                    "The selected project files contain no indexed definition. "
                    "Add the folders containing declarations/definitions, or configure "
                    "the project's compilation database so the language service can "
                    "resolve dependencies outside the indexed selection.");
            } else {
                const QSignalBlocker block(definitionChoices_);
                definitionChoices_->clear();
                for (const auto &entry : fallbackTargets_)
                    definitionChoices_->addItem(entry.symbol.qualifiedName + " - " + entry.path +
                                                ':' + QString::number(entry.symbol.line + 1));
                semanticChoice_ = false;
                definitionChoices_->setCurrentIndex(-1);
                definitionChoices_->setPlaceholderText("Select a candidate");
                definitionChoices_->show();
                contextTitle_->setText(
                    QString("Ambiguous syntax lookup: %1 (%2 matches%3)")
                        .arg(lookupName_)
                        .arg(candidates.total)
                        .arg(!candidates.complete
                                 ? QString("; first %1 shown")
                                       .arg(RuntimeLimits::DefinitionCandidateLimit)
                                 : ""));
            }
        }
    }
}

QVector<SymbolOverlay> MainWindow::liveOverlays() const {
    QVector<OpenSource> open;
    open.reserve(documents_.size());
    for (const auto &doc : documents_)
        open.append({doc.get(), doc->editor->isModified()});
    return captureDocumentOverlays(open, projectPath_, projectOptions_);
}

void MainWindow::displaySymbols(const SymbolPage &page) {
    const QSignalBlocker symbolsBlock(symbolList_), categoriesBlock(categoryTree_);
    const auto navigation = std::exchange(symbolNavigationAfterFilter_, std::nullopt);
    symbolFilterPending_ = false;
    if (!page.error.isEmpty()) {
        revealSelectedSymbol_ = false;
        symbolList_->clear();
        categoryTree_->clear();
        symbolPage_ = {};
        resetSymbolBrowser();
        symbolScope_->setText("Search error");
        symbolScope_->setToolTip(page.error);
        reportError(page.error);
        if (!workspaceIndex_->busy())
            offerIndexRecreation(page.error);
        return;
    }
    symbolPage_ = page;
    virtualSymbolActivation_.reset();
    virtualCategoryActivation_.reset();
    const bool symbolFocus = projectSymbolView()->hasFocus();
    const bool categoryFocus = projectCategoryView()->hasFocus();
    QSet<QString> expanded;
    for (int i = 0; virtualCategoryMode_ && i < categoryModel_->rowCount(); ++i) {
        const auto group = categoryModel_->index(i, 0);
        if (virtualCategories_->isExpanded(group))
            expanded.insert(categoryModel_->category(group));
    }
    virtualSymbolMode_ = page.total > 4096;
    int categoryTotal = 0;
    for (int count : page.categories)
        categoryTotal += count;
    virtualCategoryMode_ = categoryTotal > 4096;
    projectSymbolView()->show();
    projectCategoryView()->show();
    if (symbolFocus)
        projectSymbolView()->setFocus();
    if (categoryFocus)
        projectCategoryView()->setFocus();
    symbolList_->setVisible(!virtualSymbolMode_);
    virtualSymbols_->setVisible(virtualSymbolMode_);
    categoryTree_->setVisible(!virtualCategoryMode_);
    virtualCategories_->setVisible(virtualCategoryMode_);
    SymbolQuery query;
    query.projectFile = projectFile_;
    query.text = symbolFilter_->text();
    query.category = category_;
    query.overlays = liveOverlays();
    query.allowPendingSchema = workspaceIndex_->busy();
    if (virtualSymbolMode_)
        symbolModel_->setCatalog(projectPath_, query, page);
    else
        symbolModel_->clear();
    if (virtualCategoryMode_) {
        categoryModel_->setCatalog(projectPath_, query, page);
        for (int i = 0; i < categoryModel_->rowCount(); ++i) {
            const auto group = categoryModel_->index(i, 0);
            virtualCategories_->setExpanded(group,
                                            expanded.contains(categoryModel_->category(group)));
        }
    } else
        categoryModel_->clear();
    auto visible = page;
    if (virtualSymbolMode_)
        visible.symbols.clear();
    if (virtualCategoryMode_)
        visible.categorySymbols.clear();
    SymbolBrowser::display(symbolList_, categoryTree_, visible, projectPath_, category_);
    if (page.anchorRow >= 0 && page.anchor && selectedProjectSymbol_ &&
        page.anchor->path == selectedProjectSymbol_->path &&
        page.anchor->symbol.nameStart == selectedProjectSymbol_->symbol.nameStart &&
        page.anchor->symbol.name == selectedProjectSymbol_->symbol.name) {
        if (virtualSymbolMode_) {
            const QSignalBlocker selection(virtualSymbols_->selectionModel());
            const auto row = symbolModel_->index(page.anchorRow, 0);
            virtualSymbols_->setCurrentIndex(row);
            virtualSymbols_->scrollTo(row);
        } else if (page.anchorRow < symbolList_->topLevelItemCount()) {
            symbolList_->setCurrentItem(symbolList_->topLevelItem(page.anchorRow));
            symbolList_->scrollToItem(symbolList_->currentItem());
        }
    }
    if (revealSelectedSymbol_) {
        if (auto *selected = symbolList_->currentItem())
            symbolList_->scrollToItem(selected);
        revealSelectedSymbol_ = false;
    }
    symbolScope_->setText(QString("%1 symbols").arg(page.total));
    symbolScope_->setToolTip(
        projectPath_.isEmpty()
            ? "Open buffers; open a folder to index a workspace."
            : workspaceDatabase(projectPath_, projectFile_) +
                  "\nLive buffers override disk symbols. Syntax index with compiler recovery, "
                  "not semantic resolution.");
    if (navigation) {
        if (virtualSymbolMode_) {
            if (!virtualSymbols_->currentIndex().isValid() && symbolModel_->rowCount() > 0)
                virtualSymbols_->setCurrentIndex(symbolModel_->index(0, 0));
            navigateProjectSymbol(*navigation);
            return;
        }
        if (!symbolList_->currentItem() && symbolList_->topLevelItemCount())
            symbolList_->setCurrentItem(symbolList_->topLevelItem(0));
        activateProjectSymbol(false);
        navigateProjectSymbol(*navigation);
    }
}

void MainWindow::activateProjectSymbol(bool open) {
    if (symbolFilterPending_) {
        if (open)
            navigateProjectSymbol(true);
        return;
    }
    if (virtualSymbolMode_) {
        if (!virtualSymbols_->currentIndex().isValid())
            return;
        const auto entry = symbolModel_->entry(virtualSymbols_->currentIndex());
        if (entry) {
            virtualSymbolActivation_.reset();
            selectedProjectSymbol_ = *entry;
            activateIndexedSymbol(*entry, open);
        } else {
            virtualSymbolActivation_ = open;
            virtualSymbolActivationIndex_ = virtualSymbols_->currentIndex();
        }
        return;
    }
    const auto *item = symbolList_->currentItem();
    if (!item || (pinContextAction_->isChecked() && !open))
        return;
    const int row = item->data(0, SymbolItemRole::Index).toInt();
    if (row < 0 || row >= symbolPage_.symbols.size())
        return;
    selectedProjectSymbol_ = symbolPage_.symbols[row];
    activateIndexedSymbol(symbolPage_.symbols[row], open);
}

void MainWindow::activateCategorySymbol(bool open) {
    if (virtualCategoryMode_) {
        const auto entry = categoryModel_->entry(virtualCategories_->currentIndex());
        if (entry) {
            virtualCategoryActivation_.reset();
            activateIndexedSymbol(*entry, open);
        } else if (virtualCategories_->currentIndex().parent().isValid()) {
            virtualCategoryActivation_ = open;
            virtualCategoryActivationIndex_ = virtualCategories_->currentIndex();
        }
        return;
    }
    const auto *item = categoryTree_->currentItem();
    if (!item || !item->parent())
        return;
    const int row = item->data(0, SymbolItemRole::Index).toInt();
    if (row < 0 || row >= symbolPage_.categorySymbols.size())
        return;
    activateIndexedSymbol(symbolPage_.categorySymbols[row], open);
}

QTreeView *MainWindow::projectSymbolView() const {
    return virtualSymbolMode_ ? virtualSymbols_ : symbolList_;
}

QTreeView *MainWindow::projectCategoryView() const {
    return virtualCategoryMode_ ? virtualCategories_ : categoryTree_;
}

void MainWindow::resetSymbolBrowser() {
    selectedProjectSymbol_.reset();
    virtualSymbolActivation_.reset();
    virtualCategoryActivation_.reset();
    symbolModel_->clear();
    categoryModel_->clear();
    virtualSymbols_->hide();
    virtualCategories_->hide();
    symbolList_->show();
    categoryTree_->show();
    virtualSymbolMode_ = false;
    virtualCategoryMode_ = false;
}

void MainWindow::activateIndexedSymbol(const IndexedSymbol &entry, bool open) {
    if (pinContextAction_->isChecked() && !open)
        return;
    if (open) {
        const auto start =
            pinContextAction_->isChecked() ? entry.symbol.nameStart : entry.symbol.start;
        const auto end = pinContextAction_->isChecked() ? entry.symbol.nameEnd : entry.symbol.end;
        navigateTo({entry.path, start, end, -1, entry.hash}, true, true, true, entry);
        return;
    }
    openAfterPreview_ = open;
    previewTarget(entry, "Project index");
}

void MainWindow::previewTarget(const IndexedSymbol &target, const QString &provenance) {
    if (pinContextAction_->isChecked())
        return;
    contextTimer_->stop();
    contextFollowsEditor_ = false;
    definitionChoices_->hide();
    if (auto *doc = findDocument(target.path)) {
        const auto bytes = doc->editor->bytes();
        if (QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() != target.hash) {
            contextTitle_->setText(
                "Buffer changed since this symbol result; select the updated result.");
            contextPreviewEditor_->loadBytes({});
            contextOpen_->setEnabled(false);
            contextPath_.clear();
            updateOpenSymbols();
            return;
        }
        contextPath_ = doc->path;
        showContext(target.symbol.name + " - " + provenance, bytes, doc->analysis,
                    target.symbol.start, target.symbol.end, provenance == "Reference");
        if (std::exchange(openAfterPreview_, false))
            openContext();
    } else {
        ++contextGeneration_;
        contextPath_.clear();
        contextOpen_->setEnabled(false);
        contextPreviewEditor_->loadBytes({});
        contextTitle_->setText("Loading " + target.symbol.name + "...");
        previewTarget_ = target;
        hasPreviewTarget_ = true;
        pendingSemantic_ = false;
        pendingProvenance_ = provenance;
        pendingPreviewPath_ = target.path;
        startDiskPreview();
    }
}

void MainWindow::resolveDefinition(const QString &path, const QByteArray &source, int version,
                                   int position) {
    const auto selection = contextIdentifier(source, position);
    lookupName_ = selection.name;
    ++contextGeneration_;
    const auto token = contextGeneration_;
    pendingPreviewPath_.clear();
    definitionChoices_->hide();
    openAfterPreview_ = false;
    if (lookupName_.isEmpty() || path.startsWith(":/")) {
        if (auto *doc = findDocument(path)) {
            contextPath_ = doc->path;
            showContext(QFileInfo(path).fileName(), source,
                        doc->analyzedRevision == doc->revision ? doc->analysis : SourceAnalysis{},
                        position, position);
        }
        return;
    }
    contextPath_.clear();
    contextOpen_->setEnabled(false);
    contextPreviewEditor_->loadBytes({});
    contextTitle_->setText("Resolving " + lookupName_ + "...");
    DefinitionRequest request;
    request.root = projectPath_;
    request.projectFile = projectFile_;
    request.path = path;
    request.source = source;
    request.version = version;
    request.position = position;
    request.identifier = lookupName_;
    request.identifierStart = selection.start;
    request.identifierEnd = selection.end;
    request.overlays = liveOverlays();
    for (const auto &doc : documents_)
        request.versions.insert(doc->path, static_cast<int>(doc->revision));
    if (auto *doc = findDocument(path); doc && doc->revision == doc->analyzedRevision)
        request.analysis = doc->analysis;
    request.context = true;
    navigationService_->lookup(request, token, "context");
}

void MainWindow::displayContextDefinitions(quint64 token, const DefinitionResult &result) {
    if (token != contextGeneration_ || pinContextAction_->isChecked())
        return;
    if (!result.warning.isEmpty())
        statusBar()->showMessage(result.warning + " Using syntax-index candidates.", 6000);
    if (!result.error.isEmpty()) {
        reportError(result.error);
        contextTitle_->setText("Lookup failed: " + result.error);
        return;
    }
    if (!result.semantic) {
        displayDefinitionCandidates(token, result.candidates);
        return;
    }
    lookupProvider_ = result.provider;
    const auto &definitions = result.locations;
    if (definitions.size() == 1) {
        semanticPreview(definitions.first());
    } else if (!definitions.isEmpty()) {
        semanticTargets_ = definitions;
        semanticChoice_ = true;
        const QSignalBlocker block(definitionChoices_);
        definitionChoices_->clear();
        for (const auto &target : definitions)
            definitionChoices_->addItem(target.path + ':' + QString::number(target.line + 1));
        definitionChoices_->setCurrentIndex(-1);
        definitionChoices_->setPlaceholderText("Select a definition");
        definitionChoices_->show();
        contextTitle_->setText(
            QString("%1 definitions: %2").arg(definitions.size()).arg(lookupName_));
    }
}

void MainWindow::chooseDefinition(int choice) {
    if (choice < 0 || pinContextAction_->isChecked())
        return;
    if (semanticChoice_) {
        if (choice < semanticTargets_.size())
            semanticPreview(semanticTargets_[choice]);
    } else if (choice < fallbackTargets_.size()) {
        previewTarget(fallbackTargets_[choice], "Selected syntax candidate");
    }
}

void MainWindow::semanticPreview(const LspLocation &target) {
    QString provenance = lookupProvider_;
    if (auto *doc = findDocument(target.path)) {
        const auto bytes = doc->editor->bytes();
        int start = sourceByteOffset(bytes, target.line, target.character);
        int end = sourceByteOffset(bytes, target.endLine, target.endCharacter);
        if (start < 0 || end < start) {
            contextTitle_->setText("Definition range changed; retry lookup.");
            return;
        }
        if (doc->revision == doc->analyzedRevision) {
            for (const auto &symbol : doc->analysis.symbols)
                if (!symbol.outlineOnly && symbol.nameStart == start) {
                    if (symbol.kind == "Function" && !symbol.definition)
                        provenance += " - declaration (implementation not available yet)";
                    start = symbol.start;
                    end = symbol.end;
                    break;
                }
        }
        contextPath_ = doc->path;
        showContext(QFileInfo(doc->path).fileName() + " - " + provenance, bytes,
                    doc->revision == doc->analyzedRevision ? doc->analysis : SourceAnalysis{},
                    start, end);
    } else {
        ++contextGeneration_;
        pendingSemantic_ = true;
        hasPreviewTarget_ = false;
        pendingDefinition_ = target;
        pendingProvenance_ = provenance;
        pendingPreviewPath_ = target.path;
        startDiskPreview();
    }
}

void MainWindow::refreshWorkspace(IndexRefresh mode) {
    if (!projectPath_.isEmpty()) {
        indexRecoveryOffered_ = false;
        workspaceIndex_->refresh(mode);
    }
}

void MainWindow::reconcileOpenDocuments(const IndexResult &result) {
    for (const auto &owned : documents_) {
        auto *doc = owned.get();
        if (!pathInWorkspace(doc->path, projectPath_))
            continue;
        doc->reconcileDiskHash(result.sourceHashes.value(workspacePath(doc->path)));
    }
}

void MainWindow::restartLanguageService() {
    if (projectPath_.isEmpty())
        return;
    ++contextGeneration_;
    ++navigationGeneration_;
    ++linkGeneration_;
    workspaceServices_->restartLanguageServices(projectPath_, compilationDirectory_, projectFile_);
    navigationService_->cancelLookups();
    navigationService_->cancelRead();
    synchronizeLanguageDocuments();
}

void MainWindow::synchronizeLanguageDocuments() {
    for (const auto &doc : documents_)
        doc->setCompilationDirectory(projectContains(doc->path)
                                         ? workspaceClangdDirectory(projectPath_, projectFile_)
                                         : QString{});
    for (const auto &doc : documents_)
        if (auto *service = languageService(doc->path, snapshot(doc.get())))
            service->syncDocument(doc->path, snapshot(doc.get()), static_cast<int>(doc->revision));
}

void MainWindow::readWorkspaceOptions() {
    const auto error = workspaceServices_->reloadProjectOptions();
    if (!error.isEmpty()) {
        reportError(error);
        return;
    }
    projectOptions_ = workspaceServices_->project().options;
    compilationDirectory_ = projectOptions_.compilationDirectory;
}

bool MainWindow::writeWorkspaceOptions(const QStringList &excludedDirectories) {
    const auto error = WorkspaceOptionsStore::saveOptions(projectPath_, compilationDirectory_,
                                                          excludedDirectories, projectFile_);
    if (!error.isEmpty()) {
        reportError(error);
        return false;
    }
    readWorkspaceOptions();
    return true;
}

bool MainWindow::saveProjectSession() {
    if (!persistSettings_ || projectPath_.isEmpty())
        return true;
    WorkspaceSession session;
    for (const auto &group : referenceGroups_) {
        auto saved = group;
        saved.hits.erase(std::remove_if(saved.hits.begin(), saved.hits.end(),
                                        [this](const auto &hit) {
                                            return !pathInWorkspace(hit.location.path,
                                                                    projectPath_);
                                        }),
                         saved.hits.end());
        session.references.append(saved);
    }
    for (int i = 0; i < documentsTabs_->count(); ++i) {
        for (const auto &doc : documents_) {
            if (doc->editor != documentsTabs_->widget(i) ||
                !pathInWorkspace(doc->path, projectPath_))
                continue;
            session.files.append({doc->path,
                                  static_cast<int>(doc->editor->send(SCI_GETSELECTIONSTART)),
                                  static_cast<int>(doc->editor->send(SCI_GETSELECTIONEND)),
                                  static_cast<int>(doc->editor->send(SCI_GETFIRSTVISIBLELINE)),
                                  {}});
        }
    }
    const auto *active = currentDocument();
    session.activeFile =
        active && pathInWorkspace(active->path, projectPath_) ? active->path : QString{};
    const auto error = stateStore_.saveSession(projectPath_, session, projectFile_);
    if (error.isEmpty())
        return true;
    reportError(error);
    return false;
}

void MainWindow::restoreProjectSession() {
    if (!persistSettings_ || projectPath_.isEmpty())
        return;
    const auto loaded = stateStore_.loadSession(projectPath_, projectFile_);
    if (!loaded.session) {
        reportError(loaded.error);
        return;
    }
    const auto &session = *loaded.session;
    referenceGroups_ = session.references;
    if (!referenceGroups_.isEmpty()) {
        const bool existingPage = referenceResultsPage_ != nullptr;
        ensureReferenceResults();
        if (existingPage)
            rebuildReferenceResults();
        referencesStatus_->setText(
            "Restored search history. Source links are validated before navigation.");
    }
    QStringList warnings;
    for (const auto &location : session.files) {
        const auto path = location.path;
        if (path.isEmpty() || !pathInWorkspace(path, projectPath_)) {
            warnings.append("Invalid saved project file: " + path);
            continue;
        }
        auto *doc = findDocument(path);
        if (!doc) {
            const auto file = readSourceFile(path);
            if (!file.error.isEmpty()) {
                warnings.append("Could not restore " + path + ": " + file.error);
                continue;
            }
            addDocument(path, file.bytes, file.utf8Bom, false);
            doc = findDocument(path);
        }
        const int length = static_cast<int>(doc->editor->send(SCI_GETLENGTH));
        const int start = qBound(0, location.start, length);
        doc->editor->selectRange(start, qBound(start, location.end, length));
        doc->editor->send(SCI_SETFIRSTVISIBLELINE, qMax(0, location.firstVisibleLine));
        pinDocument(doc);
    }
    if (auto *active = findDocument(session.activeFile))
        documentsTabs_->setCurrentWidget(active->editor);
    if (!warnings.isEmpty())
        reportError(warnings.join('\n'));
}

bool MainWindow::closeProject() {
    for (const auto &doc : documents_)
        if (!confirmClose(doc.get()))
            return false;
    if (!saveProjectSession())
        return false;
    if (persistSettings_) {
        auto state = applicationState_;
        state.lastWorkspace.clear();
        state.lastProjectFile.clear();
        const auto error = stateStore_.saveState(state);
        if (!error.isEmpty()) {
            reportError("Could not remember that the project was closed: " + error);
            return false;
        }
        applicationState_ = state;
    }
    if (referencesDialog_)
        referencesDialog_->close();
    navigationService_->cancelLookups();
    navigationService_->cancelRead();
    coreSession_->closeProject();
    while (!documents_.empty())
        closeTab(documentsTabs_->indexOf(documents_.back()->editor), false);
    if (referenceResultsPage_)
        closeTab(documentsTabs_->indexOf(referenceResultsPage_), false);
    referenceGroups_.clear();
    referenceHits_.clear();
    referenceRows_.clear();
    selectedReferenceIndex_ = -1;
    cancelReferenceSearch();
    projectPath_.clear();
    projectFile_.clear();
    projectName_.clear();
    projectOptions_ = {};
    largeFileCatalog_.clear();
    folderFiles_.clear();
    moreFiles_->setEnabled(false);
    compilationDirectory_.clear();
    indexWarnings_.clear();
    ++contextGeneration_;
    ++navigationGeneration_;
    pendingOpen_.reset();
    pendingPreviewPath_.clear();
    workspaceIndex_->invalidateSearch();
    contextTimer_->stop();
    symbolQueryTimer_->stop();
    fileList_->clear();
    folderTree_->clear();
    symbolList_->clear();
    categoryTree_->clear();
    symbolPage_ = {};
    resetSymbolBrowser();
    symbolScope_->setText("No workspace");
    contextPath_.clear();
    contextPreviewEditor_->loadBytes({});
    setWindowTitle("CodeCairn");
    statusBar()->clearMessage();
    updateStatus();
    return true;
}

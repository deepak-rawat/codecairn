#include "MainWindow.h"
#include "FileIO.h"
#include "RuntimeLimits.h"
#include "WorkspaceOptionsStore.h"
#include "SymbolBrowser.h"

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
    workspaceServices_ = new WorkspaceServices(this);
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
        fileList_->setToolTip(result.warnings.isEmpty() ? text : result.warnings.join('\n'));
        if (!result.automatic || result.parsed > 0 || result.warnings != indexWarnings_) {
            statusBar()->showMessage(text, 8000);
            for (const auto &warning : result.warnings)
                qWarning().noquote() << warning;
        }
        indexWarnings_ = result.warnings;
        reconcileOpenDocuments(result);
        if (result.changed)
            updateOpenSymbols();
        emit indexReady();
    });
    connect(cppLanguageService_, &LanguageServerClient::status, this,
            [this](const QString &message) {
                statusBar()->showMessage(message, 8000);
                contextTitle_->setToolTip(message);
            });
    connect(cppLanguageService_, &LanguageServerClient::definitionReady, this,
            &MainWindow::handleDefinitions);
    connect(cppLanguageService_, &LanguageServerClient::ready, this, [this] {
        if (contextFollowsEditor_)
            contextTimer_->start();
    });
    definitionResolver_ = new DefinitionResolver(this);
    connect(definitionResolver_, &DefinitionResolver::candidatesReady, this,
            [this](quint64 token, const DefinitionCandidates &candidates) {
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
                                                   " - check language-service status");
                        } else {
                            const QSignalBlocker block(definitionChoices_);
                            definitionChoices_->clear();
                            for (const auto &entry : fallbackTargets_)
                                definitionChoices_->addItem(entry.symbol.qualifiedName + " - " +
                                                            entry.path + ':' +
                                                            QString::number(entry.symbol.line + 1));
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
            });
    connect(definitionChoices_, &QComboBox::activated, this, &MainWindow::chooseDefinition);
}

QVector<SymbolOverlay> MainWindow::liveOverlays() const {
    QVector<SymbolOverlay> overlays;
    for (const auto &doc : documents_) {
        if (!projectPath_.isEmpty() && !pathInWorkspace(doc->path, projectPath_))
            continue;
        overlays.append({doc->path, snapshot(doc.get()),
                         doc->analyzedRevision == doc->revision ? doc->analysis : SourceAnalysis{},
                         doc->snapshotHash});
    }
    return overlays;
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
        symbolScope_->setText("Search error");
        symbolScope_->setToolTip(page.error);
        reportError(page.error);
        return;
    }
    symbolPage_ = page;
    SymbolBrowser::display(symbolList_, categoryTree_, page, projectPath_, category_);
    if (revealSelectedSymbol_) {
        if (auto *selected = symbolList_->currentItem())
            symbolList_->scrollToItem(selected);
        revealSelectedSymbol_ = false;
    }
    symbolScope_->setText(QString("%1 symbols").arg(page.total));
    symbolScope_->setToolTip(
        projectPath_.isEmpty()
            ? "Open buffers; open a folder to index a workspace."
            : workspaceDatabase(projectPath_) +
                  "\nLive buffers override disk symbols. Syntax index, not semantic resolution.");
    if (navigation) {
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
    const auto *item = symbolList_->currentItem();
    if (!item || (pinContextAction_->isChecked() && !open))
        return;
    const int row = item->data(0, SymbolItemRole::Index).toInt();
    if (row < 0 || row >= symbolPage_.symbols.size())
        return;
    activateIndexedSymbol(symbolPage_.symbols[row], open);
}

void MainWindow::activateCategorySymbol(bool open) {
    const auto *item = categoryTree_->currentItem();
    if (!item || !item->parent())
        return;
    const int row = item->data(0, SymbolItemRole::Index).toInt();
    if (row < 0 || row >= symbolPage_.categorySymbols.size())
        return;
    activateIndexedSymbol(symbolPage_.categorySymbols[row], open);
}

void MainWindow::activateIndexedSymbol(const IndexedSymbol &entry, bool open) {
    if (pinContextAction_->isChecked() && !open)
        return;
    if (open && pinContextAction_->isChecked()) {
        const auto target = entry;
        if (openFile(target.path)) {
            auto *editor = activeEditor();
            if (QCryptographicHash::hash(editor->bytes(), QCryptographicHash::Sha256).toHex() ==
                target.hash)
                editor->selectRange(target.symbol.nameStart, target.symbol.nameEnd);
            else
                reportError(
                    "Source changed since indexing; refresh before navigating to a symbol.");
        }
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
    const auto isWord = [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u >= 128 || c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9');
    };
    int start = qBound(0, position, static_cast<int>(source.size()));
    int end = start;
    while (start > 0 && isWord(source[start - 1]))
        --start;
    while (end < source.size() && isWord(source[end]))
        ++end;
    lookupName_ = QString::fromUtf8(source.mid(start, end - start));
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
    if (auto *service = languageService(path, source)) {
        lookupProvider_ = service->name() + " definition";
        if (service == cppLanguageService_ && !service->hasCompilationDatabase())
            lookupProvider_ += " (fallback compile flags)";
        service->definition(path, source, version, position, token);
    } else
        handleDefinitions(token, {}, "No semantic provider for this language.");
}

void MainWindow::handleDefinitions(quint64 token, const QVector<LspLocation> &targets,
                                   const QString &error) {
    if (token != contextGeneration_ || pinContextAction_->isChecked())
        return;
    if (!error.isEmpty()) {
        statusBar()->showMessage(error + " Using syntax-index candidates.", 6000);
    }
    if (targets.size() == 1) {
        semanticPreview(targets.first());
    } else if (!targets.isEmpty()) {
        semanticTargets_ = targets;
        semanticChoice_ = true;
        const QSignalBlocker block(definitionChoices_);
        definitionChoices_->clear();
        for (const auto &target : targets)
            definitionChoices_->addItem(target.path + ':' + QString::number(target.line + 1));
        definitionChoices_->setCurrentIndex(-1);
        definitionChoices_->setPlaceholderText("Select a definition");
        definitionChoices_->show();
        contextTitle_->setText(QString("%1 definitions: %2").arg(targets.size()).arg(lookupName_));
    } else {
        SymbolQuery query;
        query.text = lookupName_;
        query.exact = true;
        query.limit = RuntimeLimits::DefinitionCandidateLimit;
        query.overlays = liveOverlays();
        definitionResolver_->request(projectPath_, query, token);
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

void MainWindow::refreshWorkspace(bool force, bool rescan) {
    if (!projectPath_.isEmpty())
        workspaceIndex_->refresh(force, rescan);
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
    workspaceServices_->restartLanguageServices(projectPath_, compilationDirectory_);
    for (const auto &doc : documents_)
        if (auto *service = languageService(doc->path, snapshot(doc.get())))
            service->syncDocument(doc->path, snapshot(doc.get()), static_cast<int>(doc->revision));
}

void MainWindow::readWorkspaceOptions() {
    const auto options = WorkspaceOptionsStore::loadOptions(projectPath_);
    compilationDirectory_ = options.compilationDirectory;
    if (!options.error.isEmpty())
        reportError(options.error);
    if (options.excludedDirectories)
        workspaceIndex_->setExcludes(*options.excludedDirectories);
}

void MainWindow::writeWorkspaceOptions() {
    const auto error = WorkspaceOptionsStore::saveOptions(projectPath_, compilationDirectory_,
                                                          workspaceIndex_->excludes());
    if (!error.isEmpty())
        reportError(error);
}

bool MainWindow::saveProjectSession() {
    if (!persistSettings_ || projectPath_.isEmpty())
        return true;
    WorkspaceSession session;
    for (const auto &group : referenceGroups_) {
        auto saved = group;
        saved.hits.erase(std::remove_if(saved.hits.begin(), saved.hits.end(), [this](const auto &hit) {
            return !pathInWorkspace(hit.location.path, projectPath_);
        }), saved.hits.end());
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
    const auto error = stateStore_.saveSession(projectPath_, session);
    if (error.isEmpty())
        return true;
    reportError(error);
    return false;
}

void MainWindow::restoreProjectSession() {
    if (!persistSettings_ || projectPath_.isEmpty())
        return;
    const auto loaded = stateStore_.loadSession(projectPath_);
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
        referencesStatus_->setText("Restored search history. Source links are validated before navigation.");
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
        const auto error = stateStore_.saveState(state);
        if (!error.isEmpty()) {
            reportError("Could not remember that the project was closed: " + error);
            return false;
        }
        applicationState_ = state;
    }
    if (referencesDialog_)
        referencesDialog_->close();
    workspaceIndex_->close();
    workspaceServices_->stopLanguageServices();
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
    symbolScope_->setText("No workspace");
    contextPath_.clear();
    contextPreviewEditor_->loadBytes({});
    setWindowTitle("CodeCairn");
    statusBar()->clearMessage();
    updateStatus();
    return true;
}

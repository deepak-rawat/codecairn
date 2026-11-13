#include "MainWindow.h"
#include <codecairn/CompilationDatabase.h>
#include "ProjectDialog.h"
#include <codecairn/ProjectSession.h>
#include "SymbolBrowser.h"
#include "UiAppearance.h"

#include <QAction>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QStatusBar>
#include <QSignalBlocker>
#include <QScrollBar>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <algorithm>

void MainWindow::openManagedProject(const QString &projectFile) {
    const auto loaded = WorkspaceOptionsStore::loadProject(projectFile);
    if (!loaded.error.isEmpty()) {
        reportError(loaded.error);
        return;
    }
    openWorkspace(loaded.root, workspacePath(projectFile));
}

void MainWindow::openWorkspace(const QString &root, const QString &projectFile) {
    const auto prepared = prepareProject(root, projectFile);
    if (!prepared.error.isEmpty()) {
        reportError(prepared.error);
        return;
    }
    const auto &rootPath = prepared.root;
    const auto &options = prepared.options;
    const bool switching = projectPath_ != rootPath || projectFile_ != projectFile;
    if (switching) {
        if (!projectPath_.isEmpty() && (!projectFile_.isEmpty() || !projectFile.isEmpty())) {
            if (!closeProject())
                return;
        } else if (!saveProjectSession()) {
            return;
        }
    }
    if (switching) {
        cancelReferenceSearch();
        project_->cancel(contextRequest_);
        project_->cancel(hoverRequest_);
        project_->cancel(jumpRequest_);
        engine_->cancel(sourceReadRequest_);
    }
    const codecairn::ProjectOptions startup{
        switching ? codecairn::IndexStartup::Automatic : codecairn::IndexStartup::Manual,
        switching ? codecairn::LanguageStartup::Automatic : codecairn::LanguageStartup::Manual};
    const auto opened = project_->open(prepared, startup);
    if (!opened) {
        reportError(opened.error().message);
        return;
    }
    projectPath_ = rootPath;
    projectFile_ = projectFile;
    projectOptions_ = options;
    indexRecoveryOffered_ = false;
    projectName_ = prepared.name;
    suppressAutomaticProject_ = false;
    fileLimit_ = 1000;
    rememberProject();
    setWindowTitle(projectName_ + " - CodeCairn");
    if (switching) {
        ++navigationGeneration_;
        ++linkGeneration_;
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
        largeFileCatalog_.clear();
        folderFiles_.clear();
        symbolList_->clear();
        categoryTree_->clear();
        symbolPage_ = {};
        resetSymbolBrowser();
        readWorkspaceOptions();
        synchronizeLanguageDocuments();
        restoreProjectSession();
    } else {
        readWorkspaceOptions();
        project_->refreshIndex();
    }
    updateOpenSymbols();
}

void MainWindow::newProject() {
    ProjectDialog dialog({}, {}, this, configuration_);
    while (dialog.exec() == QDialog::Accepted) {
        const auto file = dialog.projectFile();
        const auto error =
            WorkspaceOptionsStore::saveProject(file, dialog.root(), dialog.options());
        if (error.isEmpty()) {
            openManagedProject(file);
            return;
        }
        reportError(error);
        dialog.setSaveError(error);
    }
}

void MainWindow::manageProjectFiles() {
    const auto root = projectPath_;
    const auto file = projectFile_;
    if (file.isEmpty()) {
        reportError("Use New Project to create a managed file selection.");
        return;
    }
    const auto options = WorkspaceOptionsStore::loadOptions(root, file);
    if (!options.error.isEmpty()) {
        reportError(options.error);
        return;
    }
    ProjectDialog dialog(root, options, this, configuration_);
    bool closed = false;
    while (dialog.exec() == QDialog::Accepted) {
        if (!closed && (projectFile_ != file || projectPath_ != root)) {
            reportError(
                "The active project changed while editing its files. Reopen Manage Project Files.");
            return;
        }
        if (!closed && dialog.root() != root) {
            if (!closeProject())
                return;
            closed = true;
        }
        const auto error =
            WorkspaceOptionsStore::saveProject(file, dialog.root(), dialog.options());
        if (error.isEmpty()) {
            openManagedProject(file);
            return;
        }
        reportError(error);
        dialog.setSaveError(error);
    }
}

bool MainWindow::projectContains(const QString &path) const {
    return projectIncludesPath(projectPath_, path, projectOptions_);
}

void MainWindow::rememberProject() {
    applicationState_.lastWorkspace = projectPath_;
    applicationState_.lastProjectFile = projectFile_;
    auto &recent = applicationState_.recentProjects;
    recent.erase(std::remove_if(recent.begin(), recent.end(),
                                [this](const auto &entry) {
                                    return entry.projectFile == projectFile_ &&
                                           (projectFile_.isEmpty() ? entry.root == projectPath_
                                                                   : true);
                                }),
                 recent.end());
    recent.prepend({projectPath_, projectFile_, projectName_});
    if (recent.size() > 12)
        recent.resize(12);
    updateRecentProjects();
    if (persistSettings_) {
        const auto error = stateStore_.saveState(applicationState_);
        if (!error.isEmpty())
            reportError("Could not remember the project: " + error);
    }
}

void MainWindow::updateRecentProjects() {
    if (!recentProjectsMenu_)
        return;
    recentProjectsMenu_->clear();
    for (const auto &entry : applicationState_.recentProjects) {
        auto *action =
            recentProjectsMenu_->addAction(QString(entry.name).replace('&', "&&") +
                                           (entry.projectFile.isEmpty() ? " (folder)" : ""));
        action->setToolTip(entry.projectFile.isEmpty() ? entry.root : entry.projectFile);
        connect(action, &QAction::triggered, this, [this, entry] {
            if (entry.projectFile.isEmpty())
                openProject(entry.root);
            else
                openManagedProject(entry.projectFile);
        });
    }
    recentProjectsMenu_->setEnabled(!applicationState_.recentProjects.isEmpty());
}

void MainWindow::offerIndexRecreation(const QString &error) {
    const QStringList databaseErrors{"file is not a database",
                                     "database disk image is malformed",
                                     "database or disk is full",
                                     "database is locked",
                                     "database table is locked",
                                     "unable to open database file",
                                     "disk I/O error",
                                     "malformed database schema",
                                     "no such table:",
                                     "no such column:",
                                     "Unsupported workspace database version",
                                     "SQLite allocation failed",
                                     "FOREIGN KEY constraint failed",
                                     "database corruption"};
    const bool databaseFailure =
        std::any_of(databaseErrors.cbegin(), databaseErrors.cend(), [&](const auto &prefix) {
            return error.startsWith(prefix, Qt::CaseInsensitive);
        });
    if (indexRecoveryOffered_ || recreateIndexDialog_ || projectPath_.isEmpty() || !databaseFailure)
        return;
    indexRecoveryOffered_ = true;
    const auto root = projectPath_;
    const auto file = projectFile_;
    auto *dialog = new QMessageBox(
        QMessageBox::Warning, "Project index unavailable",
        error + "\n\nRecreate this project's index? The old database will be archived. "
                "Project file selections and source files will not be changed.",
        QMessageBox::Yes | QMessageBox::No, this);
    dialog->setObjectName("recreateProjectIndex");
    dialog->setDefaultButton(QMessageBox::No);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    recreateIndexDialog_ = dialog;
    connect(dialog, &QMessageBox::finished, this, [this, root, file](int result) {
        recreateIndexDialog_.clear();
        if (result != QMessageBox::Yes || root != projectPath_ || file != projectFile_)
            return;
        resetSymbolBrowser();
        const auto recreated = project_->recreateIndex();
        if (!recreated)
            reportError(recreated.error().message);
    });
    dialog->open();
}

namespace {
constexpr int DirectoryRole = Qt::UserRole + 50;
constexpr int PendingFilesRole = Qt::UserRole + 51;
}

void MainWindow::populateLargeFiles(const QStringList &paths) {
    if (largeFileCatalog_ == paths) {
        populateLargeFilePage();
        return;
    }
    const bool initial = largeFileCatalog_.isEmpty() || folderTree_->topLevelItemCount() == 0;
    largeFileCatalog_ = paths;
    folderFiles_.clear();
    const QSignalBlocker block(folderTree_);
    const int scroll = folderTree_->verticalScrollBar()->value();
    QSet<QString> expanded;
    QHash<QString, int> loaded;
    const auto *selected = folderTree_->currentItem();
    const auto selectedPath =
        selected ? selected->data(0, SymbolItemRole::Path).toString() : QString{};
    const auto selectedDirectory =
        selected ? selected->data(0, DirectoryRole).toString() : QString{};
    for (QTreeWidgetItemIterator it(folderTree_); *it; ++it) {
        const auto directory = (*it)->data(0, DirectoryRole).toString();
        if ((*it)->isExpanded())
            expanded.insert(directory);
        loaded.insert(directory, (*it)->data(0, Qt::UserRole + 52).toInt());
    }
    folderTree_->clear();
    QHash<QString, QTreeWidgetItem *> directories;
    auto *root = new QTreeWidgetItem(folderTree_, {projectName_});
    root->setData(0, DirectoryRole, projectPath_);
    root->setIcon(0, UiAppearance::icon("Folder"));
    directories.insert(projectPath_, root);
    for (const auto &path : paths) {
        const auto directory = QFileInfo(path).absolutePath();
        folderFiles_[directory].append(path);
        if (directories.contains(directory))
            continue;
        const auto relative = QDir(projectPath_).relativeFilePath(directory);
        auto *parent = root;
        QString current = projectPath_;
        for (const auto &part : relative.split('/', Qt::SkipEmptyParts)) {
            if (part == ".")
                continue;
            current = QDir(current).filePath(part);
            if (!directories.contains(current)) {
                auto *child = new QTreeWidgetItem(parent, {part});
                child->setData(0, DirectoryRole, current);
                child->setIcon(0, UiAppearance::icon("Folder"));
                directories.insert(current, child);
            }
            parent = directories.value(current);
        }
    }
    for (auto it = folderFiles_.cbegin(); it != folderFiles_.cend(); ++it) {
        auto *directory = directories.value(it.key());
        directory->setData(0, PendingFilesRole, true);
        directory->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
    }
    for (QTreeWidgetItemIterator it(folderTree_); *it; ++it) {
        const auto directory = (*it)->data(0, DirectoryRole).toString();
        if (!directory.isEmpty() && (expanded.contains(directory) || (initial && *it == root))) {
            (*it)->setExpanded(true);
            expandProjectFolder(*it);
            while ((*it)->data(0, PendingFilesRole).toBool() &&
                   (*it)->data(0, Qt::UserRole + 52).toInt() < loaded.value(directory))
                expandProjectFolder(*it);
        }
        if ((!selectedPath.isEmpty() &&
             (*it)->data(0, SymbolItemRole::Path).toString() == selectedPath) ||
            (!selectedDirectory.isEmpty() && directory == selectedDirectory))
            folderTree_->setCurrentItem(*it);
    }
    folderTree_->doItemsLayout();
    folderTree_->verticalScrollBar()->setValue(scroll);
    populateLargeFilePage();
}

void MainWindow::expandProjectFolder(QTreeWidgetItem *item) {
    if (!item || !item->data(0, PendingFilesRole).toBool())
        return;
    const QSignalBlocker block(folderTree_);
    const auto directory = item->data(0, DirectoryRole).toString();
    const auto &paths = folderFiles_[directory];
    int shown = item->data(0, Qt::UserRole + 52).toInt();
    if (shown > 0) {
        delete item->takeChild(item->childCount() - 1);
    }
    const int end = qMin(shown + 1000, static_cast<int>(paths.size()));
    for (; shown < end; ++shown) {
        auto *leaf = new QTreeWidgetItem(item, {QFileInfo(paths[shown]).fileName()});
        leaf->setData(0, SymbolItemRole::Path, paths[shown]);
        leaf->setIcon(0, UiAppearance::icon("file"));
        leaf->setToolTip(0, paths[shown]);
    }
    item->setData(0, Qt::UserRole + 52, shown);
    item->setData(0, PendingFilesRole, shown < paths.size());
    if (shown < paths.size()) {
        auto *more = new QTreeWidgetItem(item, {"Load more files..."});
        more->setData(0, Qt::UserRole + 53, true);
    }
}

void MainWindow::populateLargeFilePage() {
    if (largeFileCatalog_.isEmpty())
        return;
    const QSignalBlocker block(fileList_);
    const auto selected = fileList_->currentItem()
                              ? fileList_->currentItem()->data(0, SymbolItemRole::Path).toString()
                              : QString{};
    QSet<QString> selectedPaths;
    for (const auto *item : fileList_->selectedItems())
        selectedPaths.insert(item->data(0, SymbolItemRole::Path).toString());
    const int scroll = fileList_->verticalScrollBar()->value();
    fileList_->clear();
    const auto fragments = fileFilter_->text().split(' ', Qt::SkipEmptyParts);
    int total = 0;
    QTreeWidgetItem *selectedItem = nullptr;
    for (const auto &path : largeFileCatalog_) {
        const QFileInfo info(path);
        const auto relative = QDir(projectPath_).relativeFilePath(info.absolutePath());
        const auto searchable = info.fileName() + ' ' + relative;
        if (!std::all_of(fragments.cbegin(), fragments.cend(), [&](const auto &fragment) {
                return searchable.contains(fragment, Qt::CaseInsensitive);
            }))
            continue;
        if (++total > fileLimit_ && path != selected && !selectedPaths.contains(path))
            continue;
        auto *item = new QTreeWidgetItem(fileList_,
                                         {info.fileName(), relative, QString::number(info.size())});
        item->setData(0, SymbolItemRole::Path, path);
        item->setToolTip(0, path);
        item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        item->setSelected(selectedPaths.contains(path));
        if (path == selected) {
            fileList_->setCurrentItem(item, 0, QItemSelectionModel::NoUpdate);
            selectedItem = item;
        }
    }
    moreFiles_->setEnabled(total > fileLimit_);
    fileList_->setToolTip(QString("%1 of %2 matching files shown. Filter or use Load More Files.")
                              .arg(fileList_->topLevelItemCount())
                              .arg(total));
    fileList_->verticalScrollBar()->setValue(scroll);
    if (selectedItem)
        fileList_->scrollToItem(selectedItem, QAbstractItemView::EnsureVisible);
}

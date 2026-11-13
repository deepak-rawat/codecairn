#include "MainWindow.h"
#include "UiAppearance.h"
#include "TestStorage.h"
#include "ProjectDialog.h"
#include "SymbolCatalogModel.h"
#include <QAction>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QTreeView>
#include <QtConcurrentRun>

#include "WorkspaceIndex.h"
class StartupTests : public QObject {
    Q_OBJECT
    TestStorage storage_;
private slots:
    void initTestCase();
    void init();
    void firstLaunchIsEmptyAndHasNoSampleResource();
    void applicationIconUsesMidnightPalette();
    void defaultJsonProfileUsesEmbeddedDefaults();
    void restoresLastWorkspaceWithoutOpeningDocuments();
    void unavailableWorkspaceStartsEmpty();
    void explicitPathsOverrideLastWorkspace();
    void noProjectBypassesRestoreAndAutomaticWorkspace();
    void managedProjectsRestoreIndependentSessions();
    void managedBaseFolderChangeCanSaveAndReopenSession();
    void newProjectStartsBlankAndSavesOutsideSourceTree();
    void corruptIndexOffersRecreation();
    void largeCatalogIsBrowsableDuringIndexing();
    void cachedWorkspacePopulatesSymbols();
    void externalWorkspaceStartup();
    void latestWorkspaceIsRemembered();
    void persistenceDisabledDoesNotRestoreOrSaveWorkspace();
    void restoresOpenFilesAndActiveSelection();
    void closeAndReopenProjectRestoresFiles();
    void shutdownWaitsForOwnedWorkers();
    void layoutStateDoesNotRewritePreferences();
    void malformedStateIsReportedAndPreserved();
    void invalidQtGeometryIsReportedAndPreserved();

private:
    QTemporaryDir settingsDirectory_;
    QString stateDirectory_;
};

void StartupTests::initTestCase() {
    QVERIFY(settingsDirectory_.isValid());
}

void StartupTests::init() {
    stateDirectory_ =
        settingsDirectory_.filePath(QString::fromLatin1(QTest::currentTestFunction()));
}

void StartupTests::firstLaunchIsEmptyAndHasNoSampleResource() {
    MainWindow window(true, nullptr, settingsDirectory_.filePath("settings.json"), stateDirectory_);
    window.openStartupPaths({});
    QCOMPARE(window.windowTitle(), QString("CodeCairn"));
    QVERIFY(!window.windowIcon().isNull());
    auto *about = window.findChild<QAction *>("about");
    QVERIFY(about);
    QCOMPARE(about->text(), QString("About CodeCairn"));
    auto *logs = window.findChild<QAction *>("openLogsFolder");
    QVERIFY(logs);
    QCOMPARE(logs->text(), QString("Open Logs Folder"));
    QVERIFY(logs->isEnabled());
    QCOMPARE(window.documentCount(), 0);
    QVERIFY(!window.activeEditor());
    QVERIFY(window.findChild<WorkspaceIndex *>()->root().isEmpty());
    QVERIFY(!window.findChild<QAction *>("openSample"));
    QVERIFY(!QFile::exists(":/fixtures/typography.cpp"));
    QVERIFY(QFile::exists(":/config/default-settings.json"));
}

void StartupTests::applicationIconUsesMidnightPalette() {
    MainWindow window(false);
    const auto icon = UiAppearance::applicationIcon();
    const QList<QSize> sizes{{16, 16}, {24, 24},   {32, 32},   {48, 48},
                             {64, 64}, {128, 128}, {256, 256}, {512, 512}};
    QCOMPARE(icon.availableSizes(), sizes);
    for (const auto &size : sizes) {
        const QImage source(QString(":/icons/codecairn-%1.png").arg(size.width()));
        QVERIFY(!source.isNull());
        QCOMPARE(source.size(), size);
        QCOMPARE(source.pixelColor(0, 0).alpha(), 0);
        const auto pixmap = window.windowIcon().pixmap(size, 1.0);
        QVERIFY(!pixmap.isNull());
        QCOMPARE(pixmap.size(), size);
    }
    const auto image = icon.pixmap(QSize(256, 256), 1.0).toImage();
    QCOMPARE(image.pixelColor(128, 30), QColor("#142438"));
    QCOMPARE(image.pixelColor(128, 74), QColor("#f1bb62"));
    QCOMPARE(image.pixelColor(128, 128), QColor("#68d4c4"));
    QCOMPARE(image.pixelColor(128, 182), QColor("#e6edf5"));
}

void StartupTests::defaultJsonProfileUsesEmbeddedDefaults() {
    const auto previousApplicationName = QCoreApplication::applicationName();
    const bool previousTestMode = QStandardPaths::isTestModeEnabled();
    auto restoreEnvironment = qScopeGuard([&] {
        QCoreApplication::setApplicationName(previousApplicationName);
        QStandardPaths::setTestModeEnabled(previousTestMode);
    });
    QCoreApplication::setApplicationName(QDir(settingsDirectory_.path()).dirName() + "-JsonOnly");
    QStandardPaths::setTestModeEnabled(true);
    const auto profilePath = defaultSettingsPath();
    QVERIFY(!QFileInfo::exists(profilePath));
    auto removeProfile = qScopeGuard([&] {
        if (QFile::exists(profilePath) && !QFile::remove(profilePath))
            qWarning("Could not remove the temporary JSON profile.");
        const auto directory = QFileInfo(profilePath).absolutePath();
        if (QDir(directory).exists() && !QDir().rmdir(directory))
            qWarning("Could not remove the temporary JSON profile directory.");
    });
    {
        MainWindow window(true, nullptr, {}, stateDirectory_);
        QCOMPARE(window.configurationPath(), profilePath);
        QCOMPARE(settingsToJson(window.configuration()), settingsToJson(defaultAppSettings()));
        QVERIFY(QFile::exists(profilePath));
    }
    SettingsFile profile(profilePath);
    const auto loaded = profile.load();
    QVERIFY2(loaded.settings.has_value(), qPrintable(loaded.error));
    QCOMPARE(settingsToJson(*loaded.settings), settingsToJson(defaultAppSettings()));
}

void StartupTests::restoresLastWorkspaceWithoutOpeningDocuments() {
    QTemporaryDir project;
    QVERIFY(writeSourceFile(project.filePath("main.c"), "int main(void) { return 0; }\n", false)
                .isEmpty());
    {
        MainWindow original(true, nullptr, settingsDirectory_.filePath("settings.json"),
                            stateDirectory_);
        QSignalSpy indexed(&original, &MainWindow::indexReady);
        original.openProject(project.path());
        QTRY_VERIFY_WITH_TIMEOUT(!indexed.isEmpty(), 10000);
    }
    MainWindow restored(true, nullptr, settingsDirectory_.filePath("settings.json"),
                        stateDirectory_);
    QSignalSpy indexed(&restored, &MainWindow::indexReady);
    restored.openStartupPaths({});
    QCOMPARE(restored.findChild<WorkspaceIndex *>()->root(), workspacePath(project.path()));
    QTRY_VERIFY_WITH_TIMEOUT(!indexed.isEmpty(), 10000);
    QCOMPARE(restored.documentCount(), 0);
    QCOMPARE(restored.findChild<WorkspaceIndex *>()->files().size(), 1);
}

void StartupTests::unavailableWorkspaceStartsEmpty() {
    const auto missing = settingsDirectory_.filePath("unavailable-project");
    ApplicationStateStore store(stateDirectory_);
    QVERIFY(store.loadState().state.has_value());
    ApplicationState state;
    state.lastWorkspace = missing;
    QVERIFY(store.saveState(state).isEmpty());
    MainWindow window(true, nullptr, settingsDirectory_.filePath("settings.json"), stateDirectory_);
    window.openStartupPaths({});
    QCOMPARE(window.documentCount(), 0);
    QVERIFY(window.findChild<WorkspaceIndex *>()->root().isEmpty());
    QVERIFY(window.statusBar()->currentMessage().contains("unavailable"));
    QVERIFY(!QFileInfo::exists(missing));
}

void StartupTests::noProjectBypassesRestoreAndAutomaticWorkspace() {
    QTemporaryDir project;
    const auto source = project.filePath("main.c");
    QVERIFY(writeSourceFile(source, "int main(void) { return 0; }\n", false).isEmpty());
    ApplicationStateStore store(stateDirectory_);
    QVERIFY(store.loadState().state.has_value());
    ApplicationState state;
    state.lastWorkspace = project.path();
    QVERIFY(store.saveState(state).isEmpty());
    MainWindow window(true, nullptr, settingsDirectory_.filePath("settings.json"), stateDirectory_);
    window.openStartupPaths({}, true);
    auto *index = window.findChild<WorkspaceIndex *>();
    QVERIFY(index->root().isEmpty());
    QVERIFY(window.openFile(source));
    QCOMPARE(window.documentCount(), 1);
    QVERIFY(index->root().isEmpty());
    QVERIFY(!QFileInfo::exists(workspaceDatabase(project.path())));
    window.openProject(project.path());
    QCOMPARE(index->root(), workspacePath(project.path()));
}

void StartupTests::managedProjectsRestoreIndependentSessions() {
    QTemporaryDir project;
    const auto firstFile = project.filePath("first.c");
    const auto secondFile = project.filePath("second.c");
    QVERIFY(writeSourceFile(firstFile, "int first(void) { return 1; }\n", false).isEmpty());
    QVERIFY(writeSourceFile(secondFile, "int second(void) { return 2; }\n", false).isEmpty());
    const auto first = project.filePath("first.cairn");
    const auto second = project.filePath("second.cairn");
    WorkspaceOptions options;
    options.managed = true;
    options.name = "First subset";
    options.includedPaths = {"first.c"};
    QVERIFY(WorkspaceOptionsStore::saveProject(first, project.path(), options).isEmpty());
    options.name = "Second subset";
    options.includedPaths = {"second.c"};
    QVERIFY(WorkspaceOptionsStore::saveProject(second, project.path(), options).isEmpty());
    {
        MainWindow window(true, nullptr, settingsDirectory_.filePath("settings.json"),
                          stateDirectory_);
        auto *index = window.findChild<WorkspaceIndex *>();
        window.openManagedProject(first);
        QTRY_VERIFY_WITH_TIMEOUT(!index->busy(), 10000);
        QCOMPARE(index->files(), QStringList{workspacePath(firstFile)});
        QCOMPARE(window.windowTitle(), QString("First subset - CodeCairn"));
        QVERIFY(window.openFile(firstFile));
        window.openManagedProject(second);
        QTRY_VERIFY_WITH_TIMEOUT(!index->busy(), 10000);
        QCOMPARE(index->files(), QStringList{workspacePath(secondFile)});
        QCOMPARE(window.documentCount(), 0);
        QVERIFY(window.openFile(secondFile));
        auto *recent = window.findChild<QMenu *>("recentProjects");
        QVERIFY(recent);
        QCOMPARE(recent->actions().size(), 2);
        QVERIFY(window.close());
    }
    MainWindow restored(true, nullptr, settingsDirectory_.filePath("settings.json"),
                        stateDirectory_);
    restored.openStartupPaths({});
    QCOMPARE(restored.windowTitle(), QString("Second subset - CodeCairn"));
    QCOMPARE(restored.documentCount(), 1);
    QCOMPARE(restored.activeEditor()->bytes(), QByteArray("int second(void) { return 2; }\n"));
    QVERIFY(restored.closeProject());
    restored.openManagedProject(first);
    QCOMPARE(restored.documentCount(), 1);
    QCOMPARE(restored.activeEditor()->bytes(), QByteArray("int first(void) { return 1; }\n"));
    QVERIFY(workspaceDatabase(project.path(), first) != workspaceDatabase(project.path(), second));
}

void StartupTests::managedBaseFolderChangeCanSaveAndReopenSession() {
    QTemporaryDir first, second, metadata;
    const auto descriptor = metadata.filePath("managed.cairn");
    const auto firstFile = first.filePath("first.c");
    const auto secondFile = second.filePath("second.c");
    QVERIFY(writeSourceFile(firstFile, "int first;\n", false).isEmpty());
    QVERIFY(writeSourceFile(secondFile, "int second;\n", false).isEmpty());
    WorkspaceOptions options;
    options.managed = true;
    options.name = "Root change";
    options.includedPaths = {"."};
    QVERIFY(WorkspaceOptionsStore::saveProject(descriptor, first.path(), options).isEmpty());
    MainWindow window(true, nullptr, settingsDirectory_.filePath("settings.json"), stateDirectory_);
    window.openManagedProject(descriptor);
    auto *index = window.findChild<WorkspaceIndex *>();
    QTRY_VERIFY_WITH_TIMEOUT(!index->busy(), 10000);
    QVERIFY(window.openFile(firstFile));
    bool changed = false;
    bool timedOut = false;
    bool closeCancelled = false;
    QString requestedRoot = second.path();
    int ticks = 0;
    QTimer driver;
    driver.setInterval(10);
    connect(&driver, &QTimer::timeout, &window, [&] {
        if (auto *message = window.findChild<QMessageBox *>(); message && message->isVisible()) {
            closeCancelled = true;
            message->done(QMessageBox::Cancel);
            return;
        }
        auto *dialog = window.findChild<ProjectDialog *>("projectDialog");
        if (!dialog)
            return;
        if (++ticks > 1000) {
            timedOut = true;
            dialog->reject();
            return;
        }
        if (!changed && !dialog->findChild<QPushButton *>("projectStopScan")->isEnabled()) {
            changed = true;
            auto *root = dialog->findChild<QLineEdit *>("projectRoot");
            root->setText(requestedRoot);
            QMetaObject::invokeMethod(root, "editingFinished", Qt::DirectConnection);
            dialog->findChild<QPushButton *>("projectAddBase")->click();
        }
        if (changed &&
            dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->isEnabled())
            dialog->accept();
    });
    driver.start();
    window.findChild<QAction *>("manageProjectFiles")->trigger();
    driver.stop();
    QVERIFY(changed && !timedOut);
    QCOMPARE(index->root(), workspacePath(second.path()));
    QCOMPARE(window.documentCount(), 0);
    QVERIFY(window.openFile(secondFile));
    QVERIFY(window.closeProject());
    window.openManagedProject(descriptor);
    QCOMPARE(window.documentCount(), 1);
    QCOMPARE(window.activeEditor()->bytes(), QByteArray("int second;\n"));
    window.activeEditor()->sends(SCI_APPENDTEXT, 8, "// edit\n");
    QVERIFY(window.activeEditor()->isModified());
    requestedRoot = first.path();
    changed = false;
    ticks = 0;
    driver.start();
    window.findChild<QAction *>("manageProjectFiles")->trigger();
    driver.stop();
    QVERIFY(closeCancelled && !timedOut);
    QCOMPARE(index->root(), workspacePath(second.path()));
    QCOMPARE(WorkspaceOptionsStore::loadProject(descriptor).root, workspacePath(second.path()));
    QVERIFY(window.activeEditor()->isModified());
    window.activeEditor()->markSaved();
    QVERIFY(window.closeProject());
}

void StartupTests::newProjectStartsBlankAndSavesOutsideSourceTree() {
    QTemporaryDir existing, project, metadata;
    QVERIFY(writeSourceFile(existing.filePath("old.c"), "int old;\n", false).isEmpty());
    QVERIFY(writeSourceFile(project.filePath("main.c"), "int main_symbol;\n", false).isEmpty());
    const auto descriptor = metadata.filePath("managed.cairn");
    MainWindow window(false);
    window.openProject(existing.path());
    bool initialized = false;
    bool blank = false;
    bool timedOut = false;
    int ticks = 0;
    QTimer driver;
    driver.setInterval(10);
    connect(&driver, &QTimer::timeout, &window, [&] {
        auto *dialog = window.findChild<ProjectDialog *>("projectDialog");
        if (!dialog)
            return;
        if (++ticks > 1000) {
            timedOut = true;
            dialog->reject();
            return;
        }
        if (!initialized) {
            blank = dialog->root().isEmpty() && dialog->projectFile().isEmpty() &&
                    dialog->options().name.isEmpty() && dialog->options().includedPaths.isEmpty();
            initialized = true;
            auto *root = dialog->findChild<QLineEdit *>("projectRoot");
            root->setText(project.path());
            QMetaObject::invokeMethod(root, "editingFinished", Qt::DirectConnection);
            dialog->findChild<QLineEdit *>("projectName")->setText("Selected project");
            dialog->findChild<QLineEdit *>("projectFile")->setText(descriptor);
            dialog->findChild<QPushButton *>("projectAddBase")->click();
        }
        if (dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->isEnabled())
            dialog->accept();
    });
    driver.start();
    window.findChild<QAction *>("newProject")->trigger();
    driver.stop();
    QVERIFY(!timedOut);
    QVERIFY(initialized);
    QVERIFY(blank);
    QVERIFY(QFileInfo::exists(descriptor));
    QCOMPARE(window.windowTitle(), QString("Selected project - CodeCairn"));
    const auto saved = WorkspaceOptionsStore::loadProject(descriptor);
    QVERIFY2(saved.error.isEmpty(), qPrintable(saved.error));
    QCOMPARE(saved.root, workspacePath(project.path()));
    QCOMPARE(saved.options.name, QString("Selected project"));
    QCOMPARE(saved.options.includedPaths, QStringList{"."});
    QCOMPARE(QDir(project.path()).entryList(QDir::Files), QStringList{"main.c"});
    QVERIFY(window.findChildren<QFileDialog *>().isEmpty());
}

void StartupTests::corruptIndexOffersRecreation() {
    QTemporaryDir project;
    QVERIFY(writeSourceFile(project.filePath("main.c"), "int seed;\n", false).isEmpty());
    QVERIFY(WorkspaceOptionsStore::initialize(project.path()).isEmpty());
    QFile database(workspaceDatabase(project.path()));
    QVERIFY(database.open(QIODevice::WriteOnly));
    QCOMPARE(database.write("not sqlite"), qint64(10));
    database.close();
    MainWindow window(false);
    window.openProject(project.path());
    QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QMessageBox *>("recreateProjectIndex"), 10000);
    auto *prompt = window.findChild<QMessageBox *>("recreateProjectIndex");
    prompt->done(QMessageBox::No);
    QVERIFY(database.open(QIODevice::ReadOnly));
    QCOMPARE(database.readAll(), QByteArray("not sqlite"));
    database.close();
    window.openProject(project.path());
    QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QMessageBox *>("recreateProjectIndex"), 10000);
    prompt = window.findChild<QMessageBox *>("recreateProjectIndex");
    prompt->done(QMessageBox::Yes);
    auto *index = window.findChild<WorkspaceIndex *>();
    QTRY_VERIFY_WITH_TIMEOUT(!index->busy(), 10000);
    SymbolQuery query;
    const auto page = queryWorkspaceSymbols(project.path(), query);
    QVERIFY2(page.error.isEmpty(), qPrintable(page.error));
    QCOMPARE(page.total, 1);
    auto *filter = window.findChild<QLineEdit *>("symbolFilter");
    filter->setText("?database[");
    QTRY_COMPARE_WITH_TIMEOUT(window.findChild<QLabel *>("symbolScope")->text(),
                              QString("Search error"), 10000);
    QVERIFY(!window.findChild<QMessageBox *>("recreateProjectIndex"));
}

void StartupTests::largeCatalogIsBrowsableDuringIndexing() {
    QTemporaryDir project;
    for (int i = 0; i < 2101; ++i)
        QVERIFY(writeSourceFile(project.filePath(QString("file_%1.c").arg(i)),
                                "int visible_symbol;\n", false)
                    .isEmpty());
    MainWindow window(false);
    auto *index = window.findChild<WorkspaceIndex *>();
    auto *files = window.findChild<QTreeWidget *>("fileList");
    bool catalogWhileBusy = false;
    bool symbolsWhileBusy = false;
    connect(index, &WorkspaceIndex::catalogReady, &window,
            [&] { catalogWhileBusy = index->busy() && files->topLevelItemCount() > 0; });
    connect(index, &WorkspaceIndex::symbolsReady, &window, [&](const SymbolPage &page) {
        symbolsWhileBusy = symbolsWhileBusy || (index->busy() && page.total > 0);
    });
    window.openProject(project.path());
    QTRY_VERIFY_WITH_TIMEOUT(catalogWhileBusy, 15000);
    QCOMPARE(index->files().size(), 2101);
    QVERIFY(files->topLevelItemCount() <= 1000);
    auto *filter = window.findChild<QLineEdit *>("fileFilter");
    filter->setText("file_2100.c");
    QCOMPARE(files->topLevelItemCount(), 1);
    QCOMPARE(files->topLevelItem(0)->text(0), QString("file_2100.c"));
    filter->clear();
    auto *more = window.findChild<QAction *>("moreFiles");
    QVERIFY(more->isEnabled());
    more->trigger();
    QCOMPARE(files->topLevelItemCount(), 2000);
    QTRY_VERIFY_WITH_TIMEOUT(!index->busy(), 30000);
    QVERIFY(symbolsWhileBusy);
    auto *symbols = window.findChild<QTreeWidget *>("symbolList");
    QTRY_COMPARE_WITH_TIMEOUT(symbols->topLevelItemCount(), 2101, 10000);
    QVERIFY(!window.findChild<QAction *>("moreSymbols"));
}

void StartupTests::explicitPathsOverrideLastWorkspace() {
    QTemporaryDir project;
    const auto path = project.filePath("explicit.cpp");
    const QByteArray source = "int explicit_target() { return 1; }\n";
    QVERIFY(writeSourceFile(path, source, false).isEmpty());
    ApplicationStateStore store(stateDirectory_);
    QVERIFY(store.loadState().state.has_value());
    ApplicationState state;
    state.lastWorkspace = settingsDirectory_.filePath("missing");
    QVERIFY(store.saveState(state).isEmpty());
    {
        MainWindow window(true, nullptr, settingsDirectory_.filePath("settings.json"),
                          stateDirectory_);
        window.openStartupPaths({project.path()});
        QCOMPARE(window.documentCount(), 0);
        QCOMPARE(window.findChild<WorkspaceIndex *>()->root(), workspacePath(project.path()));
    }
    MainWindow window(true, nullptr, settingsDirectory_.filePath("settings.json"), stateDirectory_);
    window.openStartupPaths({path});
    QCOMPARE(window.documentCount(), 1);
    QCOMPARE(window.activeEditor()->bytes(), source);
    QCOMPARE(window.findChild<WorkspaceIndex *>()->root(), workspacePath(project.path()));
}

void StartupTests::latestWorkspaceIsRemembered() {
    QTemporaryDir first, second;
    {
        MainWindow window(true, nullptr, settingsDirectory_.filePath("settings.json"),
                          stateDirectory_);
        window.openProject(first.path());
        window.openProject(second.path());
    }
    ApplicationStateStore store(stateDirectory_);
    const auto loaded = store.loadState();
    QVERIFY2(loaded.state.has_value(), qPrintable(loaded.error));
    QCOMPARE(loaded.state->lastWorkspace, workspacePath(second.path()));
}

void StartupTests::cachedWorkspacePopulatesSymbols() {
    QTemporaryDir project;
    QVERIFY(writeSourceFile(project.filePath("main.c"), "int cached_symbol(void) { return 0; }\n",
                            false)
                .isEmpty());
    const auto built =
        buildWorkspaceIndex(workspacePath(project.path()), IndexRefresh::DiscoverChanges, {},
                            std::make_shared<std::atomic_bool>(false));
    QVERIFY2(built.error.isEmpty(), qPrintable(built.error));
    MainWindow window(false);
    window.openStartupPaths({project.path() + QDir::separator()});
    auto *scope = window.findChild<QLabel *>("symbolScope");
    auto *symbols = window.findChild<QTreeWidget *>("symbolList");
    QTRY_COMPARE_WITH_TIMEOUT(scope->text(), "1 symbols", 10000);
    QCOMPARE(symbols->topLevelItemCount(), 1);
    QCOMPARE(symbols->topLevelItem(0)->text(0), "cached_symbol");
}

void StartupTests::externalWorkspaceStartup() {
    const auto root = qEnvironmentVariable("CODECAIRN_VALIDATE_WORKSPACE");
    if (root.isEmpty())
        QSKIP("Set CODECAIRN_VALIDATE_WORKSPACE to verify the real-workspace startup UI.");
    MainWindow window(false);
    window.show();
    QSignalSpy ready(&window, &MainWindow::indexReady);
    window.openStartupPaths({root + QDir::separator()});
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 30000);
    SymbolQuery all;
    all.limit = std::numeric_limits<int>::max();
    const auto expected = queryWorkspaceSymbols(root, all);
    QVERIFY2(expected.error.isEmpty(), qPrintable(expected.error));
    QVERIFY(expected.total > 0);
    auto *scope = window.findChild<QLabel *>("symbolScope");
    auto *symbols = window.findChild<QTreeWidget *>("symbolList");
    QTRY_COMPARE_WITH_TIMEOUT(scope->text(), QString("%1 symbols").arg(expected.total), 30000);
    QCOMPARE(window.findChild<WorkspaceIndex *>()->root(), workspacePath(root));
    if (expected.total > 4096)
        QCOMPARE(window.findChild<QTreeView *>("virtualSymbolList")->model()->rowCount(),
                 expected.total);
    else
        QCOMPARE(symbols->topLevelItemCount(), expected.symbols.size());
    auto *categories = window.findChild<QTreeWidget *>("categoryTree");
    auto *virtualCategories = window.findChild<QTreeView *>("virtualCategoryTree");
    QCOMPARE(expected.total > 4096 ? virtualCategories->model()->rowCount()
                                   : categories->topLevelItemCount(),
             expected.categories.size());
    int categorySymbols = 0;
    for (int i = 0; i < expected.categories.size(); ++i) {
        if (expected.total > 4096) {
            const auto group = virtualCategories->model()->index(i, 0);
            const auto name = group.data(Qt::UserRole).toString();
            QCOMPARE(virtualCategories->model()->rowCount(group), expected.categories.value(name));
            categorySymbols += virtualCategories->model()->rowCount(group);
        } else {
            const auto *category = categories->topLevelItem(i);
            const auto name = category->data(0, Qt::UserRole).toString();
            QCOMPARE(category->childCount(), expected.categories.value(name));
            QCOMPARE(category->text(0), QString("%1 (%2)").arg(name).arg(category->childCount()));
            categorySymbols += category->childCount();
        }
    }
    QCOMPARE(categorySymbols, expected.total);
    QCOMPARE(window.documentCount(), 0);
    auto targets =
        qEnvironmentVariable("CODECAIRN_VALIDATE_SYMBOLS").split(',', Qt::SkipEmptyParts);
    if (targets.isEmpty())
        targets.append(expected.symbols.last().symbol.name);
    for (const auto &target : targets) {
        window.findChild<QLineEdit *>("symbolFilter")->setText(target);
        SymbolQuery query;
        query.text = target;
        query.limit = std::numeric_limits<int>::max();
        const auto filtered = queryWorkspaceSymbols(root, query);
        QVERIFY2(filtered.error.isEmpty(), qPrintable(filtered.error));
        QVERIFY2(filtered.total > 0, qPrintable("Missing symbol: " + target));
        QTRY_COMPARE_WITH_TIMEOUT(scope->text(), QString("%1 symbols").arg(filtered.total), 10000);
        QTRY_COMPARE_WITH_TIMEOUT(symbols->topLevelItemCount(), filtered.symbols.size(), 10000);
        for (int i = 0; i < filtered.symbols.size(); ++i) {
            const auto &symbol = filtered.symbols[i].symbol;
            QTRY_COMPARE_WITH_TIMEOUT(
                symbols->topLevelItem(i)->text(0),
                symbol.qualifiedName.isEmpty() ? symbol.name : symbol.qualifiedName, 10000);
        }
        qInfo() << "Startup search for" << target << "returned" << filtered.total;
    }
    qInfo() << "Startup displayed" << expected.total << "symbols;" << expected.symbols.size()
            << "in the list";
    ready.clear();
    QTemporaryDir metadataNoise(
        QDir(workspaceStorageDirectory(root)).filePath("refresh-check-XXXXXX"));
    QVERIFY(metadataNoise.isValid());
    QTest::qWait(2500);
    QCOMPARE(ready.count(), 0);
    QVERIFY(!window.findChild<WorkspaceIndex *>()->busy());
    qInfo() << "Metadata activity did not trigger another index build";
}

void StartupTests::restoresOpenFilesAndActiveSelection() {
    QTemporaryDir project;
    const auto first = project.filePath("first.c"), second = project.filePath("second.c");
    QVERIFY(writeSourceFile(first, "int first();\n", false).isEmpty());
    QVERIFY(writeSourceFile(second, "int second();\n", false).isEmpty());
    const auto config = settingsDirectory_.filePath("restore-files.json");
    {
        MainWindow window(true, nullptr, config, stateDirectory_);
        QVERIFY(window.openFile(first));
        QVERIFY(window.openFile(second));
        QVERIFY(window.openFile(first));
        window.activeEditor()->selectRange(4, 9);
        QVERIFY(window.close());
    }
    {
        MainWindow restored(true, nullptr, config, stateDirectory_);
        restored.openStartupPaths({});
        QCOMPARE(restored.documentCount(), 2);
        QCOMPARE(restored.activeEditor()->bytes(), QByteArray("int first();\n"));
        QCOMPARE(restored.activeEditor()->send(SCI_GETSELECTIONSTART), 4);
        QCOMPARE(restored.activeEditor()->send(SCI_GETSELECTIONEND), 9);
        auto *tabs = restored.findChild<QTabWidget *>("sourceTabs");
        QCOMPARE(tabs->tabText(0), "first.c");
        QCOMPARE(tabs->tabText(1), "second.c");
    }
    QVERIFY(QFile::remove(second));
    MainWindow missing(true, nullptr, config, stateDirectory_);
    missing.openStartupPaths({project.path()});
    QCOMPARE(missing.documentCount(), 1);
    QVERIFY(missing.statusBar()->currentMessage().contains("Could not restore"));
}

void StartupTests::closeAndReopenProjectRestoresFiles() {
    QTemporaryDir project;
    const auto path = project.filePath("main.c");
    QVERIFY(writeSourceFile(path, "int main(void) { return 0; }\n", false).isEmpty());
    MainWindow window(true, nullptr, settingsDirectory_.filePath("reopen-files.json"),
                      stateDirectory_);
    QVERIFY(window.openFile(path));
    QVERIFY(window.closeProject());
    QCOMPARE(window.documentCount(), 0);
    QVERIFY(window.findChild<WorkspaceIndex *>()->root().isEmpty());
    window.openProject(project.path());
    QCOMPARE(window.documentCount(), 1);
    QVERIFY(window.activeEditor()->bytes().contains("main"));
    QVERIFY(window.closeProject());
    window.openStartupPaths({path});
    QCOMPARE(window.documentCount(), 1);
}

void StartupTests::shutdownWaitsForOwnedWorkers() {
    QTemporaryDir project;
    const auto path = project.filePath("main.c");
    QVERIFY(writeSourceFile(path, "int main(void) { return 0; }\n", false).isEmpty());
    auto *window = new MainWindow(false);
    QVERIFY(window->openFile(path));
    std::atomic_bool finished{false};
    bool delivered = false;
    auto *job = new QFutureWatcher<void>(window->activeEditor());
    connect(job, &QFutureWatcher<void>::finished, window->activeEditor(),
            [&] { delivered = true; });
    job->setFuture(QtConcurrent::run([&] {
        QThread::msleep(100);
        finished.store(true);
    }));
    QVERIFY(window->close());
    delete window;
    QVERIFY(finished.load());
    QCoreApplication::processEvents();
    QVERIFY(!delivered);
}

void StartupTests::persistenceDisabledDoesNotRestoreOrSaveWorkspace() {
    QTemporaryDir first, second;
    ApplicationStateStore store(stateDirectory_);
    QVERIFY(store.loadState().state.has_value());
    ApplicationState state;
    state.lastWorkspace = first.path();
    QVERIFY(store.saveState(state).isEmpty());
    QFile stateFile(store.statePath());
    QVERIFY(stateFile.open(QIODevice::ReadOnly));
    const auto before = stateFile.readAll();
    stateFile.close();
    MainWindow window(false, nullptr, {}, stateDirectory_);
    window.openStartupPaths({});
    QVERIFY(window.findChild<WorkspaceIndex *>()->root().isEmpty());
    window.openProject(second.path());
    QVERIFY(window.close());
    QVERIFY(stateFile.open(QIODevice::ReadOnly));
    QCOMPARE(stateFile.readAll(), before);
    QVERIFY(!QDir(QDir(stateDirectory_).filePath("sessions")).exists());
}

void StartupTests::layoutStateDoesNotRewritePreferences() {
    const auto config = settingsDirectory_.filePath("preferences.json");
    const QByteArray preferences = "{\"schemaVersion\":1,\"editor\":{\"showLineNumbers\":true}}\n";
    QVERIFY(writeSourceFile(config, preferences, false).isEmpty());
    {
        MainWindow window(true, nullptr, config, stateDirectory_);
        window.show();
        window.findChild<QTabWidget *>("browserTabs")->setCurrentIndex(3);
        QVERIFY(window.configuration().editor.lineNumbers);
        QVERIFY(window.close());
    }
    const auto unchanged = readSourceFile(config);
    QVERIFY2(unchanged.error.isEmpty(), qPrintable(unchanged.error));
    QCOMPARE(unchanged.bytes, preferences);
    ApplicationStateStore store(stateDirectory_);
    const auto state = store.loadState();
    QVERIFY2(state.state.has_value(), qPrintable(state.error));
    QCOMPARE(state.state->browserTab, 3);
    QVERIFY(!state.state->geometry.isEmpty());
    QVERIFY(!state.state->mainSplitter.isEmpty());
    QVERIFY(!state.state->rightSplitter.isEmpty());
    MainWindow restored(true, nullptr, config, stateDirectory_);
    QCOMPARE(restored.findChild<QTabWidget *>("browserTabs")->currentIndex(), 3);
    QVERIFY(restored.configuration().editor.lineNumbers);
    QVERIFY(restored.close());
    QCOMPARE(readSourceFile(config).bytes, preferences);
}

void StartupTests::malformedStateIsReportedAndPreserved() {
    QVERIFY(QDir().mkpath(stateDirectory_));
    ApplicationStateStore store(stateDirectory_);
    const QByteArray original = "{\"schemaVersion\":2}\n";
    QVERIFY(writeSourceFile(store.statePath(), original, false).isEmpty());
    QTemporaryDir project;
    QVERIFY(project.isValid());
    MainWindow window(true, nullptr, settingsDirectory_.filePath("settings.json"), stateDirectory_);
    QVERIFY(window.statusBar()->currentMessage().contains("schemaVersion"));
    window.openStartupPaths({});
    QVERIFY(window.findChild<WorkspaceIndex *>()->root().isEmpty());
    window.openProject(project.path());
    QVERIFY(!window.closeProject());
    QCOMPARE(readSourceFile(store.statePath()).bytes, original);
}

void StartupTests::invalidQtGeometryIsReportedAndPreserved() {
    ApplicationStateStore store(stateDirectory_);
    QVERIFY(store.loadState().state.has_value());
    ApplicationState state;
    state.geometry = "invalid Qt geometry";
    QVERIFY(store.saveState(state).isEmpty());
    const auto original = readSourceFile(store.statePath());
    QVERIFY2(original.error.isEmpty(), qPrintable(original.error));
    QTemporaryDir project;
    QVERIFY(project.isValid());
    MainWindow window(true, nullptr, settingsDirectory_.filePath("settings.json"), stateDirectory_);
    QVERIFY(window.statusBar()->currentMessage().contains("Invalid saved window geometry"));
    window.openProject(project.path());
    QVERIFY(!window.closeProject());
    QCOMPARE(readSourceFile(store.statePath()).bytes, original.bytes);
}

QTEST_MAIN(StartupTests)
#include "StartupTests.moc"

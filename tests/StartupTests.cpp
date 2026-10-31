#include "MainWindow.h"
#include "UiAppearance.h"
#include "TestStorage.h"
#include <QAction>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QSignalSpy>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QTabWidget>
#include <QThread>
#include <QtConcurrentRun>

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
    const QList<QSize> sizes{{16, 16}, {24, 24}, {32, 32}, {48, 48},
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
    const auto built = buildWorkspaceIndex(workspacePath(project.path()), false, true, {},
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
    QCOMPARE(symbols->topLevelItemCount(), expected.symbols.size());
    auto *categories = window.findChild<QTreeWidget *>("categoryTree");
    QCOMPARE(categories->topLevelItemCount(), expected.categories.size());
    int categorySymbols = 0;
    for (int i = 0; i < categories->topLevelItemCount(); ++i) {
        const auto *category = categories->topLevelItem(i);
        const auto name = category->data(0, Qt::UserRole).toString();
        QCOMPARE(category->childCount(), expected.categories.value(name));
        QCOMPARE(category->text(0), QString("%1 (%2)").arg(name).arg(category->childCount()));
        categorySymbols += category->childCount();
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

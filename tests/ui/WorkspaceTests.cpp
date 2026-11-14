#include <codecairn/Indexing.h>
#include "LanguageServerClient.h"
#include <codecairn/FileIO.h>
#include "MainWindow.h"
#include "TestStorage.h"
#include <codecairn/WorkspaceOptionsStore.h>
#include <codecairn/CompilationDatabase.h>
#include "CompilerSymbolRecovery.h"
#include "ProjectIndexStore.h"
#include "SymbolBrowser.h"
#include "SymbolCatalogModel.h"

#include <QAction>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QInputDialog>
#include <QTimer>
#include <QLockFile>
#include <QProcess>
#include <QSet>
#include <QSignalSpy>
#include <QStatusBar>
#include <QScrollBar>
#include <QSemaphore>
#include <QScopeGuard>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QTabWidget>
#include <QtConcurrentRun>

#include "ProjectIndexer.h"
namespace {
IndexResult index(const QString &root, bool force = false, bool rescan = true) {
    const auto mode =
        force ? (rescan ? IndexRefresh::RebuildFilesAndSymbols : IndexRefresh::ReparseKnownFiles)
              : (rescan ? IndexRefresh::DiscoverChanges : IndexRefresh::CheckKnownFiles);
    return buildWorkspaceIndex(workspacePath(root), mode, {".git", "build"},
                               std::make_shared<std::atomic_bool>(false));
}
void write(const QString &path, const QByteArray &bytes) {
    // Simulate an external tool writing in place while the editor/indexer runs.
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size())
        qFatal("Fixture write failed: %s", qPrintable(file.errorString()));
}
void compilationDatabase(const QString &root, const QStringList &files) {
    QJsonArray commands;
    for (const auto &file : files) {
        commands.append(QJsonObject{{"directory", root},
                                    {"file", QDir(root).filePath(file)},
                                    {"arguments", QJsonArray{"clang++", "-std=c++17", "-I" + root,
                                                             "-c", QDir(root).filePath(file)}}});
    }
    write(QDir(root).filePath("compile_commands.json"), QJsonDocument(commands).toJson());
}
}

class WorkspaceTests : public QObject {
    Q_OBJECT
    TestStorage storage_;
private slots:
    void persistsUnopenedSymbolsAndSkipsMetadata();
    void refreshRebuildAndDeletion();
    void dirtyOverlayReplacesDiskSymbols();
    void queryPaginationFragmentsAndRegex();
    void symbolCategoriesPersistAndOverlay();
    void cancellationPreservesCompletedIndex();
    void syntaxOnlyAnalysisPreservesSymbolsWithoutStyles();
    void unsupportedSchemaRequiresExplicitRecreation();
    void databaseRecreationArchivesAndHonorsLock();
    void managedProjectsIndexOnlySelectedMembership();
    void failedReplacementRetainsPreviousSymbols();
    void parallelParsersOverlapAndPublishWhileBusy();
    void resourceLoggingReportsStartAndCompletion();
    void initialPassUsesConfiguredExclusions();
    void pendingInitialSchemaDoesNotMaskUnsupportedDatabases();
    void corruptDatabaseIsReported();
    void missingSchemaIsNotSilentlyRecreated();
    void unwritableMetadataIsReported();
    void workspaceStorageIsIsolatedAndCoLocated();
    void invalidWorkspaceIdentityIsRejected();
    void workspaceIndexLockIsPerWorkspace();
    void readOnlySourceFolderCanBeIndexed();
    void compilerRecoveryUsesRelocatedCompilationDatabase();
    void managedCompilerRecoveryUsesOwnConfiguration();
    void managedCompilerSettingsDoNotBorrowFolderCache();
    void excludedNamesCanBeRemoved();
    void watcherRefreshAndWorkspaceSwitch();
    void automaticRefreshIgnoresMetadata();
    void unchangedAutomaticRefreshIsQuiet();
    void refreshesCoalesceWithoutCancelling();
    void indexingDoesNotWaitForEditorWorkers();
    void refreshPreservesBrowserState();
    void largeFileRefreshPreservesBrowserState();
    void editedExclusionsUpdateOpenBufferMembership();
    void externalWorkspaceIndex();
    void externalWorkspaceSymbolPreviews();
    void projectSymbolListContainsAllSymbols();
    void compilerGeneratedProjectSymbolsPreviewDefinitions();
    void utf16Conversions();
    void discoversClangdWithoutShellPath();
    void realClangdCrossFileOverloads();
    void realClangdUnsavedAndStaleRequests();
    void independentLookupChannelsAndReferences();
    void openFileIndexesParentAndPreviewsUnopenedSymbol();
    void contextShowsCalleeNotCaller();
    void missingCDefinitionDoesNotPreviewItsCallAsDefinition();
    void externalChangesReloadCleanBuffersOnly();
    void ambiguousNamesAreNotGuessed();
    void qualifiedMethodsRetainScope();
};

void WorkspaceTests::persistsUnopenedSymbolsAndSkipsMetadata() {
    QTemporaryDir dir;
    QVERIFY(QDir(dir.path()).mkpath(".git/private"));
    write(dir.filePath(".git/private/ignored.cpp"), "int not_visible();\n");
    write(dir.filePath("api.h"), "int run(int);\n");
    write(dir.filePath("impl.cpp"), "int run(int x) { return x; }\n");
    write(dir.filePath("notes.txt"), "not source\n");
    const auto result = index(dir.path());
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QCOMPARE(result.files.size(), 3);
    QCOMPARE(result.parsed, 2);
    QVERIFY(QFileInfo::exists(workspaceDatabase(dir.path())));
    QVERIFY(!pathInWorkspace(workspaceDatabase(dir.path()), dir.path()));
    QCOMPARE(QDir(dir.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot),
             QStringList({".git", "api.h", "impl.cpp", "notes.txt"}));
    const auto page = queryWorkspaceSymbols(dir.path(), {});
    QVERIFY2(page.error.isEmpty(), qPrintable(page.error));
    QCOMPARE(page.total, 2);
    int definitions = 0;
    for (const auto &entry : page.symbols) {
        QCOMPARE(entry.symbol.name, "run");
        if (entry.symbol.definition)
            ++definitions;
    }
    QCOMPARE(definitions, 1);
    const auto cached = index(dir.path());
    QCOMPARE(cached.parsed, 0);
    QCOMPARE(cached.reused, 2);
    QVERIFY(!cached.changed);
    write(dir.filePath("notes.txt"), "changed non-source file\n");
    QVERIFY(index(dir.path()).changed);
    QVERIFY(!index(dir.path()).changed);
    QFile source(dir.filePath("api.h"));
    QVERIFY(source.open(QIODevice::ReadWrite));
    QVERIFY(source.setFileTime(QDateTime::currentDateTime().addSecs(10),
                               QFileDevice::FileModificationTime));
    source.close();
    const auto touched = index(dir.path());
    QVERIFY(touched.changed);
    QCOMPARE(touched.parsed, 0);
    QVERIFY(!index(dir.path()).changed);
}

void WorkspaceTests::refreshRebuildAndDeletion() {
    QTemporaryDir dir;
    const auto a = dir.filePath("a.cpp");
    write(a, "int before();\n");
    QVERIFY(index(dir.path()).error.isEmpty());
    write(a, "int after_();\n");
    write(dir.filePath("new.cpp"), "int newcomer();\n");
    auto refreshed = index(dir.path());
    QCOMPARE(refreshed.parsed, 2);
    QCOMPARE(queryWorkspaceSymbols(dir.path(), {}).total, 2);
    QVERIFY(QFile::remove(a));
    QVERIFY(QFile::rename(dir.filePath("new.cpp"), dir.filePath("renamed.cpp")));
    refreshed = index(dir.path());
    QCOMPARE(refreshed.files.size(), 1);
    const auto page = queryWorkspaceSymbols(dir.path(), {});
    QCOMPARE(page.total, 1);
    QVERIFY(page.symbols.first().path.endsWith("renamed.cpp"));
    const auto rebuild = index(dir.path(), true, false);
    QCOMPARE(rebuild.parsed, 1);
    QCOMPARE(rebuild.reused, 0);
}

void WorkspaceTests::dirtyOverlayReplacesDiskSymbols() {
    QTemporaryDir dir;
    const auto path = dir.filePath("doc.cpp");
    write(path, "int disk_name();\n");
    QVERIFY(index(dir.path()).error.isEmpty());
    const QByteArray dirty = "int unsaved_name();\n";
    SymbolQuery query;
    query.overlays.append({path, dirty, analyzeSource(dirty, path)});
    const auto result = queryWorkspaceSymbols(dir.path(), query);
    QCOMPARE(result.total, 1);
    QCOMPARE(result.symbols.first().symbol.name, "unsaved_name");
    QVERIFY(result.symbols.first().live);
    QCOMPARE(queryWorkspaceSymbols(dir.path(), {}).symbols.first().symbol.name, "disk_name");
    query.overlays[0].analysis = {};
    QCOMPARE(queryWorkspaceSymbols(dir.path(), query).total, 0);
}

void WorkspaceTests::queryPaginationFragmentsAndRegex() {
    QTemporaryDir dir;
    QByteArray text = "int WriteOpenDoc();\nint DocWrite();\nstruct Config { int count; };\n";
    for (int i = 0; i < 650; ++i)
        text += "int function_" + QByteArray::number(i) + "();\n";
    write(dir.filePath("many.cpp"), text);
    QVERIFY(index(dir.path()).error.isEmpty());
    SymbolQuery query;
    auto result = queryWorkspaceSymbols(dir.path(), query);
    QCOMPARE(result.total, 654);
    QCOMPARE(result.symbols.size(), 300);
    query.limit = 17;
    query.includeCategorySymbols = true;
    const auto bounded = queryWorkspaceSymbols(dir.path(), query);
    QCOMPARE(bounded.total, 654);
    QCOMPARE(bounded.symbols.size(), 17);
    QCOMPARE(bounded.categorySymbols.size(), bounded.total);
    QCOMPARE(bounded.categories, result.categories);
    query.limit = RuntimeLimits::SymbolPageSize;
    query.includeCategorySymbols = false;
    query.offset = 600;
    QCOMPARE(queryWorkspaceSymbols(dir.path(), query).symbols.size(), 54);
    query.offset = 0;
    query.text = "doc write(";
    QCOMPARE(queryWorkspaceSymbols(dir.path(), query).total, 2);
    query.text = "?^function_[0-9]+$";
    QCOMPARE(queryWorkspaceSymbols(dir.path(), query).total, 650);
    query.text = "?[invalid";
    QVERIFY(!queryWorkspaceSymbols(dir.path(), query).error.isEmpty());
    query.text.clear();
    query.category = "Structures";
    QCOMPARE(queryWorkspaceSymbols(dir.path(), query).total, 1);
}

void WorkspaceTests::symbolCategoriesPersistAndOverlay() {
    QTemporaryDir dir;
    const auto path = dir.filePath("categories.cpp");
    const QByteArray source = "#define LIMIT 10\n"
                              "#define APPLY(x) ((x) + LIMIT)\n"
                              "struct Record { int field; void method(); };\n"
                              "class Object {};\n"
                              "union Storage { int value; };\n"
                              "typedef int Number;\n"
                              "using Alias = Number;\n"
                              "enum State { Ready, Busy };\n"
                              "typedef enum { Only } Anonymous;\n"
                              "int global;\n"
                              "int run(int);\n"
                              "int run(int x) { int local = x; return local; }\n";
    write(path, source);
    const auto built = index(dir.path());
    QVERIFY2(built.error.isEmpty(), qPrintable(built.error));
    QCOMPARE(built.parsed, 1);
    const QMap<QString, int> expected{
        {"Structures", 3},          {"Type Definitions", 2}, {"Enumerations", 2}, {"Constants", 1},
        {"Enum Constants", 3},      {"Variables", 1},        {"Functions", 1},    {"Macros", 1},
        {"Function Prototypes", 2}, {"Data Members", 2}};
    SymbolQuery query;
    query.includeCategorySymbols = true;
    const auto page = queryWorkspaceSymbols(dir.path(), query);
    QVERIFY2(page.error.isEmpty(), qPrintable(page.error));
    QCOMPARE(page.categories, expected);
    QCOMPARE(page.total, 18);
    QCOMPARE(page.categorySymbols.size(), page.total);
    QCOMPARE(index(dir.path()).reused, 1);
    for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
        query.category = it.key();
        query.limit = 1;
        const auto filtered = queryWorkspaceSymbols(dir.path(), query);
        QCOMPARE(filtered.total, it.value());
        QCOMPARE(filtered.symbols.size(), 1);
        QCOMPARE(symbolCategory(filtered.symbols.first().symbol), it.key());
        QCOMPARE(filtered.categories, expected);
        QCOMPARE(filtered.categorySymbols.size(), page.total);
    }
    query.category.clear();
    query.text = "run(";
    const auto filtered = queryWorkspaceSymbols(dir.path(), query);
    QCOMPARE(filtered.total, 2);
    QCOMPARE(filtered.categories, expected);
    QCOMPARE(filtered.categorySymbols.size(), page.total);
    query.text.clear();
    query.limit = 300;
    const QByteArray dirty = "enum Changed { NewValue };\nint replacement() { return 0; }\n";
    query.overlays.append({path, dirty, analyzeSource(dirty, path)});
    const auto overlay = queryWorkspaceSymbols(dir.path(), query);
    const QMap<QString, int> overlayCategories{
        {"Enumerations", 1}, {"Enum Constants", 1}, {"Functions", 1}};
    QCOMPARE(overlay.categories, overlayCategories);
    QCOMPARE(overlay.categorySymbols.size(), 3);
    for (const auto &entry : overlay.categorySymbols)
        QVERIFY(entry.live);
    QCOMPARE(queryWorkspaceSymbols(dir.path(), {}).categories, expected);
}

void WorkspaceTests::cancellationPreservesCompletedIndex() {
    QTemporaryDir dir;
    write(dir.filePath("seed.cpp"), "int seed();\n");
    QVERIFY(index(dir.path()).error.isEmpty());
    for (int i = 0; i < 40; ++i)
        write(dir.filePath(QString("new_%1.cpp").arg(i)),
              "int function_" + QByteArray::number(i) + "();\n");
    const auto cancel = std::make_shared<std::atomic_bool>(false);
    const auto progress = std::make_shared<IndexProgress>();
    bool catalogPublished = false;
    progress->catalogReady = [&](const QStringList &paths) {
        catalogPublished = true;
        QCOMPARE(paths.size(), 41);
        QCOMPARE(progress->parsed.load(), 0);
        QCOMPARE(queryWorkspaceSymbols(dir.path(), {}).total, 1);
    };
    progress->batchReady = [&] {
        QVERIFY(catalogPublished);
        const auto page = queryWorkspaceSymbols(dir.path(), {});
        QVERIFY2(page.error.isEmpty(), qPrintable(page.error));
        QVERIFY(page.total > 1);
        QVERIFY(page.total < 41);
        cancel->store(true);
    };
    const auto stopped = buildWorkspaceIndex(workspacePath(dir.path()),
                                             IndexRefresh::DiscoverChanges, {}, cancel, progress);
    QVERIFY(stopped.cancelled);
    QVERIFY(stopped.parsed > 0);
    const auto partial = queryWorkspaceSymbols(dir.path(), {});
    QCOMPARE(partial.total, stopped.parsed + 1);
    const auto resumed = index(dir.path());
    QVERIFY2(resumed.error.isEmpty(), qPrintable(resumed.error));
    QCOMPARE(resumed.reused, stopped.parsed + 1);
    QCOMPARE(resumed.parsed + stopped.parsed, 40);
    QCOMPARE(queryWorkspaceSymbols(dir.path(), {}).total, 41);
}

void WorkspaceTests::syntaxOnlyAnalysisPreservesSymbolsWithoutStyles() {
    const QByteArray source = "namespace Example { struct Item { int run(int value); }; }\n";
    const auto editor = analyzeSource(source, "example.cpp");
    const auto bulk = analyzeSource(source, "example.cpp", SourceAnalysisMode::SyntaxOnly);
    QCOMPARE(editor.styles.size(), source.size());
    QVERIFY(bulk.styles.isEmpty());
    QVERIFY(!bulk.compilerEnriched);
    QCOMPARE(bulk.symbols.size(), editor.symbols.size());
    for (qsizetype i = 0; i < bulk.symbols.size(); ++i) {
        QCOMPARE(bulk.symbols[i].name, editor.symbols[i].name);
        QCOMPARE(bulk.symbols[i].qualifiedName, editor.symbols[i].qualifiedName);
        QCOMPARE(bulk.symbols[i].nameStart, editor.symbols[i].nameStart);
    }
    const auto broken =
        analyzeSource("int broken( {\n", "broken.cpp", SourceAnalysisMode::SyntaxOnly);
    QVERIFY(broken.syntaxErrors);
    QVERIFY(!broken.compilerEnriched);
    QVERIFY(broken.styles.isEmpty());
    QVERIFY(broken.diagnostics.size() <= 12);
}

void WorkspaceTests::unsupportedSchemaRequiresExplicitRecreation() {
    QTemporaryDir dir;
    write(dir.filePath("one.cpp"), "int one();\n");
    QVERIFY(index(dir.path()).error.isEmpty());
    {
        codecairn::detail::ProjectIndexStore database(workspaceDatabase(dir.path()), true);
        database.exec("PRAGMA user_version=1;");
    }
    QVERIFY(index(dir.path()).error.contains("Unsupported workspace database version"));
    QVERIFY(queryWorkspaceSymbols(dir.path(), {})
                .error.contains("Unsupported workspace database version"));
    codecairn::detail::ProjectIndexStore database(workspaceDatabase(dir.path()), false);
    codecairn::detail::IndexStatement count(database, "SELECT count(*) FROM symbols");
    QVERIFY(count.next());
    QCOMPARE(count.integer(0), 1);
}

void WorkspaceTests::databaseRecreationArchivesAndHonorsLock() {
    QTemporaryDir dir;
    write(dir.filePath("one.cpp"), "int one();\n");
    QVERIFY(WorkspaceOptionsStore::initialize(dir.path()).isEmpty());
    const auto database = workspaceDatabase(dir.path());
    write(database, "broken database");
    ProjectIndexer workspace;
    QSignalSpy failed(&workspace, &ProjectIndexer::failed);
    QSignalSpy ready(&workspace, &ProjectIndexer::ready);
    QSignalSpy catalog(&workspace, &ProjectIndexer::catalogReady);
    QSignalSpy batches(&workspace, &ProjectIndexer::batchReady);
    workspace.open(dir.path());
    QTRY_COMPARE_WITH_TIMEOUT(failed.size(), 1, 10000);
    QLockFile lock(QDir(workspaceStorageDirectory(dir.path())).filePath("index.lock"));
    QVERIFY(lock.tryLock(0));
    QVERIFY(!workspace.recreateDatabase().isEmpty());
    QCOMPARE(readSourceFile(database).bytes, QByteArray("broken database"));
    lock.unlock();
    QVERIFY(workspace.recreateDatabase().isEmpty());
    QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 10000);
    QVERIFY(!catalog.isEmpty());
    QVERIFY(!batches.isEmpty());
    QCOMPARE(queryWorkspaceSymbols(dir.path(), {}).total, 1);
    const auto backups =
        QDir(QFileInfo(database).absolutePath()).entryList({"index.sqlite3.backup-*"}, QDir::Files);
    QCOMPARE(backups.size(), 1);
    QCOMPARE(readSourceFile(QDir(QFileInfo(database).absolutePath()).filePath(backups[0])).bytes,
             QByteArray("broken database"));
}

void WorkspaceTests::managedProjectsIndexOnlySelectedMembership() {
    QTemporaryDir dir;
    const auto root = workspacePath(dir.path());
    QVERIFY(QDir(dir.path()).mkpath("selected/excluded"));
    QVERIFY(QDir(dir.path()).mkpath("other"));
    write(dir.filePath("selected/one.cpp"), "int selected_symbol();\n");
    write(dir.filePath("selected/excluded/no.cpp"), "int excluded_symbol();\n");
    write(dir.filePath("other/two.cpp"), "int other_symbol();\n");
    WorkspaceOptions options;
    options.managed = true;
    options.name = "Selected";
    options.includedPaths = {"selected"};
    options.excludedPaths = {"selected/excluded"};
    const auto first = QDir(root).filePath("first.cairn");
    const auto projectError = WorkspaceOptionsStore::saveProject(first, root, options);
    QVERIFY2(projectError.isEmpty(), qPrintable(projectError));
    options.name = "Other";
    options.includedPaths = {"other"};
    options.excludedPaths.clear();
    const auto second = QDir(root).filePath("second.cairn");
    QVERIFY(WorkspaceOptionsStore::saveProject(second, root, options).isEmpty());
    const auto cancel = std::make_shared<std::atomic_bool>(false);
    const auto selected =
        buildWorkspaceIndex(root, IndexRefresh::DiscoverChanges, {}, cancel, {}, first);
    QVERIFY2(selected.error.isEmpty(), qPrintable(selected.error));
    QCOMPARE(selected.files, QStringList{workspacePath(dir.filePath("selected/one.cpp"))});
    QVERIFY(!selected.directories.contains(workspacePath(dir.filePath("other"))));
    QVERIFY(!selected.directories.contains(workspacePath(dir.filePath("selected/excluded"))));
    const auto other =
        buildWorkspaceIndex(root, IndexRefresh::DiscoverChanges, {}, cancel, {}, second);
    QVERIFY2(other.error.isEmpty(), qPrintable(other.error));
    SymbolQuery query;
    query.projectFile = first;
    const auto firstPage = queryWorkspaceSymbols(dir.path(), query);
    QVERIFY2(firstPage.error.isEmpty(), qPrintable(firstPage.error));
    QCOMPARE(firstPage.total, 1);
    QCOMPARE(firstPage.symbols.first().symbol.name, "selected_symbol");
    query.projectFile = second;
    const auto secondPage = queryWorkspaceSymbols(dir.path(), query);
    QCOMPARE(secondPage.total, 1);
    QCOMPARE(secondPage.symbols.first().symbol.name, "other_symbol");
    QVERIFY(workspaceDatabase(dir.path(), first) != workspaceDatabase(dir.path(), second));
}

void WorkspaceTests::failedReplacementRetainsPreviousSymbols() {
    QTemporaryDir dir;
    const auto path = dir.filePath("one.cpp");
    write(path, "int previous_symbol();\n");
    QVERIFY(index(dir.path()).error.isEmpty());
    write(path, QByteArray("\xff", 1));
    const auto failed = index(dir.path());
    QVERIFY(failed.error.isEmpty());
    QVERIFY(!failed.warnings.isEmpty());
    QCOMPARE(queryWorkspaceSymbols(dir.path(), {}).symbols.first().symbol.name, "previous_symbol");
    write(path, "int replacement_symbol();\n");
    const auto replaced = index(dir.path());
    QVERIFY2(replaced.error.isEmpty(), qPrintable(replaced.error));
    QCOMPARE(replaced.parsed, 1);
    QCOMPARE(queryWorkspaceSymbols(dir.path(), {}).symbols.first().symbol.name,
             "replacement_symbol");
}

void WorkspaceTests::parallelParsersOverlapAndPublishWhileBusy() {
    if (QThread::idealThreadCount() < 2)
        QSKIP("Parallel parsing requires at least two available worker threads.");
    QTemporaryDir dir;
    QByteArray source;
    for (int i = 0; i < 4000; ++i)
        source += "int parallel_symbol_" + QByteArray::number(i) + "(int value);\n";
    for (int i = 0; i < 12; ++i)
        write(dir.filePath(QString("parallel_%1.cpp").arg(i)), source);
    const auto progress = std::make_shared<IndexProgress>();
    const auto built = buildWorkspaceIndex(dir.path(), IndexRefresh::DiscoverChanges, {},
                                           std::make_shared<std::atomic_bool>(false), progress);
    QVERIFY2(built.error.isEmpty(), qPrintable(built.error));
    QCOMPARE(built.parsed, 12);
    QVERIFY(progress->peakParsers.load() >= 2);
    QVERIFY(progress->peakParsers.load() <= 8);
    QCOMPARE(progress->activeParsers.load(), 0);

    ProjectIndexer workspace;
    QSignalSpy ready(&workspace, &ProjectIndexer::ready);
    bool catalogWhileBusy = false;
    bool queriedWhileBusy = false;
    connect(&workspace, &ProjectIndexer::catalogReady, this, [&](const QStringList &paths) {
        catalogWhileBusy = workspace.busy();
        QCOMPARE(paths.size(), 12);
    });
    connect(&workspace, &ProjectIndexer::batchReady, this, [&] {
        if (queriedWhileBusy)
            return;
        const auto page = queryWorkspaceSymbols(dir.path(), {});
        QVERIFY2(page.error.isEmpty(), qPrintable(page.error));
        QCOMPARE(page.total, 48000);
        queriedWhileBusy = workspace.busy();
    });
    workspace.open(dir.path());
    QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 20000);
    QVERIFY(catalogWhileBusy);
    QVERIFY(queriedWhileBusy);
}

void WorkspaceTests::resourceLoggingReportsStartAndCompletion() {
    QTemporaryDir dir;
    write(dir.filePath("one.cpp"), "int one();\n");
    const auto record = [](const QString &stage) {
        return QRegularExpression(
            "^index\\.resources stage= " + stage +
            " root= .* generation= [0-9]+ discovered= [0-9]+ parsed= [0-9]+"
            " committed= [0-9]+ rssBytes= (-1|[0-9]+) rssError= .* elapsedMs= [0-9]+$");
    };
    QTest::ignoreMessage(QtInfoMsg, record("start"));
    QTest::ignoreMessage(QtInfoMsg, record("complete"));
    ProjectIndexer workspace;
    QSignalSpy ready(&workspace, &ProjectIndexer::ready);
    workspace.open(dir.path());
    QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 10000);
}

void WorkspaceTests::initialPassUsesConfiguredExclusions() {
    QTemporaryDir dir;
    QVERIFY(QDir(dir.path()).mkpath("build"));
    write(dir.filePath("build/one.cpp"), "int included_on_first_pass();\n");
    QVERIFY(WorkspaceOptionsStore::saveOptions(workspacePath(dir.path()), {}, {".git"}).isEmpty());
    ProjectIndexer workspace;
    QSignalSpy ready(&workspace, &ProjectIndexer::ready);
    QSignalSpy catalog(&workspace, &ProjectIndexer::catalogReady);
    workspace.open(dir.path());
    QCOMPARE(workspace.excludes(), QStringList{".git"});
    QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 10000);
    QCOMPARE(catalog.size(), 1);
    QCOMPARE(catalog.first().first().toStringList(),
             QStringList{workspacePath(dir.filePath("build/one.cpp"))});
    const auto page = queryWorkspaceSymbols(dir.path(), {});
    QCOMPARE(page.total, 1);
    QCOMPARE(page.symbols.first().symbol.name, "included_on_first_pass");
}

void WorkspaceTests::pendingInitialSchemaDoesNotMaskUnsupportedDatabases() {
    QTemporaryDir dir;
    const auto root = workspacePath(dir.path());
    QVERIFY(WorkspaceOptionsStore::initialize(root).isEmpty());
    codecairn::detail::ProjectIndexStore writer(workspaceDatabase(root), true);
    writer.exec("PRAGMA journal_mode=WAL; BEGIN IMMEDIATE;"
                "CREATE TABLE files(path TEXT PRIMARY KEY);");
    SymbolQuery query;
    query.allowPendingSchema = true;
    const auto pending = queryWorkspaceSymbols(root, query);
    QVERIFY2(pending.error.isEmpty(), qPrintable(pending.error));
    QCOMPARE(pending.total, 0);
    const auto path = QDir(root).filePath("live.cpp");
    const QByteArray source = "int live_during_initialization();\n";
    query.overlays.append({path, source, analyzeSource(source, path)});
    const auto live = queryWorkspaceSymbols(root, query);
    QVERIFY2(live.error.isEmpty(), qPrintable(live.error));
    QCOMPARE(live.total, 1);
    QCOMPARE(live.symbols.first().symbol.name, "live_during_initialization");
    query.overlays.clear();
    query.allowPendingSchema = false;
    QVERIFY(queryWorkspaceSymbols(root, query)
                .error.contains("Unsupported workspace database version"));
    writer.exec("ROLLBACK; PRAGMA user_version=1;");
    query.allowPendingSchema = true;
    QVERIFY(queryWorkspaceSymbols(root, query)
                .error.contains("Unsupported workspace database version"));
    writer.exec("PRAGMA user_version=0; CREATE TABLE legacy_marker(value INTEGER);");
    QVERIFY(queryWorkspaceSymbols(root, query)
                .error.contains("Unsupported workspace database version"));
    writer.exec("DROP TABLE legacy_marker;");
    writer.ensureSchema();
    const auto ready = queryWorkspaceSymbols(root, query);
    QVERIFY2(ready.error.isEmpty(), qPrintable(ready.error));
    QCOMPARE(ready.total, 0);
}

void WorkspaceTests::corruptDatabaseIsReported() {
    QTemporaryDir dir;
    QVERIFY(WorkspaceOptionsStore::initialize(dir.path()).isEmpty());
    const auto path = workspaceDatabase(dir.path());
    write(path, "not a sqlite database");
    const auto result = index(dir.path());
    QVERIFY(!result.error.isEmpty());
    const auto query = queryWorkspaceSymbols(dir.path(), {});
    QVERIFY(!query.error.isEmpty());
    QCOMPARE(readSourceFile(path).bytes, QByteArray("not a sqlite database"));
}

void WorkspaceTests::missingSchemaIsNotSilentlyRecreated() {
    QTemporaryDir directory;
    write(directory.filePath("one.c"), "int preserved;\n");
    QVERIFY(index(directory.path()).error.isEmpty());
    {
        codecairn::detail::ProjectIndexStore db(workspaceDatabase(directory.path()), true);
        db.exec("DROP TABLE symbols;");
    }
    const auto result = index(directory.path());
    QVERIFY(result.error.contains("no such table"));
    const auto page = queryWorkspaceSymbols(directory.path(), {});
    QVERIFY(page.error.contains("no such table"));
    codecairn::detail::ProjectIndexStore db(workspaceDatabase(directory.path()), false);
    codecairn::detail::IndexStatement table(db, "SELECT 1 FROM sqlite_master WHERE name='symbols'");
    QVERIFY(!table.next());
}

void WorkspaceTests::unwritableMetadataIsReported() {
    QTemporaryDir dir;
    const auto metadata = workspaceStorageDirectory(dir.path());
    QVERIFY(QDir().mkpath(QFileInfo(metadata).absolutePath()));
    write(metadata, "a file occupies the metadata directory");
    write(dir.filePath("source.cpp"), "int source();\n");
    const auto result = index(dir.path());
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(readSourceFile(metadata).bytes, QByteArray("a file occupies the metadata directory"));
}

void WorkspaceTests::workspaceStorageIsIsolatedAndCoLocated() {
    QTemporaryDir sources;
    const auto first = sources.filePath("left/project");
    const auto second = sources.filePath("right/project");
    QVERIFY(QDir().mkpath(first));
    QVERIFY(QDir().mkpath(second));
    write(QDir(first).filePath("first.cpp"), "int first();\n");
    write(QDir(second).filePath("second.cpp"), "int second();\n");
    QVERIFY2(index(first).error.isEmpty(), "First workspace indexing failed");
    QVERIFY2(index(second).error.isEmpty(), "Second workspace indexing failed");
    QVERIFY(workspaceStorageDirectory(first) != workspaceStorageDirectory(second));
    QCOMPARE(workspaceStorageDirectory(first),
             workspaceStorageDirectory(QDir(first).filePath(".")));
    ApplicationStateStore state(defaultStateDirectory());
    QCOMPARE(QFileInfo(state.sessionPath(first)).absolutePath(), workspaceStorageDirectory(first));
    QCOMPARE(QFileInfo(workspaceOptionsPath(first)).absolutePath(),
             workspaceStorageDirectory(first));
    QCOMPARE(QFileInfo(workspaceDatabase(first)).absolutePath(), workspaceStorageDirectory(first));
    QCOMPARE(QFileInfo(workspaceClangdDirectory(first)).absolutePath(),
             workspaceStorageDirectory(first));
    QVERIFY(WorkspaceOptionsStore::saveOptions(first, "build-one", {"generated"}).isEmpty());
    QVERIFY(WorkspaceOptionsStore::saveOptions(second, "build-two", {}).isEmpty());
    QCOMPARE(WorkspaceOptionsStore::loadOptions(first).compilationDirectory, "build-one");
    QCOMPARE(WorkspaceOptionsStore::loadOptions(second).compilationDirectory, "build-two");
    QCOMPARE(queryWorkspaceSymbols(first, {}).symbols.first().symbol.name, "first");
    QCOMPARE(queryWorkspaceSymbols(second, {}).symbols.first().symbol.name, "second");
    QCOMPARE(QDir(first).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot),
             QStringList({"first.cpp"}));
    QCOMPARE(QDir(second).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot),
             QStringList({"second.cpp"}));
}

void WorkspaceTests::invalidWorkspaceIdentityIsRejected() {
    QTemporaryDir first, other;
    QVERIFY(WorkspaceOptionsStore::initialize(first.path()).isEmpty());
    QFile file(workspaceOptionsPath(first.path()));
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto object = QJsonDocument::fromJson(file.readAll()).object();
    file.close();
    object["workspaceRoot"] = other.path();
    const auto bytes = QJsonDocument(object).toJson();
    write(file.fileName(), bytes);
    QVERIFY(!WorkspaceOptionsStore::loadOptions(first.path()).error.isEmpty());
    QVERIFY(!WorkspaceOptionsStore::saveOptions(first.path(), {}, {}).isEmpty());
    QVERIFY(!index(first.path()).error.isEmpty());
    QVERIFY(!queryWorkspaceSymbols(first.path(), {}).error.isEmpty());
    QCOMPARE(readSourceFile(file.fileName()).bytes, bytes);
    QVERIFY(!QFileInfo::exists(workspaceDatabase(first.path())));
}

void WorkspaceTests::workspaceIndexLockIsPerWorkspace() {
    QTemporaryDir first, other;
    write(first.filePath("one.cpp"), "int one();\n");
    write(other.filePath("two.cpp"), "int two();\n");
    QVERIFY(WorkspaceOptionsStore::initialize(first.path()).isEmpty());
    QLockFile lock(QDir(workspaceStorageDirectory(first.path())).filePath("index.lock"));
    QVERIFY(lock.tryLock(0));
    QVERIFY(index(first.path()).error.contains("Another CodeCairn process"));
    QVERIFY(index(other.path()).error.isEmpty());
    lock.unlock();
    QVERIFY(index(first.path()).error.isEmpty());
}

void WorkspaceTests::readOnlySourceFolderCanBeIndexed() {
    QTemporaryDir source;
    const auto path = source.filePath("one.cpp");
    write(path, "int one();\n");
#ifdef Q_OS_WIN
    const auto user = qEnvironmentVariable("USERDOMAIN") + '\\' + qEnvironmentVariable("USERNAME");
    const auto changePermissions = [&](const QStringList &arguments) {
        QProcess command;
        command.start("icacls.exe", QStringList{source.path()} + arguments);
        return command.waitForFinished(10000) && command.exitStatus() == QProcess::NormalExit &&
               command.exitCode() == 0;
    };
    QVERIFY(changePermissions({"/deny", user + ":(WD,AD,WEA,WA)"}));
    const auto restore = qScopeGuard([&] {
        QVERIFY2(changePermissions({"/remove:d", user}), "Could not restore fixture permissions.");
    });
#else
    const auto permissions = QFileInfo(source.path()).permissions();
    const auto restore = qScopeGuard([&] { QFile::setPermissions(source.path(), permissions); });
    QVERIFY(QFile::setPermissions(source.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner));
#endif
    QFile probe(source.filePath("cannot-write"));
    QVERIFY(!probe.open(QIODevice::WriteOnly));
    const auto result = index(source.path());
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QCOMPARE(result.parsed, 1);
    const auto symbols = queryWorkspaceSymbols(source.path(), {});
    QCOMPARE(symbols.total, 1);
    QCOMPARE(symbols.symbols.first().symbol.name, "one");
    QVERIFY(prepareCompilationDatabase(source.path(), {}).error.isEmpty());
    QCOMPARE(QDir(source.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot),
             QStringList({"one.cpp"}));
}

void WorkspaceTests::compilerRecoveryUsesRelocatedCompilationDatabase() {
    QTemporaryDir source, build;
    const auto path = source.filePath("configured.c");
    const QByteArray contents = "#ifdef USE_SELECTED_CONFIGURATION\n"
                                "int configured_symbol(void);\n"
                                "#else\nint fallback_symbol(void);\n#endif\n";
    write(path, contents);
    const QJsonArray commands{QJsonObject{
        {"directory", source.path()},
        {"file", path},
        {"arguments", QJsonArray{"clang", "-DUSE_SELECTED_CONFIGURATION", "-c", path}}}};
    write(build.filePath("compile_commands.json"), QJsonDocument(commands).toJson());
    const auto database = prepareCompilationDatabase(source.path(), build.path());
    QVERIFY2(database.error.isEmpty(), qPrintable(database.error));
    QVERIFY(database.configured);
    QCOMPARE(database.directory, workspaceClangdDirectory(source.path()));
    QFile normalized(QDir(database.directory).filePath("compile_commands.json"));
    QVERIFY(normalized.open(QIODevice::ReadOnly));
    const auto normalizedEntry =
        QJsonDocument::fromJson(normalized.readAll()).array().first().toObject();
    QCOMPARE(normalizedEntry.value("directory").toString(), workspacePath(source.path()));
    QCOMPARE(normalizedEntry.value("file").toString(), workspacePath(path));
    QCOMPARE(normalizedEntry.value("arguments"), commands.first().toObject().value("arguments"));
    SourceAnalysis analysis;
    analysis.language = LanguageId::C;
    recoverCompilerSymbols(analysis, contents, path);
    QVERIFY(analysis.compilerEnriched);
    QSet<QString> names;
    for (const auto &symbol : analysis.symbols)
        names.insert(symbol.name);
    QVERIFY(names.contains("configured_symbol"));
    QVERIFY(!names.contains("fallback_symbol"));
    QCOMPARE(QDir(source.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot),
             QStringList({"configured.c"}));
}

void WorkspaceTests::managedCompilerRecoveryUsesOwnConfiguration() {
    QTemporaryDir directory, commands;
    const auto path = directory.filePath("macro.c");
    const QByteArray source = "#ifdef PROJECT_FLAG\n"
                              "#define TEST(name) int configured_##name(void)\n"
                              "#else\n#define TEST(name) int wrong_##name(void)\n#endif\n"
                              "TEST(function) { return 1; }\n";
    write(path, source);
    write(commands.filePath("compile_commands.json"),
          QJsonDocument(QJsonArray{QJsonObject{
                            {"directory", directory.path()},
                            {"file", path},
                            {"arguments", QJsonArray{"clang", "-DPROJECT_FLAG", "-c", path}}}})
              .toJson());
    WorkspaceOptions options;
    options.managed = true;
    options.name = "Configured";
    options.includedPaths = {"macro.c"};
    options.compilationDirectory = commands.path();
    const auto projectFile = directory.filePath("configured.cairn");
    QVERIFY(WorkspaceOptionsStore::saveProject(projectFile, directory.path(), options).isEmpty());
    const auto prepared =
        prepareCompilationDatabase(directory.path(), commands.path(), projectFile);
    QVERIFY2(prepared.error.isEmpty(), qPrintable(prepared.error));
    const auto built =
        buildWorkspaceIndex(directory.path(), IndexRefresh::RebuildFilesAndSymbols, {},
                            std::make_shared<std::atomic_bool>(false), {}, projectFile);
    QVERIFY2(built.error.isEmpty(), qPrintable(built.error));
    SymbolQuery query;
    query.projectFile = projectFile;
    query.text = "configured_function";
    query.exact = true;
    const auto page = queryWorkspaceSymbols(directory.path(), query);
    QVERIFY2(page.error.isEmpty(), qPrintable(page.error));
    QCOMPARE(page.total, 1);
    QCOMPARE(page.symbols.first().symbol.provider, QString("compiler"));
    const auto editor = analyzeSource(source, path, SourceAnalysisMode::Editor, prepared.directory);
    QVERIFY(std::any_of(editor.symbols.cbegin(), editor.symbols.cend(), [](const auto &symbol) {
        return symbol.name == "configured_function" && symbol.generated;
    }));
    QVERIFY(!QFileInfo::exists(workspaceDatabase(directory.path())));
}

void WorkspaceTests::managedCompilerSettingsDoNotBorrowFolderCache() {
    QTemporaryDir directory, folderCommands, managedCommands;
    const auto path = directory.filePath("configured.c");
    const QByteArray source = "#ifdef FOLDER_FLAG\n"
                              "#define TEST(name) int folder_##name(void)\n"
                              "int needle = 1;\n"
                              "#else\n#define TEST(name) int managed_##name(void)\n"
                              "int needle = 2;\n#endif\nTEST(function) { return needle; }\n";
    write(path, source);
    const auto commands = [&](const QString &target, bool flag) {
        QJsonArray arguments{"clang", "-c", path};
        if (flag)
            arguments.append("-DFOLDER_FLAG");
        write(QDir(target).filePath("compile_commands.json"),
              QJsonDocument(QJsonArray{QJsonObject{{"directory", directory.path()},
                                                   {"file", path},
                                                   {"arguments", arguments}}})
                  .toJson());
    };
    commands(folderCommands.path(), true);
    commands(managedCommands.path(), false);
    QVERIFY(prepareCompilationDatabase(directory.path(), folderCommands.path()).error.isEmpty());
    WorkspaceOptions options;
    options.managed = true;
    options.name = "Managed";
    options.includedPaths = {"configured.c"};
    const auto descriptor = directory.filePath("managed.cairn");
    QVERIFY(WorkspaceOptionsStore::saveProject(descriptor, directory.path(), options).isEmpty());
    const auto empty = prepareCompilationDatabase(directory.path(), {}, descriptor);
    QVERIFY2(empty.error.isEmpty(), qPrintable(empty.error));
    QVERIFY(!empty.configured);
    const auto analysis = analyzeSource(source, path, SourceAnalysisMode::Index, empty.directory);
    QVERIFY(std::any_of(analysis.symbols.cbegin(), analysis.symbols.cend(),
                        [](const auto &symbol) { return symbol.name == "managed_function"; }));
    QVERIFY(std::none_of(analysis.symbols.cbegin(), analysis.symbols.cend(),
                         [](const auto &symbol) { return symbol.name == "folder_function"; }));
    const auto prepared =
        prepareCompilationDatabase(directory.path(), managedCommands.path(), descriptor);
    QVERIFY2(prepared.error.isEmpty(), qPrintable(prepared.error));
    ReferenceSearchRequest request;
    request.workspaceRoot = directory.path();
    request.projectFile = descriptor;
    request.paths = {path};
    request.query = "needle";
    request.options.method = SearchMethod::Literal;
    request.options.skipInactive = true;
    const auto result = searchReferences(request, {});
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QVERIFY2(result.warnings.isEmpty(), qPrintable(result.warnings.join('\n')));
    QCOMPARE(result.hits.size(), 2);
    QVERIFY(std::any_of(result.hits.cbegin(), result.hits.cend(),
                        [](const auto &hit) { return hit.text.contains("needle = 2"); }));
    QVERIFY(std::none_of(result.hits.cbegin(), result.hits.cend(),
                         [](const auto &hit) { return hit.text.contains("needle = 1"); }));
    const QVector<LspLocation> targets{{path, 2, 4, 2, 10}, {path, 5, 4, 5, 10}};
    const auto semantic = resolveSemanticReferences(targets, {path}, "needle", {}, {}, 0, 100, true,
                                                    {}, prepared.directory);
    QVERIFY2(semantic.error.isEmpty(), qPrintable(semantic.error));
    QCOMPARE(semantic.hits.size(), 1);
    QVERIFY(semantic.hits.first().text.contains("needle = 2"));
}

void WorkspaceTests::excludedNamesCanBeRemoved() {
    QTemporaryDir directory;
    QVERIFY(QDir(directory.path()).mkdir("build"));
    write(directory.filePath("build/included.c"), "int included;\n");
    const auto skipped = index(directory.path());
    QVERIFY2(skipped.error.isEmpty(), qPrintable(skipped.error));
    QVERIFY(skipped.files.isEmpty());
    const auto rebuilt = buildWorkspaceIndex(directory.path(), IndexRefresh::DiscoverChanges, {},
                                             std::make_shared<std::atomic_bool>(false));
    QVERIFY2(rebuilt.error.isEmpty(), qPrintable(rebuilt.error));
    QCOMPARE(rebuilt.files.size(), 1);
    QCOMPARE(queryWorkspaceSymbols(directory.path(), {}).total, 1);
}

void WorkspaceTests::watcherRefreshAndWorkspaceSwitch() {
    QTemporaryDir first, second;
    write(first.filePath("one.cpp"), "int one();\n");
    write(second.filePath("two.cpp"), "int two();\n");
    ProjectIndexer service;
    QSignalSpy ready(&service, &ProjectIndexer::ready);
    service.open(first.path());
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() >= 1, 10000);
    write(first.filePath("added.cpp"), "int added();\n");
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() >= 2, 10000);
    QCOMPARE(service.files().size(), 2);
    service.refresh(IndexRefresh::RebuildFilesAndSymbols);
    service.open(second.path());
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 15000);
    QCOMPARE(service.root(), workspacePath(second.path()));
    QCOMPARE(service.files().size(), 1);
    QVERIFY(service.files().first().endsWith("two.cpp"));
}

void WorkspaceTests::utf16Conversions() {
    const QByteArray source = "// " + QByteArray::fromHex("f09f9880") + "\r\nint target();\n";
    const auto offset = source.indexOf("target");
    const auto position = lspPosition(source, static_cast<int>(offset));
    QCOMPARE(position.value("line").toInt(), 1);
    QCOMPARE(position.value("character").toInt(), 4);
    QCOMPARE(sourceByteOffset(source, 1, 4), offset);
    QCOMPARE(sourceByteOffset(source, 0, 4), -1);
    QCOMPARE(sourceByteOffset(source, 100, 0), -1);
}

void WorkspaceTests::automaticRefreshIgnoresMetadata() {
    QTemporaryDir dir;
    QVERIFY(QDir(dir.path()).mkpath("build"));
    write(dir.filePath("one.cpp"), "int one();\n");
    ProjectIndexer service;
    QSignalSpy ready(&service, &ProjectIndexer::ready);
    service.open(dir.path());
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty() && !service.busy(), 10000);
    ready.clear();
    write(QDir(workspaceStorageDirectory(dir.path())).filePath("cache-noise"),
          "background service activity\n");
    write(dir.filePath("build/output.log"), "build output\n");
    QTest::qWait(2500);
    QCOMPARE(ready.count(), 0);
    QVERIFY(!service.busy());
    service.refresh();
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty() && !service.busy(), 10000);
    ready.clear();
    QTest::qWait(2500);
    QCOMPARE(ready.count(), 0);
}

void WorkspaceTests::unchangedAutomaticRefreshIsQuiet() {
    QTemporaryDir dir;
    const auto path = dir.filePath("one.cpp");
    const QByteArray source = "int one();\n";
    write(path, source);
    MainWindow window(false);
    QSignalSpy ready(&window, &MainWindow::indexReady);
    window.openProject(dir.path());
    auto *service = window.findChild<ProjectIndexer *>();
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty() && !service->busy(), 10000);
    window.findChild<LanguageServerClient *>()->stop();
    ready.clear();
    window.statusBar()->showMessage("Keep browsing");
    write(path, source);
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty() && !service->busy(), 10000);
    QCOMPARE(window.statusBar()->currentMessage(), "Keep browsing");
    ready.clear();
    service->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty() && !service->busy(), 10000);
    QVERIFY(window.statusBar()->currentMessage().contains("0 parsed, 1 cached"));
    ready.clear();
    const auto modified = QFileInfo(path).lastModified();
    write(path, "int two();\n");
    QFile changed(path);
    QVERIFY(changed.open(QIODevice::ReadWrite));
    QVERIFY(changed.setFileTime(modified, QFileDevice::FileModificationTime));
    changed.close();
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty() && !service->busy(), 10000);
    QVERIFY(window.statusBar()->currentMessage().contains("1 parsed"));
    ready.clear();
    write(path, QByteArray(1, '\xff'));
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty() && !service->busy(), 10000);
    QVERIFY(window.statusBar()->currentMessage().contains("1 indexing warnings"));
}

void WorkspaceTests::refreshesCoalesceWithoutCancelling() {
    QTemporaryDir dir;
    write(dir.filePath("one.cpp"), "int one();\n");
    ProjectIndexer service;
    QSignalSpy ready(&service, &ProjectIndexer::ready);
    QSignalSpy failed(&service, &ProjectIndexer::failed);
    service.open(dir.path());
    QVERIFY(service.busy());
    for (int i = 0; i < 10; ++i)
        service.refresh();
    QTRY_VERIFY_WITH_TIMEOUT(!service.busy(), 15000);
    QCOMPARE(failed.count(), 0);
    QCOMPARE(ready.count(), 2);
    QCOMPARE(qvariant_cast<IndexResult>(ready[0][0]).parsed, 1);
    QCOMPARE(qvariant_cast<IndexResult>(ready[1][0]).reused, 1);
}

void WorkspaceTests::refreshPreservesBrowserState() {
    QTemporaryDir dir;
    QVERIFY(QDir(dir.path()).mkpath("nested"));
    for (int i = 0; i < 120; ++i)
        write(dir.filePath(QString("nested/file_%1.cpp").arg(i, 3, 10, QLatin1Char('0'))),
              "int symbol();\n");
    MainWindow window(false);
    window.show();
    QSignalSpy ready(&window, &MainWindow::indexReady);
    window.openProject(dir.path());
    auto *service = window.findChild<ProjectIndexer *>();
    auto *files = window.findChild<QTreeWidget *>("fileList");
    auto *folders = window.findChild<QTreeWidget *>("folderTree");
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() > 0 && !service->busy(), 15000);
    auto *first = files->topLevelItem(0);
    files->setCurrentItem(first);
    files->topLevelItem(1)->setSelected(true);
    auto *nested = folders->topLevelItem(0)->child(0);
    nested->setExpanded(true);
    QTRY_VERIFY(files->verticalScrollBar()->maximum() > 20);
    files->verticalScrollBar()->setValue(20);
    const int scroll = files->verticalScrollBar()->value();
    ready.clear();
    service->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() > 0 && !service->busy(), 15000);
    QCOMPARE(files->topLevelItem(0), first);
    QCOMPARE(files->verticalScrollBar()->value(), scroll);
    QCOMPARE(files->selectedItems().size(), 2);
    QVERIFY(nested->isExpanded());
    ready.clear();
    write(dir.filePath("nested/new.cpp"), "int added();\n");
    service->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() > 0 && !service->busy(), 15000);
    QCOMPARE(files->topLevelItemCount(), 121);
    QCOMPARE(files->verticalScrollBar()->value(), scroll);
    QCOMPARE(files->selectedItems().size(), 2);
    QVERIFY(folders->topLevelItem(0)->child(0)->isExpanded());
}

void WorkspaceTests::indexingDoesNotWaitForEditorWorkers() {
    QTemporaryDir dir;
    write(dir.filePath("one.cpp"), "int one();\n");
    auto *pool = QThreadPool::globalInstance();
    const int threads = pool->maxThreadCount();
    pool->setMaxThreadCount(1);
    QSemaphore started, release;
    auto blocked = QtConcurrent::run([&] {
        started.release();
        release.acquire();
    });
    started.acquire();
    ProjectIndexer service;
    QSignalSpy ready(&service, &ProjectIndexer::ready);
    service.open(dir.path());
    const bool completed = ready.wait(10000);
    release.release();
    blocked.waitForFinished();
    pool->setMaxThreadCount(threads);
    QVERIFY(completed);
    QCOMPARE(service.files().size(), 1);
}

void WorkspaceTests::projectSymbolListContainsAllSymbols() {
    QTemporaryDir dir;
    QByteArray source;
    for (int i = 0; i < 650; ++i)
        source += "int function_" + QByteArray::number(i) + "();\n";
    write(dir.filePath("many.cpp"), source);
    MainWindow window(false);
    window.openProject(dir.path());
    auto *symbols = window.findChild<QTreeWidget *>("symbolList");
    auto *scope = window.findChild<QLabel *>("symbolScope");
    QTRY_COMPARE_WITH_TIMEOUT(scope->text(), "650 symbols", 15000);
    QCOMPARE(symbols->topLevelItemCount(), 650);
    QVERIFY(!window.findChild<QAction *>("moreSymbols"));
    QVERIFY(!window.findChild<QAction *>("nextSymbolPage"));
    QVERIFY(!window.findChild<QAction *>("previousSymbolPage"));
    QSet<QString> names;
    for (int i = 0; i < symbols->topLevelItemCount(); ++i)
        names.insert(symbols->topLevelItem(i)->text(0));
    QCOMPARE(names.size(), 650);
    window.findChild<QLineEdit *>("symbolFilter")->setText("function_");
    QTRY_COMPARE_WITH_TIMEOUT(scope->text(), "650 symbols", 10000);
    QCOMPARE(symbols->topLevelItemCount(), 650);
    window.findChild<QLineEdit *>("symbolFilter")->setText("function_649");
    QTRY_COMPARE_WITH_TIMEOUT(symbols->topLevelItemCount(), 1, 10000);
    QCOMPARE(symbols->topLevelItem(0)->text(0), "function_649");
}

void WorkspaceTests::compilerGeneratedProjectSymbolsPreviewDefinitions() {
    QTemporaryDir dir;
    write(dir.filePath("harness.h"), "#define TEST(name) static void test_##name(void)\n");
    const QByteArray source = "#include \"harness.h\"\n"
                              "TEST(first) { int local = 1; }\n"
                              "TEST(second) { }\n";
    write(dir.filePath("tests.c"), source);
    const auto editor = analyzeSource(source, dir.filePath("tests.c"));
    const auto indexed = analyzeSource(source, dir.filePath("tests.c"), SourceAnalysisMode::Index);
    QVERIFY(indexed.compilerEnriched);
    QVERIFY(indexed.styles.isEmpty());
    QCOMPARE(indexed.symbols.size(), editor.symbols.size());
    for (qsizetype i = 0; i < indexed.symbols.size(); ++i) {
        QCOMPARE(indexed.symbols[i].name, editor.symbols[i].name);
        QCOMPARE(indexed.symbols[i].nameStart, editor.symbols[i].nameStart);
        QCOMPARE(indexed.symbols[i].nameEnd, editor.symbols[i].nameEnd);
        QCOMPARE(indexed.symbols[i].start, editor.symbols[i].start);
        QCOMPARE(indexed.symbols[i].end, editor.symbols[i].end);
        QCOMPARE(indexed.symbols[i].provider, editor.symbols[i].provider);
    }
    MainWindow window(false);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.openProject(dir.path());
    auto *index = window.findChild<ProjectIndexer *>();
    auto *symbols = window.findChild<QTreeWidget *>("symbolList");
    QTRY_VERIFY_WITH_TIMEOUT(!index->busy(), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(symbols->topLevelItemCount(), 3, 10000);
    window.findChild<QTabWidget *>("browserTabs")->setCurrentIndex(1);
    QTreeWidgetItem *target = nullptr;
    for (int i = 0; i < symbols->topLevelItemCount(); ++i) {
        QVERIFY(!symbols->topLevelItem(i)->text(0).isEmpty());
        if (symbols->topLevelItem(i)->text(0) == "test_second")
            target = symbols->topLevelItem(i);
    }
    QVERIFY(target);
    symbols->scrollToItem(target);
    QSignalSpy analyzed(&window, &MainWindow::analysisReady);
    QTest::mouseClick(symbols->viewport(), Qt::LeftButton, Qt::NoModifier,
                      symbols->visualItemRect(target).center());
    QTRY_VERIFY_WITH_TIMEOUT(!analyzed.isEmpty(), 10000);
    QTRY_COMPARE_WITH_TIMEOUT(window.contextEditor()->bytes(), source, 10000);
    QTest::qWait(window.configuration().behavior.contextDebounceMs + 100);
    QCOMPARE(window.contextEditor()->bytes(), source);
    QVERIFY(symbols->hasFocus());
    const auto start = window.contextEditor()->send(SCI_GETSELECTIONSTART);
    const auto end = window.contextEditor()->send(SCI_GETSELECTIONEND);
    QCOMPARE(source.mid(start, end - start), QByteArray("TEST"));
    QCOMPARE(start, source.lastIndexOf("TEST"));
}

void WorkspaceTests::largeFileRefreshPreservesBrowserState() {
    QTemporaryDir directory;
    QVERIFY(QDir(directory.path()).mkdir("nested"));
    for (int i = 0; i < 2101; ++i)
        write(directory.filePath(QString("nested/file_%1.txt").arg(i)), "notes\n");
    MainWindow window(false);
    window.openProject(directory.path());
    auto *index = window.findChild<ProjectIndexer *>();
    QTRY_VERIFY_WITH_TIMEOUT(!index->busy(), 30000);
    auto *files = window.findChild<QTreeWidget *>("fileList");
    auto *folders = window.findChild<QTreeWidget *>("folderTree");
    QCOMPARE(files->topLevelItemCount(), 1000);
    files->setCurrentItem(files->topLevelItem(1));
    files->topLevelItem(2)->setSelected(true);
    QSet<QString> selected;
    for (const auto *item : files->selectedItems())
        selected.insert(item->data(0, SymbolItemRole::Path).toString());
    auto *nested = folders->topLevelItem(0)->child(0);
    nested->setExpanded(true);
    QVERIFY(nested->childCount() > 0);
    folders->setCurrentItem(nested);
    const auto nestedName = nested->text(0);
    QSignalSpy ready(index, &ProjectIndexer::ready);
    write(directory.filePath("new.txt"), "new\n");
    index->refresh();
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty() && !index->busy(), 30000);
    QSet<QString> after;
    for (const auto *item : files->selectedItems())
        after.insert(item->data(0, SymbolItemRole::Path).toString());
    QCOMPARE(after, selected);
    QCOMPARE(folders->currentItem()->text(0), nestedName);
    QVERIFY(folders->currentItem()->isExpanded());
    QCOMPARE(folders->currentItem()->childCount(), 1001);
}

void WorkspaceTests::editedExclusionsUpdateOpenBufferMembership() {
    QTemporaryDir directory;
    QVERIFY(QDir(directory.path()).mkdir("sub"));
    write(directory.filePath("main.c"), "int kept_symbol;\n");
    write(directory.filePath("sub/one.c"), "int excluded_symbol;\n");
    WorkspaceOptions options;
    options.managed = true;
    options.name = "Exclusions";
    options.includedPaths = {"."};
    const auto descriptor = directory.filePath("project.cairn");
    QVERIFY(WorkspaceOptionsStore::saveProject(descriptor, directory.path(), options).isEmpty());
    MainWindow window(false);
    window.openManagedProject(descriptor);
    auto *index = window.findChild<ProjectIndexer *>();
    QTRY_VERIFY_WITH_TIMEOUT(!index->busy(), 10000);
    QSignalSpy analyzed(&window, &MainWindow::analysisReady);
    QVERIFY(window.openFile(directory.filePath("sub/one.c")));
    QTRY_VERIFY_WITH_TIMEOUT(!analyzed.isEmpty(), 10000);
    QTimer::singleShot(0, &window, [&] {
        auto *dialog = window.findChild<QInputDialog *>();
        QVERIFY(dialog);
        dialog->setTextValue("sub");
        dialog->accept();
    });
    window.findChild<QAction *>("workspaceExclusions")->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!index->busy(), 10000);
    auto *symbols = window.findChild<QTreeWidget *>("symbolList");
    QTRY_COMPARE_WITH_TIMEOUT(symbols->topLevelItemCount(), 1, 10000);
    QCOMPARE(symbols->topLevelItem(0)->text(0), QString("kept_symbol"));
    QCOMPARE(window.documentCount(), 1);
    QCOMPARE(WorkspaceOptionsStore::loadOptions(directory.path(), descriptor)
                 .excludedDirectories.value(),
             QStringList{"sub"});
    QLockFile lock(descriptor + ".lock");
    QVERIFY(lock.tryLock(0));
    QTimer::singleShot(0, &window, [&] {
        auto *dialog = window.findChild<QInputDialog *>();
        QVERIFY(dialog);
        dialog->setTextValue({});
        dialog->accept();
    });
    window.findChild<QAction *>("workspaceExclusions")->trigger();
    QCOMPARE(index->excludes(), QStringList{"sub"});
    QVERIFY(!index->busy());
    QCOMPARE(symbols->topLevelItemCount(), 1);
    QVERIFY(window.statusBar()->currentMessage().contains("lock", Qt::CaseInsensitive));
}

void WorkspaceTests::externalWorkspaceIndex() {
    const auto root = qEnvironmentVariable("CODECAIRN_VALIDATE_WORKSPACE_INDEX");
    if (root.isEmpty())
        QSKIP("Set CODECAIRN_VALIDATE_WORKSPACE_INDEX to rebuild and validate a real workspace "
              "catalog.");
    const auto rebuilt = index(root, true);
    QVERIFY2(rebuilt.error.isEmpty(), qPrintable(rebuilt.error));
    QVERIFY(!rebuilt.cancelled);
    QVERIFY(rebuilt.parsed > 0);
    for (const auto &warning : rebuilt.warnings) {
        qInfo().noquote() << warning;
        QVERIFY2(!warning.contains("'stdbool.h' file not found") &&
                     !warning.contains("'stdatomic.h' file not found") &&
                     !warning.contains("'x86intrin.h' file not found"),
                 qPrintable(warning));
    }
    QSet<QString> indexed;
    SymbolQuery query;
    int total = 0;
    do {
        const auto page = queryWorkspaceSymbols(root, query);
        QVERIFY2(page.error.isEmpty(), qPrintable(page.error));
        total = page.total;
        for (const auto &entry : page.symbols)
            indexed.insert(entry.path + ':' + QString::number(entry.symbol.nameStart) + ':' +
                           entry.symbol.name);
        query.offset += page.symbols.size();
        QVERIFY(query.offset >= total || !page.symbols.isEmpty());
    } while (query.offset < total);
    int expected = 0;
    for (const auto &path : rebuilt.files) {
        if (!rebuilt.sourceHashes.contains(path))
            continue;
        const auto source = readSourceFile(path);
        QVERIFY2(source.error.isEmpty(), qPrintable(source.error));
        const auto analysis = analyzeSource(source.bytes, path);
        QVERIFY2(analysis.error.isEmpty(), qPrintable(analysis.error));
        for (const auto &symbol : analysis.symbols) {
            if (symbol.outlineOnly || symbol.local)
                continue;
            ++expected;
            QVERIFY2(indexed.contains(path + ':' + QString::number(symbol.nameStart) + ':' +
                                      symbol.name),
                     qPrintable(path + ": missing " + symbol.name));
        }
    }
    QCOMPARE(total, expected);
    const auto cached = index(root);
    QVERIFY2(cached.error.isEmpty(), qPrintable(cached.error));
    QCOMPARE(cached.parsed, 0);
    QCOMPARE(cached.reused, rebuilt.parsed);
    QVERIFY(!cached.changed);
    qInfo() << rebuilt.files.size() << "files;" << rebuilt.parsed << "parsed;" << total
            << "symbols verified across all pages;" << rebuilt.warnings.size() << "syntax warnings";
}

void WorkspaceTests::externalWorkspaceSymbolPreviews() {
    const auto root = qEnvironmentVariable("CODECAIRN_VALIDATE_WORKSPACE");
    if (root.isEmpty())
        QSKIP(
            "Set CODECAIRN_VALIDATE_WORKSPACE to verify macro-generated project-symbol previews.");
    MainWindow window(false);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.openProject(root);
    auto *index = window.findChild<ProjectIndexer *>();
    auto *symbols = window.findChild<QTreeWidget *>("symbolList");
    QTRY_VERIFY_WITH_TIMEOUT(!index->busy(), 30000);
    SymbolQuery query;
    query.limit = std::numeric_limits<int>::max();
    const auto page = queryWorkspaceSymbols(root, query);
    QVERIFY2(page.error.isEmpty(), qPrintable(page.error));
    auto *virtualView = window.findChild<QTreeView *>("virtualSymbolList");
    if (page.total > 4096)
        QTRY_COMPARE_WITH_TIMEOUT(virtualView->model()->rowCount(), page.total, 10000);
    else
        QTRY_COMPARE_WITH_TIMEOUT(symbols->topLevelItemCount(), page.total, 10000);
    window.findChild<QTabWidget *>("browserTabs")->setCurrentIndex(1);
    QSet<QString> files;
    for (const auto &entry : page.symbols) {
        if (!entry.symbol.generated || entry.symbol.provider != "compiler" ||
            files.contains(entry.path))
            continue;
        const auto key =
            entry.path + ':' + QString::number(entry.symbol.nameStart) + ':' + entry.symbol.name;
        auto *filter = window.findChild<QLineEdit *>("symbolFilter");
        filter->setText(entry.symbol.name);
        SymbolQuery search;
        search.text = entry.symbol.name;
        search.limit = std::numeric_limits<int>::max();
        const auto filtered = queryWorkspaceSymbols(root, search);
        QTRY_COMPARE_WITH_TIMEOUT(symbols->topLevelItemCount(), filtered.total, 10000);
        QTreeWidgetItem *item = nullptr;
        for (int i = 0; i < symbols->topLevelItemCount(); ++i)
            if (symbols->topLevelItem(i)->data(0, SymbolItemRole::Key).toString() == key)
                item = symbols->topLevelItem(i);
        QVERIFY2(item, qPrintable("Missing project symbol: " + entry.symbol.name));
        const auto source = readSourceFile(entry.path);
        QVERIFY2(source.error.isEmpty(), qPrintable(source.error));
        symbols->scrollToItem(item);
        QTest::mouseClick(symbols->viewport(), Qt::LeftButton, Qt::NoModifier,
                          symbols->visualItemRect(item).center());
        QTRY_COMPARE_WITH_TIMEOUT(window.contextEditor()->bytes(), source.bytes, 10000);
        QTest::qWait(window.configuration().behavior.contextDebounceMs + 100);
        QCOMPARE(window.contextEditor()->bytes(), source.bytes);
        QCOMPARE(window.contextEditor()->send(SCI_GETSELECTIONSTART), entry.symbol.nameStart);
        QCOMPARE(window.contextEditor()->send(SCI_GETSELECTIONEND), entry.symbol.nameEnd);
        files.insert(entry.path);
    }
    qInfo() << page.total << "project symbols;" << files.size()
            << "files verified through macro-generated definition clicks";
}

void WorkspaceTests::discoversClangdWithoutShellPath() {
#ifndef CODECAIRN_TEST_CLANGD
    QSKIP("clangd is not installed.");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    write(dir.filePath("main.cpp"), "int main() { return 0; }\n");
    compilationDatabase(dir.path(), {"main.cpp"});
    const auto path = qgetenv("PATH");
    const bool hadPath = qEnvironmentVariableIsSet("PATH");
    const auto restorePath = qScopeGuard([&] {
        if (hadPath)
            qputenv("PATH", path);
        else
            qunsetenv("PATH");
    });
    qputenv("PATH", QByteArray());
    LanguageServerClient client;
    QSignalSpy ready(&client, &LanguageServerClient::ready);
    client.start(dir.path());
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 15000);
#endif
}

void WorkspaceTests::realClangdCrossFileOverloads() {
#ifndef CODECAIRN_TEST_CLANGD
    QSKIP("clangd is not installed.");
#else
    QTemporaryDir dir;
    write(dir.filePath("api.h"), "int compute(int);\ndouble compute(double);\n");
    write(dir.filePath("impl.cpp"), "#include \"api.h\"\nint compute(int x) { return x; }\ndouble "
                                    "compute(double x) { return x; }\n");
    const QByteArray caller = "#include \"api.h\"\ndouble caller() { return compute(1.5); }\n";
    const auto path = dir.filePath("caller.cpp");
    write(path, caller);
    compilationDatabase(dir.path(), {"impl.cpp", "caller.cpp"});
    LanguageServerClient client;
    QSignalSpy ready(&client, &LanguageServerClient::ready);
    QSignalSpy definitions(&client, &LanguageServerClient::definitionReady);
    client.start(dir.path(), {}, CODECAIRN_TEST_CLANGD);
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() == 1, 15000);
    QVERIFY(client.hasCompilationDatabase());
    const int position = static_cast<int>(caller.indexOf("compute"));
    QVector<LspLocation> targets;
    for (int attempt = 0; attempt < 30; ++attempt) {
        const int count = definitions.count();
        client.definition(path, caller, 1, position, attempt + 1);
        QTRY_VERIFY_WITH_TIMEOUT(definitions.count() > count, 15000);
        const auto values = definitions.last();
        QVERIFY2(values[2].toString().isEmpty(), qPrintable(values[2].toString()));
        targets = qvariant_cast<QVector<LspLocation>>(values[1]);
        if (!targets.isEmpty() && targets.first().path.endsWith("impl.cpp"))
            break;
        QTest::qWait(200);
    }
    QCOMPARE(targets.size(), 1);
    QVERIFY2(targets.first().path.endsWith("impl.cpp"), qPrintable(targets.first().path));
    QCOMPARE(targets.first().line, 2);
    QVERIFY(QFileInfo::exists(
        QDir(workspaceClangdDirectory(dir.path())).filePath("compile_commands.json")));
    QTRY_VERIFY_WITH_TIMEOUT(
        !QDir(QDir(workspaceClangdDirectory(dir.path())).filePath(".cache/clangd/index"))
             .entryList({"*.idx"}, QDir::Files)
             .isEmpty(),
        15000);
    QCOMPARE(QDir(dir.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot),
             QStringList({"api.h", "caller.cpp", "compile_commands.json", "impl.cpp"}));
#endif
}

void WorkspaceTests::realClangdUnsavedAndStaleRequests() {
#ifndef CODECAIRN_TEST_CLANGD
    QSKIP("clangd is not installed.");
#else
    QTemporaryDir dir;
    const auto path = dir.filePath("live.cpp");
    const QByteArray disk =
        "int first() { return 1; }\nint second() { return 2; }\nint caller() { return first(); }\n";
    write(path, disk);
    compilationDatabase(dir.path(), {"live.cpp"});
    LanguageServerClient client;
    QSignalSpy ready(&client, &LanguageServerClient::ready),
        replies(&client, &LanguageServerClient::definitionReady);
    client.start(dir.path(), {}, CODECAIRN_TEST_CLANGD);
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() > 0, 15000);
    const QByteArray unsaved = "// unsaved line\n" + disk;
    client.definition(path, disk, 1, static_cast<int>(disk.lastIndexOf("first")), 100);
    client.definition(path, unsaved, 2, static_cast<int>(unsaved.indexOf("second")), 101);
    QTRY_VERIFY_WITH_TIMEOUT(replies.count() > 0, 15000);
    const auto values = replies.last();
    QCOMPARE(values[0].toULongLong(), 101);
    QVERIFY2(values[2].toString().isEmpty(), qPrintable(values[2].toString()));
    const auto targets = qvariant_cast<QVector<LspLocation>>(values[1]);
    QCOMPARE(targets.size(), 1);
    QCOMPARE(targets.first().line, 2);
    QCOMPARE(readSourceFile(path).bytes, disk);
#endif
}

void WorkspaceTests::independentLookupChannelsAndReferences() {
#ifndef CODECAIRN_TEST_CLANGD
    QSKIP("clangd is not installed.");
#else
    QTemporaryDir dir;
    const auto path = dir.filePath("channels.cpp");
    const QByteArray source = "int callee() { return 1; }\nint caller() { return callee(); }\n";
    write(path, source);
    compilationDatabase(dir.path(), {"channels.cpp"});
    LanguageServerClient client;
    QSignalSpy ready(&client, &LanguageServerClient::ready),
        definitions(&client, &LanguageServerClient::definitionReady),
        replies(&client, &LanguageServerClient::locationsReady);
    client.start(dir.path(), {}, CODECAIRN_TEST_CLANGD);
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 15000);
    const int position = static_cast<int>(source.lastIndexOf("callee"));
    client.definition(path, source, 1, position, 10);
    client.locations(path, source, 1, position, 11, "hover");
    client.locations(path, source, 1, position, 12, "navigation");
    client.locations(path, source, 1, position, 13, "references", true, false);
    QTRY_COMPARE_WITH_TIMEOUT(replies.count(), 3, 15000);
    QTRY_COMPARE_WITH_TIMEOUT(definitions.count(), 1, 15000);
    for (const auto &reply : replies) {
        QVERIFY2(reply[3].toString().isEmpty(), qPrintable(reply[3].toString()));
        const auto targets = qvariant_cast<QVector<LspLocation>>(reply[2]);
        QCOMPARE(targets.size(), 1);
        QCOMPARE(targets.first().line, reply[1].toString() == "references" ? 1 : 0);
    }
#endif
}

void WorkspaceTests::openFileIndexesParentAndPreviewsUnopenedSymbol() {
    QTemporaryDir dir;
    write(dir.filePath("one.cpp"), "int one() { return 1; }\n");
    const QByteArray other = "int hidden_function() { return 3; }\n";
    write(dir.filePath("unopened.cpp"), other);
    MainWindow window(false);
    window.show();
    QSignalSpy indexed(&window, &MainWindow::indexReady);
    QVERIFY(window.openFile(dir.filePath("one.cpp")));
    QTRY_VERIFY_WITH_TIMEOUT(indexed.count() > 0, 15000);
    QVERIFY(QFileInfo::exists(workspaceDatabase(dir.path())));
    auto *symbols = window.findChild<QTreeWidget *>("symbolList");
    QTRY_COMPARE_WITH_TIMEOUT(symbols->topLevelItemCount(), 2, 15000);
    QTreeWidgetItem *hidden = nullptr;
    for (int i = 0; i < symbols->topLevelItemCount(); ++i)
        if (symbols->topLevelItem(i)->text(0) == "hidden_function")
            hidden = symbols->topLevelItem(i);
    QVERIFY(hidden);
    symbols->setCurrentItem(hidden);
    QTRY_COMPARE_WITH_TIMEOUT(window.contextEditor()->bytes(), other, 10000);
    QCOMPARE(window.documentCount(), 1);
    QVERIFY(window.findChild<QAction *>("rebuildSymbols")->isEnabled());
    window.findChild<QAction *>("rebuildAll")->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(indexed.count() > 1, 10000);
}

void WorkspaceTests::contextShowsCalleeNotCaller() {
#ifndef CODECAIRN_TEST_CLANGD
    QSKIP("clangd is not installed.");
#else
    QTemporaryDir dir;
    const QByteArray code = "int callee(int x) { return x; }\nint caller() { return callee(4); }\n";
    write(dir.filePath("example.cpp"), code);
    compilationDatabase(dir.path(), {"example.cpp"});
    MainWindow window(false);
    window.show();
    QSignalSpy parsed(&window, &MainWindow::analysisReady);
    QSignalSpy languageReplies(window.findChild<LanguageServerClient *>(),
                               &LanguageServerClient::definitionReady);
    QSignalSpy languageStatus(window.findChild<LanguageServerClient *>(),
                              &LanguageServerClient::status);
    QVERIFY(window.openFile(dir.filePath("example.cpp")));
    QTRY_VERIFY_WITH_TIMEOUT(parsed.count() > 0, 10000);
    auto *editor = window.activeEditor();
    const int call = static_cast<int>(code.lastIndexOf("callee"));
    editor->selectRange(call, call);
    auto *title = window.findChild<QLabel *>("contextTitle");
    QTRY_VERIFY2_WITH_TIMEOUT(
        title->text().contains("clangd definition"),
        qPrintable(title->text() + " | last error: " +
                   (languageReplies.isEmpty() ? "none" : languageReplies.last()[2].toString()) +
                   " | status: " +
                   (languageStatus.isEmpty() ? "none" : languageStatus.last()[0].toString())),
        10000);
    QCOMPARE(window.contextEditor()->send(SCI_GETSELECTIONSTART), code.indexOf("callee"));
    QCOMPARE(window.documentCount(), 1);
#endif
}

void WorkspaceTests::missingCDefinitionDoesNotPreviewItsCallAsDefinition() {
#ifndef CODECAIRN_TEST_CLANGD
    QSKIP("clangd is not installed.");
#else
    QTemporaryDir directory;
    const QByteArray source = "#include <missing_project_dependency.h>\n"
                              "int caller(void) { return unavailable_definition(1); }\n";
    const auto path = directory.filePath("missing.c");
    write(path, source);
    MainWindow window(false);
    window.show();
    QSignalSpy analyzed(&window, &MainWindow::analysisReady);
    QVERIFY(window.openFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(!analyzed.isEmpty(), 10000);
    auto *service = window.findChild<LanguageServerClient *>();
    QVERIFY(!service->hasCompilationDatabase());
    QSignalSpy definitions(service, &LanguageServerClient::definitionReady);
    const int call = source.indexOf("unavailable_definition");
    window.activeEditor()->setFocus();
    window.activeEditor()->selectRange(call, call + qstrlen("unavailable_definition"));
    QTRY_VERIFY_WITH_TIMEOUT(!definitions.isEmpty(), 10000);
    auto *title = window.findChild<QLabel *>("contextTitle");
    QTRY_VERIFY2_WITH_TIMEOUT(title->text().startsWith("Unresolved: unavailable_definition"),
                              qPrintable(title->text()), 10000);
    QVERIFY(title->toolTip().contains("compilation database"));
    QVERIFY(window.contextEditor()->bytes().isEmpty());
    window.activeEditor()->selectRange(source.indexOf("caller"), source.indexOf("caller") + 6);
    QTRY_VERIFY_WITH_TIMEOUT(title->text().contains("clangd definition"), 10000);
    QCOMPARE(window.contextEditor()->send(SCI_GETSELECTIONSTART), source.indexOf("caller"));
#endif
}

void WorkspaceTests::externalChangesReloadCleanBuffersOnly() {
    QTemporaryDir dir;
    const auto path = dir.filePath("changing.cpp");
    write(path, "int before();\n");
    MainWindow window(false);
    QSignalSpy indexed(&window, &MainWindow::indexReady),
        parsed(&window, &MainWindow::analysisReady);
    QVERIFY(window.openFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(indexed.count() > 0 && parsed.count() > 0, 10000);
    write(path, "int changed_on_disk();\n");
    window.findChild<QAction *>("refreshFiles")->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(window.activeEditor()->bytes(),
                              QByteArray("int changed_on_disk();\n"), 10000);
    QVERIFY(!window.activeEditor()->isModified());
    window.activeEditor()->send(SCI_SELECTALL);
    window.activeEditor()->sends(SCI_REPLACESEL, 0, "int unsaved();\n");
    const auto count = indexed.count();
    write(path, "int disk_again();\n");
    window.findChild<QAction *>("refreshFiles")->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(indexed.count() > count, 10000);
    QCOMPARE(window.activeEditor()->bytes(), QByteArray("int unsaved();\n"));
    QVERIFY(window.activeEditor()->isModified());
}

void WorkspaceTests::ambiguousNamesAreNotGuessed() {
    QTemporaryDir dir;
    write(dir.filePath("first.cpp"), "static int duplicate() { return 1; }\n");
    write(dir.filePath("second.cpp"), "static int duplicate() { return 2; }\n");
    QVERIFY(index(dir.path()).error.isEmpty());
    SymbolQuery query;
    query.text = "duplicate";
    query.exact = true;
    const auto page = queryWorkspaceSymbols(dir.path(), query);
    QCOMPARE(page.total, 2);
    QCOMPARE(page.symbols.size(), 2);
    QVERIFY(page.symbols[0].path != page.symbols[1].path);
}

void WorkspaceTests::qualifiedMethodsRetainScope() {
    const auto result = analyzeSource("namespace N { struct Worker { int run(); }; }\n"
                                      "int N::Worker::run() { return 1; }\n",
                                      "methods.cpp");
    QVERIFY(result.error.isEmpty());
    int count = 0;
    for (const auto &symbol : result.symbols) {
        if (symbol.name == "run") {
            QCOMPARE(symbol.qualifiedName, "N::Worker::run");
            ++count;
        }
    }
    QCOMPARE(count, 2);
}

QTEST_MAIN(WorkspaceTests)
#include "WorkspaceTests.moc"

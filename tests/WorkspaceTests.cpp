#include "WorkspaceIndex.h"
#include "LanguageServerClient.h"
#include "FileIO.h"
#include "MainWindow.h"
#include "TestStorage.h"
#include "WorkspaceOptionsStore.h"
#include "CompilationDatabase.h"
#include "CompilerSymbolRecovery.h"

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
#include <QLockFile>
#include <QProcess>
#include <QSet>
#include <QSignalSpy>
#include <QStatusBar>
#include <QScrollBar>
#include <QSemaphore>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QtConcurrentRun>

namespace {
IndexResult index(const QString &root, bool force = false, bool rescan = true) {
    return buildWorkspaceIndex(workspacePath(root), force, rescan, {".git", "build"},
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
    void corruptDatabaseIsReported();
    void unwritableMetadataIsReported();
    void workspaceStorageIsIsolatedAndCoLocated();
    void invalidWorkspaceIdentityIsRejected();
    void workspaceIndexLockIsPerWorkspace();
    void readOnlySourceFolderCanBeIndexed();
    void compilerRecoveryUsesRelocatedCompilationDatabase();
    void watcherRefreshAndWorkspaceSwitch();
    void automaticRefreshIgnoresMetadata();
    void unchangedAutomaticRefreshIsQuiet();
    void refreshesCoalesceWithoutCancelling();
    void indexingDoesNotWaitForEditorWorkers();
    void refreshPreservesBrowserState();
    void externalWorkspaceIndex();
    void projectSymbolListContainsAllSymbols();
    void utf16Conversions();
    void discoversClangdWithoutShellPath();
    void realClangdCrossFileOverloads();
    void realClangdUnsavedAndStaleRequests();
    void independentLookupChannelsAndReferences();
    void openFileIndexesParentAndPreviewsUnopenedSymbol();
    void contextShowsCalleeNotCaller();
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
    QByteArray large;
    for (int i = 0; i < 2500; ++i)
        large += "int function_" + QByteArray::number(i) + "(int x) { return x + 1; }\n";
    for (int i = 0; i < 20; ++i)
        write(dir.filePath(QString("large_%1.cpp").arg(i)), large);
    const auto cancel = std::make_shared<std::atomic_bool>(false);
    const auto progress = std::make_shared<IndexProgress>();
    auto future = QtConcurrent::run([&] {
        return buildWorkspaceIndex(workspacePath(dir.path()), true, true, {}, cancel,
                                   progress);
    });
    QTRY_VERIFY_WITH_TIMEOUT(progress->parsed.load() > 0, 10000);
    cancel->store(true);
    future.waitForFinished();
    QVERIFY(future.result().cancelled);
    const auto result = queryWorkspaceSymbols(dir.path(), {});
    QCOMPARE(result.total, 1);
    QCOMPARE(result.symbols.first().symbol.name, "seed");
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

void WorkspaceTests::unwritableMetadataIsReported() {
    QTemporaryDir dir;
    const auto metadata = workspaceStorageDirectory(dir.path());
    QVERIFY(QDir().mkpath(QFileInfo(metadata).absolutePath()));
    write(metadata, "a file occupies the metadata directory");
    write(dir.filePath("source.cpp"), "int source();\n");
    const auto result = index(dir.path());
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(readSourceFile(metadata).bytes,
             QByteArray("a file occupies the metadata directory"));
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
    QCOMPARE(QFileInfo(workspaceOptionsPath(first)).absolutePath(), workspaceStorageDirectory(first));
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
        {"directory", source.path()}, {"file", path},
        {"arguments", QJsonArray{"clang", "-DUSE_SELECTED_CONFIGURATION", "-c", path}}}};
    write(build.filePath("compile_commands.json"), QJsonDocument(commands).toJson());
    const auto database = prepareCompilationDatabase(source.path(), build.path());
    QVERIFY2(database.error.isEmpty(), qPrintable(database.error));
    QVERIFY(database.configured);
    QCOMPARE(database.directory, workspaceClangdDirectory(source.path()));
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

void WorkspaceTests::watcherRefreshAndWorkspaceSwitch() {
    QTemporaryDir first, second;
    write(first.filePath("one.cpp"), "int one();\n");
    write(second.filePath("two.cpp"), "int two();\n");
    WorkspaceIndex service;
    QSignalSpy ready(&service, &WorkspaceIndex::ready);
    service.open(first.path());
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() >= 1, 10000);
    write(first.filePath("added.cpp"), "int added();\n");
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() >= 2, 10000);
    QCOMPARE(service.files().size(), 2);
    service.refresh(true, true);
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
    WorkspaceIndex service;
    QSignalSpy ready(&service, &WorkspaceIndex::ready);
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
    auto *service = window.findChild<WorkspaceIndex *>();
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
    WorkspaceIndex service;
    QSignalSpy ready(&service, &WorkspaceIndex::ready);
    QSignalSpy failed(&service, &WorkspaceIndex::failed);
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
    auto *service = window.findChild<WorkspaceIndex *>();
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
    WorkspaceIndex service;
    QSignalSpy ready(&service, &WorkspaceIndex::ready);
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
    QVERIFY(!window.findChild<QAction *>("nextSymbolPage"));
    QVERIFY(!window.findChild<QAction *>("previousSymbolPage"));
    QSet<QString> names;
    for (int i = 0; i < symbols->topLevelItemCount(); ++i)
        names.insert(symbols->topLevelItem(i)->text(0));
    QCOMPARE(names.size(), 650);
    window.findChild<QLineEdit *>("symbolFilter")->setText("function_");
    QTRY_COMPARE_WITH_TIMEOUT(symbols->topLevelItemCount(), 650, 10000);
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
    QVERIFY(QFileInfo::exists(QDir(workspaceClangdDirectory(dir.path()))
                                  .filePath("compile_commands.json")));
    QTRY_VERIFY_WITH_TIMEOUT(
        !QDir(QDir(workspaceClangdDirectory(dir.path())).filePath(".cache/clangd/index"))
             .entryList({"*.idx"}, QDir::Files).isEmpty(),
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

#include <codecairn/Engine.h>
#include <codecairn/Project.h>
#include <codecairn/FileIO.h>
#include <codecairn/ProjectDiscovery.h>
#include "TestStorage.h"

#include <QDir>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>

#if defined(QT_GUI_LIB) || defined(QT_WIDGETS_LIB) || defined(SCINTILLA_QT)
#error Core tests must not inherit GUI dependencies.
#endif

#if __has_include("ProjectLanguages.h") ||                                                         \
                  __has_include("LanguageServerClient.h") ||                                       \
                                __has_include("ProjectIndexer.h") ||                               \
                                              __has_include("ProjectIndexStore.h") ||              \
                                                            __has_include("ProjectSymbolQuery.h")
#error Domain consumers must not inherit implementation service headers.
#endif

using namespace codecairn;

class CoreTests : public QObject {
    Q_OBJECT
    TestStorage storage_;
    const ProjectOptions manual_{IndexStartup::Manual, LanguageStartup::Manual};

private slots:
    void projectIndexLifecyclePublishesAndReusesSavedFiles() {
        QTemporaryDir directory;
        const auto first = directory.filePath("first.py");
        const auto second = directory.filePath("second.py");
        QVERIFY(writeSourceFile(first, "def first(): pass\n", false).isEmpty());
        QVERIFY(writeSourceFile(second, "def second(): pass\n", false).isEmpty());
        Engine engine;
        auto *project = engine.createProject();
        QSignalSpy indexed(project, &Project::indexReady);
        QSignalSpy failures(project, &Project::failed);
        bool catalogWhileBusy = false;
        bool symbolsWhileBusy = false;
        connect(project, &Project::catalogReady, this, [&](const QStringList &paths) {
            catalogWhileBusy = project->indexing() && paths.size() == 2;
        });
        connect(project, &Project::indexBatchReady, this,
                [&] { symbolsWhileBusy = symbolsWhileBusy || project->indexing(); });
        QVERIFY(project->open(ProjectSpec::folder(directory.path()),
                              {IndexStartup::Automatic, LanguageStartup::Manual}));
        QTRY_COMPARE(indexed.size(), 1);
        QVERIFY(catalogWhileBusy);
        QVERIFY(symbolsWhileBusy);
        QCOMPARE(qvariant_cast<IndexResult>(indexed[0][1]).parsed, 2);
        QSignalSpy pages(project, &Project::symbolsReady);
        codecairn::SymbolQuery all;
        all.pageSize = 100;
        project->findSymbols(all);
        QTRY_COMPARE(pages.size(), 1);
        auto page = qvariant_cast<SymbolPage>(pages[0][1]);
        QVERIFY(page.error.isEmpty());
        QCOMPARE(page.total, 2);
        project->refreshIndex();
        QTRY_COMPARE(indexed.size(), 2);
        auto result = qvariant_cast<IndexResult>(indexed[1][1]);
        QCOMPARE(result.parsed, 0);
        QCOMPARE(result.reused, 2);
        auto document = engine.openDocument(first);
        QVERIFY(document);
        document.value()->updateContents("def unsaved(): pass\n");
        QSignalSpy analyses(project, &Project::analysisReady);
        project->analyze(document.value()->snapshot());
        QTRY_COMPARE(analyses.size(), 1);
        all.text = "unsaved";
        project->findSymbols(all);
        QTRY_COMPARE(pages.size(), 2);
        QCOMPARE(qvariant_cast<SymbolPage>(pages[1][1]).total, 1);
        QCOMPARE(readSourceFile(first).bytes, QByteArray("def first(): pass\n"));
        QVERIFY(engine.closeDocument(document.value()));
        QVERIFY(QFile::remove(second));
        QVERIFY(writeSourceFile(first, "def updated(): pass\n", false).isEmpty());
        project->refreshIndex();
        QTRY_COMPARE(indexed.size(), 3);
        result = qvariant_cast<IndexResult>(indexed[2][1]);
        QCOMPARE(result.files.size(), 1);
        QCOMPARE(result.parsed, 1);
        all.text.clear();
        project->findSymbols(all);
        QTRY_COMPARE(pages.size(), 3);
        page = qvariant_cast<SymbolPage>(pages[2][1]);
        QCOMPARE(page.total, 1);
        QCOMPARE(page.symbols[0].symbol.name, "updated");
        QVERIFY(failures.isEmpty());
    }

    void switchingProjectsRejectsQueuedCatalogAndQueries() {
        QTemporaryDir first, second;
        QVERIFY(
            writeSourceFile(first.filePath("first.py"), "def first(): pass\n", false).isEmpty());
        QVERIFY(
            writeSourceFile(second.filePath("second.py"), "def second(): pass\n", false).isEmpty());
        Engine engine;
        auto *project = engine.createProject();
        QSignalSpy indexed(project, &Project::indexReady);
        QSignalSpy pages(project, &Project::symbolsReady);
        const ProjectOptions automatic{IndexStartup::Automatic, LanguageStartup::Manual};
        QVERIFY(project->open(ProjectSpec::folder(first.path()), automatic));
        project->findSymbols({});
        QVERIFY(project->open(ProjectSpec::folder(second.path()), automatic));
        QTRY_COMPARE(indexed.size(), 1);
        QCOMPARE(qvariant_cast<IndexResult>(indexed[0][1]).root, workspacePath(second.path()));
        codecairn::SymbolQuery query;
        query.text = "second";
        const auto id = project->findSymbols(query);
        QTRY_VERIFY(std::any_of(pages.cbegin(), pages.cend(), [&](const auto &arguments) {
            return qvariant_cast<RequestId>(arguments[0]) == id &&
                   qvariant_cast<SymbolPage>(arguments[1]).total == 1;
        }));
        QCOMPARE(project->files(), QStringList{workspacePath(second.filePath("second.py"))});
        project->close();
        QVERIFY(project->files().isEmpty());
        QVERIFY(!project->indexing());
    }

    void cancelledIndexRetainsPublishedSymbolsAndResumes() {
        QTemporaryDir directory;
        for (int i = 0; i < 40; ++i)
            QVERIFY(writeSourceFile(directory.filePath(QString("file%1.py").arg(i)),
                                    "def target(): pass\n", false)
                        .isEmpty());
        Engine engine;
        auto *project = engine.createProject();
        bool cancelled = false;
        connect(project, &Project::indexBatchReady, this, [&] {
            if (!cancelled) {
                cancelled = true;
                project->cancelIndexing();
            }
        });
        QVERIFY(project->open(ProjectSpec::folder(directory.path()),
                              {IndexStartup::Automatic, LanguageStartup::Manual}));
        QTRY_VERIFY(cancelled && !project->indexing());
        QSignalSpy indexed(project, &Project::indexReady);
        project->refreshIndex();
        QTRY_COMPARE(indexed.size(), 1);
        const auto result = qvariant_cast<IndexResult>(indexed[0][1]);
        QVERIFY(!result.cancelled);
        QCOMPARE(result.files.size(), 40);
        QVERIFY(result.reused > 0);
        QSignalSpy pages(project, &Project::symbolsReady);
        codecairn::SymbolQuery query;
        query.text = "target";
        query.pageSize = 100;
        project->findSymbols(query);
        QTRY_COMPARE(pages.size(), 1);
        QCOMPARE(qvariant_cast<SymbolPage>(pages[0][1]).total, 40);
    }

    void documentsHaveOneIdentityAndIndependentProjectLifetime() {
        QTemporaryDir directory;
        const auto path = directory.filePath("shared.py");
        QVERIFY(writeSourceFile(path, "def saved(): pass\n", false).isEmpty());
        Engine engine;
        auto opened = engine.openDocument(path);
        QVERIFY(opened);
        auto *document = opened.value();
        document->updateContents("def unsaved(): pass\n");
        auto again = engine.openDocument(directory.filePath("./shared.py"));
        QVERIFY(again);
        QCOMPARE(again.value(), document);
        auto duplicate = engine.createDocument(path, "must not overwrite");
        QVERIFY(duplicate);
        QCOMPARE(duplicate.value(), document);
        QCOMPARE(document->snapshot().bytes, QByteArray("def unsaved(): pass\n"));
        QSignalSpy closed(&engine, &Engine::documentClosed);
        const auto revision = document->revision();
        QVERIFY(engine.renameDocument(document, directory.filePath("./shared.py")));
        QCOMPARE(document->revision(), revision);
        QCOMPARE(closed.size(), 0);
        auto first = engine.openProject(ProjectSpec::folder(directory.path()), manual_);
        auto second = engine.openProject(prepareProject(directory.path()), manual_);
        QVERIFY(first);
        QVERIFY(second);
        QVERIFY(first.value() != second.value());
        QVERIFY(engine.closeProject(first.value()));
        QCOMPARE(engine.document(path), document);
        QVERIFY(document->modified());
        QCOMPARE(readSourceFile(path).bytes, QByteArray("def saved(): pass\n"));
        QPointer<Document> released = document;
        QVERIFY(engine.closeDocument(document));
        QVERIFY(released.isNull());
        QVERIFY(!engine.document(path));
    }

    void standaloneAnalysisAndSourceReadsUseUnsavedSnapshots() {
        QTemporaryDir directory;
        const auto path = directory.filePath("source.py");
        const QByteArray disk = "def saved(): pass\n";
        const QByteArray live = "def unsaved(): pass\n";
        QVERIFY(writeSourceFile(path, disk, false).isEmpty());
        Engine engine;
        auto opened = engine.openDocument(path);
        QVERIFY(opened);
        auto *document = opened.value();
        document->updateContents(live);
        QSignalSpy analyses(&engine, &Engine::analysisReady);
        const auto id = engine.analyze(document->snapshot());
        QTRY_COMPARE(analyses.size(), 1);
        QCOMPARE(qvariant_cast<RequestId>(analyses[0][0]), id);
        const auto analyzed = qvariant_cast<AnalysisResult>(analyses[0][1]);
        QVERIFY(analyzed.analysis.error.isEmpty());
        QCOMPARE(analyzed.analysis.symbols[0].name, "unsaved");
        engine.analyze(document->snapshot(), AnalysisDetail::Symbols);
        QTRY_COMPARE(analyses.size(), 2);
        const auto symbolsOnly = qvariant_cast<AnalysisResult>(analyses[1][1]);
        QVERIFY(symbolsOnly.analysis.styles.isEmpty());
        QCOMPARE(symbolsOnly.analysis.symbols[0].name, "unsaved");
        QSignalSpy reads(&engine, &Engine::sourceReady);
        const SourceLocation target{path, 4, 11, -1, document->snapshot().hash};
        const auto read = engine.readSource(target);
        QTRY_COMPARE(reads.size(), 1);
        QCOMPARE(qvariant_cast<RequestId>(reads[0][0]), read);
        const auto source = qvariant_cast<SourceRead>(reads[0][1]);
        QCOMPARE(source.file.bytes, live);
        QVERIFY(source.range.error.isEmpty());
        QCOMPARE(readSourceFile(path).bytes, disk);
    }

    void cancelledReadsAndAnalysisCannotDeliverLateResults() {
        Engine engine;
        auto document = engine.createDocument("standalone.py", "def target(): pass\n");
        QVERIFY(document);
        QSignalSpy analyses(&engine, &Engine::analysisReady);
        engine.cancel(engine.analyze(document.value()->snapshot()));
        QSignalSpy reads(&engine, &Engine::sourceReady);
        engine.cancel(engine.readSource({document.value()->path(), 0, 0}));
        const auto current = engine.analyze(document.value()->snapshot());
        QTRY_COMPARE(analyses.size(), 1);
        QCOMPARE(qvariant_cast<RequestId>(analyses[0][0]), current);
        QCOMPARE(reads.size(), 0);
    }

    void projectAnalysesRemainIndependent() {
        QTemporaryDir directory;
        const auto path = directory.filePath("source.py");
        Engine engine;
        auto document = engine.createDocument(path, "def target(): pass\n");
        QVERIFY(document);
        auto a = engine.openProject(prepareProject(directory.path()), manual_);
        auto b = engine.openProject(prepareProject(directory.path()), manual_);
        QVERIFY(a);
        QVERIFY(b);
        QSignalSpy first(a.value(), &Project::analysisReady);
        QSignalSpy second(b.value(), &Project::analysisReady);
        const auto snapshot = document.value()->snapshot();
        const auto obsolete = a.value()->analyze(snapshot);
        const auto accepted = b.value()->analyze(snapshot);
        a.value()->close();
        QTRY_COMPARE(second.size(), 1);
        QCOMPARE(qvariant_cast<RequestId>(second[0][0]), accepted);
        QVERIFY(obsolete != accepted);
        QCOMPARE(first.size(), 0);
        QCOMPARE(qvariant_cast<AnalysisResult>(second[0][1]).source.bytes, snapshot.bytes);
        QVERIFY(engine.document(path));
    }

    void sharedFileUsesEachProjectsCompilerConfiguration() {
        QTemporaryDir directory, firstCommands, secondCommands;
        const auto path = directory.filePath("macro.c");
        const QByteArray source =
            "#ifdef PROJECT_FLAG\n#define TEST(name) int configured_##name(void)\n"
            "#else\n#define TEST(name) int other_##name(void)\n#endif\n"
            "TEST(function) { return 1; }\n";
        QVERIFY(writeSourceFile(path, source, false).isEmpty());
        const auto commands = [&](const QString &folder, bool flag) {
            QJsonArray arguments{"clang", "-c", path};
            if (flag)
                arguments.append("-DPROJECT_FLAG");
            return writeSourceFile(
                QDir(folder).filePath("compile_commands.json"),
                QJsonDocument(QJsonArray{QJsonObject{{"directory", directory.path()},
                                                     {"file", path},
                                                     {"arguments", arguments}}})
                    .toJson(),
                false);
        };
        QVERIFY(commands(firstCommands.path(), true).isEmpty());
        QVERIFY(commands(secondCommands.path(), false).isEmpty());
        WorkspaceOptions options;
        options.managed = true;
        options.includedPaths = {"macro.c"};
        options.name = "First";
        options.compilationDirectory = firstCommands.path();
        const auto firstFile = directory.filePath("first.cairn");
        QVERIFY(WorkspaceOptionsStore::saveProject(firstFile, directory.path(), options).isEmpty());
        options.name = "Second";
        options.compilationDirectory = secondCommands.path();
        const auto secondFile = directory.filePath("second.cairn");
        QVERIFY(
            WorkspaceOptionsStore::saveProject(secondFile, directory.path(), options).isEmpty());
        Engine engine;
        auto document = engine.openDocument(path);
        auto first = engine.openProject(prepareProject(directory.path(), firstFile), manual_);
        auto second = engine.openProject(prepareProject(directory.path(), secondFile), manual_);
        QVERIFY(document);
        QVERIFY(first);
        QVERIFY(second);
        QSignalSpy firstAnalysis(first.value(), &Project::analysisReady);
        QSignalSpy secondAnalysis(second.value(), &Project::analysisReady);
        const auto captured = document.value()->snapshot();
        first.value()->analyze(captured);
        second.value()->analyze(captured);
        QTRY_COMPARE(firstAnalysis.size(), 1);
        QTRY_COMPARE(secondAnalysis.size(), 1);
        const auto a = qvariant_cast<AnalysisResult>(firstAnalysis[0][1]);
        const auto b = qvariant_cast<AnalysisResult>(secondAnalysis[0][1]);
        QVERIFY2(a.analysis.error.isEmpty(), qPrintable(a.analysis.error));
        QVERIFY2(b.analysis.error.isEmpty(), qPrintable(b.analysis.error));
        QVERIFY(
            std::any_of(a.analysis.symbols.cbegin(), a.analysis.symbols.cend(),
                        [](const auto &symbol) { return symbol.name == "configured_function"; }));
        QVERIFY(std::any_of(b.analysis.symbols.cbegin(), b.analysis.symbols.cend(),
                            [](const auto &symbol) { return symbol.name == "other_function"; }));
        QCOMPARE(document.value()->revision(), captured.revision);
        QVERIFY(document.value()->snapshot().bytes.constData() == captured.bytes.constData());
    }

    void emptyCancellationDoesNotStopAutomaticIndexing() {
        QTemporaryDir directory;
        QVERIFY(writeSourceFile(directory.filePath("a.py"), "def a(): pass\n", false).isEmpty());
        Engine engine;
        auto *project = engine.createProject();
        QSignalSpy ready(project, &Project::indexReady);
        QVERIFY(project->open(prepareProject(directory.path()),
                              {IndexStartup::Automatic, LanguageStartup::Manual}));
        project->cancel({});
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(qvariant_cast<IndexResult>(ready[0][1]).files.size(), 1);
    }

    void cachedMembershipChangesWithProjectSelection() {
        QTemporaryDir directory;
        const auto first = directory.filePath("first.py");
        const auto second = directory.filePath("second.py");
        QVERIFY(writeSourceFile(first, "def first(): pass\n", false).isEmpty());
        QVERIFY(writeSourceFile(second, "def second(): pass\n", false).isEmpty());
        WorkspaceOptions configuration;
        configuration.managed = true;
        configuration.name = "Selected";
        configuration.includedPaths = {"first.py"};
        const auto file = directory.filePath("selection.cairn");
        QVERIFY(
            WorkspaceOptionsStore::saveProject(file, directory.path(), configuration).isEmpty());
        Engine engine;
        const auto project = engine.openProject(ProjectSpec::managed(file), manual_);
        QVERIFY(project);
        QVERIFY(engine.openDocument(first));
        QVERIFY(engine.openDocument(second));
        QVERIFY(project.value()->contains(first));
        QVERIFY(!project.value()->contains(second));
        configuration = project.value()->info().configuration;
        configuration.includedPaths = {"second.py"};
        QVERIFY(
            WorkspaceOptionsStore::saveProject(file, directory.path(), configuration).isEmpty());
        QVERIFY(project.value()->reloadOptions());
        QVERIFY(!project.value()->contains(first));
        QVERIFY(project.value()->contains(second));
    }

    void definitionsResolveTheSuppliedRevisionWithoutCallerOverlays() {
        QTemporaryDir directory;
        Engine engine;
        auto opened = engine.openProject(prepareProject(directory.path()), manual_);
        QVERIFY(opened);
        auto doc = engine.createDocument(directory.filePath("source.py"), "def before(): pass\n");
        QVERIFY(doc);
        doc.value()->updateContents("def after(): pass\n");
        DefinitionQuery query;
        query.at = doc.value()->position(5);
        query.resolution = DefinitionResolution::SyntaxOnly;
        QSignalSpy definitions(opened.value(), &Project::definitionsReady);
        const auto id = opened.value()->findDefinitions(query);
        QTRY_COMPARE(definitions.size(), 1);
        QCOMPARE(qvariant_cast<RequestId>(definitions[0][0]), id);
        const auto page = qvariant_cast<DefinitionPage>(definitions[0][1]);
        QVERIFY2(page.error.isEmpty(), qPrintable(page.error));
        QCOMPARE(page.definitions.size(), 1);
        QVERIFY(page.definitions[0].symbol);
        QCOMPARE(page.definitions[0].symbol->symbol.name, "after");
    }

    void searchesUseCursorsAndRejectChangedSources() {
        QTemporaryDir directory;
        const auto path = directory.filePath("source.txt");
        QVERIFY(writeSourceFile(path, "target target target\n", false).isEmpty());
        Engine engine;
        auto project = engine.openProject(prepareProject(directory.path()), manual_);
        QVERIFY(project);
        auto document = engine.openDocument(path);
        QVERIFY(document);
        TextQuery query;
        query.text = "target";
        query.files.scope = SearchScope::CurrentFile;
        query.files.currentFile = path;
        query.pageSize = 1;
        QSignalSpy pages(project.value(), &Project::textMatchesReady);
        project.value()->searchText(query);
        QTRY_COMPARE(pages.size(), 1);
        auto page = qvariant_cast<SearchPage>(pages[0][1]);
        QVERIFY(page.error.isEmpty());
        QCOMPARE(page.hits.size(), 1);
        QVERIFY(page.next);
        project.value()->continueSearch(page.next);
        QTRY_COMPARE(pages.size(), 2);
        page = qvariant_cast<SearchPage>(pages[1][1]);
        QCOMPARE(page.hits[0].location.start, 7);
        QVERIFY(page.next);
        document.value()->updateContents("changed");
        project.value()->continueSearch(page.next);
        QTRY_COMPARE(pages.size(), 3);
        QVERIFY(!qvariant_cast<SearchPage>(pages[2][1]).error.isEmpty());
    }

    void invalidOpenDoesNotCancelCurrentQueries() {
        QTemporaryDir directory;
        const auto path = directory.filePath("source.txt");
        QVERIFY(writeSourceFile(path, "target\n", false).isEmpty());
        Engine engine;
        auto opened = engine.openProject(prepareProject(directory.path()), manual_);
        QVERIFY(opened);
        TextQuery query;
        query.text = "target";
        query.files.scope = SearchScope::CurrentFile;
        query.files.currentFile = path;
        QSignalSpy pages(opened.value(), &Project::textMatchesReady);
        opened.value()->searchText(query);
        QVERIFY(!engine.openProject(ProjectSpec::folder(""), manual_));
        auto invalid = prepareProject(directory.filePath("missing"));
        QVERIFY(!opened.value()->open(invalid, manual_));
        QTRY_COMPARE(pages.size(), 1);
        QCOMPARE(qvariant_cast<SearchPage>(pages[0][1]).hits.size(), 1);
    }

    void referenceCursorErrorsStayOnTheReferenceOperation() {
        QTemporaryDir directory;
        const auto path = directory.filePath("source.txt");
        Engine engine;
        const auto source = engine.createDocument(path, "target target\n");
        QVERIFY(source);
        auto *project = engine.createProject();
        ReferenceQuery query;
        query.symbol = source.value()->position(0);
        query.resolution = ReferenceResolution::IdentifierOccurrences;
        query.files.scope = SearchScope::CurrentFile;
        query.files.currentFile = path;
        query.pageSize = 1;
        QSignalSpy references(project, &Project::referencesReady);
        QSignalSpy text(project, &Project::textMatchesReady);
        const auto request = project->findReferences(query);
        QTRY_COMPARE(references.size(), 1);
        const auto page = qvariant_cast<SearchPage>(references[0][1]);
        QVERIFY2(page.error.isEmpty(), qPrintable(page.error));
        QVERIFY(page.next);
        project->cancel(request);
        project->continueSearch(page.next);
        QTRY_COMPARE(references.size(), 2);
        QVERIFY(!qvariant_cast<SearchPage>(references[1][1]).error.isEmpty());
        QCOMPARE(text.size(), 0);
    }

    void cancellingDeferredValidationSuppressesItsCompletion() {
        Engine engine;
        auto *project = engine.createProject();
        QSignalSpy definitions(project, &Project::definitionsReady);
        const auto invalid = project->findDefinitions({});
        project->cancel(invalid);
        QSignalSpy analyses(&engine, &Engine::analysisReady);
        const auto document = engine.createDocument("standalone.py", "def flush(): pass\n");
        QVERIFY(document);
        engine.analyze(document.value()->snapshot());
        QTRY_COMPARE(analyses.size(), 1);
        QCOMPARE(definitions.size(), 0);
    }

    void refreshModesRetainDiscoveryAndReuseContracts() {
        QTemporaryDir directory;
        QVERIFY(writeSourceFile(directory.filePath("a.py"), "def a(): pass\n", false).isEmpty());
        Engine engine;
        auto *project = engine.createProject();
        QSignalSpy ready(project, &Project::indexReady);
        QSignalSpy failures(project, &Project::failed);
        QVERIFY(project->open(prepareProject(directory.path()),
                              {IndexStartup::Automatic, LanguageStartup::Manual}));
        QTRY_COMPARE(ready.size(), 1);
        project->cancelIndexing();
        QVERIFY(writeSourceFile(directory.filePath("b.py"), "def b(): pass\n", false).isEmpty());
        project->refreshIndex(IndexRefresh::ReparseKnownFiles);
        QTRY_COMPARE(ready.size(), 2);
        QCOMPARE(qvariant_cast<IndexResult>(ready[1][1]).files.size(), 1);
        project->cancelIndexing();
        project->refreshIndex(IndexRefresh::RebuildFilesAndSymbols);
        QTRY_COMPARE(ready.size(), 3);
        QCOMPARE(qvariant_cast<IndexResult>(ready[2][1]).files.size(), 2);
        QCOMPARE(qvariant_cast<IndexResult>(ready[2][1]).parsed, 2);
        QVERIFY(failures.isEmpty());
    }
};

QTEST_GUILESS_MAIN(CoreTests)
#include "CoreTests.moc"

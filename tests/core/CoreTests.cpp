#include "FileIO.h"
#include "NavigationTarget.h"
#include "NavigationService.h"
#include "ProjectDiscovery.h"
#include "ProjectSession.h"
#include "ReferenceSession.h"
#include "TestStorage.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#if defined(QT_GUI_LIB) || defined(QT_WIDGETS_LIB) || defined(SCINTILLA_QT)
#error Core tests must not inherit GUI dependencies.
#endif

class CoreTests : public QObject {
    Q_OBJECT
    TestStorage storage_;

private slots:
    void preparedProjectDoesNotSwitchActiveServices() {
        QTemporaryDir first;
        QTemporaryDir second;
        WorkspaceServices services;
        const auto initial = prepareProject(first.path());
        QVERIFY(services.commitProject(initial, false, false).isEmpty());
        const auto prepared = prepareProject(second.path());
        QCOMPARE(services.project().root, workspacePath(first.path()));
        auto invalid = prepared;
        invalid.error = "Preparation failed";
        QVERIFY(!services.commitProject(invalid, false, false).isEmpty());
        QCOMPARE(services.project().root, workspacePath(first.path()));
        QVERIFY(services.commitProject(prepared, false, false).isEmpty());
        QCOMPARE(services.project().root, workspacePath(second.path()));
        services.closeProject();
        QVERIFY(services.project().root.isEmpty());
        QVERIFY(services.index()->root().isEmpty());
    }

    void navigationOwnsLookupPolicyAndRejectsLateResults() {
        QTemporaryDir directory;
        const auto path = directory.filePath("source.py");
        const QByteArray source = "def target(): pass\n";
        QVERIFY(writeSourceFile(path, source, false).isEmpty());
        const auto cancelled = std::make_shared<std::atomic_bool>(false);
        const auto indexed = buildWorkspaceIndex(directory.path(), false, true, {}, cancelled);
        QVERIFY2(indexed.error.isEmpty(), qPrintable(indexed.error));
        WorkspaceServices services;
        NavigationService navigation(&services);
        DefinitionRequest request;
        request.root = directory.path();
        request.path = path;
        request.source = source;
        request.position = 5;
        request.policy = DefinitionPolicy::SyntaxOnly;
        QSignalSpy results(&navigation, &NavigationService::definitionReady);
        navigation.lookup(request, 1, "navigation");
        navigation.lookup(request, 2, "navigation");
        QTRY_COMPARE(results.size(), 1);
        QCOMPARE(results[0][0].toULongLong(), 2);
        auto result = qvariant_cast<DefinitionResult>(results[0][2]);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY(!result.semantic);
        QCOMPARE(result.candidates.targets.size(), 1);
        QCOMPARE(result.candidates.targets[0].symbol.name, "target");
        QByteArray live = source;
        navigation.setSnapshotProvider(
            [&](const QString &) -> std::optional<QByteArray> { return live; });
        navigation.lookup(request, 3, "navigation");
        live = "changed";
        QTRY_COMPARE(results.size(), 2);
        QVERIFY(qvariant_cast<DefinitionResult>(results[1][2]).error.contains("Source changed"));
        request.root.clear();
        request.policy = DefinitionPolicy::SemanticOnly;
        navigation.lookup(request, 4, "navigation");
        QCOMPARE(results.size(), 3);
        QVERIFY(!qvariant_cast<DefinitionResult>(results[2][2]).error.isEmpty());
    }

    void navigationReadsCoalesceAndValidateHashes() {
        QTemporaryDir directory;
        const auto path = directory.filePath("source.txt");
        const QByteArray bytes = "target\n";
        QVERIFY(writeSourceFile(path, bytes, false).isEmpty());
        WorkspaceServices services;
        NavigationService navigation(&services);
        QSignalSpy results(&navigation, &NavigationService::readReady);
        NavigationLocation location{path, 0, 6, -1, sourceContentHash(bytes)};
        navigation.read(location, 1);
        navigation.read(location, 2);
        navigation.read(location, 3);
        QTRY_COMPARE(results.size(), 1);
        QCOMPARE(results[0][0].toULongLong(), 3);
        auto result = qvariant_cast<NavigationRead>(results[0][1]);
        QVERIFY(result.file.error.isEmpty());
        QVERIFY(result.range.error.isEmpty());
        QCOMPARE(result.file.bytes, bytes);
        location.expectedHash = "outdated";
        navigation.read(location, 4);
        QTRY_COMPARE(results.size(), 2);
        QVERIFY(qvariant_cast<NavigationRead>(results[1][1]).range.stale);
    }

    void referenceInputsKeepScopeAndOverlayRules() {
        QTemporaryDir directory;
        const auto first = directory.filePath("a.py");
        const auto second = directory.filePath("b.cpp");
        DocumentAnalysis clean(first, "def first(): pass\n", false, 0);
        DocumentAnalysis dirty(second, "int saved;\n", false, 0);
        dirty.setBytes("int unsaved;\n");
        const auto overlays =
            captureDocumentOverlays({{&clean, false}, {&dirty, true}}, directory.path(), {});
        QCOMPARE(overlays.size(), 1);
        QCOMPARE(overlays[0].source, QByteArray("int unsaved;\n"));
        ReferenceInputs inputs;
        inputs.root = directory.path();
        inputs.catalog = {first};
        inputs.overlays = overlays;
        inputs.filePatterns = "*.cpp";
        QCOMPARE(resolveReferenceScope(inputs), QStringList{second});
        inputs.scope = ReferenceScope::CurrentFile;
        inputs.currentFile = first;
        QVERIFY(resolveReferenceScope(inputs).isEmpty());
        inputs.scope = ReferenceScope::SelectedFiles;
        inputs.selectedFiles = {second, second, first};
        QCOMPARE(resolveReferenceScope(inputs), QStringList{second});
    }

    void discoveryAndProjectPreparation() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(QDir(directory.path()).mkpath("src"));
        QVERIFY(QDir(directory.path()).mkpath("build"));
        QVERIFY(writeSourceFile(directory.filePath("src/a.py"), "def target(): pass\n", false)
                    .isEmpty());
        QVERIFY(writeSourceFile(directory.filePath("build/ignored.py"), "pass\n", false).isEmpty());
        const auto project = prepareProject(directory.path());
        QVERIFY2(project.error.isEmpty(), qPrintable(project.error));
        auto options = project.options;
        options.includedPaths = {"."};
        const auto cancel = std::make_shared<std::atomic_bool>(false);
        const auto progress = std::make_shared<ProjectScanProgress>();
        const auto scan = discoverProjectFiles(project.root, options, 17, cancel, progress);
        QVERIFY(scan.errors.isEmpty());
        QCOMPARE(scan.generation, 17);
        QCOMPARE(scan.files, QStringList{"src/a.py"});
        QCOMPARE(progress->found.load(), 1);
        QVERIFY(writeSourceFile(directory.filePath("notes.txt"), "not a source file\n", false)
                    .isEmpty());
        const auto sources = discoverProjectFiles(project.root, options, 18, cancel,
                                                  std::make_shared<ProjectScanProgress>(),
                                                  ProjectFileScope::SourceFiles);
        QCOMPARE(sources.files, QStringList{"src/a.py"});
        const auto all = discoverProjectFiles(project.root, options, 19, cancel,
                                              std::make_shared<ProjectScanProgress>());
        QCOMPARE(all.files, (QStringList{"notes.txt", "src/a.py"}));
        cancel->store(true);
        QVERIFY(discoverProjectFiles(project.root, options, 18, cancel, progress).cancelled);
        QVERIFY(!prepareProject(directory.filePath("missing")).error.isEmpty());
    }

    void navigationRejectsChangedSourceAndConvertsUtf16() {
        const QByteArray source = QString::fromUtf8("a\xF0\x9F\x98\x80z\n").toUtf8();
        NavigationLocation location;
        location.path = "/source.txt";
        location.line = 0;
        location.character = 3;
        location.endLine = 0;
        location.endCharacter = 4;
        location.expectedHash = sourceContentHash(source);
        const auto resolved = resolveNavigationTarget(location, source);
        QVERIFY(resolved.error.isEmpty());
        QCOMPARE(resolved.start, 5);
        QCOMPARE(resolved.end, 6);
        const auto stale = resolveNavigationTarget(location, source + 'x');
        QVERIFY(stale.stale);
        QVERIFY(!stale.error.isEmpty());
    }

    void sharedSearchPaginationAndOverlayFreshness() {
        QTemporaryDir directory;
        const auto path = directory.filePath("source.py");
        const QByteArray bytes = "target target target\n";
        QVERIFY(writeSourceFile(path, bytes, false).isEmpty());
        WorkspaceServices services;
        ReferenceSession session(&services);
        ReferenceSearchRequest request;
        request.workspaceRoot = directory.path();
        request.query = "target";
        request.paths = {path};
        request.options.method = SearchMethod::Literal;
        request.options.comments = CommentSearch::Include;
        request.options.strings = true;
        request.options.smartMatching = false;
        request.limit = 1;
        request.overlays = {{path, bytes, {}, sourceContentHash(bytes)}};
        auto current = request.overlays;
        session.setSnapshotProvider(
            [&](const QString &) -> std::optional<QByteArray> { return current[0].source; });
        QSignalSpy pages(&session, &ReferenceSession::pageReady);
        session.start(request, 1);
        QTRY_COMPARE(pages.size(), 1);
        auto page = qvariant_cast<ReferenceResults>(pages[0][1]);
        QVERIFY2(page.error.isEmpty(), qPrintable(page.error));
        QCOMPARE(page.hits.size(), 1);
        QVERIFY(page.truncated);
        session.nextPage(2);
        QTRY_COMPARE(pages.size(), 2);
        page = qvariant_cast<ReferenceResults>(pages[1][1]);
        QCOMPARE(page.hits[0].location.start, 7);
        current[0].source = "changed";
        session.nextPage(3);
        QCOMPARE(pages.size(), 3);
        QVERIFY(!qvariant_cast<ReferenceResults>(pages[2][1]).error.isEmpty());
    }

    void cancelledSearchCannotDeliverToNewRequest() {
        QTemporaryDir directory;
        const auto path = directory.filePath("source.txt");
        QVERIFY(writeSourceFile(path, "old new\n", false).isEmpty());
        WorkspaceServices services;
        ReferenceSession session(&services);
        ReferenceSearchRequest request;
        request.paths = {path};
        request.options.method = SearchMethod::Literal;
        request.options.smartMatching = false;
        request.query = "old";
        QSignalSpy pages(&session, &ReferenceSession::pageReady);
        session.start(request, 1);
        session.cancel();
        request.query = "new";
        session.start(request, 2);
        QTRY_COMPARE(pages.size(), 1);
        QCOMPARE(pages[0][0].toULongLong(), 2);
        const auto page = qvariant_cast<ReferenceResults>(pages[0][1]);
        QVERIFY(page.error.isEmpty());
        QCOMPARE(page.hits.size(), 1);
        QCOMPARE(page.hits[0].location.start, 4);
    }

    void replacementHistoryRollsBackOnFailure() {
        ReferenceHistoryModel history;
        const ReferenceGroup original{"original", {}, {}, "old"};
        const ReferenceGroup replacement{"replacement", {}, {}, "new"};
        QVERIFY(history.append(original, false).isEmpty());
        QVERIFY(history.append(replacement, true).isEmpty());
        QCOMPARE(history.groups[0].query, "old");
        QVERIFY(!history.commitReplacement([] { return false; }));
        QCOMPARE(history.groups[0].query, "old");
        QVERIFY(history.pending.isEmpty());
        QVERIFY(history.append(replacement, true).isEmpty());
        QVERIFY(history.commitReplacement([] { return true; }));
        QCOMPARE(history.groups.size(), 1);
        QCOMPARE(history.groups[0].query, "new");
        auto stale = replacement;
        ReferenceHit hit;
        hit.stale = true;
        stale.hits.append(hit);
        QVERIFY(!history.append(stale, true).isEmpty());
        QVERIFY(history.pending.isEmpty());
        QCOMPARE(history.groups[0].query, "new");
    }

    void projectSelectionRulesAreTransactional() {
        QTemporaryDir directory;
        QVERIFY(QDir(directory.path()).mkpath("src"));
        WorkspaceOptions options;
        options.managed = true;
        QVERIFY(includeProjectPaths(directory.path(), options, {"."}).isEmpty());
        excludeProjectPaths(options, {"src"});
        QCOMPARE(options.includedPaths, QStringList{"."});
        QCOMPARE(options.excludedPaths, QStringList{"src"});
        QVERIFY(!includeProjectPaths(directory.path(), options, {"src/a.py"}).isEmpty());
        QCOMPARE(options.excludedPaths, QStringList{"src"});
        QVERIFY(includeProjectPaths(directory.path(), options, {"src"}).isEmpty());
        QVERIFY(options.excludedPaths.isEmpty());
        QCOMPARE(options.includedPaths, QStringList{"."});
    }
};

QTEST_GUILESS_MAIN(CoreTests)
#include "CoreTests.moc"

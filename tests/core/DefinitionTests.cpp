#include <codecairn/Engine.h>
#include <codecairn/Project.h>
#include <codecairn/FileIO.h>
#include "TestStorage.h"

#include <QDir>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

using namespace codecairn;

class DefinitionTests : public QObject {
    Q_OBJECT
    TestStorage storage_;
    const ProjectOptions manual_{IndexStartup::Manual, LanguageStartup::Manual};

    static DefinitionQuery syntaxQuery(Document *document, int position) {
        DefinitionQuery query;
        query.at = document->position(position);
        query.resolution = DefinitionResolution::SyntaxOnly;
        return query;
    }

private slots:
    void syntaxMatchesPreserveDeclarationsUnderBothOriginPolicies_data() {
        QTest::addColumn<bool>("excludeSelf");
        QTest::addColumn<bool>("atCall");
        QTest::addColumn<bool>("atIdentifierEnd");
        QTest::newRow("include-declaration") << false << false << false;
        QTest::newRow("exclude-declaration") << true << false << false;
        QTest::newRow("include-call") << false << true << false;
        QTest::newRow("exclude-call") << true << true << false;
        QTest::newRow("exclude-declaration-end") << true << false << true;
        QTest::newRow("exclude-call-end") << true << true << true;
    }

    void syntaxMatchesPreserveDeclarationsUnderBothOriginPolicies() {
        QFETCH(bool, excludeSelf);
        QFETCH(bool, atCall);
        QFETCH(bool, atIdentifierEnd);
        QTemporaryDir directory(QDir::current().filePath(".definition-tests-XXXXXX"));
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("source.py");
        const QByteArray saved = "def saved(): pass\n";
        const QByteArray live = "def target(): pass\n\ntarget()\n";
        QVERIFY(writeSourceFile(path, saved, false).isEmpty());
        Engine engine;
        const auto opened = engine.openProject(ProjectSpec::folder(directory.path()), manual_);
        QVERIFY(opened);
        const auto document = engine.openDocument(path);
        QVERIFY(document);
        document.value()->updateContents(live);
        const auto start = atCall ? live.lastIndexOf("target") : 4;
        auto query = syntaxQuery(document.value(), start + (atIdentifierEnd ? 6 : 1));
        query.origin =
            excludeSelf ? DefinitionOrigin::ExcludeUnresolvedSelf : DefinitionOrigin::Include;
        QSignalSpy ready(opened.value(), &Project::definitionsReady);
        const auto id = opened.value()->findDefinitions(query);
        QVERIFY(id);
        QCOMPARE(ready.size(), 0);
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(qvariant_cast<RequestId>(ready[0][0]), id);
        const auto page = qvariant_cast<DefinitionPage>(ready[0][1]);
        QVERIFY2(page.error.isEmpty(), qPrintable(page.error));
        QVERIFY(page.warning.isEmpty());
        QVERIFY(!page.semantic);
        QVERIFY(page.complete);
        QCOMPARE(page.total, 1);
        QCOMPARE(page.definitions.size(), 1);
        const auto &definition = page.definitions[0];
        QVERIFY(definition.symbol);
        QCOMPARE(definition.symbol->symbol.name, "target");
        QCOMPARE(definition.location.path, document.value()->path());
        QCOMPARE(definition.location.start, 4);
        QCOMPARE(definition.location.end, 10);
        QCOMPARE(definition.location.expectedHash, document.value()->snapshot().hash);
        QCOMPARE(readSourceFile(path).bytes, saved);
        QVERIFY(document.value()->modified());
    }

    void staleSourcesNeverPublishTargets_data() {
        QTest::addColumn<bool>("changeBeforeSubmission");
        QTest::newRow("already-stale") << true;
        QTest::newRow("changed-while-queued") << false;
    }

    void staleSourcesNeverPublishTargets() {
        QFETCH(bool, changeBeforeSubmission);
        QTemporaryDir directory(QDir::current().filePath(".definition-tests-XXXXXX"));
        QVERIFY(directory.isValid());
        Engine engine;
        const auto project = engine.openProject(ProjectSpec::folder(directory.path()), manual_);
        QVERIFY(project);
        const auto document =
            engine.createDocument(directory.filePath("source.py"), "def target(): pass\n");
        QVERIFY(document);
        const auto query = syntaxQuery(document.value(), 5);
        if (changeBeforeSubmission)
            document.value()->updateContents("def changed(): pass\n");
        QSignalSpy ready(project.value(), &Project::definitionsReady);
        const auto id = project.value()->findDefinitions(query);
        if (!changeBeforeSubmission)
            document.value()->updateContents("def changed(): pass\n");
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(qvariant_cast<RequestId>(ready[0][0]), id);
        const auto page = qvariant_cast<DefinitionPage>(ready[0][1]);
        QVERIFY2(page.error.contains("Source changed"), qPrintable(page.error));
        QVERIFY(page.definitions.isEmpty());
        QCOMPARE(page.total, 0);
    }

    void simultaneousSyntaxRequestsKeepTheirOwnIdentity() {
        QTemporaryDir directory(QDir::current().filePath(".definition-tests-XXXXXX"));
        QVERIFY(directory.isValid());
        Engine engine;
        const auto project = engine.openProject(ProjectSpec::folder(directory.path()), manual_);
        QVERIFY(project);
        const QByteArray source = "def first(): pass\n\ndef second(): pass\n";
        const auto document = engine.createDocument(directory.filePath("source.py"), source);
        QVERIFY(document);
        QSignalSpy ready(project.value(), &Project::definitionsReady);
        const auto first = project.value()->findDefinitions(syntaxQuery(document.value(), 5));
        const auto second = project.value()->findDefinitions(
            syntaxQuery(document.value(), source.indexOf("second") + 2));
        QVERIFY(first != second);
        QTRY_COMPARE(ready.size(), 2);
        bool sawFirst = false;
        bool sawSecond = false;
        for (const auto &arguments : ready) {
            const auto id = qvariant_cast<RequestId>(arguments[0]);
            const auto page = qvariant_cast<DefinitionPage>(arguments[1]);
            QVERIFY2(page.error.isEmpty(), qPrintable(page.error));
            QCOMPARE(page.definitions.size(), 1);
            QVERIFY(page.definitions[0].symbol);
            if (id == first) {
                QVERIFY(!sawFirst);
                sawFirst = true;
                QCOMPARE(page.definitions[0].symbol->symbol.name, "first");
            } else {
                QCOMPARE(id, second);
                QVERIFY(!sawSecond);
                sawSecond = true;
                QCOMPARE(page.definitions[0].symbol->symbol.name, "second");
            }
        }
        QVERIFY(sawFirst && sawSecond);
    }

    void simultaneousSemanticContextRequestsKeepDistinctTargets() {
        if (QStandardPaths::findExecutable("clangd").isEmpty())
            QSKIP("clangd is not available.");
        QTemporaryDir directory(QDir::current().filePath(".definition-tests-XXXXXX"));
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("source.c");
        const QByteArray source = "int first(void) { return 1; }\n"
                                  "int second(void) { return 2; }\n"
                                  "int caller(void) { return first() + second(); }\n";
        QVERIFY(writeSourceFile(path, source, false).isEmpty());
        Engine engine;
        const auto project = engine.openProject(ProjectSpec::folder(directory.path()),
                                                {IndexStartup::Manual, LanguageStartup::Automatic});
        QVERIFY(project);
        const auto document = engine.openDocument(path);
        QVERIFY(document);
        auto firstQuery = syntaxQuery(document.value(), source.lastIndexOf("first") + 2);
        firstQuery.resolution = DefinitionResolution::SemanticOnly;
        firstQuery.origin = DefinitionOrigin::ExcludeUnresolvedSelf;
        auto secondQuery = firstQuery;
        secondQuery.at = document.value()->position(source.lastIndexOf("second") + 2);
        QSignalSpy ready(project.value(), &Project::definitionsReady);
        QSignalSpy reads(&engine, &Engine::sourceReady);
        const auto first = project.value()->findDefinitions(firstQuery);
        const auto second = project.value()->findDefinitions(secondQuery);
        QVERIFY(first != second);
        QCOMPARE(ready.size(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 2, 30000);
        bool sawFirst = false;
        bool sawSecond = false;
        for (const auto &arguments : ready) {
            const auto id = qvariant_cast<RequestId>(arguments[0]);
            const auto page = qvariant_cast<DefinitionPage>(arguments[1]);
            QVERIFY2(page.error.isEmpty(), qPrintable(page.error));
            QVERIFY(page.warning.isEmpty());
            QVERIFY(page.semantic);
            QCOMPARE(page.total, 1);
            QCOMPARE(page.definitions.size(), 1);
            QByteArray name;
            if (id == first) {
                QVERIFY(!sawFirst);
                sawFirst = true;
                name = "first";
            } else {
                QCOMPARE(id, second);
                QVERIFY(!sawSecond);
                sawSecond = true;
                name = "second";
            }
            QCOMPARE(page.definitions[0].location.path, document.value()->path());
            reads.clear();
            const auto read = engine.readSource(page.definitions[0].location);
            QTRY_COMPARE(reads.size(), 1);
            QCOMPARE(qvariant_cast<RequestId>(reads[0][0]), read);
            const auto target = qvariant_cast<SourceRead>(reads[0][1]);
            QVERIFY2(target.file.error.isEmpty(), qPrintable(target.file.error));
            QVERIFY2(target.range.error.isEmpty(), qPrintable(target.range.error));
            QCOMPARE(target.file.bytes, source);
            QCOMPARE(target.range.start, source.indexOf(name));
            QCOMPARE(target.range.end, source.indexOf(name) + name.size());
            QCOMPARE(source.mid(target.range.start, target.range.end - target.range.start), name);
        }
        QVERIFY(sawFirst && sawSecond);
    }

    void cancellationSuppressesSyntaxAndQueuedValidationOnlyForTheirIds() {
        QTemporaryDir directory(QDir::current().filePath(".definition-tests-XXXXXX"));
        QVERIFY(directory.isValid());
        Engine engine;
        const auto project = engine.openProject(ProjectSpec::folder(directory.path()), manual_);
        QVERIFY(project);
        const auto document =
            engine.createDocument(directory.filePath("source.py"), "def target(): pass\n");
        QVERIFY(document);
        QSignalSpy ready(project.value(), &Project::definitionsReady);
        const auto invalid = project.value()->findDefinitions({});
        const auto cancelled = project.value()->findDefinitions(syntaxQuery(document.value(), 5));
        QCOMPARE(ready.size(), 0);
        project.value()->cancel(invalid);
        project.value()->cancel(cancelled);
        const auto accepted = project.value()->findDefinitions(syntaxQuery(document.value(), 5));
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(qvariant_cast<RequestId>(ready[0][0]), accepted);
        const auto page = qvariant_cast<DefinitionPage>(ready[0][1]);
        QVERIFY2(page.error.isEmpty(), qPrintable(page.error));
        QCOMPARE(page.definitions.size(), 1);

        ready.clear();
        const auto rejected = project.value()->findDefinitions({});
        QCOMPARE(ready.size(), 0);
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(qvariant_cast<RequestId>(ready[0][0]), rejected);
        const auto failure = qvariant_cast<DefinitionPage>(ready[0][1]);
        QVERIFY(!failure.error.isEmpty());
        QVERIFY(failure.definitions.isEmpty());
    }

    void closeSuppressesPendingDefinitionCompletions() {
        QTemporaryDir directory(QDir::current().filePath(".definition-tests-XXXXXX"));
        QVERIFY(directory.isValid());
        Engine engine;
        const auto project = engine.openProject(ProjectSpec::folder(directory.path()), manual_);
        QVERIFY(project);
        const auto document =
            engine.createDocument(directory.filePath("source.py"), "def target(): pass\n");
        QVERIFY(document);
        QSignalSpy ready(project.value(), &Project::definitionsReady);
        project.value()->findDefinitions(syntaxQuery(document.value(), 5));
        project.value()->findDefinitions({});
        project.value()->close();
        QVERIFY(project.value()->open(ProjectSpec::folder(directory.path()), manual_));
        const auto accepted = project.value()->findDefinitions(syntaxQuery(document.value(), 5));
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(qvariant_cast<RequestId>(ready[0][0]), accepted);
        QVERIFY(qvariant_cast<DefinitionPage>(ready[0][1]).error.isEmpty());
    }

    void unavailableSemanticProviderHonorsResolutionPolicy_data() {
        QTest::addColumn<bool>("semanticOnly");
        QTest::newRow("semantic-only-does-not-fallback") << true;
        QTest::newRow("semantic-preferred-exposes-fallback-warning") << false;
    }

    void unavailableSemanticProviderHonorsResolutionPolicy() {
        QFETCH(bool, semanticOnly);
        QTemporaryDir directory(QDir::current().filePath(".definition-tests-XXXXXX"));
        QVERIFY(directory.isValid());
        Engine engine;
        const auto project = engine.openProject(ProjectSpec::folder(directory.path()), manual_);
        QVERIFY(project);
        const auto document =
            engine.createDocument(directory.filePath("source.py"), "def target(): pass\n");
        QVERIFY(document);
        auto query = syntaxQuery(document.value(), 5);
        query.resolution = semanticOnly ? DefinitionResolution::SemanticOnly
                                        : DefinitionResolution::SemanticPreferred;
        QSignalSpy ready(project.value(), &Project::definitionsReady);
        const auto id = project.value()->findDefinitions(query);
        QCOMPARE(ready.size(), 0);
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(qvariant_cast<RequestId>(ready[0][0]), id);
        const auto page = qvariant_cast<DefinitionPage>(ready[0][1]);
        if (semanticOnly) {
            QVERIFY2(page.error.contains("No semantic provider"), qPrintable(page.error));
            QVERIFY(page.definitions.isEmpty());
            QCOMPARE(page.total, 0);
        } else {
            QVERIFY2(page.error.isEmpty(), qPrintable(page.error));
            QVERIFY2(page.warning.contains("No semantic provider"), qPrintable(page.warning));
            QVERIFY(!page.semantic);
            QCOMPARE(page.definitions.size(), 1);
            QVERIFY(page.definitions[0].symbol);
            QCOMPARE(page.definitions[0].symbol->symbol.name, "target");
        }
    }

    void crossFileDefinitionUsesAnalyzedUnsavedContext() {
        QTemporaryDir directory(QDir::current().filePath(".definition-tests-XXXXXX"));
        QVERIFY(directory.isValid());
        const auto targetPath = directory.filePath("target.py");
        const auto originPath = directory.filePath("origin.py");
        const QByteArray saved = "def saved(): pass\n";
        QVERIFY(writeSourceFile(targetPath, saved, false).isEmpty());
        QVERIFY(writeSourceFile(originPath, "target()\n", false).isEmpty());
        Engine engine;
        const auto project = engine.openProject(ProjectSpec::folder(directory.path()), manual_);
        QVERIFY(project);
        QSignalSpy indexed(project.value(), &Project::indexReady);
        project.value()->refreshIndex();
        QTRY_COMPARE(indexed.size(), 1);
        const auto target = engine.openDocument(targetPath);
        const auto origin = engine.openDocument(originPath);
        QVERIFY(target);
        QVERIFY(origin);
        target.value()->updateContents("def target(): pass\n");
        QSignalSpy analyses(project.value(), &Project::analysisReady);
        const auto analysisId = project.value()->analyze(target.value()->snapshot());
        QTRY_COMPARE(analyses.size(), 1);
        QCOMPARE(qvariant_cast<RequestId>(analyses[0][0]), analysisId);
        const auto analysis = qvariant_cast<AnalysisResult>(analyses[0][1]);
        QVERIFY2(analysis.analysis.error.isEmpty(), qPrintable(analysis.analysis.error));
        QCOMPARE(analysis.analysis.symbols.size(), 1);
        QCOMPARE(analysis.analysis.symbols[0].name, "target");
        QSignalSpy ready(project.value(), &Project::definitionsReady);
        const auto id = project.value()->findDefinitions(syntaxQuery(origin.value(), 2));
        QTRY_COMPARE(ready.size(), 1);
        QCOMPARE(qvariant_cast<RequestId>(ready[0][0]), id);
        const auto page = qvariant_cast<DefinitionPage>(ready[0][1]);
        QVERIFY2(page.error.isEmpty(), qPrintable(page.error));
        QCOMPARE(page.definitions.size(), 1);
        QVERIFY(page.definitions[0].symbol);
        QCOMPARE(page.definitions[0].symbol->symbol.name, "target");
        QCOMPARE(page.definitions[0].location.path, target.value()->path());
        QCOMPARE(page.definitions[0].location.expectedHash, target.value()->snapshot().hash);
        QCOMPARE(readSourceFile(targetPath).bytes, saved);
    }
};

QTEST_GUILESS_MAIN(DefinitionTests)
#include "DefinitionTests.moc"

#include <codecairn/Engine.h>
#include <codecairn/Project.h>
#include <codecairn/FileIO.h>
#include "TestStorage.h"
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QStandardPaths>

using namespace codecairn;

class SearchTests : public QObject {
    Q_OBJECT
    TestStorage storage_;

private slots:
    void semanticSearchesUseIndependentProviderRequests() {
        if (QStandardPaths::findExecutable("clangd").isEmpty())
            QSKIP("clangd is unavailable.");
        QTemporaryDir directory;
        const auto path = directory.filePath("source.c");
        const QByteArray source = "int alpha(void) { return 1; }\n"
                                  "int beta(void) { return 2; }\n"
                                  "int caller(void) { return alpha() + beta(); }\n";
        QVERIFY(writeSourceFile(path, source, false).isEmpty());
        Engine engine;
        const auto opened = engine.openProject(ProjectSpec::folder(directory.path()),
                                               {IndexStartup::Manual, LanguageStartup::Automatic});
        const auto document = engine.openDocument(path);
        QVERIFY(opened);
        QVERIFY(document);
        auto *project = opened.value();
        QSignalSpy replies(project, &Project::referencesReady);
        ReferenceQuery query;
        query.files.scope = SearchScope::CurrentFile;
        query.files.currentFile = path;
        query.symbol = document.value()->position(source.lastIndexOf("alpha"));
        const auto first = project->findReferences(query);
        query.symbol = document.value()->position(source.lastIndexOf("beta"));
        const auto second = project->findReferences(query);
        QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 2, 30000);
        for (const auto &reply : replies) {
            const auto id = qvariant_cast<RequestId>(reply[0]);
            const auto page = qvariant_cast<SearchPage>(reply[1]);
            QVERIFY2(page.error.isEmpty(), qPrintable(page.error));
            QVERIFY(id == first || id == second);
            QCOMPARE(page.hits.size(), 2);
            const QByteArray expected = id == first ? "alpha" : "beta";
            for (const auto &hit : page.hits)
                QCOMPARE(source.mid(hit.location.start, hit.location.end - hit.location.start),
                         expected);
        }
    }

    void concurrentSearchesKeepTheirOwnSourcesAndCursors() {
        QTemporaryDir directory;
        const auto path = directory.filePath("source.py");
        const QByteArray bytes = "alpha alpha\nbeta beta\n";
        QVERIFY(writeSourceFile(path, bytes, false).isEmpty());
        Engine engine;
        const auto opened = engine.openProject(ProjectSpec::folder(directory.path()),
                                               {IndexStartup::Manual, LanguageStartup::Manual});
        QVERIFY(opened);
        auto *project = opened.value();
        QSignalSpy replies(project, &Project::textMatchesReady);
        TextQuery query;
        query.files.scope = SearchScope::CurrentFile;
        query.files.currentFile = path;
        query.comments = CommentSearch::Include;
        query.strings = true;
        query.pageSize = 1;
        query.text = "alpha";
        const auto alpha = project->searchText(query);
        query.text = "beta";
        const auto beta = project->searchText(query);
        QTRY_COMPARE(replies.size(), 2);
        SearchPage alphaPage, betaPage;
        for (const auto &reply : replies) {
            if (qvariant_cast<RequestId>(reply[0]) == alpha)
                alphaPage = qvariant_cast<SearchPage>(reply[1]);
            if (qvariant_cast<RequestId>(reply[0]) == beta)
                betaPage = qvariant_cast<SearchPage>(reply[1]);
        }
        QVERIFY(alphaPage.error.isEmpty());
        QVERIFY(betaPage.error.isEmpty());
        QCOMPARE(alphaPage.hits[0].location.start, 0);
        QCOMPARE(betaPage.hits[0].location.start, 12);
        QVERIFY(alphaPage.next);
        QVERIFY(betaPage.next);
        const auto alphaNext = project->continueSearch(alphaPage.next);
        const auto betaNext = project->continueSearch(betaPage.next);
        QTRY_COMPARE(replies.size(), 4);
        bool alphaFound = false, betaFound = false;
        for (const auto &reply : replies) {
            const auto id = qvariant_cast<RequestId>(reply[0]);
            const auto page = qvariant_cast<SearchPage>(reply[1]);
            if (id == alphaNext) {
                QCOMPARE(page.hits[0].location.start, 6);
                alphaFound = true;
            }
            if (id == betaNext) {
                QCOMPARE(page.hits[0].location.start, 17);
                betaFound = true;
            }
        }
        QVERIFY(alphaFound && betaFound);
    }

    void cursorCannotBeUsedByAnotherProject() {
        QTemporaryDir directory;
        Engine engine;
        auto *first = engine.createProject();
        auto *second = engine.createProject();
        const auto path = directory.filePath("unsaved.txt");
        const auto document = engine.createDocument(path, "target target\n");
        QVERIFY(document);
        TextQuery query;
        query.text = "target";
        query.files.scope = SearchScope::CurrentFile;
        query.files.currentFile = path;
        query.pageSize = 1;
        QSignalSpy firstReplies(first, &Project::textMatchesReady);
        QSignalSpy secondReplies(second, &Project::textMatchesReady);
        first->searchText(query);
        QTRY_COMPARE(firstReplies.size(), 1);
        const auto page = qvariant_cast<SearchPage>(firstReplies[0][1]);
        QVERIFY(page.next);
        second->continueSearch(page.next);
        QTRY_COMPARE(secondReplies.size(), 1);
        QVERIFY(!qvariant_cast<SearchPage>(secondReplies[0][1]).error.isEmpty());
        first->continueSearch(page.next);
        QTRY_COMPARE(firstReplies.size(), 2);
        QCOMPARE(qvariant_cast<SearchPage>(firstReplies[1][1]).hits[0].location.start, 7);
    }

    void closedProjectSuppressesPendingPagesAndPreservesDocuments() {
        QTemporaryDir directory;
        Engine engine;
        const auto path = directory.filePath("source.txt");
        const auto document = engine.createDocument(path, "target target\n");
        QVERIFY(document);
        auto *project = engine.createProject();
        TextQuery query;
        query.text = "target";
        query.files.scope = SearchScope::CurrentFile;
        query.files.currentFile = path;
        QSignalSpy replies(project, &Project::textMatchesReady);
        project->searchText(query);
        project->close();
        QSignalSpy analysis(&engine, &Engine::analysisReady);
        engine.analyze(document.value()->snapshot());
        QTRY_COMPARE(analysis.size(), 1);
        QCOMPARE(replies.size(), 0);
        QCOMPARE(document.value()->contents(), QByteArray("target target\n"));
    }

    void failuresAreDistinctFromSuccessfulEmptyPages() {
        QTemporaryDir directory;
        const auto path = directory.filePath("source.txt");
        QVERIFY(writeSourceFile(path, "target\n", false).isEmpty());
        Engine engine;
        auto *project = engine.createProject();
        QSignalSpy replies(project, &Project::textMatchesReady);
        TextQuery query;
        query.files.scope = SearchScope::CurrentFile;
        query.files.currentFile = path;
        query.text = "[";
        query.method = TextMatch::RegularExpression;
        const auto bad = project->searchText(query);
        QTRY_COMPARE(replies.size(), 1);
        QCOMPARE(qvariant_cast<RequestId>(replies[0][0]), bad);
        QVERIFY(!qvariant_cast<SearchPage>(replies[0][1]).error.isEmpty());
        query.text = "absent";
        query.method = TextMatch::Literal;
        const auto zero = project->searchText(query);
        QTRY_COMPARE(replies.size(), 2);
        QCOMPARE(qvariant_cast<RequestId>(replies[1][0]), zero);
        const auto page = qvariant_cast<SearchPage>(replies[1][1]);
        QVERIFY(page.error.isEmpty());
        QVERIFY(page.hits.isEmpty());
        QVERIFY(!page.cancelled);
        QVERIFY(!page.next);
    }
};

QTEST_GUILESS_MAIN(SearchTests)
#include "SearchTests.moc"

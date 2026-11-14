#include <codecairn/FileIO.h>
#include <codecairn/Engine.h>
#include "CodeEditor.h"
#include <codecairn/Indexing.h>
#include <codecairn/ReferenceSearch.h>
#include "SourceDocument.h"
#include "SymbolBrowser.h"
#include "TestStorage.h"
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <memory>

class RefactoringBenchmarks : public QObject {
    Q_OBJECT
    TestStorage storage_;
private slots:
    void initTestCase();
    void textOccurrencesWithLiveOverlays();
    void warmWorkspaceQuery();
    void symbolBrowserRendering();
    void editorByteRead();
    void cachedDocumentSnapshot();

private:
    QTemporaryDir directory_;
    QStringList paths_;
    QVector<codecairn::SymbolOverlay> overlays_;
    codecairn::SymbolPage page_;
    std::shared_ptr<std::atomic_bool> cancel_ = std::make_shared<std::atomic_bool>(false);
};

void RefactoringBenchmarks::initTestCase() {
    QVERIFY(directory_.isValid());
    for (int i = 0; i < 128; ++i) {
        const auto path = directory_.filePath(QString("source_%1.py").arg(i));
        QByteArray source = "def target():\n    return target()\n";
        for (int j = 0; j < 7; ++j)
            source += "def function_" + QByteArray::number(i) + '_' + QByteArray::number(j) +
                      "(): pass\n";
        QVERIFY(writeSourceFile(path, source, false).isEmpty());
        paths_.append(path);
        overlays_.append({path, source, analyzeSource(source, path)});
    }
    const auto result =
        buildProjectIndex(directory_.path(), codecairn::IndexUpdate::DiscoverChanges, {}, cancel_);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    codecairn::SymbolCatalogQuery query;
    query.limit = 2000;
    query.includeCategorySymbols = true;
    page_ = queryProjectSymbols(directory_.path(), query);
    QVERIFY2(page_.error.isEmpty(), qPrintable(page_.error));
    QCOMPARE(page_.total, 1024);
}

void RefactoringBenchmarks::textOccurrencesWithLiveOverlays() {
    ReferenceResults results;
    QBENCHMARK {
        results = findTextOccurrences(paths_, "target", overlays_, false, false, cancel_);
    }
    QCOMPARE(results.hits.size(), 256);
    QVERIFY(results.warnings.isEmpty());
}

void RefactoringBenchmarks::warmWorkspaceQuery() {
    codecairn::SymbolCatalogQuery query;
    query.text = "function_127";
    codecairn::SymbolPage page;
    QBENCHMARK {
        page = queryProjectSymbols(directory_.path(), query);
    }
    QVERIFY(page.error.isEmpty());
    QCOMPARE(page.total, 7);
}

void RefactoringBenchmarks::symbolBrowserRendering() {
    QTreeWidget symbols;
    QTreeWidget categories;
    symbols.setColumnCount(2);
    categories.setColumnCount(2);
    QBENCHMARK {
        SymbolBrowser::display(&symbols, &categories, page_, directory_.path(), {});
    }
    QCOMPARE(symbols.topLevelItemCount(), 1024);
    QCOMPARE(categories.topLevelItem(0)->childCount(), 1024);
}

void RefactoringBenchmarks::editorByteRead() {
    codecairn::Engine engine;
    const auto source = engine.createDocument("large.txt", QByteArray(1024 * 1024, 'x'), false);
    QVERIFY(source);
    SourceDocument document(source.value(), &engine, 10);
    std::unique_ptr<CodeEditor> editor(document.editor);
    QByteArray bytes;
    QBENCHMARK {
        bytes = editor->bytes();
    }
    QCOMPARE(bytes.size(), 1024 * 1024);
}

void RefactoringBenchmarks::cachedDocumentSnapshot() {
    codecairn::Engine engine;
    const auto source = engine.createDocument("large.txt", QByteArray(1024 * 1024, 'x'), false);
    QVERIFY(source);
    SourceDocument document(source.value(), &engine, 10);
    std::unique_ptr<CodeEditor> editor(document.editor);
    document.snapshotBytes();
    QByteArray bytes;
    QBENCHMARK {
        bytes = document.snapshotBytes();
    }
    QCOMPARE(bytes.size(), 1024 * 1024);
}

QTEST_MAIN(RefactoringBenchmarks)
#include "RefactoringBenchmarks.moc"

#include "FileIO.h"
#include "TestStorage.h"
#include "WorkspaceIndex.h"

#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#ifdef Q_OS_UNIX
#include <sys/resource.h>
#endif

class IndexingBenchmarks : public QObject {
    Q_OBJECT
    TestStorage storage_;
private slots:
    void largeWorkspace_data() {
        QTest::addColumn<int>("fileCount");
        QTest::newRow("1k") << 1000;
        QTest::newRow("25k") << 25000;
        QTest::newRow("100k") << 100000;
    }

    void largeWorkspace() {
        QFETCH(int, fileCount);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        for (int i = 0; i < fileCount; ++i) {
            QFile file(directory.filePath(QString("source_%1.c").arg(i)));
            QVERIFY(file.open(QIODevice::WriteOnly));
            const auto source = "int symbol_" + QByteArray::number(i) + "(void) { return 1; }\n";
            QCOMPARE(file.write(source), source.size());
        }
        WorkspaceIndex index;
        QElapsedTimer elapsed;
        qint64 catalogMs = -1;
        qint64 symbolsMs = -1;
        connect(&index, &WorkspaceIndex::catalogReady, this, [&](const QStringList &paths) {
            catalogMs = elapsed.elapsed();
            QCOMPARE(paths.size(), fileCount);
            QVERIFY(index.busy());
        });
        connect(&index, &WorkspaceIndex::batchReady, this, [&] {
            if (symbolsMs >= 0)
                return;
            const auto page = queryWorkspaceSymbols(directory.path(), {});
            QVERIFY2(page.error.isEmpty(), qPrintable(page.error));
            if (page.total > 0) {
                symbolsMs = elapsed.elapsed();
                QVERIFY(index.busy());
            }
        });
        QSignalSpy failures(&index, &WorkspaceIndex::failed);
        elapsed.start();
        index.open(directory.path());
        QTRY_VERIFY_WITH_TIMEOUT(!index.busy(), 300000);
        const auto coldMs = elapsed.elapsed();
        QVERIFY2(failures.isEmpty(),
                 failures.isEmpty() ? "" : qPrintable(failures.first().first().toString()));
        QCOMPARE(index.files().size(), fileCount);
        const auto page = queryWorkspaceSymbols(directory.path(), {});
        QVERIFY2(page.error.isEmpty(), qPrintable(page.error));
        QCOMPARE(page.total, fileCount);
        QVERIFY(catalogMs >= 0);
        QVERIFY(symbolsMs >= 0);

        const auto cancel = std::make_shared<std::atomic_bool>(false);
        elapsed.restart();
        const auto warm = buildWorkspaceIndex(directory.path(), false, true, {}, cancel);
        const auto warmMs = elapsed.elapsed();
        QVERIFY2(warm.error.isEmpty(), qPrintable(warm.error));
        QCOMPARE(warm.reused, fileCount);
        QCOMPARE(warm.parsed, 0);
        qInfo().nospace() << "files=" << fileCount << " catalog_ms=" << catalogMs
                          << " first_symbols_ms=" << symbolsMs << " cold_ms=" << coldMs
                          << " warm_ms=" << warmMs;
#ifdef Q_OS_UNIX
        struct rusage usage{};
        QVERIFY(getrusage(RUSAGE_SELF, &usage) == 0);
#ifdef Q_OS_MACOS
        const auto peakBytes = usage.ru_maxrss;
#else
        const auto peakBytes = usage.ru_maxrss * 1024;
#endif
        qInfo() << "process_peak_rss_bytes=" << peakBytes
                << "(includes fixture generation and earlier cases; not a leak measurement)";
#endif
    }
};

QTEST_MAIN(IndexingBenchmarks)
#include "IndexingBenchmarks.moc"

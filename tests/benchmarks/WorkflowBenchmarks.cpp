#include "FileIO.h"
#include "NavigationService.h"
#include "ReferenceSession.h"
#include "TestStorage.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QtConcurrentRun>
#include <algorithm>

class WorkflowBenchmarks : public QObject {
    Q_OBJECT
    TestStorage storage_;
    QTemporaryDir directory_;
    ReferenceSearchRequest request_;
    QVector<qint64> legacy_;
    QVector<qint64> shared_;

    void report(const char *name) {
        QVERIFY(!legacy_.isEmpty());
        QCOMPARE(legacy_.size(), shared_.size());
        std::sort(legacy_.begin(), legacy_.end());
        std::sort(shared_.begin(), shared_.end());
        qInfo().nospace() << name << " samples=" << legacy_.size()
                          << " legacy_median_us=" << legacy_[legacy_.size() / 2] / 1000.0
                          << " shared_median_us=" << shared_[shared_.size() / 2] / 1000.0
                          << " legacy_p95_us=" << legacy_[legacy_.size() * 95 / 100] / 1000.0
                          << " shared_p95_us=" << shared_[shared_.size() * 95 / 100] / 1000.0;
        legacy_.clear();
        shared_.clear();
    }

private slots:
    void initTestCase() {
        QVERIFY(directory_.isValid());
        request_.workspaceRoot = directory_.path();
        request_.query = "target";
        request_.options.method = SearchMethod::Literal;
        request_.options.smartMatching = false;
        request_.limit = 10000;
        for (int i = 0; i < 128; ++i) {
            const auto path = directory_.filePath(QString("source_%1.py").arg(i));
            const QByteArray source = "def target():\n    return target()\n";
            QVERIFY(writeSourceFile(path, source, false).isEmpty());
            request_.paths.append(path);
            request_.overlays.append(
                {path, source, analyzeSource(source, path), sourceContentHash(source)});
        }
    }

    void referenceDispatch() {
        WorkspaceServices services;
        ReferenceSession session(&services);
        const auto run = [&](bool shared) {
            ReferenceResults result;
            QEventLoop loop;
            QTimer timeout;
            timeout.setSingleShot(true);
            connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
            timeout.start(30000);
            QElapsedTimer elapsed;
            QFutureWatcher<ReferenceResults> watcher;
            const auto connection = connect(&session, &ReferenceSession::pageReady, &loop,
                                            [&](quint64, const ReferenceResults &page, bool) {
                                                result = page;
                                                loop.quit();
                                            });
            connect(&watcher, &QFutureWatcher<ReferenceResults>::finished, &loop, [&] {
                result = watcher.result();
                loop.quit();
            });
            elapsed.start();
            if (shared)
                session.start(request_, 1);
            else
                watcher.setFuture(QtConcurrent::run(searchReferences, request_,
                                                    std::make_shared<std::atomic_bool>(false)));
            loop.exec();
            const auto duration = elapsed.nsecsElapsed();
            disconnect(connection);
            QVERIFY(timeout.isActive());
            QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
            QCOMPARE(result.hits.size(), 256);
            QVERIFY(!result.cancelled);
            (shared ? shared_ : legacy_).append(duration);
        };
        for (int i = 0; i < 40; ++i) {
            run(i % 2 == 0);
            run(i % 2 != 0);
        }
        report("reference_dispatch");
    }

    void navigationRead() {
        WorkspaceServices services;
        NavigationService navigation(&services);
        const auto path = request_.paths.first();
        const auto bytes = request_.overlays.first().source;
        const NavigationLocation location{path, 4, 10, -1, sourceContentHash(bytes)};
        const auto run = [&](bool shared) {
            QEventLoop loop;
            QTimer timeout;
            timeout.setSingleShot(true);
            connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
            timeout.start(30000);
            NavigationRead result;
            QElapsedTimer elapsed;
            QFutureWatcher<FileContents> watcher;
            const auto connection = connect(&navigation, &NavigationService::readReady, &loop,
                                            [&](quint64, const NavigationRead &read) {
                                                result = read;
                                                loop.quit();
                                            });
            connect(&watcher, &QFutureWatcher<FileContents>::finished, &loop, [&] {
                result.file = watcher.result();
                result.range = resolveNavigationTarget(location, result.file.bytes);
                loop.quit();
            });
            elapsed.start();
            if (shared)
                navigation.read(location, 1);
            else
                watcher.setFuture(QtConcurrent::run([path] { return readSourceFile(path); }));
            loop.exec();
            const auto duration = elapsed.nsecsElapsed();
            disconnect(connection);
            QVERIFY(timeout.isActive());
            QVERIFY2(result.file.error.isEmpty(), qPrintable(result.file.error));
            QVERIFY2(result.range.error.isEmpty(), qPrintable(result.range.error));
            QCOMPARE(result.file.bytes, bytes);
            (shared ? shared_ : legacy_).append(duration);
        };
        for (int i = 0; i < 100; ++i) {
            run(i % 2 == 0);
            run(i % 2 != 0);
        }
        report("navigation_read");
    }
};

QTEST_GUILESS_MAIN(WorkflowBenchmarks)
#include "WorkflowBenchmarks.moc"

#include <codecairn/FileIO.h>
#include <codecairn/Engine.h>
#include <codecairn/Project.h>
#include "TestStorage.h"

#include <QElapsedTimer>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QSignalSpy>
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
        codecairn::Engine engine;
        auto *project = engine.createProject();
        QVERIFY(
            project->open(prepareProject(directory_.path()),
                          {codecairn::IndexStartup::Manual, codecairn::LanguageStartup::Manual}));
        QSignalSpy analyzed(project, &codecairn::Project::analysisReady);
        for (const auto &overlay : request_.overlays) {
            const auto document = engine.createDocument(overlay.path, overlay.source);
            QVERIFY(document);
            project->analyze(document.value()->snapshot());
        }
        QTRY_COMPARE(analyzed.size(), request_.overlays.size());
        codecairn::TextQuery query;
        query.text = request_.query;
        query.files.scope = codecairn::SearchScope::SelectedFiles;
        query.files.selectedFiles = request_.paths;
        query.pageSize = request_.limit;
        query.wholeWords = request_.options.wholeWords;
        query.comments = request_.options.comments;
        query.strings = request_.options.strings;
        const auto run = [&](bool shared) {
            ReferenceResults result;
            QEventLoop loop;
            QTimer timeout;
            timeout.setSingleShot(true);
            connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
            timeout.start(30000);
            QElapsedTimer elapsed;
            QFutureWatcher<ReferenceResults> watcher;
            const auto connection =
                connect(project, &codecairn::Project::textMatchesReady, &loop,
                        [&](codecairn::RequestId, const codecairn::SearchPage &page) {
                            result.hits = page.hits;
                            result.error = page.error;
                            result.cancelled = page.cancelled;
                            loop.quit();
                        });
            connect(&watcher, &QFutureWatcher<ReferenceResults>::finished, &loop, [&] {
                result = watcher.result();
                loop.quit();
            });
            elapsed.start();
            if (shared)
                project->searchText(query);
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
        codecairn::Engine engine;
        const auto path = request_.paths.first();
        const auto bytes = request_.overlays.first().source;
        const NavigationLocation location{path, 4, 10, -1, sourceContentHash(bytes)};
        const auto run = [&](bool shared) {
            QEventLoop loop;
            QTimer timeout;
            timeout.setSingleShot(true);
            connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
            timeout.start(30000);
            codecairn::SourceRead result;
            QElapsedTimer elapsed;
            QFutureWatcher<FileContents> watcher;
            const auto connection =
                connect(&engine, &codecairn::Engine::sourceReady, &loop,
                        [&](codecairn::RequestId, const codecairn::SourceRead &read) {
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
                engine.readSource(location);
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

#include <codecairn/Engine.h>
#include <codecairn/Project.h>
#include <codecairn/FileIO.h>
#include <codecairn/Paths.h>
#include <codecairn/Diagnostics.h>
#include <codecairn/RuntimeLimits.h>

#include <QCoreApplication>
#include <QDir>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QScopeGuard>
#include <QSemaphore>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <atomic>

using namespace codecairn;

namespace {
class AnalysisRuns {
public:
    explicit AnalysisRuns(bool holdWorkers = false) : holdWorkers_(holdWorkers) {
        active_ = this;
        previous_ = qInstallMessageHandler(observe);
    }
    ~AnalysisRuns() {
        qInstallMessageHandler(previous_);
        active_ = nullptr;
    }
    int started() const { return started_.load(); }
    bool waitForWorkers(int count = RuntimeLimits::WorkerThreads) {
        return entered_.tryAcquire(count, 5000);
    }
    void releaseWorker(int index) { release_[index].release(); }
    void releaseWorkers() {
        for (auto &worker : release_)
            worker.release();
    }

private:
    static void observe(QtMsgType type, const QMessageLogContext &context, const QString &message) {
        auto *runs = active_;
        if (qstrcmp(context.category, "codecairn.analysis") == 0 &&
            message.startsWith("analysis.started")) {
            const auto started = ++runs->started_;
            if (runs->holdWorkers_ && started <= RuntimeLimits::WorkerThreads) {
                runs->entered_.release();
                runs->release_[started - 1].acquire();
            }
        }
        if (runs->previous_)
            runs->previous_(type, context, message);
    }
    inline static AnalysisRuns *active_ = nullptr;
    QtMessageHandler previous_ = nullptr;
    std::atomic<int> started_{0};
    bool holdWorkers_ = false;
    QSemaphore entered_;
    QSemaphore release_[RuntimeLimits::WorkerThreads];
};

struct ExpectedAnalysis {
    SourceSnapshot source;
    AnalysisDetail detail;
};

bool hasSymbol(const AnalysisResult &result, const QString &name) {
    return std::any_of(result.analysis.symbols.cbegin(), result.analysis.symbols.cend(),
                       [&](const auto &symbol) { return symbol.name == name; });
}

void compareSource(const SourceSnapshot &actual, const SourceSnapshot &expected) {
    QCOMPARE(actual.path, expected.path);
    QCOMPARE(actual.bytes, expected.bytes);
    QCOMPARE(actual.hash, expected.hash);
    QCOMPARE(actual.revision, expected.revision);
    QVERIFY(actual.bytes.constData() == expected.bytes.constData());
}
}

class AnalysisTests : public QObject {
    Q_OBJECT
    QTemporaryDir storage_{QDir::current().filePath(".analysis-test-state-XXXXXX")};
    const ProjectOptions manual_{IndexStartup::Manual, LanguageStartup::Manual};

private slots:
    void initTestCase() {
        QVERIFY(storage_.isValid());
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setOrganizationName({});
        QCoreApplication::setApplicationName("CodeCairnAnalysisTests-" +
                                             QDir(storage_.path()).dirName());
        QVERIFY(configureStateDirectory(storage_.path()).isEmpty());
    }

    void repeatedRequestsKeepEverySourceIdentityAndDetail_data() {
        QTest::addColumn<bool>("projectContext");
        QTest::newRow("standalone") << false;
        QTest::newRow("project") << true;
    }

    void repeatedRequestsKeepEverySourceIdentityAndDetail() {
        QFETCH(bool, projectContext);
        QTemporaryDir directory(QDir::current().filePath(".analysis-test-source-XXXXXX"));
        QVERIFY(directory.isValid());
        Engine engine;
        auto *project = projectContext ? engine.createProject() : nullptr;
        if (project)
            QVERIFY(project->open(ProjectSpec::folder(directory.path()), manual_));
        auto first = engine.createDocument(directory.filePath("first.py"), "def before(): pass\n");
        auto second =
            engine.createDocument(directory.filePath("second.py"), "def before(): pass\n");
        QVERIFY(first);
        QVERIFY(second);
        const auto original = first.value()->snapshot();
        first.value()->updateContents("def after(): pass\n");
        const auto changed = first.value()->snapshot();
        first.value()->updateContents(original.bytes);
        const auto repeatedBytes = first.value()->snapshot();
        const QVector<SourceSnapshot> sources{original, changed, repeatedBytes,
                                              second.value()->snapshot()};
        QSignalSpy results(project ? static_cast<QObject *>(project) : &engine,
                           project ? QMetaMethod::fromSignal(&Project::analysisReady)
                                   : QMetaMethod::fromSignal(&Engine::analysisReady));
        QHash<quint64, ExpectedAnalysis> expected;
        for (int i = 0; i < 24; ++i) {
            const auto source = sources[i % sources.size()];
            const auto detail = (i / sources.size()) % 2 ? AnalysisDetail::Symbols
                                                         : AnalysisDetail::StylesAndSymbols;
            const auto id =
                project ? project->analyze(source, detail) : engine.analyze(source, detail);
            QVERIFY(id.value != 0);
            QVERIFY(!expected.contains(id.value));
            expected.insert(id.value, {source, detail});
        }
        QVERIFY(results.isEmpty());
        QTRY_COMPARE(results.size(), expected.size());
        for (const auto &arguments : results) {
            const auto id = qvariant_cast<RequestId>(arguments[0]);
            QVERIFY(expected.contains(id.value));
            const auto request = expected.take(id.value);
            const auto result = qvariant_cast<AnalysisResult>(arguments[1]);
            compareSource(result.source, request.source);
            QVERIFY2(result.analysis.error.isEmpty(), qPrintable(result.analysis.error));
            QVERIFY(result.analysis.supported);
            QVERIFY(hasSymbol(result, request.source.bytes == changed.bytes ? "after" : "before"));
            if (request.detail == AnalysisDetail::Symbols)
                QVERIFY(result.analysis.styles.isEmpty());
            else
                QCOMPARE(result.analysis.styles.size(), request.source.bytes.size());
        }
        QVERIFY(expected.isEmpty());
    }

    void sharedSourceKeepsEachProjectsCompilationContext() {
        QTemporaryDir directory(QDir::current().filePath(".analysis-test-projects-XXXXXX"));
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("macro.c");
        const QByteArray source =
            "#ifdef PROJECT_FLAG\n#define TEST(name) int configured_##name(void)\n"
            "#else\n#define TEST(name) int other_##name(void)\n#endif\n"
            "TEST(function) { return 1; }\n";
        QVERIFY(writeSourceFile(path, source, false).isEmpty());
        QVector<QString> projectFiles;
        for (int i = 0; i < 2; ++i) {
            const auto compilation = directory.filePath(QString("commands%1").arg(i));
            QVERIFY(QDir().mkpath(compilation));
            QJsonArray arguments{"clang", "-c", path};
            if (i == 0)
                arguments.append("-DPROJECT_FLAG");
            QVERIFY(writeSourceFile(
                        QDir(compilation).filePath("compile_commands.json"),
                        QJsonDocument(QJsonArray{QJsonObject{{"directory", directory.path()},
                                                             {"file", path},
                                                             {"arguments", arguments}}})
                            .toJson(),
                        false)
                        .isEmpty());
            codecairn::ProjectConfiguration options;
            options.managed = true;
            options.includedPaths = {"macro.c"};
            options.name = QString("Project%1").arg(i);
            options.compilationDirectory = compilation;
            const auto file = directory.filePath(QString("project%1.cairn").arg(i));
            QVERIFY(
                codecairn::ProjectConfigurationStore::saveProject(file, directory.path(), options)
                    .isEmpty());
            projectFiles.append(file);
        }
        Engine engine;
        auto document = engine.openDocument(path);
        auto first = engine.openProject(ProjectSpec::managed(projectFiles[0]), manual_);
        auto second = engine.openProject(ProjectSpec::managed(projectFiles[1]), manual_);
        QVERIFY(document);
        QVERIFY(first);
        QVERIFY(second);
        QSignalSpy firstResults(first.value(), &Project::analysisReady);
        QSignalSpy secondResults(second.value(), &Project::analysisReady);
        QSignalSpy standalone(&engine, &Engine::analysisReady);
        const auto captured = document.value()->snapshot();
        for (int round = 0; round < 2; ++round) {
            const auto firstId = first.value()->analyze(captured);
            const auto secondId = second.value()->analyze(captured);
            QVERIFY(firstId != secondId);
            QTRY_COMPARE(firstResults.size(), round + 1);
            QTRY_COMPARE(secondResults.size(), round + 1);
            QCOMPARE(qvariant_cast<RequestId>(firstResults[round][0]), firstId);
            QCOMPARE(qvariant_cast<RequestId>(secondResults[round][0]), secondId);
            const auto a = qvariant_cast<AnalysisResult>(firstResults[round][1]);
            const auto b = qvariant_cast<AnalysisResult>(secondResults[round][1]);
            compareSource(a.source, captured);
            compareSource(b.source, captured);
            QVERIFY2(a.analysis.error.isEmpty(), qPrintable(a.analysis.error));
            QVERIFY2(b.analysis.error.isEmpty(), qPrintable(b.analysis.error));
            QVERIFY(hasSymbol(a, "configured_function"));
            QVERIFY(!hasSymbol(a, "other_function"));
            QVERIFY(hasSymbol(b, "other_function"));
            QVERIFY(!hasSymbol(b, "configured_function"));
        }
        QVERIFY(standalone.isEmpty());
    }

    void cancellingOneEquivalentRequestKeepsOtherSubscribers() {
        Engine engine;
        auto document = engine.createDocument(":/analysis-cancel.py", "def target(): pass\n");
        QVERIFY(document);
        const auto captured = document.value()->snapshot();
        QSignalSpy results(&engine, &Engine::analysisReady);
        QSet<quint64> accepted;
        const auto cancelled = engine.analyze(captured);
        for (int i = 0; i < 8; ++i)
            accepted.insert(engine.analyze(captured).value);
        engine.cancel(cancelled);
        engine.cancel(cancelled);
        QTRY_COMPARE(results.size(), accepted.size());
        for (const auto &arguments : results) {
            const auto id = qvariant_cast<RequestId>(arguments[0]);
            QVERIFY(id != cancelled);
            QVERIFY(accepted.remove(id.value));
            const auto result = qvariant_cast<AnalysisResult>(arguments[1]);
            compareSource(result.source, captured);
            QVERIFY(result.analysis.error.isEmpty());
            QVERIFY(hasSymbol(result, "target"));
        }
        QVERIFY(accepted.isEmpty());
    }

    void cancellationDuringDeliverySuppressesRemainingEquivalentSubscribers() {
        Engine engine;
        auto document = engine.createDocument(":/analysis-delivery.py", "def target(): pass\n");
        QVERIFY(document);
        QSignalSpy results(&engine, &Engine::analysisReady);
        QVector<RequestId> requests;
        RequestId firstDelivered;
        connect(&engine, &Engine::analysisReady, this, [&](RequestId id, const AnalysisResult &) {
            if (firstDelivered.value != 0)
                return;
            firstDelivered = id;
            for (const auto request : requests)
                if (request != id)
                    engine.cancel(request);
        });
        for (int i = 0; i < 8; ++i)
            requests.append(engine.analyze(document.value()->snapshot()));
        QTRY_COMPARE(results.size(), 1);
        QVERIFY(firstDelivered.value != 0);
        document.value()->updateContents("def sentinel(): pass\n");
        const auto sentinel = engine.analyze(document.value()->snapshot());
        QTRY_COMPARE(results.size(), 2);
        QCOMPARE(qvariant_cast<RequestId>(results[0][0]), firstDelivered);
        QCOMPARE(qvariant_cast<RequestId>(results[1][0]), sentinel);
        QVERIFY(hasSymbol(qvariant_cast<AnalysisResult>(results[1][1]), "sentinel"));
    }

    void closingAndReopeningProjectCancelsOnlyItsObsoleteAnalyses() {
        QTemporaryDir directory(QDir::current().filePath(".analysis-test-reopen-XXXXXX"));
        QVERIFY(directory.isValid());
        Engine engine;
        auto document =
            engine.createDocument(directory.filePath("source.py"), "def before(): pass\n");
        QVERIFY(document);
        auto first = engine.openProject(ProjectSpec::folder(directory.path()), manual_);
        auto second = engine.openProject(ProjectSpec::folder(directory.path()), manual_);
        QVERIFY(first);
        QVERIFY(second);
        QSignalSpy firstResults(first.value(), &Project::analysisReady);
        QSignalSpy secondResults(second.value(), &Project::analysisReady);
        QSignalSpy standalone(&engine, &Engine::analysisReady);
        const auto captured = document.value()->snapshot();
        const auto obsolete = first.value()->analyze(captured);
        const auto independent = second.value()->analyze(captured);
        first.value()->close();
        QVERIFY(first.value()->open(ProjectSpec::folder(directory.path()), manual_));
        document.value()->updateContents("def after(): pass\n");
        const auto current = first.value()->analyze(document.value()->snapshot());
        QTRY_COMPARE(firstResults.size(), 1);
        QTRY_COMPARE(secondResults.size(), 1);
        QCOMPARE(qvariant_cast<RequestId>(firstResults[0][0]), current);
        QVERIFY(qvariant_cast<RequestId>(firstResults[0][0]) != obsolete);
        QCOMPARE(qvariant_cast<RequestId>(secondResults[0][0]), independent);
        QVERIFY(hasSymbol(qvariant_cast<AnalysisResult>(firstResults[0][1]), "after"));
        compareSource(qvariant_cast<AnalysisResult>(secondResults[0][1]).source, captured);
        QVERIFY(standalone.isEmpty());
    }

    void reloadingChangedConfigurationCancelsObsoleteAnalyses() {
        QTemporaryDir directory(QDir::current().filePath(".analysis-test-config-XXXXXX"));
        QVERIFY(directory.isValid());
        const auto projectFile = directory.filePath("configured.cairn");
        codecairn::ProjectConfiguration options;
        options.managed = true;
        options.name = "Configured";
        options.includedPaths = {"source.py"};
        const auto initialSave = codecairn::ProjectConfigurationStore::saveProject(
            projectFile, directory.path(), options);
        QVERIFY2(initialSave.isEmpty(), qPrintable(initialSave));
        Engine engine;
        auto document =
            engine.createDocument(directory.filePath("source.py"), "def target(): pass\n");
        QVERIFY(document);
        auto opened = engine.openProject(ProjectSpec::managed(projectFile), manual_);
        QVERIFY(opened);
        auto *project = opened.value();
        QSignalSpy results(project, &Project::analysisReady);
        QSignalSpy changed(project, &Project::contextChanged);
        const auto obsolete = project->analyze(document.value()->snapshot());
        options = project->info().configuration;
        options.compilationDirectory = directory.filePath("changed-build");
        const auto updatedSave = codecairn::ProjectConfigurationStore::saveProject(
            projectFile, directory.path(), options);
        QVERIFY2(updatedSave.isEmpty(), qPrintable(updatedSave));
        QVERIFY(project->reloadOptions());
        QCOMPARE(changed.size(), 1);
        const auto current = project->analyze(document.value()->snapshot());
        QTRY_COMPARE(results.size(), 1);
        QCOMPARE(qvariant_cast<RequestId>(results[0][0]), current);
        QVERIFY(qvariant_cast<RequestId>(results[0][0]) != obsolete);
        QVERIFY(hasSymbol(qvariant_cast<AnalysisResult>(results[0][1]), "target"));
    }

    void manuallyReopeningSameProjectWithChangedConfigurationCancelsObsoleteAnalysis() {
        QVERIFY(analysisLog().isInfoEnabled());
        QTemporaryDir directory(QDir::current().filePath(".analysis-test-manual-config-XXXXXX"));
        QVERIFY(directory.isValid());
        const auto path = directory.filePath("macro.c");
        const QByteArray source =
            "#ifdef PROJECT_FLAG\n#define TEST(name) int configured_##name(void)\n"
            "#else\n#define TEST(name) int other_##name(void)\n#endif\n"
            "TEST(function) { return 1; }\n";
        QVERIFY(writeSourceFile(path, source, false).isEmpty());
        QStringList compilations;
        for (int i = 0; i < 2; ++i) {
            const auto compilation = directory.filePath(QString("commands%1").arg(i));
            QVERIFY(QDir().mkpath(compilation));
            QJsonArray arguments{"clang", "-c", path};
            if (i == 0)
                arguments.append("-DPROJECT_FLAG");
            const auto saved = writeSourceFile(
                QDir(compilation).filePath("compile_commands.json"),
                QJsonDocument(QJsonArray{QJsonObject{{"directory", directory.path()},
                                                     {"file", path},
                                                     {"arguments", arguments}}})
                    .toJson(),
                false);
            QVERIFY2(saved.isEmpty(), qPrintable(saved));
            compilations.append(compilation);
        }
        const auto projectFile = directory.filePath("configured.cairn");
        ProjectConfiguration configuration;
        configuration.managed = true;
        configuration.name = "Configured";
        configuration.includedPaths = {"macro.c"};
        configuration.compilationDirectory = compilations[0];
        const auto initialSave =
            ProjectConfigurationStore::saveProject(projectFile, directory.path(), configuration);
        QVERIFY2(initialSave.isEmpty(), qPrintable(initialSave));
        AnalysisRuns runs(true);
        Engine engine;
        auto unblock = qScopeGuard([&] { runs.releaseWorkers(); });
        auto document = engine.openDocument(path);
        auto opened = engine.openProject(ProjectSpec::managed(projectFile), manual_);
        QVERIFY(document);
        QVERIFY(opened);
        auto *project = opened.value();
        const auto root = project->info().root;
        const auto captured = document.value()->snapshot();
        QSignalSpy results(project, &Project::analysisReady);
        QSignalSpy changed(project, &Project::contextChanged);
        QSignalSpy standalone(&engine, &Engine::analysisReady);
        const auto obsolete = project->analyze(captured);
        QVERIFY(runs.waitForWorkers(1));
        configuration = project->info().configuration;
        configuration.compilationDirectory = compilations[1];
        const auto updatedSave =
            ProjectConfigurationStore::saveProject(projectFile, directory.path(), configuration);
        QVERIFY2(updatedSave.isEmpty(), qPrintable(updatedSave));
        QVERIFY(project->open(ProjectSpec::managed(projectFile), manual_));
        QCOMPARE(project->info().root, root);
        QCOMPARE(project->info().configuration.compilationDirectory, compilations[1]);
        QCOMPARE(changed.size(), 1);
        const auto current = project->analyze(captured);
        QVERIFY(current != obsolete);
        QVERIFY(runs.waitForWorkers(1));
        auto sentinelDocument =
            engine.createDocument(":/analysis-config-sentinel.py", "def sentinel(): pass\n");
        QVERIFY(sentinelDocument);
        const auto sentinel = engine.analyze(sentinelDocument.value()->snapshot());
        // The fresh job stays blocked; only the obsolete job's worker can reach this sentinel.
        runs.releaseWorker(0);
        QTRY_COMPARE(standalone.size(), 1);
        QCOMPARE(qvariant_cast<RequestId>(standalone[0][0]), sentinel);
        QVERIFY(results.isEmpty());
        runs.releaseWorker(1);
        unblock.dismiss();
        QTRY_COMPARE(results.size(), 1);
        QCOMPARE(qvariant_cast<RequestId>(results[0][0]), current);
        const auto result = qvariant_cast<AnalysisResult>(results[0][1]);
        compareSource(result.source, captured);
        QVERIFY2(result.analysis.error.isEmpty(), qPrintable(result.analysis.error));
        QVERIFY(hasSymbol(result, "other_function"));
        QVERIFY(!hasSymbol(result, "configured_function"));
    }

    void cancellingAnotherProjectsRequestDoesNotCancelItsAnalysis() {
        QTemporaryDir directory(QDir::current().filePath(".analysis-test-cancel-owner-XXXXXX"));
        QVERIFY(directory.isValid());
        Engine engine;
        auto document =
            engine.createDocument(directory.filePath("source.py"), "def target(): pass\n");
        auto first = engine.openProject(ProjectSpec::folder(directory.path()), manual_);
        auto second = engine.openProject(ProjectSpec::folder(directory.path()), manual_);
        QVERIFY(document);
        QVERIFY(first);
        QVERIFY(second);
        QSignalSpy firstResults(first.value(), &Project::analysisReady);
        QSignalSpy secondResults(second.value(), &Project::analysisReady);
        const auto captured = document.value()->snapshot();
        const auto request = second.value()->analyze(captured);
        first.value()->cancel(request);
        QTRY_COMPARE(secondResults.size(), 1);
        QCOMPARE(qvariant_cast<RequestId>(secondResults[0][0]), request);
        const auto result = qvariant_cast<AnalysisResult>(secondResults[0][1]);
        compareSource(result.source, captured);
        QVERIFY(result.analysis.error.isEmpty());
        QVERIFY(hasSymbol(result, "target"));
        QVERIFY(firstResults.isEmpty());
    }

    void oversizedRequestsReturnDeferredExplicitErrors_data() {
        QTest::addColumn<bool>("projectContext");
        QTest::addColumn<AnalysisDetail>("detail");
        QTest::newRow("standalone-styles") << false << AnalysisDetail::StylesAndSymbols;
        QTest::newRow("standalone-symbols") << false << AnalysisDetail::Symbols;
        QTest::newRow("project-styles") << true << AnalysisDetail::StylesAndSymbols;
        QTest::newRow("project-symbols") << true << AnalysisDetail::Symbols;
    }

    void oversizedRequestsReturnDeferredExplicitErrors() {
        QFETCH(bool, projectContext);
        QFETCH(AnalysisDetail, detail);
        Engine engine;
        auto *project = projectContext ? engine.createProject() : nullptr;
        auto document =
            engine.createDocument(":/analysis-large.py", QByteArray(MaximumDocumentBytes + 1, 'x'));
        QVERIFY(document);
        QSignalSpy results(project ? static_cast<QObject *>(project) : &engine,
                           project ? QMetaMethod::fromSignal(&Project::analysisReady)
                                   : QMetaMethod::fromSignal(&Engine::analysisReady));
        const auto captured = document.value()->snapshot();
        const auto id =
            project ? project->analyze(captured, detail) : engine.analyze(captured, detail);
        QVERIFY(results.isEmpty());
        QTRY_COMPARE(results.size(), 1);
        QCOMPARE(qvariant_cast<RequestId>(results[0][0]), id);
        const auto result = qvariant_cast<AnalysisResult>(results[0][1]);
        compareSource(result.source, captured);
        QVERIFY(!result.analysis.error.isEmpty());
        QVERIFY(result.analysis.styles.isEmpty());
        QVERIFY(result.analysis.symbols.isEmpty());
    }

    void documentChangesAndClosurePreserveCapturedResults() {
        Engine engine;
        auto document = engine.createDocument(":/analysis-snapshot.py", "def before(): pass\n");
        QVERIFY(document);
        QSignalSpy results(&engine, &Engine::analysisReady);
        const auto before = document.value()->snapshot();
        const auto first = engine.analyze(before);
        document.value()->updateContents("def after(): pass\n");
        const auto after = document.value()->snapshot();
        const auto second = engine.analyze(after);
        QVERIFY(engine.closeDocument(document.value()));
        QTRY_COMPARE(results.size(), 2);
        QSet<quint64> seen;
        for (const auto &arguments : results) {
            const auto id = qvariant_cast<RequestId>(arguments[0]);
            QVERIFY(id == first || id == second);
            QVERIFY(!seen.contains(id.value));
            seen.insert(id.value);
            const auto result = qvariant_cast<AnalysisResult>(arguments[1]);
            compareSource(result.source, id == first ? before : after);
            QVERIFY(result.analysis.error.isEmpty());
            QVERIFY(hasSymbol(result, id == first ? "before" : "after"));
        }
        QCOMPARE(seen.size(), 2);
        auto retained = qvariant_cast<AnalysisResult>(results[0][1]);
        const auto original = retained.source.bytes;
        retained.source.bytes[0] = '#';
        QCOMPARE(qvariant_cast<AnalysisResult>(results[0][1]).source.bytes, original);
        QCOMPARE(before.bytes, QByteArray("def before(): pass\n"));
        QCOMPARE(after.bytes, QByteArray("def after(): pass\n"));
    }

    void equivalentRequestsRunOneAnalysis() {
        QVERIFY(analysisLog().isInfoEnabled());
        AnalysisRuns runs;
        Engine engine;
        auto document = engine.createDocument(":/analysis-coalesce.py", "def target(): pass\n");
        QVERIFY(document);
        const auto captured = document.value()->snapshot();
        QSignalSpy results(&engine, &Engine::analysisReady);
        QSet<quint64> requests;
        for (int i = 0; i < 24; ++i)
            requests.insert(engine.analyze(captured).value);
        QTRY_COMPARE(results.size(), 24);
        QCOMPARE(runs.started(), 1);
        for (const auto &arguments : results) {
            QVERIFY(requests.remove(qvariant_cast<RequestId>(arguments[0]).value));
            const auto result = qvariant_cast<AnalysisResult>(arguments[1]);
            compareSource(result.source, captured);
            QVERIFY(hasSymbol(result, "target"));
        }
        QVERIFY(requests.isEmpty());
    }

    void queuedCancellationAvoidsParsingWithoutDroppingOtherRequests() {
        QVERIFY(analysisLog().isInfoEnabled());
        AnalysisRuns runs(true);
        Engine engine;
        auto unblock = qScopeGuard([&] { runs.releaseWorkers(); });
        QSignalSpy results(&engine, &Engine::analysisReady);
        QSet<quint64> requests;
        RequestId cancelled;
        for (int i = 0; i < 24; ++i) {
            if (i == RuntimeLimits::WorkerThreads)
                QVERIFY(runs.waitForWorkers());
            auto document = engine.createDocument(QString(":/analysis-queue-%1.py").arg(i),
                                                  "def target(): pass\n");
            QVERIFY(document);
            const auto id = engine.analyze(document.value()->snapshot());
            requests.insert(id.value);
            cancelled = id;
        }
        engine.cancel(cancelled);
        requests.remove(cancelled.value);
        runs.releaseWorkers();
        unblock.dismiss();
        QTRY_COMPARE(results.size(), 23);
        QCOMPARE(runs.started(), 23);
        for (const auto &arguments : results) {
            QVERIFY(requests.remove(qvariant_cast<RequestId>(arguments[0]).value));
            QVERIFY(hasSymbol(qvariant_cast<AnalysisResult>(arguments[1]), "target"));
        }
        QVERIFY(requests.isEmpty());
    }

    void subscriberOverflowReturnsExplicitCancellableErrors_data() {
        QTest::addColumn<bool>("cancelOverflow");
        QTest::newRow("all-outcomes") << false;
        QTest::newRow("cancel-overflow") << true;
    }

    void subscriberOverflowReturnsExplicitCancellableErrors() {
        QFETCH(bool, cancelOverflow);
        Engine engine;
        auto document = engine.createDocument(":/analysis-overflow.py", "def target(): pass\n");
        QVERIFY(document);
        const auto captured = document.value()->snapshot();
        QSignalSpy results(&engine, &Engine::analysisReady);
        QSet<quint64> requests;
        RequestId last;
        for (int i = 0; i < 600; ++i) {
            last = engine.analyze(captured);
            requests.insert(last.value);
        }
        QVERIFY(results.isEmpty());
        if (cancelOverflow) {
            engine.cancel(last);
            requests.remove(last.value);
        }
        const auto expected = requests.size();
        QTRY_COMPARE(results.size(), expected);
        int successes = 0;
        int errors = 0;
        for (const auto &arguments : results) {
            QVERIFY(requests.remove(qvariant_cast<RequestId>(arguments[0]).value));
            const auto result = qvariant_cast<AnalysisResult>(arguments[1]);
            compareSource(result.source, captured);
            if (result.analysis.error.isEmpty()) {
                ++successes;
                QVERIFY(hasSymbol(result, "target"));
            } else {
                ++errors;
                QVERIFY(result.analysis.error.contains("queue is full"));
                QVERIFY(result.analysis.error.contains("retry"));
                QVERIFY(result.analysis.styles.isEmpty());
                QVERIFY(result.analysis.symbols.isEmpty());
            }
        }
        QVERIFY(requests.isEmpty());
        QVERIFY(successes > 0);
        QVERIFY(errors > 0);
        QCOMPARE(successes + errors, expected);
    }

    void distinctJobOverflowReturnsErrorsWithoutLosingAcceptedRequests() {
        Engine engine;
        QSignalSpy results(&engine, &Engine::analysisReady);
        QHash<quint64, SourceSnapshot> requests;
        for (int i = 0; i < 200; ++i) {
            auto document = engine.createDocument(QString(":/analysis-distinct-%1.py").arg(i),
                                                  "def target(): pass\n");
            QVERIFY(document);
            const auto source = document.value()->snapshot();
            requests.insert(engine.analyze(source).value, source);
        }
        QVERIFY(results.isEmpty());
        QTRY_COMPARE(results.size(), 200);
        int successes = 0;
        int errors = 0;
        for (const auto &arguments : results) {
            const auto id = qvariant_cast<RequestId>(arguments[0]);
            QVERIFY(requests.contains(id.value));
            const auto source = requests.take(id.value);
            const auto result = qvariant_cast<AnalysisResult>(arguments[1]);
            compareSource(result.source, source);
            if (result.analysis.error.isEmpty()) {
                ++successes;
                QVERIFY(hasSymbol(result, "target"));
            } else {
                ++errors;
                QVERIFY(result.analysis.error.contains("queue is full"));
                QVERIFY(result.analysis.symbols.isEmpty());
            }
        }
        QVERIFY(requests.isEmpty());
        QVERIFY(successes > 0);
        QVERIFY(errors > 0);
        QCOMPARE(successes + errors, 200);
    }
};

QTEST_GUILESS_MAIN(AnalysisTests)
#include "AnalysisTests.moc"

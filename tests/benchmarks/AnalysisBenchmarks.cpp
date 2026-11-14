#include <codecairn/Engine.h>
#include <codecairn/FileIO.h>
#include <codecairn/RuntimeLimits.h>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTest>
#include <QTimer>
#include <QThreadPool>
#include <algorithm>

class AnalysisBenchmarks : public QObject {
    Q_OBJECT

    void measure(int count, bool equivalent, const char *label, int extraFunctions = 0) {
        codecairn::Engine engine;
        QByteArray source = "def target():\n    return 1\n";
        for (int i = 0; i < extraFunctions; ++i)
            source += "def function_" + QByteArray::number(i) + "():\n    return " +
                      QByteArray::number(i) + "\n";
        const auto hash = sourceContentHash(source);
        QVector<codecairn::SourceSnapshot> inputs;
        for (int i = 0; i < count; ++i)
            inputs.append({QString(":/analysis_%1.py").arg(equivalent ? 0 : i), source, hash, 1});
        QVector<qint64> timings;
        for (int iteration = 0; iteration < 7; ++iteration) {
            QEventLoop loop;
            QTimer timeout;
            timeout.setSingleShot(true);
            int completed = 0;
            QString error;
            const auto connection =
                connect(&engine, &codecairn::Engine::analysisReady, &loop,
                        [&](codecairn::RequestId, const codecairn::AnalysisResult &result) {
                            if (!result.analysis.error.isEmpty()) {
                                error = result.analysis.error;
                                loop.quit();
                            } else if (++completed == count)
                                loop.quit();
                        });
            connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
            timeout.start(30000);
            QElapsedTimer elapsed;
            elapsed.start();
            for (const auto &input : inputs)
                engine.analyze(input);
            loop.exec();
            timings.append(elapsed.nsecsElapsed());
            disconnect(connection);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QVERIFY(timeout.isActive());
            QCOMPARE(completed, count);
        }
        std::sort(timings.begin(), timings.end());
        qInfo().nospace() << label << " requests=" << count << " source_bytes=" << source.size()
                          << " median_ms=" << timings[timings.size() / 2] / 1000000.0
                          << " max_ms=" << timings.last() / 1000000.0;
    }

private slots:
    void initTestCase() {
        QThreadPool::globalInstance()->setMaxThreadCount(RuntimeLimits::WorkerThreads);
    }
    void distinctAnalysisBurst() { measure(128, false, "distinct_analysis"); }
    void equivalentAnalysisBurst() { measure(512, true, "equivalent_analysis"); }
    void representativeAnalysisBurst() { measure(128, false, "representative_analysis", 100); }
};

QTEST_GUILESS_MAIN(AnalysisBenchmarks)
#include "AnalysisBenchmarks.moc"

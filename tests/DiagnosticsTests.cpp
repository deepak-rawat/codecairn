#include "Diagnostics.h"
#include "AppSettings.h"
#include "ApplicationStateStore.h"
#include "CompilationDatabase.h"
#include "CompilerSymbolRecovery.h"
#include "FileIO.h"
#include "LanguageServerClient.h"
#include "ReferenceSearch.h"
#include "SourceAnalysis.h"
#include "WorkspaceIndex.h"
#include "WorkspaceOptionsStore.h"
#include "TestStorage.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QSet>
#include <QTest>
#include <QtConcurrentRun>
#include <QUrl>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

namespace {
std::atomic_int forwarded = 0;
void sink(QtMsgType, const QMessageLogContext &, const QString &) {
    ++forwarded;
}

QVector<QJsonObject> records(const QString &directory) {
    QVector<QJsonObject> result;
    const auto files = QDir(directory).entryInfoList({"*.jsonl"}, QDir::Files, QDir::Name);
    for (const auto &entry : files) {
        QFile file(entry.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly))
            qFatal("Cannot read test diagnostic log.");
        while (!file.atEnd()) {
            QJsonParseError error;
            const auto document = QJsonDocument::fromJson(file.readLine(), &error);
            if (error.error != QJsonParseError::NoError || !document.isObject())
                qFatal("Invalid JSONL diagnostic record.");
            result.append(document.object());
        }
    }
    return result;
}

QJsonObject marker(const QString &directory) {
    QFile file(QDir(directory).filePath("session.json"));
    if (!file.open(QIODevice::ReadOnly))
        qFatal("Cannot read diagnostic session marker.");
    return QJsonDocument::fromJson(file.readAll()).object();
}

bool hasEvent(const QString &directory, const QString &event) {
    for (const auto &record : records(directory))
        if (record.value("event") == event)
            return true;
    return false;
}

int languageServerHelper(const QString &mode) {
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    const auto write = [](const QByteArray &body) {
        const auto frame = "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
        return std::fwrite(frame.constData(), 1, frame.size(), stdout) ==
                   static_cast<size_t>(frame.size()) &&
               std::fflush(stdout) == 0;
    };
    const auto send = [&](const QJsonObject &object) {
        return write(QJsonDocument(object).toJson(QJsonDocument::Compact));
    };
    while (true) {
        QByteArray header;
        while (!header.endsWith("\r\n\r\n")) {
            const auto ch = std::getchar();
            if (ch == EOF)
                return 0;
            header.append(static_cast<char>(ch));
            if (header.size() > 8192)
                return 30;
        }
        bool ok = false;
        const auto length = header.mid(15).trimmed().toInt(&ok);
        if (!ok || length < 0 || length > 32768)
            return 31;
        QByteArray body(length, Qt::Uninitialized);
        if (std::fread(body.data(), 1, length, stdin) != static_cast<size_t>(length))
            return 32;
        const auto message = QJsonDocument::fromJson(body).object();
        const auto method = message.value("method").toString();
        if (method == "exit")
            return 0;
        if (method == "initialize") {
            if (mode == "json") {
                if (!write("{"))
                    return 33;
            } else if (mode == "length") {
                if (std::printf("Content-Length: -1\r\n\r\n") < 0 || std::fflush(stdout))
                    return 34;
            } else if (!send({{"jsonrpc", "2.0"},
                              {"id", message.value("id")},
                              {"result", QJsonObject{{"capabilities", QJsonObject{}}}}}))
                return 35;
        } else if (method == "initialized" && mode == "diagnostics") {
            QJsonArray diagnostics;
            for (int i = 0; i < 13; ++i)
                diagnostics.append(QJsonObject{
                    {"severity", i % 2 + 1},
                    {"message", "test semantic parse failure"},
                    {"range", QJsonObject{{"start", QJsonObject{{"line", 4}, {"character", 6}}}}}});
            if (!send({{"jsonrpc", "2.0"},
                       {"method", "textDocument/publishDiagnostics"},
                       {"params", QJsonObject{{"uri", QUrl::fromLocalFile(
                                                          QDir::current().filePath("semantic.cpp"))
                                                          .toString()},
                                              {"diagnostics", diagnostics}}}}))
                return 36;
        } else if (method == "textDocument/references") {
            const int code = mode == "cancelled" ? -32800 : -32603;
            if (!send(
                    {{"jsonrpc", "2.0"},
                     {"id", message.value("id")},
                     {"error", QJsonObject{{"code", code}, {"message", "test request failure"}}}}))
                return 37;
        }
    }
}

int helper(int argc, char **argv) {
    Diagnostics diagnostics;
    {
        QCoreApplication app(argc, argv);
        app.setApplicationName("CodeCairnDiagnosticsHelper");
        app.setApplicationVersion("test");
        DiagnosticOptions options;
        options.directory = QString::fromLocal8Bit(argv[2]);
        const auto error = diagnostics.start(options);
        if (!error.isEmpty()) {
            std::fprintf(stderr, "%s\n", qPrintable(error));
            return 10;
        }
        qWarning("helper diagnostic is flushed");
        const auto mode = QString::fromLocal8Bit(argv[3]);
        if (mode == "abrupt")
            std::_Exit(23);
        if (mode == "active") {
            const auto bytes = diagnostics.sessionDirectory().toUtf8();
            std::printf("READY:%s\n", bytes.constData());
            std::fflush(stdout);
            std::getchar();
        }
    }
    return diagnostics.finish(0).isEmpty() ? 0 : 11;
}
}

class DiagnosticsTests : public QObject {
    Q_OBJECT
    TestStorage storage_;
private slots:
    void platformDirectory();
    void structuredRecordsAndForwarding();
    void concurrentWriters();
    void boundedRotation();
    void uncleanShutdownRecovery();
    void activeSessionsAreNotRecoveredOrDeleted();
    void retentionAndUnknownFiles();
    void failuresAreExplicit();
    void analysisContextWithoutSourceBuffers();
    void normalShutdownAfterApplicationDestruction();
    void fatalSeverityIsFlushed();
    void invalidMarkersArePreserved();
    void fileAndStorageFailuresAreLogged();
    void searchAndCompilerFailuresAreLogged();
    void expectedOutcomesAreNotWarnings();
    void languageServiceFailures_data();
    void languageServiceFailures();
    void syntaxWarningLimit();
};

void DiagnosticsTests::platformDirectory() {
    const auto path = Diagnostics::defaultDirectory();
    QVERIFY(QDir::isAbsolutePath(path));
    QVERIFY(path.contains(QCoreApplication::applicationName()));
#ifdef Q_OS_WIN
    QCOMPARE(path, QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                       .filePath("Logs"));
#elif defined(Q_OS_MACOS)
    QCOMPARE(path, QDir::home().filePath("Library/Logs/" + QCoreApplication::applicationName()));
#else
    QVERIFY(path.endsWith("/logs"));
#endif
}

void DiagnosticsTests::structuredRecordsAndForwarding() {
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const auto previous = qInstallMessageHandler(sink);
    const auto restore = qScopeGuard([&] { qInstallMessageHandler(previous); });
    forwarded = 0;
    QString session;
    {
        Diagnostics diagnostics;
        DiagnosticOptions options;
        options.directory = root.path();
        QVERIFY(diagnostics.start(options).isEmpty());
        session = diagnostics.sessionDirectory();
        QCOMPARE(Diagnostics::directory(), root.path());
        QCOMPARE(Diagnostics::currentLogPath(), diagnostics.logPath());
        qInfo("two\nlines");
        qWarning("warning record");
        qDebug("debug is not written by default");
        QVERIFY(diagnostics.finish(0).isEmpty());
    }
    QCOMPARE(forwarded.load(), 3);
    const auto lines = records(session);
    bool info = false, warning = false, debug = false;
    for (const auto &line : lines) {
        QVERIFY(line.contains("time") && line.contains("sequence") && line.contains("thread"));
        QCOMPARE(line.value("pid").toInteger(), QCoreApplication::applicationPid());
        info = info || line.value("message") == "two\nlines";
        warning = warning ||
                  (line.value("message") == "warning record" && line.value("level") == "warning");
        debug = debug || line.value("level") == "debug";
    }
    QVERIFY(info && warning && !debug);
    QCOMPARE(marker(session).value("state").toString(), "completed");
    QVERIFY(marker(session).value("loggingComplete").toBool());
}

void DiagnosticsTests::concurrentWriters() {
    QTemporaryDir root;
    const auto previous = qInstallMessageHandler(sink);
    const auto restore = qScopeGuard([&] { qInstallMessageHandler(previous); });
    Diagnostics diagnostics;
    DiagnosticOptions options;
    options.directory = root.path();
    options.debug = true;
    QVERIFY(diagnostics.start(options).isEmpty());
    QVector<QFuture<void>> jobs;
    for (int thread = 0; thread < 4; ++thread)
        jobs.append(QtConcurrent::run([thread] {
            for (int row = 0; row < 50; ++row)
                qInfo().nospace() << "worker-" << thread << '-' << row;
        }));
    for (auto &job : jobs)
        job.waitForFinished();
    qDebug("debug enabled");
    QVERIFY(diagnostics.finish(0).isEmpty());
    const auto lines = records(diagnostics.sessionDirectory());
    QSet<qint64> sequence;
    int workers = 0;
    bool debug = false;
    for (const auto &line : lines) {
        QVERIFY(!sequence.contains(line.value("sequence").toInteger()));
        sequence.insert(line.value("sequence").toInteger());
        workers += line.value("message").toString().startsWith("worker-");
        debug = debug || line.value("message") == "debug enabled";
    }
    QCOMPARE(workers, 200);
    QVERIFY(debug);
}

void DiagnosticsTests::boundedRotation() {
    QTemporaryDir root;
    const auto previous = qInstallMessageHandler(sink);
    const auto restore = qScopeGuard([&] { qInstallMessageHandler(previous); });
    Diagnostics diagnostics;
    DiagnosticOptions options;
    options.directory = root.path();
    options.maxFileBytes = 4096;
    options.maxFiles = 3;
    QVERIFY(diagnostics.start(options).isEmpty());
    for (int i = 0; i < 20; ++i)
        qWarning().noquote() << QString(20000, QChar(0x4e2d));
    QVERIFY(diagnostics.finish(0).isEmpty());
    const auto files = QDir(diagnostics.sessionDirectory()).entryInfoList({"*.jsonl"}, QDir::Files);
    QCOMPARE(files.size(), 3);
    for (const auto &file : files)
        QVERIFY(file.size() <= options.maxFileBytes);
    bool truncated = false;
    for (const auto &line : records(diagnostics.sessionDirectory()))
        truncated = truncated || line.value("truncated").toBool();
    QVERIFY(truncated);
}

void DiagnosticsTests::uncleanShutdownRecovery() {
    QTemporaryDir root;
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(),
                {"--diagnostics-child", root.path(), "abrupt"});
    QVERIFY(child.waitForFinished(10000));
    QCOMPARE(child.exitCode(), 23);
    const auto sessions = QDir(root.path()).entryList({"session-*"}, QDir::Dirs);
    QCOMPARE(sessions.size(), 1);
    const auto old = QDir(root.path()).filePath(sessions.first());
    QCOMPARE(marker(old).value("state").toString(), "running");
    QVERIFY(records(old).last().value("message").toString().contains("flushed"));
    Diagnostics diagnostics;
    DiagnosticOptions options;
    options.directory = root.path();
    QVERIFY(diagnostics.start(options).isEmpty());
    QCOMPARE(diagnostics.uncleanSessions(), QStringList{old});
    QCOMPARE(marker(old).value("state").toString(), "unclean");
    QVERIFY(diagnostics.finish(0).isEmpty());
}

void DiagnosticsTests::activeSessionsAreNotRecoveredOrDeleted() {
    QTemporaryDir root;
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(),
                {"--diagnostics-child", root.path(), "active"});
    const auto stop = qScopeGuard([&] {
        if (child.state() != QProcess::NotRunning) {
            child.write("\n");
            child.waitForFinished(10000);
        }
    });
    QVERIFY(child.waitForReadyRead(10000));
    const auto ready = QString::fromUtf8(child.readAllStandardOutput()).trimmed();
    QVERIFY2(ready.startsWith("READY:"), qPrintable(ready));
    const auto old = ready.mid(6);
    Diagnostics diagnostics;
    DiagnosticOptions options;
    options.directory = root.path();
    options.maxSessions = 1;
    QVERIFY(diagnostics.start(options).isEmpty());
    QVERIFY(diagnostics.uncleanSessions().isEmpty());
    QVERIFY(QFileInfo::exists(old));
    QCOMPARE(marker(old).value("state").toString(), "running");
    QVERIFY(diagnostics.finish(0).isEmpty());
    child.write("\n");
    QVERIFY(child.waitForFinished(10000));
    QCOMPARE(child.exitCode(), 0);
    QCOMPARE(marker(old).value("state").toString(), "completed");
}

void DiagnosticsTests::retentionAndUnknownFiles() {
    QTemporaryDir root;
    DiagnosticOptions options;
    options.directory = root.path();
    options.maxSessions = 2;
    QString first;
    for (int i = 0; i < 3; ++i) {
        Diagnostics diagnostics;
        QVERIFY(diagnostics.start(options).isEmpty());
        if (i == 0)
            first = diagnostics.sessionDirectory();
        QVERIFY(diagnostics.finish(0).isEmpty());
        QTest::qWait(2);
    }
    QVERIFY(!QFileInfo::exists(first));
    QCOMPARE(QDir(root.path()).entryList({"session-*"}, QDir::Dirs).size(), 2);
    auto oldest = QDir(root.path())
                      .entryInfoList({"session-*"}, QDir::Dirs, QDir::Name)
                      .first()
                      .absoluteFilePath();
    QFile extra(QDir(oldest).filePath("notes.txt"));
    QVERIFY(extra.open(QIODevice::WriteOnly));
    extra.write("not owned by log retention");
    extra.close();
    options.maxSessions = 1;
    Diagnostics diagnostics;
    QVERIFY(diagnostics.start(options).isEmpty());
    QVERIFY(QFileInfo::exists(extra.fileName()));
    QVERIFY(!diagnostics.startupWarnings().isEmpty());
    QVERIFY(diagnostics.finish(0).isEmpty());
}

void DiagnosticsTests::failuresAreExplicit() {
    QTemporaryDir root;
    QFile notDirectory(root.filePath("file"));
    QVERIFY(notDirectory.open(QIODevice::WriteOnly));
    notDirectory.close();
    {
        Diagnostics diagnostics;
        DiagnosticOptions options;
        options.directory = notDirectory.fileName();
        QVERIFY(!diagnostics.start(options).isEmpty());
        QCOMPARE(Diagnostics::directory(), notDirectory.fileName());
        QVERIFY(Diagnostics::currentLogPath().isEmpty());
    }
    {
        Diagnostics diagnostics;
        DiagnosticOptions options;
        options.directory = root.path();
        options.maxFiles = 0;
        QVERIFY(!diagnostics.start(options).isEmpty());
        options.maxFiles = 5;
        options.maxFileBytes = 4095;
        QVERIFY(!diagnostics.start(options).isEmpty());
        options.maxFileBytes = 4096;
        options.directory = "relative-logs";
        QVERIFY(!diagnostics.start(options).isEmpty());
    }
    const auto previous = qInstallMessageHandler(sink);
    const auto restore = qScopeGuard([&] { qInstallMessageHandler(previous); });
    Diagnostics diagnostics;
    DiagnosticOptions options;
    options.directory = root.path();
    options.maxFileBytes = 4096;
    options.maxFiles = 2;
    QVERIFY(diagnostics.start(options).isEmpty());
    QVERIFY(QDir().mkdir(QDir(diagnostics.sessionDirectory()).filePath("codecairn.1.jsonl")));
    qWarning().noquote() << QString(3000, 'a');
    qWarning().noquote() << QString(3000, 'b');
    QVERIFY(!Diagnostics::takePendingError().isEmpty());
    QVERIFY(Diagnostics::takePendingError().isEmpty());
    QVERIFY(diagnostics.finish(0).isEmpty());
    QVERIFY(!marker(diagnostics.sessionDirectory()).value("loggingComplete").toBool());
}

void DiagnosticsTests::analysisContextWithoutSourceBuffers() {
    QTemporaryDir root;
    const auto previous = qInstallMessageHandler(sink);
    const auto restore = qScopeGuard([&] { qInstallMessageHandler(previous); });
    Diagnostics diagnostics;
    DiagnosticOptions options;
    options.directory = root.path();
    QVERIFY(diagnostics.start(options).isEmpty());
    const QByteArray source = "// PRIVATE_SOURCE_COMMENT_NOT_FOR_LOGGING\nint broken( {\n";
    const auto path = root.filePath("broken.cpp");
    QVERIFY(writeSourceFile(path, source, false).isEmpty());
    const auto result = analyzeSource(source, path);
    QVERIFY(result.syntaxErrors);
    QVERIFY(result.compilerEnriched);
    QVERIFY(diagnostics.finish(0).isEmpty());
    bool started = false, anomaly = false, syntax = false;
    for (const auto &line : records(diagnostics.sessionDirectory())) {
        const auto message = line.value("message").toString();
        started = started || (line.value("event") == "analysis.started" && message.contains(path));
        anomaly = anomaly || (line.value("event") == "analysis.anomaly" && message.contains(path));
        if (line.value("event") == "analysis.syntax_error") {
            QCOMPARE(line.value("level").toString(), "warning");
            QVERIFY(message.contains(path) && message.contains("line=") &&
                    message.contains("byteColumn=") && message.contains("node="));
            syntax = true;
        }
        QVERIFY(!message.contains("PRIVATE_SOURCE_COMMENT_NOT_FOR_LOGGING"));
    }
    QVERIFY(started && anomaly && syntax);
    QVERIFY(hasEvent(diagnostics.sessionDirectory(), "analysis.compiler_diagnostic"));
}

void DiagnosticsTests::normalShutdownAfterApplicationDestruction() {
    QTemporaryDir root;
    QProcess child;
    child.start(QCoreApplication::applicationFilePath(),
                {"--diagnostics-child", root.path(), "normal"});
    QVERIFY(child.waitForFinished(10000));
    QCOMPARE(child.exitCode(), 0);
    const auto sessions = QDir(root.path()).entryList({"session-*"}, QDir::Dirs);
    QCOMPARE(sessions.size(), 1);
    QCOMPARE(marker(QDir(root.path()).filePath(sessions.first())).value("state").toString(),
             "completed");
}

void DiagnosticsTests::fatalSeverityIsFlushed() {
    QTemporaryDir root;
    const auto previous = qInstallMessageHandler(sink);
    const auto restore = qScopeGuard([&] { qInstallMessageHandler(previous); });
    Diagnostics diagnostics;
    DiagnosticOptions options;
    options.directory = root.path();
    QVERIFY(diagnostics.start(options).isEmpty());
    const auto handler = qInstallMessageHandler(nullptr);
    qInstallMessageHandler(handler);
    QVERIFY(handler);
    handler(QtFatalMsg, QMessageLogContext(__FILE__, __LINE__, Q_FUNC_INFO, "test"),
            "fatal record before termination");
    const auto last = records(diagnostics.sessionDirectory()).last();
    QCOMPARE(last.value("level").toString(), "fatal");
    QCOMPARE(last.value("message").toString(), "fatal record before termination");
    QVERIFY(diagnostics.finish(0).isEmpty());
}

void DiagnosticsTests::invalidMarkersArePreserved() {
    QTemporaryDir root;
    QString old;
    {
        Diagnostics diagnostics;
        DiagnosticOptions options;
        options.directory = root.path();
        QVERIFY(diagnostics.start(options).isEmpty());
        old = diagnostics.sessionDirectory();
        QVERIFY(diagnostics.finish(0).isEmpty());
    }
    QFile metadata(QDir(old).filePath("session.json"));
    QVERIFY(metadata.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray invalid = "{ invalid diagnostic metadata";
    QCOMPARE(metadata.write(invalid), invalid.size());
    metadata.close();
    QTest::qWait(2);
    Diagnostics diagnostics;
    DiagnosticOptions options;
    options.directory = root.path();
    options.maxSessions = 1;
    QVERIFY(diagnostics.start(options).isEmpty());
    QVERIFY(!diagnostics.startupWarnings().isEmpty());
    QVERIFY(diagnostics.uncleanSessions().isEmpty());
    QVERIFY(metadata.open(QIODevice::ReadOnly));
    QCOMPARE(metadata.readAll(), invalid);
    QVERIFY(diagnostics.finish(0).isEmpty());
}

void DiagnosticsTests::fileAndStorageFailuresAreLogged() {
    QTemporaryDir root;
    const auto previous = qInstallMessageHandler(sink);
    const auto restore = qScopeGuard([&] { qInstallMessageHandler(previous); });
    Diagnostics diagnostics;
    DiagnosticOptions options;
    options.directory = root.filePath("logs");
    QVERIFY(diagnostics.start(options).isEmpty());
    const auto missing = root.filePath("missing.cpp");
    QVERIFY(!readSourceFile(missing).error.isEmpty());
    QVERIFY(!writeSourceFile(root.filePath("absent/failed.cpp"), "source", false).isEmpty());
    const auto binary = root.filePath("binary.cpp");
    QVERIFY(writeSourceFile(binary, QByteArray(1, '\0') + "PRIVATE_FILE_BUFFER", false).isEmpty());
    QVERIFY(!readSourceFile(binary).error.isEmpty());
    QFile oversized(root.filePath("oversized.cpp"));
    QVERIFY(oversized.open(QIODevice::WriteOnly));
    QVERIFY(oversized.resize(MaximumDocumentBytes + 1));
    oversized.close();
    QVERIFY(!readSourceFile(oversized.fileName()).error.isEmpty());
    SettingsFile settings(root.filePath("settings.json"));
    QVERIFY(settings.load().settings);
    QVERIFY(writeSourceFile(settings.path(), "{ invalid", false).isEmpty());
    QVERIFY(!settings.load().error.isEmpty());
    QVERIFY(!settings.save(defaultAppSettings()).isEmpty());
    ApplicationStateStore state(root.filePath("state"));
    QVERIFY(state.loadState().state);
    QVERIFY(state.saveState({}).isEmpty());
    QVERIFY(writeSourceFile(state.statePath(), "[]", false).isEmpty());
    QVERIFY(!state.loadState().error.isEmpty());
    QVERIFY(!state.saveState({}).isEmpty());
    QVERIFY(state.loadSession(root.path()).session);
    QVERIFY(state.saveSession(root.path(), {}).isEmpty());
    QVERIFY(
        writeSourceFile(state.sessionPath(root.path()), "{\"schemaVersion\":99}", false).isEmpty());
    QVERIFY(!state.loadSession(root.path()).error.isEmpty());
    QVERIFY(!state.saveSession(root.path(), {}).isEmpty());
    QVERIFY(!WorkspaceOptionsStore::initialize(root.filePath("no-workspace")).isEmpty());
    QVERIFY(diagnostics.finish(0).isEmpty());
    const QStringList events{"file.read_failed",
                             "file.write_failed",
                             "storage.settings_read_failed",
                             "storage.settings_write_failed",
                             "storage.state_read_failed",
                             "storage.state_write_failed",
                             "storage.session_read_failed",
                             "storage.session_write_failed",
                             "storage.workspace_initialize_failed"};
    for (const auto &event : events)
        QVERIFY2(hasEvent(diagnostics.sessionDirectory(), event), qPrintable(event));
    for (const auto &record : records(diagnostics.sessionDirectory())) {
        const auto message = record.value("message").toString();
        QVERIFY(!message.contains("PRIVATE_FILE_BUFFER"));
        if (record.value("level") == "warning")
            QVERIFY(message.contains("path=") || message.contains("root="));
    }
}

void DiagnosticsTests::searchAndCompilerFailuresAreLogged() {
    QTemporaryDir root;
    const auto previous = qInstallMessageHandler(sink);
    const auto restore = qScopeGuard([&] { qInstallMessageHandler(previous); });
    Diagnostics diagnostics;
    DiagnosticOptions options;
    options.directory = root.filePath("logs");
    QVERIFY(diagnostics.start(options).isEmpty());
    const auto path = root.filePath("conditional.c");
    QVERIFY(!compilerInactiveRanges(path, "#if UNKNOWN_CONDITION\nint value;\n#endif\n", {})
                 .error.isEmpty());
    const auto database = root.filePath("compile_commands.json");
    QVERIFY(writeSourceFile(database, "{ invalid", false).isEmpty());
    QVERIFY(!prepareCompilationDatabase(root.path(), root.path()).error.isEmpty());
    ReferenceSearchRequest request;
    request.query = "[";
    request.options.method = SearchMethod::RegularExpression;
    QVERIFY(!searchReferences(request, {}).error.isEmpty());
    QVERIFY(!resolveSemanticReferences({}, {}, "symbol", {}, {}, -1).error.isEmpty());
    SymbolQuery query;
    query.text = "?[";
    QVERIFY(!queryWorkspaceSymbols({}, query).error.isEmpty());
    const auto cancel = std::make_shared<std::atomic_bool>(false);
    QVERIFY(!buildWorkspaceIndex(root.filePath("missing"), false, true, {}, cancel, {})
                 .error.isEmpty());
    QVERIFY(diagnostics.finish(0).isEmpty());
    const QStringList events{"analysis.inactive_failed", "lsp.compilation_failed", "search.failed",
                             "search.semantic_failed",   "index.query_failed",     "index.failed"};
    for (const auto &event : events)
        QVERIFY2(hasEvent(diagnostics.sessionDirectory(), event), qPrintable(event));
}

void DiagnosticsTests::expectedOutcomesAreNotWarnings() {
    QTemporaryDir root;
    const auto previous = qInstallMessageHandler(sink);
    const auto restore = qScopeGuard([&] { qInstallMessageHandler(previous); });
    Diagnostics diagnostics;
    DiagnosticOptions options;
    options.directory = root.filePath("logs");
    QVERIFY(diagnostics.start(options).isEmpty());
    SettingsFile settings(root.filePath("missing-settings.json"));
    QVERIFY(settings.load().settings);
    ApplicationStateStore state(root.filePath("new-state"));
    QVERIFY(state.loadState().state);
    QVERIFY(state.loadSession(root.path()).session);
    QVERIFY(!analyzeSource("unclassified text", root.filePath("plain.unknown")).supported);
    ReferenceSearchRequest request;
    request.query = "absent";
    request.options.method = SearchMethod::Literal;
    const auto source = root.filePath("valid.py");
    QVERIFY(writeSourceFile(source, "value = 1\n", false).isEmpty());
    request.paths = {source};
    const auto result = searchReferences(request, {});
    QVERIFY(result.error.isEmpty() && result.hits.isEmpty());
    const auto cancel = std::make_shared<std::atomic_bool>(true);
    QVERIFY(searchReferences(request, cancel).cancelled);
    SymbolQuery query;
    query.cancel = cancel;
    QVERIFY(queryWorkspaceSymbols(root.path(), query).cancelled);
    const auto catalog = root.filePath("catalog");
    QVERIFY(QDir().mkpath(catalog));
    const auto asset = QDir(catalog).filePath("image.png");
    QVERIFY(writeSourceFile(asset, QByteArray(1, '\0') + "BINARY_ASSET", false).isEmpty());
    const auto index = buildWorkspaceIndex(catalog, false, true, {},
                                           std::make_shared<std::atomic_bool>(false), {});
    QVERIFY(index.error.isEmpty() && index.warnings.isEmpty());
    QCOMPARE(index.files.size(), 1);
    QCOMPARE(index.parsed, 0);
    QVERIFY(diagnostics.finish(0).isEmpty());
    for (const auto &record : records(diagnostics.sessionDirectory()))
        QVERIFY(record.value("level") != "warning" && record.value("level") != "critical");
}

void DiagnosticsTests::languageServiceFailures_data() {
    QTest::addColumn<QString>("mode");
    QTest::addColumn<QString>("event");
    QTest::newRow("invalid-json") << QString("json") << QString("lsp.protocol_error");
    QTest::newRow("invalid-length") << QString("length") << QString("lsp.protocol_error");
    QTest::newRow("request-error") << QString("rpc") << QString("lsp.request_failed");
    QTest::newRow("semantic-diagnostics") << QString("diagnostics") << QString("lsp.diagnostic");
    QTest::newRow("request-cancelled") << QString("cancelled") << QString{};
}

void DiagnosticsTests::languageServiceFailures() {
    QFETCH(QString, mode);
    QFETCH(QString, event);
    QTemporaryDir root;
    const auto previous = qInstallMessageHandler(sink);
    const auto restore = qScopeGuard([&] { qInstallMessageHandler(previous); });
    Diagnostics diagnostics;
    DiagnosticOptions options;
    options.directory = root.filePath("logs");
    QVERIFY(diagnostics.start(options).isEmpty());
    {
        LanguageServerClient client;
        QSignalSpy ready(&client, &LanguageServerClient::ready);
        QSignalSpy replies(&client, &LanguageServerClient::locationsReady);
        client.startServer(root.path(),
                           {QCoreApplication::applicationFilePath(), "--diagnostics-lsp", mode});
        if (mode == "rpc" || mode == "cancelled") {
            QTRY_COMPARE_WITH_TIMEOUT(ready.size(), 1, 10000);
            client.locations(root.filePath("semantic.cpp"), "// PRIVATE_LSP_BUFFER\nint value;\n",
                             1, 27, 1, "diagnostic-test", true);
            QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, 10000);
            QCOMPARE(replies.first().at(3).toString(), QString("test request failure"));
        }
        if (!event.isEmpty())
            QTRY_VERIFY_WITH_TIMEOUT(hasEvent(diagnostics.sessionDirectory(), event), 10000);
        client.stop();
    }
    QVERIFY(diagnostics.finish(0).isEmpty());
    int semantic = 0;
    for (const auto &record : records(diagnostics.sessionDirectory())) {
        const auto message = record.value("message").toString();
        QVERIFY(!message.contains("PRIVATE_LSP_BUFFER"));
        if (record.value("event") == "lsp.diagnostic") {
            QCOMPARE(record.value("level").toString(), "warning");
            QVERIFY(message.contains(root.filePath("semantic.cpp")));
            QVERIFY(message.contains("line= 5") && message.contains("utf16Column= 7"));
            ++semantic;
        }
        if (record.value("event") == "lsp.request_failed") {
            QVERIFY(message.contains("textDocument/references"));
            QVERIFY(message.contains(root.filePath("semantic.cpp")));
            QVERIFY(message.contains("-32603"));
        }
        if (mode == "cancelled")
            QVERIFY(record.value("level") != "warning");
    }
    if (mode == "diagnostics") {
        QCOMPARE(semantic, 12);
        QVERIFY(hasEvent(diagnostics.sessionDirectory(), "lsp.diagnostic_limit"));
    }
}

void DiagnosticsTests::syntaxWarningLimit() {
    QTemporaryDir root;
    const auto previous = qInstallMessageHandler(sink);
    const auto restore = qScopeGuard([&] { qInstallMessageHandler(previous); });
    Diagnostics diagnostics;
    DiagnosticOptions options;
    options.directory = root.path();
    QVERIFY(diagnostics.start(options).isEmpty());
    const auto result =
        analyzeSource(QByteArray("let value = ;\n").repeated(30), root.filePath("broken.js"));
    QVERIFY(result.syntaxErrors);
    QVERIFY(diagnostics.finish(0).isEmpty());
    int syntax = 0;
    for (const auto &record : records(diagnostics.sessionDirectory()))
        syntax += record.value("event") == "analysis.syntax_error";
    QCOMPARE(syntax, 8);
    QVERIFY(hasEvent(diagnostics.sessionDirectory(), "analysis.syntax_limit"));
}

int main(int argc, char **argv) {
    if (argc == 3 && QString::fromLocal8Bit(argv[1]) == "--diagnostics-lsp")
        return languageServerHelper(QString::fromLocal8Bit(argv[2]));
    if (argc == 4 && QString::fromLocal8Bit(argv[1]) == "--diagnostics-child")
        return helper(argc, argv);
    QCoreApplication app(argc, argv);
    DiagnosticsTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "DiagnosticsTests.moc"

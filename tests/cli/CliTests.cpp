#include <codecairn/FileIO.h>
#include <codecairn/WorkspacePaths.h>

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#ifdef Q_OS_UNIX
#include <csignal>
#include <unistd.h>
#endif

class CliTests : public QObject {
    Q_OBJECT

    struct Reply {
        int code = -1;
        QJsonObject json;
        QByteArray errors;
        QByteArray text;
    };

    void configure(QProcess &process) {
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.remove("DISPLAY");
        environment.remove("WAYLAND_DISPLAY");
        environment.remove("QT_QPA_PLATFORM");
        process.setProcessEnvironment(environment);
    }

    Reply run(const QStringList &arguments, const QString &state, bool jsonOutput = true) {
        QProcess process;
        configure(process);
        auto options = arguments + QStringList{"--state-dir", state};
        if (jsonOutput)
            options.append("--json");
        process.start(QString::fromUtf8(CODECAIRN_CLI_PATH), options);
        if (!process.waitForFinished(30000)) {
            process.kill();
            process.waitForFinished();
            QTest::qFail("CLI did not finish within 30 seconds.", __FILE__, __LINE__);
            return {};
        }
        const auto bytes = process.readAllStandardOutput();
        if (!jsonOutput)
            return {process.exitCode(), {}, process.readAllStandardError(), bytes};
        QJsonParseError error;
        const auto json = QJsonDocument::fromJson(bytes, &error);
        if (error.error != QJsonParseError::NoError || !json.isObject()) {
            QTest::qFail(qPrintable(QString::fromUtf8(bytes)), __FILE__, __LINE__);
            return {};
        }
        return {process.exitCode(), json.object(), process.readAllStandardError(), bytes};
    }

private slots:
    void headlessAnalysisIndexAndSearch() {
        QTemporaryDir directory;
        const auto root = directory.filePath("project");
        const auto state = directory.filePath("state");
        QVERIFY(QDir().mkpath(root));
        const auto path = QDir(root).filePath("source.py");
        QVERIFY(writeSourceFile(path, "def target():\n    return target()\n", false).isEmpty());
        auto reply = run({"analyze", path}, state);
        QCOMPARE(reply.code, 0);
        QCOMPARE(reply.json["schemaVersion"].toInt(), 1);
        QCOMPARE(reply.json["symbols"].toArray().size(), 1);
        QCOMPARE(reply.json["symbols"].toArray()[0].toObject()["name"].toString(), "target");
        reply = run({"index", "--root", root}, state);
        QCOMPARE(reply.code, 0);
        QCOMPARE(reply.json["files"].toInt(), 1);
        QCOMPARE(reply.json["parsed"].toInt(), 1);
        reply = run({"index", "--root", root}, state);
        QCOMPARE(reply.code, 0);
        QCOMPARE(reply.json["parsed"].toInt(), 0);
        QCOMPARE(reply.json["reused"].toInt(), 1);
        reply = run({"symbols", "--root", root, "--query", "target"}, state);
        QCOMPARE(reply.code, 0);
        QCOMPARE(reply.json["total"].toInt(), 1);
        reply = run({"definition", "--root", root, "--file", path, "--byte-offset", "25"}, state);
        QCOMPARE(reply.code, 0);
        QCOMPARE(reply.json["symbols"].toArray().size(), 1);
        reply = run({"references", "--root", root, "--query", "target", "--limit", "1"}, state);
        QCOMPARE(reply.code, 0);
        QCOMPARE(reply.json["hits"].toArray().size(), 1);
        QCOMPARE(reply.json["status"].toString(), "partial");
        QVERIFY(reply.json["more"].toBool());
        reply = run(
            {"references", "--root", root, "--query", "target", "--offset", "1", "--limit", "1"},
            state);
        QCOMPARE(reply.code, 0);
        QCOMPARE(reply.json["hits"].toArray().size(), 1);
        QVERIFY(!reply.json["more"].toBool());
        QVERIFY(writeSourceFile(QDir(root).filePath("notes.txt"), "target\n", false).isEmpty());
        reply = run({"references", "--root", root, "--query", "target"}, state);
        QCOMPARE(reply.code, 0);
        QCOMPARE(reply.json["hits"].toArray().size(), 2);
        QCOMPARE(QDir(root).entryList(QDir::Files), (QStringList{"notes.txt", "source.py"}));
        QVERIFY(QDir(state).exists("workspaces"));
    }

    void errorsAndZeroMatchesAreDistinct() {
        QTemporaryDir directory;
        const auto state = directory.filePath("state");
        const auto path = directory.filePath("source.txt");
        QVERIFY(writeSourceFile(path, "hello\n", false).isEmpty());
        auto reply = run({"analyze", directory.filePath("missing.py")}, state);
        QCOMPARE(reply.code, 1);
        QVERIFY(!reply.json["error"].toString().isEmpty());
        reply = run({"references", "--file", path, "--query", "absent"}, state);
        QCOMPARE(reply.code, 0);
        QCOMPARE(reply.json["status"].toString(), "ok");
        QVERIFY(reply.json["hits"].toArray().isEmpty());
        reply = run({"references", "--file", path, "--query", "[", "--method", "regex"}, state);
        QCOMPARE(reply.code, 1);
        QVERIFY(!reply.json["error"].toString().isEmpty());
        reply = run({"symbols", "--root", directory.path(), "--limit", "-1"}, state);
        QCOMPARE(reply.code, 2);
        reply = run({"references", "--file", path, "--query", "hello", "--semantic"}, state);
        QCOMPARE(reply.code, 2);
    }

    void incompatibleOptionsFailBeforeStateCreation() {
        QTemporaryDir directory;
        const auto root = directory.filePath("project");
        QVERIFY(QDir().mkpath(root));
        const auto path = QDir(root).filePath("source.py");
        const QByteArray source = "def target():\n    return target()\n";
        QVERIFY(writeSourceFile(path, source, false).isEmpty());
        const QList<QStringList> cases{
            {"index", "--root", root, "--method", "regex"},
            {"index", "--root", root, "--query", "target"},
            {"analyze", path, "--root", root},
            {"symbols", "--root", root, "--force"},
            {"definition", "--file", path, "--byte-offset", "25", "--limit", "1"},
            {"definition", "--file", path, "--byte-offset", "25", "--query", "target"},
            {"definition", "--file", path, "--byte-offset", "25", "--semantic", "--offset", "0"},
            {"references", "--file", path, "--query", "target", "--byte-offset", "25"},
            {"references", "--file", path, "--semantic", "--method", "literal"},
            {"references", "--file", path, "--method", "unknown"},
            {"index", "--root", root, "--file", path},
            {"index", "--root", root, "--unknown-option"},
        };
        for (qsizetype i = 0; i < cases.size(); ++i) {
            const auto state = directory.filePath("state-" + QString::number(i));
            const auto reply = run(cases[i], state);
            QCOMPARE(reply.code, 2);
            QCOMPARE(reply.json["status"].toString(), "error");
            QVERIFY2(!reply.json["error"].toString().isEmpty(), qPrintable(cases[i].join(' ')));
            QVERIFY2(!QFileInfo::exists(state), qPrintable(cases[i].join(' ')));
        }
        QCOMPARE(readSourceFile(path).bytes, source);
        QCOMPARE(QDir(root).entryList(QDir::AllEntries | QDir::NoDotAndDotDot),
                 (QStringList{"source.py"}));
    }

    void humanReadableOutput() {
        QTemporaryDir directory;
        const auto root = directory.filePath("project");
        const auto state = directory.filePath("state");
        QVERIFY(QDir().mkpath(root));
        const auto path = QDir(root).filePath("source.py");
        QVERIFY(writeSourceFile(path, "def target():\n    return target()\n", false).isEmpty());
        auto reply = run({"analyze", path}, state, false);
        QCOMPARE(reply.code, 0);
        QVERIFY(reply.text.contains("1 symbol"));
        QVERIFY(reply.text.contains("target"));
        QVERIFY(reply.text.contains("[bytes "));
        QVERIFY(!reply.text.contains("\"schemaVersion\""));
        reply = run({"index", "--root", root}, state, false);
        QCOMPARE(reply.code, 0);
        QVERIFY(reply.text.contains("1 file, 1 parsed, 0 reused"));
        reply = run({"symbols", "--root", root, "--query", "target"}, state, false);
        QCOMPARE(reply.code, 0);
        QVERIFY(reply.text.contains("1 of 1 symbols"));
        QVERIFY(reply.text.contains("Provenance: syntax-index"));
        reply = run({"definition", "--file", path, "--byte-offset", "25"}, state, false);
        QCOMPARE(reply.code, 0);
        QVERIFY(reply.text.contains("target"));
        reply =
            run({"references", "--file", path, "--query", "target", "--limit", "1"}, state, false);
        QCOMPARE(reply.code, 0);
        QVERIFY(reply.text.contains("1 match;"));
        QVERIFY(reply.text.contains("More results; use --offset 1."));
        reply = run({"references", "--file", path, "--query", "absent"}, state, false);
        QCOMPARE(reply.code, 0);
        QVERIFY(reply.text.contains("0 matches"));
        reply = run({"analyze", directory.filePath("missing.py")}, state, false);
        QCOMPARE(reply.code, 1);
        QVERIFY(reply.text.isEmpty());
        QVERIFY(!reply.errors.isEmpty());
    }

    void unsavedOverlaysAreReadOnlySnapshots() {
        QTemporaryDir directory;
        const auto root = directory.filePath("project");
        const auto state = directory.filePath("state");
        QVERIFY(QDir().mkpath(root));
        const auto path = QDir(root).filePath("source.py");
        const auto other = QDir(root).filePath("other.py");
        const auto replacement = directory.filePath("buffer.txt");
        const auto second = directory.filePath("other-buffer.txt");
        const QByteArray disk = "def saved():\n    return saved()\n";
        const QByteArray unsaved = "def target():\n    return target()\n";
        QVERIFY(writeSourceFile(path, disk, false).isEmpty());
        QVERIFY(writeSourceFile(other, "def other():\n    pass\n", false).isEmpty());
        QVERIFY(writeSourceFile(replacement, unsaved, true).isEmpty());
        QVERIFY(writeSourceFile(second, "def extra():\n    return target()\n", false).isEmpty());
        const auto overlay = path + '=' + replacement;
        const auto otherOverlay = other + '=' + second;
        auto reply = run({"analyze", path, "--overlay", overlay}, state);
        QCOMPARE(reply.code, 0);
        QCOMPARE(reply.json["symbols"].toArray()[0].toObject()["name"].toString(), "target");
        QCOMPARE(reply.json["hash"].toString(), QString::fromLatin1(sourceContentHash(unsaved)));
        QCOMPARE(run({"index", "--root", root}, state).code, 0);
        reply = run({"symbols", "--root", root, "--overlay", overlay, "--overlay", otherOverlay},
                    state);
        QCOMPARE(reply.code, 0);
        const auto symbols = reply.json["symbols"].toArray();
        QCOMPARE(symbols.size(), 2);
        QStringList names;
        for (const auto &symbol : symbols) {
            names.append(symbol.toObject()["name"].toString());
            QVERIFY(symbol.toObject()["live"].toBool());
        }
        QVERIFY(names.contains("target"));
        QVERIFY(names.contains("extra"));
        reply =
            run({"definition", "--file", path, "--byte-offset", "25", "--overlay", overlay}, state);
        QCOMPARE(reply.code, 0);
        QCOMPARE(reply.json["symbols"].toArray().size(), 1);
        QCOMPARE(reply.json["symbols"].toArray()[0].toObject()["name"].toString(), "target");
        reply = run({"references", "--file", path, "--query", "target", "--overlay", overlay,
                     "--overlay", otherOverlay},
                    state);
        QCOMPARE(reply.code, 0);
        QCOMPARE(reply.json["hits"].toArray().size(), 2);
        QCOMPARE(reply.json["filesSearched"].toInt(), 1);
        reply = run({"references", "--root", root, "--query", "target", "--overlay", overlay,
                     "--overlay", otherOverlay},
                    state);
        QCOMPARE(reply.code, 0);
        QCOMPARE(reply.json["hits"].toArray().size(), 3);
        reply = run({"symbols", "--root", root, "--query", "saved"}, state);
        QCOMPARE(reply.code, 0);
        QCOMPARE(reply.json["total"].toInt(), 1);
        QCOMPARE(readSourceFile(path).bytes, disk);
        const auto buffer = readSourceFile(replacement);
        QCOMPARE(buffer.bytes, unsaved);
        QVERIFY(buffer.utf8Bom);
        QCOMPARE(QDir(root).entryList(QDir::Files), (QStringList{"other.py", "source.py"}));
        const auto unsavedPath = QDir(root).filePath("new.py");
        reply = run({"analyze", unsavedPath, "--overlay", unsavedPath + '=' + replacement}, state);
        QCOMPARE(reply.code, 0);
        QCOMPARE(reply.json["symbols"].toArray()[0].toObject()["name"].toString(), "target");
        reply = run({"references", "--file", unsavedPath, "--query", "target", "--overlay",
                     unsavedPath + '=' + replacement},
                    state);
        QCOMPARE(reply.code, 0);
        QCOMPARE(reply.json["hits"].toArray().size(), 2);
        QVERIFY(!QFileInfo::exists(unsavedPath));
    }

    void invalidOverlaysAreExplicitErrors() {
        QTemporaryDir directory;
        const auto state = directory.filePath("state");
        const auto path = directory.filePath("source.py");
        const auto replacement = directory.filePath("buffer.txt");
        QVERIFY(writeSourceFile(path, "def saved():\n    pass\n", false).isEmpty());
        QVERIFY(writeSourceFile(replacement, "def target():\n    pass\n", false).isEmpty());
        for (const auto &specification : QStringList{"missing-separator", "=file", "source="}) {
            const auto reply = run({"analyze", path, "--overlay", specification}, state);
            QCOMPARE(reply.code, 2);
            QVERIFY(!reply.json["error"].toString().isEmpty());
        }
        const auto overlay = path + '=' + replacement;
        auto reply = run({"analyze", path, "--overlay", overlay, "--overlay",
                          directory.path() + "/./source.py=" + replacement},
                         state);
        QCOMPARE(reply.code, 2);
        QVERIFY(reply.json["error"].toString().contains("Duplicate"));
        reply =
            run({"analyze", path, "--overlay", path + '=' + directory.filePath("missing")}, state);
        QCOMPARE(reply.code, 1);
        QVERIFY(writeSourceFile(replacement, QByteArray::fromHex("fffe6100"), false).isEmpty());
        reply = run({"analyze", path, "--overlay", overlay}, state);
        QCOMPARE(reply.code, 1);
        QVERIFY(!reply.json["error"].toString().isEmpty());
        QVERIFY(writeSourceFile(replacement, QByteArray::fromHex("c328"), false).isEmpty());
        reply = run({"analyze", path, "--overlay", overlay}, state);
        QCOMPARE(reply.code, 1);
        reply = run({"index", "--root", directory.path(), "--overlay", overlay}, state);
        QCOMPARE(reply.code, 2);
    }

    void manualProjectSearchIncludesUnsavedFilesAndFilters() {
        QTemporaryDir directory;
        const auto root = directory.filePath("project");
        const auto state = directory.filePath("state");
        QVERIFY(QDir().mkpath(root));
        const auto path = QDir(root).filePath("source.py");
        const auto unsavedPath = QDir(root).filePath("unsaved.py");
        const auto buffer = directory.filePath("buffer.txt");
        QVERIFY(writeSourceFile(path, "def target():\n    pass\n", false).isEmpty());
        QVERIFY(writeSourceFile(buffer, "target()\n", false).isEmpty());
        const auto overlay = unsavedPath + '=' + buffer;
        auto reply =
            run({"references", "--root", root, "--query", "target", "--overlay", overlay}, state);
        QCOMPARE(reply.code, 0);
        QCOMPARE(reply.json["hits"].toArray().size(), 2);
        QCOMPARE(reply.json["filesSearched"].toInt(), 2);
        reply = run({"references", "--root", root, "--query", "target", "--overlay", overlay,
                     "--file-patterns", "unsaved.py"},
                    state);
        QCOMPARE(reply.code, 0);
        QCOMPARE(reply.json["hits"].toArray().size(), 1);
        QCOMPARE(reply.json["hits"].toArray()[0].toObject()["path"].toString(),
                 workspacePath(unsavedPath));
        QCOMPARE(reply.json["filesSearched"].toInt(), 1);
        QVERIFY(!QFileInfo::exists(unsavedPath));
        QVERIFY(writeSourceFile(buffer, {}, false).isEmpty());
        reply =
            run({"references", "--file", unsavedPath, "--query", "target", "--overlay", overlay},
                state);
        QCOMPARE(reply.code, 0);
        QVERIFY(reply.json["hits"].toArray().isEmpty());
        QCOMPARE(reply.json["filesSearched"].toInt(), 1);
    }

    void textMethodsKeepTheirMatchingAndPaging() {
        QTemporaryDir directory;
        const auto path = directory.filePath("source.txt");
        const auto state = directory.filePath("state");
        QVERIFY(writeSourceFile(path, "target targetExtra target\n", false).isEmpty());
        auto reply = run({"references", "--file", path, "--query", "target"}, state);
        QCOMPARE(reply.code, 0);
        QCOMPARE(reply.json["hits"].toArray().size(), 3);
        for (const auto &method : QStringList{"occurrences", "references"}) {
            reply = run({"references", "--file", path, "--query", "target", "--method", method,
                         "--limit", "1"},
                        state);
            QCOMPARE(reply.code, 0);
            QCOMPARE(reply.json["hits"].toArray().size(), 1);
            QCOMPARE(reply.json["nextOffset"].toInt(), 1);
            QVERIFY(reply.json["more"].toBool());
            reply = run({"references", "--file", path, "--query", "target", "--method", method,
                         "--offset", "1", "--limit", "1"},
                        state);
            QCOMPARE(reply.code, 0);
            QCOMPARE(reply.json["hits"].toArray().size(), 1);
            QCOMPARE(reply.json["hits"].toArray()[0].toObject()["start"].toInt(), 19);
            QCOMPARE(reply.json["nextOffset"].toInt(), 2);
            QVERIFY(!reply.json["more"].toBool());
        }
    }

    void missingSemanticProviderNeverFallsBackToText() {
        QTemporaryDir directory;
        const auto path = directory.filePath("source.txt");
        const auto state = directory.filePath("state");
        QVERIFY(writeSourceFile(path, "target target\n", false).isEmpty());
        for (const auto &command : QStringList{"definition", "references"}) {
            const auto reply =
                run({command, "--file", path, "--byte-offset", "0", "--semantic"}, state);
            QCOMPARE(reply.code, 1);
            QCOMPARE(reply.json["status"].toString(), "error");
            QVERIFY(!reply.json["error"].toString().isEmpty());
            QVERIFY(!reply.json.contains("hits"));
            QVERIFY(!reply.json.contains("locations"));
            QVERIFY(!reply.json.contains("symbols"));
        }
    }

    void interruptCancelsOwnedWork() {
#ifdef Q_OS_UNIX
        QTemporaryDir directory;
        const auto root = directory.filePath("project");
        const auto state = directory.filePath("state");
        QVERIFY(QDir().mkpath(root));
        QByteArray source;
        while (source.size() < 512 * 1024)
            source += "def target():\n    return target()\n";
        for (int i = 0; i < 32; ++i)
            QVERIFY(writeSourceFile(QDir(root).filePath(QString::number(i) + ".py"), source, false)
                        .isEmpty());
        QProcess process;
        configure(process);
        process.start(QString::fromUtf8(CODECAIRN_CLI_PATH),
                      {"index", "--root", root, "--state-dir", state, "--json"});
        QVERIFY(process.waitForStarted());
        QTRY_VERIFY_WITH_TIMEOUT(
            QDir(state).exists("workspaces") || process.state() == QProcess::NotRunning, 10000);
        QCOMPARE(process.state(), QProcess::Running);
        QCOMPARE(::kill(static_cast<pid_t>(process.processId()), SIGINT), 0);
        QVERIFY(process.waitForFinished(30000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 130);
        const auto reply = QJsonDocument::fromJson(process.readAllStandardOutput()).object();
        QCOMPARE(reply["status"].toString(), "cancelled");
        QCOMPARE(reply["schemaVersion"].toInt(), 1);
        QCOMPARE(readSourceFile(QDir(root).filePath("0.py")).bytes, source);
#else
        QSKIP("Console Ctrl+C delivery requires an attached Windows console test harness.");
#endif
    }

    void semanticDefinitionAndReferences() {
        if (QStandardPaths::findExecutable("clangd").isEmpty())
            QSKIP("clangd is unavailable.");
        QTemporaryDir directory;
        const auto root = directory.filePath("project");
        QVERIFY(QDir().mkpath(root));
        const auto state = directory.filePath("state");
        const auto path = QDir(root).filePath("source.c");
        const QByteArray source =
            "int target(void) { return 1; }\nint caller(void) { return target(); }\n";
        QVERIFY(writeSourceFile(path, source, false).isEmpty());
        const auto position = QString::number(source.lastIndexOf("target"));
        auto reply = run(
            {"definition", "--root", root, "--file", path, "--byte-offset", position, "--semantic"},
            state);
        QCOMPARE(reply.code, 0);
        QCOMPARE(reply.json["provenance"].toString(), "semantic");
        const auto definitions = reply.json["locations"].toArray();
        QCOMPARE(definitions.size(), 1);
        QCOMPARE(definitions[0].toObject()["start"].toInt(), 4);
        reply = run(
            {"references", "--root", root, "--file", path, "--byte-offset", position, "--semantic"},
            state);
        QCOMPARE(reply.code, 0);
        QCOMPARE(reply.json["hits"].toArray().size(), 2);
        const auto buffer = directory.filePath("buffer.txt");
        const QByteArray unsaved =
            "// unsaved\nint changed(void) { return 1; }\nint caller(void) { return changed(); }\n";
        QVERIFY(writeSourceFile(buffer, unsaved, true).isEmpty());
        const auto unsavedPosition = QString::number(unsaved.lastIndexOf("changed"));
        reply = run({"definition", "--root", root, "--file", path, "--byte-offset", unsavedPosition,
                     "--semantic", "--overlay", path + '=' + buffer},
                    state);
        QCOMPARE(reply.code, 0);
        const auto overlayDefinitions = reply.json["locations"].toArray();
        QCOMPARE(overlayDefinitions.size(), 1);
        QCOMPARE(overlayDefinitions[0].toObject()["start"].toInt(),
                 static_cast<int>(unsaved.indexOf("changed")));
        QCOMPARE(overlayDefinitions[0].toObject()["hash"].toString(),
                 QString::fromLatin1(sourceContentHash(unsaved)));
        QCOMPARE(readSourceFile(path).bytes, source);
    }

    void timeoutIsAnExplicitError() {
        QTemporaryDir directory;
        const auto path = directory.filePath("large.txt");
        const QByteArray source(MaximumDocumentBytes, 'x');
        QVERIFY(writeSourceFile(path, source, false).isEmpty());
        const auto reply = run({"analyze", path, "--timeout", "1"}, directory.filePath("state"));
        QCOMPARE(reply.code, 124);
        QCOMPARE(reply.json["status"].toString(), "error");
        QVERIFY(reply.json["error"].toString().contains("timed out"));
        QCOMPARE(readSourceFile(path).bytes, source);
    }
};

QTEST_GUILESS_MAIN(CliTests)
#include "CliTests.moc"

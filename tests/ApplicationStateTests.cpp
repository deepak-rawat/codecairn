#include "ApplicationStateStore.h"
#include "WorkspacePaths.h"
#include "TestStorage.h"
#include <QDir>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

namespace {
void write(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size())
        qFatal("Could not write state fixture: %s", qPrintable(file.errorString()));
}

QByteArray read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        qFatal("Could not read state fixture: %s", qPrintable(file.errorString()));
    return file.readAll();
}

WorkspaceSession sessionFor(const QString &root) {
    WorkspaceSession session;
    session.files = {{QDir(root).filePath("second.cpp"), 4, 12, 3, {}},
                     {QDir(root).filePath("first.cpp"), 2, 8, 1, {}}};
    session.activeFile = session.files.first().path;
    return session;
}
}

class ApplicationStateTests : public QObject {
    Q_OBJECT
    TestStorage storage_;
private slots:
    void defaultPathsHaveOneApplicationComponent();
    void missingFilesDoNotCreateState();
    void stateRoundTripUsesVersionedJsonAndBase64();
    void sessionsAreOrderedAndWorkspaceSpecific();
    void missingFilesUseCanonicalParent();
    void invalidState_data();
    void invalidState();
    void invalidSession_data();
    void invalidSession();
    void oversizedFilesArePreserved();
    void invalidSavesAreRejected();
    void staleStateWritersCannotOverwriteChanges();
    void staleSessionWritersAreIndependentPerWorkspace();
    void concurrentLocksAreReportedAndRetryable();
    void unwritableDirectoriesAreReported();
};

void ApplicationStateTests::defaultPathsHaveOneApplicationComponent() {
    const auto previousName = QCoreApplication::applicationName();
    const auto restore = qScopeGuard([&] { QCoreApplication::setApplicationName(previousName); });
    QCoreApplication::setApplicationName("CodeCairn");
    QVERIFY(QCoreApplication::organizationName().isEmpty());
    const auto settings = QDir::fromNativeSeparators(defaultSettingsPath());
    const auto state = QDir::fromNativeSeparators(defaultStateDirectory());
    QVERIFY(settings.endsWith("/CodeCairn/settings.json"));
    QVERIFY(!settings.contains("/CodeCairn/CodeCairn/"));
#if QT_VERSION < QT_VERSION_CHECK(6, 7, 0)
    QVERIFY(state.endsWith("/CodeCairn/state"));
#elif defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    QVERIFY(state.endsWith("/CodeCairn/State"));
#else
    QVERIFY(state.endsWith("/CodeCairn"));
#endif
    QTemporaryDir source;
    const auto hash =
        QCryptographicHash::hash(workspacePath(source.path()).toUtf8(), QCryptographicHash::Sha256)
            .toHex();
    ApplicationStateStore store(state);
    QCOMPARE(store.sessionPath(source.path()),
             QDir(state).filePath("workspaces/" + QString::fromLatin1(hash) + "/session.json"));
}

void ApplicationStateTests::missingFilesDoNotCreateState() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("state");
    ApplicationStateStore store(path);
    const auto state = store.loadState();
    QVERIFY2(state.state.has_value(), qPrintable(state.error));
    QVERIFY(state.state->geometry.isEmpty());
    QVERIFY(state.state->lastWorkspace.isEmpty());
    QCOMPARE(state.state->browserTab, 0);
    const auto session = store.loadSession(directory.path());
    QVERIFY2(session.session.has_value(), qPrintable(session.error));
    QVERIFY(session.session->files.isEmpty());
    QVERIFY(!QFileInfo::exists(path));
}

void ApplicationStateTests::stateRoundTripUsesVersionedJsonAndBase64() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ApplicationStateStore store(directory.path());
    QVERIFY(store.loadState().state.has_value());
    ApplicationState state;
    state.geometry = QByteArray::fromHex("00ff10a50100");
    state.mainSplitter = QByteArray::fromHex("ffff000102");
    state.rightSplitter = QByteArray::fromHex("0102030000");
    state.browserTab = 3;
    state.lastWorkspace = directory.path();
    QVERIFY(store.saveState(state).isEmpty());
    const auto object = QJsonDocument::fromJson(read(store.statePath())).object();
    QCOMPARE(object.value("schemaVersion").toInt(), 1);
    QCOMPARE(object.value("window").toObject().value("geometry").toString(),
             QString::fromLatin1(state.geometry.toBase64()));
    ApplicationStateStore restored(directory.path());
    const auto loaded = restored.loadState();
    QVERIFY2(loaded.state.has_value(), qPrintable(loaded.error));
    QCOMPARE(loaded.state->geometry, state.geometry);
    QCOMPARE(loaded.state->mainSplitter, state.mainSplitter);
    QCOMPARE(loaded.state->rightSplitter, state.rightSplitter);
    QCOMPARE(loaded.state->browserTab, state.browserTab);
    QCOMPARE(loaded.state->lastWorkspace, state.lastWorkspace);
}

void ApplicationStateTests::sessionsAreOrderedAndWorkspaceSpecific() {
    QTemporaryDir directory, other;
    QVERIFY(directory.isValid());
    QVERIFY(other.isValid());
    ApplicationStateStore store(directory.filePath("state"));
    QVERIFY(store.loadSession(directory.path()).session.has_value());
    const auto session = sessionFor(directory.path());
    QVERIFY(store.saveSession(directory.path(), session).isEmpty());
    const auto path = store.sessionPath(directory.path());
    QCOMPARE(path, store.sessionPath(QDir(directory.path()).filePath(".")));
    QVERIFY(path != store.sessionPath(other.path()));
    QVERIFY(pathInWorkspace(path, directory.filePath("state")));
    const auto object = QJsonDocument::fromJson(read(path)).object();
    QCOMPARE(object.value("schemaVersion").toInt(), 1);
    QCOMPARE(object.value("workspaceRoot").toString(), workspacePath(directory.path()));
    ApplicationStateStore restored(directory.filePath("state"));
    const auto loaded = restored.loadSession(directory.path());
    QVERIFY2(loaded.session.has_value(), qPrintable(loaded.error));
    QCOMPARE(loaded.session->files, session.files);
    QCOMPARE(loaded.session->activeFile, session.activeFile);
    const auto empty = restored.loadSession(other.path());
    QVERIFY(empty.session.has_value());
    QVERIFY(empty.session->files.isEmpty());
    QVERIFY(!QFileInfo::exists(restored.sessionPath(other.path())));
}

void ApplicationStateTests::missingFilesUseCanonicalParent() {
#ifdef Q_OS_WIN
    QSKIP("Directory symlink fixture requires Unix symlink semantics.");
#else
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto root = directory.filePath("source");
    const auto alias = directory.filePath("alias");
    QVERIFY(QDir().mkpath(root));
    QVERIFY(QFile::link(root, alias));
    const auto missing = QDir(alias).filePath("nested/missing.cpp");
    QCOMPARE(workspacePath(missing), QDir(workspacePath(root)).filePath("nested/missing.cpp"));
    QVERIFY(pathInWorkspace(missing, root));
    QVERIFY(!pathInWorkspace(directory.filePath("outside.cpp"), root));
    const auto outside = directory.filePath("outside");
    QVERIFY(QDir().mkpath(outside));
    QVERIFY(QFile::link(outside, QDir(root).filePath("escape")));
    QVERIFY(!pathInWorkspace(QDir(root).filePath("escape/missing.cpp"), root));
    ApplicationStateStore store(directory.filePath("state"));
    QVERIFY(store.loadSession(root).session.has_value());
    WorkspaceSession session;
    session.files = {{missing, 0, 0, 0, {}}};
    session.activeFile = missing;
    const auto error = store.saveSession(alias, session);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(store.loadSession(root).session.has_value());
    QVERIFY(QDir().mkpath(QDir(root).filePath("nested")));
    write(missing, "int value;\n");
    QCOMPARE(workspacePath(missing), QDir(workspacePath(root)).filePath("nested/missing.cpp"));
#endif
}

void ApplicationStateTests::invalidState_data() {
    QTest::addColumn<QByteArray>("json");
    QTest::newRow("malformed") << QByteArray("{");
    QTest::newRow("array") << QByteArray("[]");
    QTest::newRow("missing-version") << QByteArray("{}");
    QTest::newRow("future-version") << QByteArray(R"({"schemaVersion":2})");
    QTest::newRow("string-version") << QByteArray(R"({"schemaVersion":"1"})");
    QTest::newRow("fractional-version") << QByteArray(R"({"schemaVersion":1.5})");
    QTest::newRow("unknown-field") << QByteArray(R"({"schemaVersion":1,"unknown":true})");
    QTest::newRow("window-type") << QByteArray(R"({"schemaVersion":1,"window":[]})");
    QTest::newRow("unknown-window-field")
        << QByteArray(R"({"schemaVersion":1,"window":{"unknown":true}})");
    QTest::newRow("bad-base64") << QByteArray(R"({"schemaVersion":1,"window":{"geometry":"!!"}})");
    QTest::newRow("unpadded-base64")
        << QByteArray(R"({"schemaVersion":1,"window":{"mainSplitter":"YQ"}})");
    QTest::newRow("binary-type") << QByteArray(
        R"({"schemaVersion":1,"window":{"rightSplitter":3}})");
    QTest::newRow("negative-tab") << QByteArray(
        R"({"schemaVersion":1,"window":{"browserTab":-1}})");
    QTest::newRow("invalid-tab") << QByteArray(R"({"schemaVersion":1,"window":{"browserTab":4}})");
    QTest::newRow("fractional-tab")
        << QByteArray(R"({"schemaVersion":1,"window":{"browserTab":1.5}})");
    QTest::newRow("workspace-type") << QByteArray(R"({"schemaVersion":1,"lastWorkspace":false})");
    QTest::newRow("relative-workspace")
        << QByteArray(R"({"schemaVersion":1,"lastWorkspace":"relative"})");
}

void ApplicationStateTests::invalidState() {
    QFETCH(QByteArray, json);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ApplicationStateStore store(directory.path());
    write(store.statePath(), json);
    const auto loaded = store.loadState();
    QVERIFY(!loaded.state);
    QVERIFY(!loaded.error.isEmpty());
    QVERIFY(!store.saveState({}).isEmpty());
    QCOMPARE(read(store.statePath()), json);
}

void ApplicationStateTests::invalidSession_data() {
    QTest::addColumn<QByteArray>("overrideJson");
    QTest::newRow("future-version") << QByteArray(R"({"schemaVersion":2})");
    QTest::newRow("unknown-field") << QByteArray(R"({"unknown":true})");
    QTest::newRow("root-mismatch") << QByteArray(R"({"workspaceRoot":"$OUTSIDE"})");
    QTest::newRow("relative-root") << QByteArray(R"({"workspaceRoot":"relative"})");
    QTest::newRow("files-type") << QByteArray(R"({"files":{}})");
    QTest::newRow("file-type") << QByteArray(R"({"files":[1]})");
    QTest::newRow("empty-path") << QByteArray(R"({"files":[{"path":""}]})");
    QTest::newRow("relative-path") << QByteArray(R"({"files":[{"path":"source.cpp"}]})");
    QTest::newRow("outside-path") << QByteArray(R"({"files":[{"path":"$OUTSIDE"}]})");
    QTest::newRow("duplicate-path")
        << QByteArray(R"({"files":[{"path":"$FILE"},{"path":"$FILE"}]})");
    QTest::newRow("reversed-selection")
        << QByteArray(R"({"files":[{"path":"$FILE","start":8,"end":3}]})");
    QTest::newRow("negative-offset") << QByteArray(R"({"files":[{"path":"$FILE","start":-1}]})");
    QTest::newRow("oversized-offset")
        << QByteArray(R"({"files":[{"path":"$FILE","end":2147483648}]})");
    QTest::newRow("fractional-offset") << QByteArray(R"({"files":[{"path":"$FILE","end":1.5}]})");
    QTest::newRow("negative-line") << QByteArray(R"({"files":[{"path":"$FILE","firstLine":-1}]})");
    QTest::newRow("unknown-file-field")
        << QByteArray(R"({"files":[{"path":"$FILE","unknown":true}]})");
    QTest::newRow("unknown-active-file") << QByteArray(R"({"activeFile":"$FILE"})");
    QTest::newRow("active-file-type") << QByteArray(R"({"activeFile":3})");
}

void ApplicationStateTests::invalidSession() {
    QFETCH(QByteArray, overrideJson);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ApplicationStateStore store(directory.filePath("state"));
    QVERIFY(QDir().mkpath(QFileInfo(store.sessionPath(directory.path())).absolutePath()));
    const auto file = directory.filePath("source.cpp");
    const auto outside = QFileInfo(directory.path()).dir().filePath("outside");
    overrideJson.replace("$FILE", file.toUtf8()).replace("$OUTSIDE", outside.toUtf8());
    QJsonObject object{{"schemaVersion", 1},
                       {"workspaceRoot", directory.path()},
                       {"files", QJsonArray{}},
                       {"activeFile", ""}};
    const auto overrides = QJsonDocument::fromJson(overrideJson).object();
    QVERIFY(!overrides.isEmpty());
    for (auto it = overrides.begin(); it != overrides.end(); ++it)
        object.insert(it.key(), it.value());
    const auto bytes = QJsonDocument(object).toJson();
    write(store.sessionPath(directory.path()), bytes);
    const auto loaded = store.loadSession(directory.path());
    QVERIFY(!loaded.session);
    QVERIFY(!loaded.error.isEmpty());
    QVERIFY(!store.saveSession(directory.path(), {}).isEmpty());
    QCOMPARE(read(store.sessionPath(directory.path())), bytes);
}

void ApplicationStateTests::oversizedFilesArePreserved() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ApplicationStateStore store(directory.path());
    const QByteArray bytes(4 * 1024 * 1024 + 1, ' ');
    write(store.statePath(), bytes);
    const auto loaded = store.loadState();
    QVERIFY(!loaded.state);
    QVERIFY(loaded.error.contains("4 MiB"));
    QVERIFY(!store.saveState({}).isEmpty());
    QCOMPARE(read(store.statePath()), bytes);
}

void ApplicationStateTests::invalidSavesAreRejected() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ApplicationStateStore store(directory.path());
    QVERIFY(!store.saveState({}).isEmpty());
    QVERIFY(!store.saveSession(directory.path(), {}).isEmpty());
    QVERIFY(store.loadState().state.has_value());
    ApplicationState state;
    state.browserTab = 4;
    QVERIFY(!store.saveState(state).isEmpty());
    state.browserTab = 0;
    state.geometry = QByteArray(4 * 1024 * 1024, 'x');
    QVERIFY(store.saveState(state).contains("4 MiB"));
    QVERIFY(!QFileInfo::exists(store.statePath()));
    QVERIFY(!store.loadSession("relative").session);
    QVERIFY(!store.saveSession("relative", {}).isEmpty());
    QVERIFY(store.loadSession(directory.path()).session.has_value());
    auto session = sessionFor(directory.path());
    session.files.first().end = -1;
    QVERIFY(!store.saveSession(directory.path(), session).isEmpty());
    QVERIFY(!QFileInfo::exists(store.sessionPath(directory.path())));
}

void ApplicationStateTests::staleStateWritersCannotOverwriteChanges() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ApplicationStateStore first(directory.path()), second(directory.path());
    QVERIFY(first.loadState().state.has_value());
    QVERIFY(second.loadState().state.has_value());
    ApplicationState state;
    state.browserTab = 1;
    QVERIFY(first.saveState(state).isEmpty());
    const auto before = read(first.statePath());
    state.browserTab = 2;
    QVERIFY(second.saveState(state).contains("changed externally"));
    QCOMPARE(read(first.statePath()), before);
    QVERIFY(second.loadState().state.has_value());
    QVERIFY(second.saveState(state).isEmpty());
    const auto changed = read(first.statePath());
    state.browserTab = 3;
    QVERIFY(first.saveState(state).contains("changed externally"));
    QCOMPARE(read(first.statePath()), changed);
    QVERIFY(first.loadState().state.has_value());
    QVERIFY(QFile::remove(first.statePath()));
    QVERIFY(first.saveState(state).contains("changed externally"));
    QVERIFY(!QFileInfo::exists(first.statePath()));
}

void ApplicationStateTests::staleSessionWritersAreIndependentPerWorkspace() {
    QTemporaryDir directory, other;
    QVERIFY(directory.isValid());
    QVERIFY(other.isValid());
    ApplicationStateStore first(directory.filePath("state")), second(directory.filePath("state"));
    QVERIFY(first.loadSession(directory.path()).session.has_value());
    QVERIFY(second.loadSession(directory.path()).session.has_value());
    const auto session = sessionFor(directory.path());
    QVERIFY(first.saveSession(directory.path(), session).isEmpty());
    const auto before = read(first.sessionPath(directory.path()));
    QVERIFY(second.saveSession(directory.path(), {}).contains("changed externally"));
    QCOMPARE(read(first.sessionPath(directory.path())), before);
    QVERIFY(second.loadSession(other.path()).session.has_value());
    QVERIFY(second.saveSession(other.path(), sessionFor(other.path())).isEmpty());
    QCOMPARE(read(first.sessionPath(directory.path())), before);
    QVERIFY(second.loadSession(directory.path()).session.has_value());
    QVERIFY(second.saveSession(directory.path(), {}).isEmpty());
}

void ApplicationStateTests::concurrentLocksAreReportedAndRetryable() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ApplicationStateStore store(directory.path());
    QVERIFY(store.loadState().state.has_value());
    QLockFile lock(store.statePath() + ".lock");
    QVERIFY(lock.tryLock(0));
    QVERIFY(store.saveState({}).contains("another process"));
    QVERIFY(!QFileInfo::exists(store.statePath()));
    lock.unlock();
    QVERIFY(store.saveState({}).isEmpty());
    QVERIFY(store.loadSession(directory.path()).session.has_value());
    const auto path = store.sessionPath(directory.path());
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QLockFile sessionLock(path + ".lock");
    QVERIFY(sessionLock.tryLock(0));
    QVERIFY(store.saveSession(directory.path(), {}).contains("another process"));
    sessionLock.unlock();
    QVERIFY(store.saveSession(directory.path(), {}).isEmpty());
}

void ApplicationStateTests::unwritableDirectoriesAreReported() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("not-a-directory");
    write(path, "original");
    ApplicationStateStore store(path);
    QVERIFY(store.loadState().state.has_value());
    QVERIFY(store.saveState({}).contains("cannot create"));
    QCOMPARE(read(path), QByteArray("original"));
    ApplicationStateStore unavailable(QString{});
    QVERIFY(!unavailable.loadState().state);
    QVERIFY(!unavailable.saveState({}).isEmpty());
}

QTEST_GUILESS_MAIN(ApplicationStateTests)
#include "ApplicationStateTests.moc"

#include "WorkspaceIndex.h"
#include "SymbolIndexStore.h"
#include "TestStorage.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTest>
#include <QUuid>
#include <sqlite3.h>
#include <cstring>

using namespace WorkspaceIndexDetail;

namespace {
class WorkspaceFixture {
public:
    WorkspaceFixture() {
        root = QDir::current().absoluteFilePath("index-parity-" +
                                                QUuid::createUuid().toString(QUuid::WithoutBraces));
        if (!QDir().mkpath(root) || !QDir().mkpath(workspaceStorageDirectory(root)))
            qFatal("Cannot create index parity fixture");
        root = workspacePath(root);
    }
    ~WorkspaceFixture() { QDir(root).removeRecursively(); }
    QString path(const QString &name) const { return QDir(root).filePath(name); }
    QString root;
};

SourceSymbol symbol(const QString &name, const QString &kind, int start) {
    SourceSymbol result;
    result.name = name;
    result.kind = kind;
    result.start = start;
    result.end = start + 17;
    result.nameStart = start + 2;
    result.nameEnd = start + 6;
    result.line = start + 1;
    result.qualifiedName = "scope::" + name;
    result.signature = name + "(int)";
    result.definition = true;
    result.category = kind;
    result.generated = true;
    result.provider = "compiler";
    return result;
}

void persist(SymbolIndexStore &database, const QString &path,
             const QVector<SourceSymbol> &symbols) {
    IndexStatement file(database,
                        "INSERT INTO files VALUES(?,'diskhash',0,0,'indexed','','parser')");
    file.bind(1, path);
    file.next();
    IndexStatement row(database, "INSERT INTO symbols VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    for (const auto &s : symbols) {
        row.bind(1, path);
        row.bind(2, s.name);
        row.bind(3, s.kind);
        row.bind(4, s.qualifiedName);
        row.bind(5, s.signature);
        row.bind(6, s.definition ? 1 : 0);
        row.bind(7, s.start);
        row.bind(8, s.end);
        row.bind(9, s.nameStart);
        row.bind(10, s.nameEnd);
        row.bind(11, s.line);
        row.bind(12, s.category);
        row.bind(13, s.generated ? 1 : 0);
        row.bind(14, s.provider);
        row.next();
        row.reset();
    }
}

void compareSymbol(const IndexedSymbol &actual, const IndexedSymbol &expected) {
    QCOMPARE(actual.path, expected.path);
    QCOMPARE(actual.hash, expected.hash);
    QCOMPARE(actual.live, expected.live);
    const auto &a = actual.symbol;
    const auto &e = expected.symbol;
    QCOMPARE(a.name, e.name);
    QCOMPARE(a.kind, e.kind);
    QCOMPARE(a.start, e.start);
    QCOMPARE(a.end, e.end);
    QCOMPARE(a.nameStart, e.nameStart);
    QCOMPARE(a.nameEnd, e.nameEnd);
    QCOMPARE(a.line, e.line);
    QCOMPARE(a.parent, e.parent);
    QCOMPARE(a.outlineOnly, e.outlineOnly);
    QCOMPARE(a.qualifiedName, e.qualifiedName);
    QCOMPARE(a.signature, e.signature);
    QCOMPARE(a.definition, e.definition);
    QCOMPARE(a.category, e.category);
    QCOMPARE(a.local, e.local);
    QCOMPARE(a.generated, e.generated);
    QCOMPARE(a.provider, e.provider);
}

std::atomic_bool *cancelOnWrite = nullptr;

int cancelFileWrite(unsigned, void *, void *statement, void *) {
    const auto *sql = sqlite3_sql(static_cast<sqlite3_stmt *>(statement));
    if (cancelOnWrite && sql && std::strncmp(sql, "INSERT INTO files", 17) == 0)
        cancelOnWrite->store(true);
    return 0;
}

int traceNewConnection(sqlite3 *database, char **, const sqlite3_api_routines *) {
    return sqlite3_trace_v2(database, SQLITE_TRACE_STMT, cancelFileWrite, nullptr);
}

class CancelDuringWrite {
public:
    explicit CancelDuringWrite(std::atomic_bool &cancel) {
        cancelOnWrite = &cancel;
        if (sqlite3_auto_extension(reinterpret_cast<void (*)()>(traceNewConnection)) != SQLITE_OK)
            qFatal("Cannot install cancellation fixture");
    }
    ~CancelDuringWrite() {
        sqlite3_cancel_auto_extension(reinterpret_cast<void (*)()>(traceNewConnection));
        cancelOnWrite = nullptr;
    }
};
}

class IndexParityTests : public QObject {
    Q_OBJECT
    TestStorage storage_;
private slots:
    void fieldsOrderingFiltersAndOverlays();
    void cancellationRollsBackWrites();
};

void IndexParityTests::fieldsOrderingFiltersAndOverlays() {
    WorkspaceFixture fixture;
    const auto a = fixture.path("a.cpp");
    const auto m = fixture.path("m.cpp");
    const auto z = fixture.path("z.cpp");
    const auto first = symbol("Same", "Function", 10);
    const auto second = symbol("Same", "Function", 30);
    const auto variable = symbol("Zed", "Variable", 70);
    {
        SymbolIndexStore database(workspaceDatabase(fixture.root), true);
        database.ensureSchema();
        persist(database, a, {second, first});
        persist(database, m, {symbol("Same", "Function", 20)});
        persist(database, z, {variable});
        IndexStatement version(database, "PRAGMA user_version");
        QVERIFY(version.next());
        QCOMPARE(version.integer(0), 2);
    }
    auto early = symbol("Same", "Function", 5);
    early.parent = 6;
    auto late = symbol("Same", "Function", 40);
    auto local = symbol("HiddenLocal", "Variable", 50);
    local.local = true;
    auto outline = symbol("HiddenOutline", "Type", 60);
    outline.outlineOnly = true;
    SymbolOverlay overlay;
    overlay.path = m;
    overlay.source = "unsaved source";
    overlay.hash = "providedhash";
    overlay.analysis.symbols = {late, local, early, outline};
    SymbolQuery query;
    query.overlays = {overlay};
    query.includeCategorySymbols = true;
    query.limit = 500000;
    const QVector<IndexedSymbol> expected{{m, "providedhash", early, true},
                                          {m, "providedhash", late, true},
                                          {a, "diskhash", first, false},
                                          {a, "diskhash", second, false},
                                          {z, "diskhash", variable, false}};
    const QMap<QString, int> categories{{"Functions", 4}, {"Variables", 1}};
    auto page = queryWorkspaceSymbols(fixture.root, query);
    QVERIFY2(page.error.isEmpty(), qPrintable(page.error));
    QVERIFY(!page.cancelled);
    QCOMPARE(page.total, 5);
    QCOMPARE(page.symbols.size(), expected.size());
    QCOMPARE(page.categorySymbols.size(), expected.size());
    QCOMPARE(page.categories, categories);
    for (int i = 0; i < expected.size(); ++i) {
        compareSymbol(page.symbols[i], expected[i]);
        compareSymbol(page.categorySymbols[i], expected[i]);
    }
    query.overlays[0].hash.clear();
    page = queryWorkspaceSymbols(fixture.root, query);
    QCOMPARE(page.symbols.first().hash,
             QCryptographicHash::hash(overlay.source, QCryptographicHash::Sha256).toHex());
    query.overlays[0].hash = overlay.hash;
    query.offset = 1;
    query.limit = 1;
    page = queryWorkspaceSymbols(fixture.root, query);
    QCOMPARE(page.total, 5);
    QCOMPARE(page.symbols.size(), 1);
    compareSymbol(page.symbols.first(), expected[1]);
    QCOMPARE(page.categorySymbols.size(), 5);
    query.text = "missing";
    query.category = "Variables";
    page = queryWorkspaceSymbols(fixture.root, query);
    QCOMPARE(page.total, 0);
    QVERIFY(page.symbols.isEmpty());
    QCOMPARE(page.categories, categories);
    QCOMPARE(page.categorySymbols.size(), 5);
    query.category.clear();
    query.offset = 0;
    query.limit = 0;
    query.text = "scope::Same";
    query.exact = true;
    page = queryWorkspaceSymbols(fixture.root, query);
    QCOMPARE(page.total, 4);
    QVERIFY(page.symbols.isEmpty());
    QCOMPARE(page.categories, categories);
    query.text = "same";
    QCOMPARE(queryWorkspaceSymbols(fixture.root, query).total, 0);
    query.text = "Same";
    QCOMPARE(queryWorkspaceSymbols(fixture.root, query).total, 4);
    query.exact = false;
    query.text = "SCOPE same(";
    QCOMPARE(queryWorkspaceSymbols(fixture.root, query).total, 4);
    query.text = "?^scope::Same$";
    QCOMPARE(queryWorkspaceSymbols(fixture.root, query).total, 4);
    query.text = "?[";
    page = queryWorkspaceSymbols(fixture.root, query);
    QVERIFY(!page.error.isEmpty());
    QCOMPARE(page.total, 0);
    QVERIFY(page.symbols.isEmpty());
    QVERIFY(page.categories.isEmpty());
    QVERIFY(page.categorySymbols.isEmpty());
    query.text.clear();
    SymbolOverlay empty;
    empty.path = a;
    query.overlays.append(empty);
    page = queryWorkspaceSymbols(fixture.root, query);
    QCOMPARE(page.total, 3);
    QCOMPARE(page.categories.value("Functions"), 2);
    QCOMPARE(page.categories.value("Variables"), 1);
    query.cancel = std::make_shared<std::atomic_bool>(true);
    page = queryWorkspaceSymbols(fixture.root, query);
    QVERIFY(page.cancelled);
    QVERIFY(page.error.isEmpty());
    QVERIFY(page.categorySymbols.isEmpty());
}

void IndexParityTests::cancellationRollsBackWrites() {
    WorkspaceFixture fixture;
    const auto path = fixture.path("a.cpp");
    const auto write = [&](const QByteArray &source) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
               file.write(source) == source.size();
    };
    QVERIFY(write("int before();\n"));
    auto cancel = std::make_shared<std::atomic_bool>(false);
    const auto initial = buildWorkspaceIndex(fixture.root, false, true, {}, cancel);
    QVERIFY2(initial.error.isEmpty(), qPrintable(initial.error));
    const auto before = queryWorkspaceSymbols(fixture.root, {});
    QCOMPARE(before.total, 1);
    QVERIFY(write("int after_cancelled();\n"));
    IndexResult cancelled;
    {
        CancelDuringWrite hook(*cancel);
        cancelled = buildWorkspaceIndex(fixture.root, true, true, {}, cancel);
    }
    QVERIFY(cancel->load());
    QVERIFY(cancelled.cancelled);
    QVERIFY2(cancelled.error.isEmpty(), qPrintable(cancelled.error));
    QCOMPARE(cancelled.parsed, 1);
    const auto after = queryWorkspaceSymbols(fixture.root, {});
    QVERIFY2(after.error.isEmpty(), qPrintable(after.error));
    QCOMPARE(after.total, before.total);
    QCOMPARE(after.categories, before.categories);
    QCOMPARE(after.symbols.size(), before.symbols.size());
    for (int i = 0; i < before.symbols.size(); ++i)
        compareSymbol(after.symbols[i], before.symbols[i]);
    SymbolIndexStore database(workspaceDatabase(fixture.root), false);
    IndexStatement row(database, "SELECT parser FROM files");
    QVERIFY(row.next());
    QCOMPARE(row.text(0), QString("codecairn-multilanguage-compiler-v11"));
}

QTEST_GUILESS_MAIN(IndexParityTests)
#include "IndexParityTests.moc"

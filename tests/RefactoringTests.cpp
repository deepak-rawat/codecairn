#include "DefinitionResolver.h"
#include "CodeEditor.h"
#include "FileIO.h"
#include "SourceDocument.h"
#include "SymbolBrowser.h"
#include "WorkspaceOptionsStore.h"
#include "ApplicationStateStore.h"
#include "TestStorage.h"
#include "ReferenceSearch.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <memory>

namespace {
IndexedSymbol candidate(const QString &name, bool definition) {
    IndexedSymbol entry;
    entry.path = "example.cpp";
    entry.symbol.name = name;
    entry.symbol.qualifiedName = name;
    entry.symbol.kind = "Function";
    entry.symbol.definition = definition;
    return entry;
}
}

class RefactoringTests : public QObject {
    Q_OBJECT
    TestStorage storage_;
private slots:
    void snapshotsAndSavedHashesHaveSeparateLifetimes();
    void latestDocumentAnalysisWins();
    void stoppingAnalysisDisconnectsCompletion();
    void workspaceOptionsPreserveMissingAndEmptyExclusions();
    void workspaceSessionPreservesFieldsInJson();
    void definitionPreferencePreservesCompleteness();
    void definitionRequestsCoalesceWithoutMixingTokens();
    void symbolBrowserPreservesAllRowsAndSelection();
    void textReferenceOverlayLookupPreservesFirstMatch();
    void cleanReloadUpdatesSavedSnapshotAndAnalysis();
    void reloadDoesNotOverwriteNewEdits();
};

void RefactoringTests::snapshotsAndSavedHashesHaveSeparateLifetimes() {
    SourceDocument document("sample.py", "def before(): pass\n", true, 10);
    std::unique_ptr<CodeEditor> editor(document.editor);
    const auto original = document.snapshotBytes();
    const auto originalHash = document.snapshotHash;
    QCOMPARE(document.savedContentHash(), originalHash);
    document.editor->loadBytes("def after(): pass\n");
    ++document.revision;
    QVERIFY(document.snapshotBytes() != original);
    QVERIFY(document.snapshotHash != originalHash);
    QCOMPARE(document.savedBytes, original);
    QCOMPARE(document.savedContentHash(), originalHash);
    document.setSavedContents(document.snapshotBytes(), false);
    QCOMPARE(document.savedContentHash(), document.snapshotHash);
    QVERIFY(!document.hasUtf8Bom);
}

void RefactoringTests::latestDocumentAnalysisWins() {
    SourceDocument document("sample.py", "def before(): pass\n", false, 10);
    std::unique_ptr<CodeEditor> editor(document.editor);
    QSignalSpy completed(&document, &SourceDocument::analysisCompleted);
    document.startAnalysis();
    document.editor->loadBytes("def after(): pass\n");
    ++document.revision;
    document.startAnalysis();
    QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 10000);
    QCOMPARE(document.analyzedRevision, document.revision);
    QCOMPARE(document.analysis.symbols.size(), 1);
    QCOMPARE(document.analysis.symbols.first().name, QString("after"));
}

void RefactoringTests::stoppingAnalysisDisconnectsCompletion() {
    SourceDocument document("sample.py", "def sample(): pass\n", false, 10);
    std::unique_ptr<CodeEditor> editor(document.editor);
    QSignalSpy completed(&document, &SourceDocument::analysisCompleted);
    document.startAnalysis();
    document.stopBackgroundWork();
    QCoreApplication::processEvents();
    QCOMPARE(completed.count(), 0);
}

void RefactoringTests::workspaceOptionsPreserveMissingAndEmptyExclusions() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    auto options = WorkspaceOptionsStore::loadOptions(directory.path());
    QVERIFY(options.error.isEmpty());
    QVERIFY(!options.excludedDirectories);
    QVERIFY(WorkspaceOptionsStore::saveOptions(directory.path(), "build", {}).isEmpty());
    options = WorkspaceOptionsStore::loadOptions(directory.path());
    QVERIFY(options.error.isEmpty());
    QCOMPARE(options.compilationDirectory, QString("build"));
    QVERIFY(options.excludedDirectories);
    QVERIFY(options.excludedDirectories->isEmpty());
    const auto configuration = workspaceOptionsPath(directory.path());
    QJsonObject object{{"schemaVersion", 1}, {"workspaceRoot", workspacePath(directory.path())},
                       {"compilationDirectory", "custom"}};
    QVERIFY(writeSourceFile(configuration, QJsonDocument(object).toJson(), false)
                .isEmpty());
    options = WorkspaceOptionsStore::loadOptions(directory.path());
    QVERIFY(!options.excludedDirectories);
    QCOMPARE(options.compilationDirectory, QString("custom"));
    QVERIFY(writeSourceFile(configuration, "{", false).isEmpty());
    options = WorkspaceOptionsStore::loadOptions(directory.path());
    QVERIFY(options.error.contains("Invalid workspace configuration JSON"));
    QVERIFY(!WorkspaceOptionsStore::saveOptions(directory.path(), {}, {}).isEmpty());
    QCOMPARE(readSourceFile(configuration).bytes, QByteArray("{"));
}

void RefactoringTests::workspaceSessionPreservesFieldsInJson() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    ApplicationStateStore store(directory.filePath("state"));
    QVERIFY(store.loadSession(directory.path()).session.has_value());
    WorkspaceSession session;
    session.files = {{directory.filePath("two.py"), 9, 13, 4, {}},
                     {directory.filePath("one.py"), 2, 7, 1, {}}};
    session.activeFile = session.files.first().path;
    QVERIFY(store.saveSession(directory.path(), session).isEmpty());
    QFile file(store.sessionPath(directory.path()));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    QCOMPARE(object.value("schemaVersion").toInt(), 1);
    const auto stored = object.value("files").toArray();
    QCOMPARE(stored.size(), 2);
    QCOMPARE(stored.first().toObject().keys(), QStringList({"end", "firstLine", "path", "start"}));
    const auto loaded = store.loadSession(directory.path());
    QVERIFY2(loaded.session.has_value(), qPrintable(loaded.error));
    QCOMPARE(loaded.session->files, session.files);
    QCOMPARE(loaded.session->activeFile, session.activeFile);
}

void RefactoringTests::definitionPreferencePreservesCompleteness() {
    SymbolPage page;
    page.symbols = {candidate("run", false), candidate("run", true)};
    page.total = 2;
    auto candidates = definitionCandidates(page);
    QCOMPARE(candidates.targets.size(), 1);
    QVERIFY(candidates.targets.first().symbol.definition);
    QVERIFY(candidates.complete);
    QCOMPARE(candidates.total, 2);
    page.total = 3;
    QVERIFY(!definitionCandidates(page).complete);
    page.symbols = {candidate("run", false)};
    QCOMPARE(definitionCandidates(page).targets.size(), 1);
    page.error = "database failure";
    QCOMPARE(definitionCandidates(page).error, page.error);
}

void RefactoringTests::definitionRequestsCoalesceWithoutMixingTokens() {
    DefinitionResolver resolver;
    QVector<quint64> tokens;
    QStringList names;
    connect(&resolver, &DefinitionResolver::candidatesReady, this,
            [&](quint64 token, const DefinitionCandidates &candidates) {
                tokens.append(token);
                names.append(candidates.targets.first().symbol.name);
            });
    const auto request = [&](const QString &name, quint64 token) {
        SymbolQuery query;
        SourceAnalysis analysis;
        analysis.symbols.append(candidate(name, true).symbol);
        query.overlays = {{"example.py", {}, analysis, "hash"}};
        resolver.request({}, query, token);
    };
    request("first", 1);
    request("superseded", 2);
    request("latest", 3);
    QTRY_COMPARE_WITH_TIMEOUT(tokens.size(), 2, 10000);
    QCOMPARE(tokens, QVector<quint64>({1, 3}));
    QCOMPARE(names, QStringList({"first", "latest"}));
}

void RefactoringTests::symbolBrowserPreservesAllRowsAndSelection() {
    QTreeWidget symbols;
    QTreeWidget categories;
    symbols.setColumnCount(2);
    categories.setColumnCount(2);
    SymbolPage page;
    for (int i = 0; i < 450; ++i) {
        auto entry = candidate(QString("function_%1").arg(i), true);
        entry.symbol.nameStart = i;
        page.symbols.append(entry);
        page.categorySymbols.append(entry);
    }
    page.total = 450;
    page.categories.insert("Functions", 450);
    SymbolBrowser::display(&symbols, &categories, page, {}, {});
    QCOMPARE(symbols.topLevelItemCount(), 450);
    QCOMPARE(categories.topLevelItem(0)->childCount(), 450);
    symbols.setCurrentItem(symbols.topLevelItem(320));
    categories.topLevelItem(0)->setExpanded(true);
    categories.setCurrentItem(categories.topLevelItem(0)->child(320));
    QSignalSpy selected(&symbols, &QTreeWidget::currentItemChanged);
    SymbolBrowser::display(&symbols, &categories, page, {}, {});
    QCOMPARE(selected.count(), 0);
    QCOMPARE(symbols.currentItem()->data(0, SymbolItemRole::Index).toInt(), 320);
    QCOMPARE(categories.currentItem()->data(0, SymbolItemRole::Index).toInt(), 320);
    QVERIFY(categories.topLevelItem(0)->isExpanded());
}

void RefactoringTests::textReferenceOverlayLookupPreservesFirstMatch() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("source.py");
    QVERIFY(writeSourceFile(path, "disk = 1\n", false).isEmpty());
    const QByteArray first = "target = 1\n";
    const QByteArray second = "target = target\n";
    const QVector<SymbolOverlay> overlays = {
        {directory.filePath("./source.py"), first, analyzeSource(first, path)},
        {path, second, analyzeSource(second, path)}};
    auto cancel = std::make_shared<std::atomic_bool>(false);
    const auto results = findTextOccurrences({path}, "target", overlays, false, false, cancel);
    QCOMPARE(results.hits.size(), 1);
    QCOMPARE(results.hits.first().location.start, 0);
    QCOMPARE(results.hits.first().location.end, 6);
    QCOMPARE(results.hits.first().location.expectedHash,
             QCryptographicHash::hash(first, QCryptographicHash::Sha256).toHex());
    QVERIFY(results.warnings.isEmpty());
    cancel->store(true);
    QVERIFY(findTextOccurrences({path}, "target", overlays, false, false, cancel).cancelled);
    QVERIFY(!findTextOccurrences({}, "target", overlays, false, false, cancel).cancelled);
}

void RefactoringTests::cleanReloadUpdatesSavedSnapshotAndAnalysis() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("source.py");
    const QByteArray original = "def before(): pass\n";
    const QByteArray replacement = "def after(): pass\n";
    QVERIFY(writeSourceFile(path, original, false).isEmpty());
    SourceDocument document(path, original, false, 10);
    std::unique_ptr<CodeEditor> editor(document.editor);
    document.snapshotBytes();
    QSignalSpy reloaded(&document, &SourceDocument::diskReloaded);
    QSignalSpy completed(&document, &SourceDocument::analysisCompleted);
    QVERIFY(writeSourceFile(path, replacement, true).isEmpty());
    const auto hash = QCryptographicHash::hash(replacement, QCryptographicHash::Sha256).toHex();
    document.reconcileDiskHash(hash);
    QTRY_COMPARE_WITH_TIMEOUT(reloaded.count(), 1, 10000);
    QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 10000);
    QCOMPARE(document.savedBytes, replacement);
    QCOMPARE(document.savedContentHash(), hash);
    QCOMPARE(document.snapshotBytes(), replacement);
    QCOMPARE(document.snapshotHash, hash);
    QVERIFY(document.hasUtf8Bom);
    QVERIFY(!editor->isModified());
    QCOMPARE(document.analysis.symbols.first().name, QString("after"));
}

void RefactoringTests::reloadDoesNotOverwriteNewEdits() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("source.py");
    const QByteArray original = "def before(): pass\n";
    const QByteArray replacement = "def after(): pass\n";
    QVERIFY(writeSourceFile(path, replacement, false).isEmpty());
    SourceDocument document(path, original, false, 10);
    std::unique_ptr<CodeEditor> editor(document.editor);
    QSignalSpy reloaded(&document, &SourceDocument::diskReloaded);
    QSignalSpy notices(&document, &SourceDocument::statusMessage);
    const auto hash = QCryptographicHash::hash(replacement, QCryptographicHash::Sha256).toHex();
    document.reconcileDiskHash(hash);
    editor->sends(SCI_APPENDTEXT, 7, "# edit\n");
    QVERIFY(editor->isModified());
    QCOMPARE(document.revision, 1);
    document.stopBackgroundWork();
    QCoreApplication::processEvents();
    QCOMPARE(reloaded.count(), 0);
    QCOMPARE(editor->bytes(), original + "# edit\n");
    QCOMPARE(document.savedBytes, original);
    document.reconcileDiskHash(hash);
    QCOMPARE(notices.count(), 1);
    document.reconcileDiskHash(hash);
    QCOMPARE(notices.count(), 1);
}

QTEST_MAIN(RefactoringTests)
#include "RefactoringTests.moc"

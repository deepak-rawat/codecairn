#include "ApplicationStateStore.h"
#include "AppSettings.h"
#include "CodeEditor.h"
#include "FileIO.h"
#include "MainWindow.h"
#include "LanguageServerClient.h"
#include "ReferenceSearch.h"
#include "NavigationTree.h"
#include "ReferenceResultsView.h"
#include "UiAppearance.h"
#include "SourceAnalysis.h"
#include "TestStorage.h"
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>
#include <QToolBar>
#include <QToolButton>
#include <QDir>
#include <QHeaderView>
#include <QClipboard>
#include <QImage>
#include <QGroupBox>
#include <QMessageBox>
#include <QTimer>
#include <QFile>
#include <QDateTime>
#include <QSpinBox>
#include <QLockFile>
#include <QMenu>
#include <QTabBar>

namespace {
void setReferenceMethod(QDialog *dialog, SearchMethod method) {
    auto *combo = dialog->findChild<QComboBox *>("referenceMethod");
    QVERIFY(combo);
    const int index = combo->findData(static_cast<int>(method));
    QVERIFY(index >= 0);
    combo->setCurrentIndex(index);
}

QByteArray fingerprint(const QByteArray &source) {
    return QCryptographicHash::hash(source, QCryptographicHash::Sha256).toHex();
}
ReferenceSearchRequest request(const QString &path, const QString &query) {
    ReferenceSearchRequest r;
    r.query = query;
    r.paths = {path};
    r.options.method = SearchMethod::Literal;
    r.options.wholeWords = false;
    r.options.comments = CommentSearch::Include;
    r.options.strings = true;
    return r;
}
ReferenceResults run(const ReferenceSearchRequest &r) {
    return searchReferences(r, std::make_shared<std::atomic_bool>(false));
}
}

class ReferenceTests : public QObject {
    Q_OBJECT
    TestStorage storage_;
private slots:
    void literalAndWholeWord();
    void regexAndInvalidExpressions();
    void commentsStringsAndScopes();
    void keywordExpressions();
    void keywordVariationsAndFragments();
    void dirtyOverlaysAndCancellation();
    void pagingIsExact();
    void inactiveRegions();
    void editTrackingAndStaleTargets();
    void semanticAliasesAndPaging();
    void historyRoundTripAndValidation();
    void sessionHistoryRoundTrip();
    void legacySettingsAndIndependentState();
    void touchDoesNotChangeContents();
    void editableDialogAndIndependentProjectSearch();
    void uiTracksEditsAndRemovesResults();
    void outputOptionsAndExport();
    void indexedSearchIncludesDirtyOverlays();
    void realSemanticOverloadsAndMembers();
    void continuationRejectsChangedSources();
    void workspaceRestoresResultRows();
    void clickableReferenceIconsAndToolbar();
    void closingResultsClearsLinksAndStartsFresh();
    void searchedSymbolsPreviewAndDelete();
    void resultsTextCaretAndHover();
    void referenceIconsHaveLogicalPixelSize();
    void lookupDialogLayoutAndOptions();
    void dialogTouchRequiresConfirmation();
    void closedResultsStayClearedAfterFailedSearch();
    void replacementCommitsAndPersists();
    void incompleteReplacementKeepsHistory();
    void replacementSaveFailureKeepsHistory();
    void closeResultsWithOpenMenuAndSearchAgain();
    void closingPendingSearchIgnoresLateResults();
    void searchDialogSurvivesResultsClose();
    void closingResultsCancelsTouchConfirmation();
};

void ReferenceTests::literalAndWholeWord() {
    QTemporaryDir dir;
    const auto path = dir.filePath("a.cpp");
    QVERIFY(writeSourceFile(path, "count Count counter\n", false).isEmpty());
    auto r = request(path, "count");
    QCOMPARE(run(r).hits.size(), 2);
    r.options.caseSensitive = false;
    QCOMPARE(run(r).hits.size(), 3);
    r.options.wholeWords = true;
    const auto result = run(r);
    QCOMPARE(result.hits.size(), 2);
    QCOMPARE(result.hits[1].location.start, 6);
    QCOMPARE(result.hits[1].location.end, 11);
    r.options.caseSensitive = true;
    QCOMPARE(run(r).hits.size(), 1);
    r.options.method = SearchMethod::Occurrences;
    r.options.wholeWords = false;
    QCOMPARE(run(r).hits.size(), 1);
}

void ReferenceTests::regexAndInvalidExpressions() {
    QTemporaryDir dir;
    const auto path = dir.filePath("a.txt");
    QVERIFY(writeSourceFile(path, "item1 item22\n", false).isEmpty());
    auto r = request(path, "item[0-9]+");
    r.options.method = SearchMethod::RegularExpression;
    QCOMPARE(run(r).hits.size(), 2);
    QVERIFY(!validateReferenceSearch("[", r.options).isEmpty());
    QVERIFY(!validateReferenceSearch("(?=item)", r.options).isEmpty());
    r.query = "^";
    QCOMPARE(run(r).hits.size(), 2);
    r.query = "[";
    QVERIFY(!run(r).error.isEmpty());
    QVERIFY(!validateReferenceSearch({}, r.options).isEmpty());
}

void ReferenceTests::commentsStringsAndScopes() {
    QTemporaryDir dir;
    const auto path = dir.filePath("a.cpp");
    QVERIFY(writeSourceFile(path, "int count;\n// count\nconst char *s = \"count\";\n", false)
                .isEmpty());
    auto r = request(path, "count");
    r.options.comments = CommentSearch::Only;
    QCOMPARE(run(r).hits.size(), 1);
    QCOMPARE(run(r).hits.first().line, 1);
    r.options.comments = CommentSearch::Exclude;
    r.options.strings = false;
    QCOMPARE(run(r).hits.size(), 1);
    r.options.comments = CommentSearch::Include;
    QCOMPARE(run(r).hits.size(), 2);
    r.options.filePatterns = "*.h;*.hpp";
    QCOMPARE(run(r).hits.size(), 0);
    r.options.filePatterns = "*.cpp";
    QCOMPARE(run(r).hits.size(), 2);
}

void ReferenceTests::keywordExpressions() {
    QTemporaryDir dir;
    const auto path = dir.filePath("a.txt");
    QVERIFY(writeSourceFile(path, "cat\nfood\n\nkitty food error\ndog food\n", false).isEmpty());
    auto r = request(path, "(cat OR kitty) AND food !error");
    r.options.method = SearchMethod::KeywordExpression;
    r.options.contextLines = 1;
    const auto result = run(r);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QCOMPARE(result.hits.size(), 1);
    QVERIFY(result.hits.first().text.contains("cat\nfood"));
    r.options.contextLines = 0;
    QCOMPARE(run(r).hits.size(), 0);
    r.query = "dog + food";
    QCOMPARE(run(r).hits.size(), 1);
    r.query = "? \"^dog\" food";
    QCOMPARE(run(r).hits.size(), 1);
    r.query = "=Dog food";
    r.options.caseSensitive = false;
    QCOMPARE(run(r).hits.size(), 0);
    QVERIFY(!validateReferenceSearch("(cat", r.options).isEmpty());
    QVERIFY(!validateReferenceSearch("cat OR", r.options).isEmpty());
    QVERIFY(!validateReferenceSearch("\"cat", r.options).isEmpty());
    QVERIFY(!validateReferenceSearch(QString(140, '(') + "cat" + QString(140, ')'), r.options)
                 .isEmpty());
}

void ReferenceTests::keywordVariationsAndFragments() {
    QTemporaryDir dir;
    const auto path = dir.filePath("a.txt");
    QVERIFY(writeSourceFile(path, "opened doc\nsaving doc\nOpenDoc\n", false).isEmpty());
    auto r = request(path, "open doc");
    r.options.method = SearchMethod::KeywordExpression;
    r.options.contextLines = 0;
    r.options.caseSensitive = false;
    QCOMPARE(run(r).hits.size(), 0);
    r.options.variations = true;
    QCOMPARE(run(r).hits.size(), 1);
    r.query = "save doc";
    QCOMPARE(run(r).hits.size(), 1);
    r.options.variations = false;
    r.options.fragments = true;
    r.query = "open doc";
    QCOMPARE(run(r).hits.size(), 2);
}

void ReferenceTests::dirtyOverlaysAndCancellation() {
    QTemporaryDir dir;
    const auto path = dir.filePath("a.cpp");
    QVERIFY(writeSourceFile(path, "int old;\n", false).isEmpty());
    auto r = request(path, "fresh");
    const QByteArray source = "int fresh;\n";
    r.overlays.append({path, source, analyzeSource(source, path)});
    QCOMPARE(run(r).hits.size(), 1);
    QCOMPARE(readSourceFile(path).bytes, QByteArray("int old;\n"));
    auto cancel = std::make_shared<std::atomic_bool>(true);
    QVERIFY(searchReferences(r, cancel).cancelled);
}

void ReferenceTests::pagingIsExact() {
    QTemporaryDir dir;
    const auto path = dir.filePath("a.cpp");
    QVERIFY(writeSourceFile(path, "a a a a a", false).isEmpty());
    auto r = request(path, "a");
    r.limit = 2;
    auto result = run(r);
    QCOMPARE(result.hits.size(), 2);
    QVERIFY(result.truncated);
    r.offset = 2;
    result = run(r);
    QCOMPARE(result.hits.first().location.start, 4);
    QVERIFY(result.truncated);
    r.offset = 4;
    result = run(r);
    QCOMPARE(result.hits.size(), 1);
    QVERIFY(!result.truncated);
    r.offset = -1;
    QVERIFY(!run(r).error.isEmpty());
}

void ReferenceTests::inactiveRegions() {
    QTemporaryDir dir;
    const auto path = dir.filePath("a.cpp");
    QVERIFY(writeSourceFile(path, "#if 0\nint hidden;\n#endif\nint hidden;\n", false).isEmpty());
    auto r = request(path, "hidden");
    QCOMPARE(run(r).hits.size(), 2);
    r.options.skipInactive = true;
    const auto filtered = run(r);
    QVERIFY2(filtered.warnings.isEmpty(), qPrintable(filtered.warnings.join('\n')));
    QCOMPARE(filtered.hits.size(), 1);
    QCOMPARE(filtered.hits.first().line, 3);
    QVERIFY(
        writeSourceFile(path, "#ifdef FEATURE\nint hidden;\n#else\nint visible;\n#endif\n", false)
            .isEmpty());
    const auto uncertain = run(r);
    QCOMPARE(uncertain.hits.size(), 1);
    QVERIFY(!uncertain.warnings.isEmpty());
    const auto commands =
        QJsonDocument(QJsonArray{QJsonObject{
                          {"directory", dir.path()},
                          {"file", path},
                          {"arguments", QJsonArray{"clang++", "-DFEATURE=1", "-c", path}}}})
            .toJson();
    QVERIFY(writeSourceFile(dir.filePath("compile_commands.json"), commands, false).isEmpty());
    r.query = "visible";
    const auto configured = run(r);
    QVERIFY2(configured.warnings.isEmpty(), qPrintable(configured.warnings.join('\n')));
    QCOMPARE(configured.hits.size(), 0);
    const auto header = dir.filePath("settings.h");
    QVERIFY(writeSourceFile(header, "#define FEATURE 0\n", false).isEmpty());
    QVERIFY(writeSourceFile(
                path,
                "#include \"settings.h\"\n#if FEATURE\nint hidden;\n#else\nint visible;\n#endif\n",
                false)
                .isEmpty());
    const QByteArray liveHeader = "#define FEATURE 1\n";
    r.overlays.append({header, liveHeader, analyzeSource(liveHeader, header)});
    const auto dirtyInclude = run(r);
    QVERIFY2(dirtyInclude.warnings.isEmpty(), qPrintable(dirtyInclude.warnings.join('\n')));
    QCOMPARE(dirtyInclude.hits.size(), 0);
    const auto other = dir.filePath("a.txt");
    QVERIFY(writeSourceFile(other, "hidden", false).isEmpty());
    r.paths = {other};
    r.query = "hidden";
    const auto unknown = run(r);
    QCOMPARE(unknown.hits.size(), 1);
    QVERIFY(!unknown.warnings.isEmpty());
}

void ReferenceTests::editTrackingAndStaleTargets() {
    const QByteArray source = "int count;\n";
    ReferenceHit hit{{"a.cpp", 4, 9, -1, fingerprint(source)}, "int count;", 0, {}};
    QVERIFY(adjustReferenceHit(hit, 0, 0, 3, fingerprint(source), "//\n" + source));
    QCOMPARE(hit.location.start, 7);
    QCOMPARE(hit.line, 1);
    QVERIFY(adjustReferenceHit(hit, 0, 3, 0, fingerprint("//\n" + source), source));
    QCOMPARE(hit.location.start, 4);
    QVERIFY(!hit.stale);
    QVERIFY(!adjustReferenceHit(hit, 5, 1, 0, fingerprint(source), "int cunt;\n"));
    QVERIFY(hit.stale);
    ReferenceHit external{{"a.cpp", 4, 9, -1, fingerprint(source)}, {}, 0, {}};
    QVERIFY(!adjustReferenceHit(external, 0, 0, 1, fingerprint("different"), " " + source));
    QVERIFY(external.stale);
}

void ReferenceTests::semanticAliasesAndPaging() {
    QTemporaryDir dir;
    const auto path = dir.filePath("a.py");
    QVERIFY(writeSourceFile(path, "alias()\nname()\n", false).isEmpty());
    const QVector<LspLocation> targets{{path, 0, 0, 0, 5}, {path, 1, 0, 1, 4}};
    auto cancel = std::make_shared<std::atomic_bool>(false);
    auto result = resolveSemanticReferences(targets, {path}, "name", {}, cancel, 0, 1);
    QCOMPARE(result.hits.size(), 1);
    QCOMPARE(result.hits.first().location.start, 0);
    QVERIFY(result.truncated);
    result = resolveSemanticReferences(targets, {path}, "name", {}, cancel, 1, 1);
    QCOMPARE(result.hits.size(), 1);
    QCOMPARE(result.hits.first().location.start, 8);
    QVERIFY(!result.truncated);
}

void ReferenceTests::historyRoundTripAndValidation() {
    QTemporaryDir dir;
    const auto path = dir.filePath("a.cpp");
    auto r = request(path, "count");
    QVERIFY(writeSourceFile(path, "int count;\n", false).isEmpty());
    QVector<ReferenceGroup> groups{{"count", {}, run(r).hits}};
    QString error;
    const auto json = referenceHistoryJson(groups);
    const auto parsed = parseReferenceHistory(json, dir.path(), error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(referenceHistoryJson(parsed), json);
    auto object = json.first().toObject();
    auto hits = object.value("hits").toArray();
    auto hit = hits.first().toObject();
    hit["start"] = -1;
    hits[0] = hit;
    object["hits"] = hits;
    QVERIFY(parseReferenceHistory({object}, dir.path(), error).isEmpty());
    QVERIFY(!error.isEmpty());
    QVERIFY(parseReferenceHistory(json, dir.filePath("other"), error).isEmpty());
    QVERIFY(!error.isEmpty());
}

void ReferenceTests::sessionHistoryRoundTrip() {
    QTemporaryDir dir, state;
    const auto path = dir.filePath("a.cpp");
    QVERIFY(writeSourceFile(path, "int count;\n", false).isEmpty());
    ApplicationStateStore store(state.path());
    const auto loaded = store.loadSession(dir.path());
    QVERIFY(loaded.session.has_value());
    WorkspaceSession session;
    session.references = {{"count", {}, run(request(path, "count")).hits}};
    QVERIFY(store.saveSession(dir.path(), session).isEmpty());
    ApplicationStateStore restored(state.path());
    const auto result = restored.loadSession(dir.path());
    QVERIFY2(result.session.has_value(), qPrintable(result.error));
    QCOMPARE(referenceHistoryJson(result.session->references),
             referenceHistoryJson(session.references));
}

void ReferenceTests::legacySettingsAndIndependentState() {
    const auto result =
        parseSettings(R"({"references":{"semantic":false,"comments":true,"strings":true}})");
    QVERIFY(result.settings);
    QCOMPARE(result.settings->references.search.method, SearchMethod::Occurrences);
    QCOMPARE(result.settings->references.search.comments, CommentSearch::Include);
    QVERIFY(result.settings->references.search.strings);
    auto settings = *result.settings;
    settings.searchProject.search.method = SearchMethod::KeywordExpression;
    settings.searchProject.search.contextLines = 42;
    const auto roundTrip = parseSettings(QJsonDocument(settingsToJson(settings)).toJson());
    QVERIFY(roundTrip.settings);
    QCOMPARE(roundTrip.settings->references.search.method, SearchMethod::Occurrences);
    QCOMPARE(roundTrip.settings->searchProject.search.method, SearchMethod::KeywordExpression);
    QCOMPARE(roundTrip.settings->searchProject.search.contextLines, 42);
    QVERIFY(!parseSettings(R"({"references":{"search":{"method":5}}})").settings);
    QVERIFY(!parseSettings(R"({"references":{"search":{"contextLines":1001}}})").settings);
}

void ReferenceTests::touchDoesNotChangeContents() {
    QTemporaryDir dir;
    const auto path = dir.filePath("a.cpp");
    const QByteArray source = "int count;\n";
    QVERIFY(writeSourceFile(path, source, false).isEmpty());
    const auto results = run(request(path, "count"));
    QVERIFY(touchReferenceFiles(results.hits).isEmpty());
    QCOMPARE(readSourceFile(path).bytes, source);
    auto missing = results.hits;
    missing[0].location.path = dir.filePath("missing.cpp");
    // ReadWrite must not create files that disappeared after the search.
    QVERIFY(!touchReferenceFiles(missing).isEmpty());
    QVERIFY(!QFileInfo::exists(missing[0].location.path));
}

void ReferenceTests::editableDialogAndIndependentProjectSearch() {
    QTemporaryDir dir;
    const auto path = dir.filePath("a.cpp");
    QVERIFY(writeSourceFile(path, "int count;\nint other;\n", false).isEmpty());
    MainWindow window(false);
    window.show();
    QVERIFY(window.openFile(path));
    window.activeEditor()->selectRange(4, 9);
    window.activeEditor()->setFocus();
    window.findChild<QAction *>("lookupReferences")->trigger();
    auto *dialog = window.findChild<QDialog *>("lookupReferencesDialog");
    QVERIFY(dialog);
    auto *query = dialog->findChild<QLineEdit *>("referenceQuery");
    QCOMPARE(query->text(), "count");
    query->setText("other");
    dialog->findChild<QPushButton *>("runReferenceLookup")->click();
    QVERIFY(dialog->isVisible());
    QVERIFY(dialog->findChild<QLabel *>("referenceDialogStatus")->text().contains("Select"));
    setReferenceMethod(dialog, SearchMethod::Literal);
    dialog->findChild<QPushButton *>("runReferenceLookup")->click();
    auto *tree = window.findChild<QTreeWidget *>("referenceResults");
    QVERIFY(tree);
    QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 1, 10000);
    QCOMPARE(tree->topLevelItem(0)->childCount(), 1);
    QVERIFY(tree->topLevelItem(0)->child(0)->text(3).contains("other"));
    window.findChild<QAction *>("searchProject")->trigger();
    auto *projectDialog = window.findChild<QDialog *>("searchProjectDialog");
    QVERIFY(projectDialog);
    QCOMPARE(projectDialog->findChild<QComboBox *>("referenceMethod")->currentData().toInt(),
             static_cast<int>(SearchMethod::Literal));
    QCOMPARE(window.configuration().references.search.method, SearchMethod::Literal);
    projectDialog->close();
}

void ReferenceTests::uiTracksEditsAndRemovesResults() {
    QTemporaryDir dir;
    const auto path = dir.filePath("a.cpp");
    const QByteArray source = "int count;\nint caller() { return count; }\n";
    QVERIFY(writeSourceFile(path, source, false).isEmpty());
    MainWindow window(false);
    window.show();
    QVERIFY(window.openFile(path));
    auto *editor = window.activeEditor();
    editor->selectRange(4, 9);
    editor->setFocus();
    window.findChild<QAction *>("lookupReferences")->trigger();
    auto *dialog = window.findChild<QDialog *>("lookupReferencesDialog");
    setReferenceMethod(dialog, SearchMethod::Occurrences);
    dialog->findChild<QPushButton *>("runReferenceLookup")->click();
    auto *tree = window.findChild<QTreeWidget *>("referenceResults");
    QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 1, 10000);
    QCOMPARE(tree->topLevelItem(0)->childCount(), 2);
    auto *row = tree->topLevelItem(0)->child(1);
    tree->setCurrentItem(row);
    tree->itemActivated(row, 0);
    QTRY_COMPARE(window.activeEditor(), editor);
    editor->sends(SCI_INSERTTEXT, 0, "// added\n");
    QCOMPARE(row->text(2), "3");
    window.findChild<QAction *>("firstReference")->trigger();
    QCOMPARE(editor->send(SCI_GETSELECTIONSTART), 9);
    window.findChild<QAction *>("jumpReference")->trigger();
    QCOMPARE(window.findChild<QTabWidget *>("sourceTabs")->currentWidget(),
             window.findChild<QWidget *>("searchResultsPage"));
    tree->clearSelection();
    row->setSelected(true);
    window.findChild<QAction *>("removeReferenceResults")->trigger();
    QCOMPARE(tree->topLevelItem(0)->childCount(), 1);
}

void ReferenceTests::outputOptionsAndExport() {
    QTemporaryDir dir;
    const auto path = dir.filePath("a.cpp");
    QVERIFY(writeSourceFile(path, "int count;\nint again = count;\n", false).isEmpty());
    auto results = run(request(path, "count"));
    SearchOutputOptions output;
    output.container = false;
    output.line = false;
    output.text = false;
    output.links = false;
    QVector<ReferenceGroup> groups{{"count", output, {results.hits.first()}}};
    const auto exportPath = dir.filePath("results.txt");
    QVERIFY(exportReferenceHistory(exportPath, groups, dir.path()).isEmpty());
    QCOMPARE(readSourceFile(exportPath).bytes, QByteArray("count\na.cpp\n\n"));
    groups[0].output.relativePaths = false;
    QVERIFY(exportReferenceHistory(exportPath, groups, dir.path()).isEmpty());
    QVERIFY(readSourceFile(exportPath).bytes.contains(path.toUtf8()));
    QVERIFY(!exportReferenceHistory(dir.path(), groups, dir.path()).isEmpty());

    const auto config = dir.filePath("settings.json");
    QVERIFY(writeSourceFile(config, R"({"references":{"semantic":false,"search":{"output":{
        "container":false,"line":false,"text":false,"links":false}}}})",
                            false)
                .isEmpty());
    MainWindow window(false, nullptr, config);
    window.show();
    QVERIFY(window.openFile(path));
    window.activeEditor()->selectRange(4, 9);
    window.activeEditor()->setFocus();
    window.findChild<QAction *>("lookupReferences")->trigger();
    auto *dialog = window.findChild<QDialog *>("lookupReferencesDialog");
    dialog->findChild<QPushButton *>("runReferenceLookup")->click();
    auto *tree = window.findChild<QTreeWidget *>("referenceResults");
    QVERIFY(tree);
    QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 1, 10000);
    QCOMPARE(tree->topLevelItem(0)->childCount(), 1);
    auto *row = tree->topLevelItem(0)->child(0);
    QCOMPARE(row->text(0), "a.cpp");
    QCOMPARE(row->text(1), "a.cpp");
    QCOMPARE(row->text(2), QString{});
    QCOMPARE(row->text(3), QString{});
    tree->setCurrentItem(row);
    tree->itemActivated(row, 0);
    QCOMPARE(window.findChild<QTabWidget *>("sourceTabs")->currentWidget(),
             window.findChild<QWidget *>("searchResultsPage"));
}

void ReferenceTests::indexedSearchIncludesDirtyOverlays() {
    QTemporaryDir dir;
    const auto path = dir.filePath("a.cpp");
    const auto other = dir.filePath("b.cpp");
    QVERIFY(writeSourceFile(path, "int disk;\n", false).isEmpty());
    QVERIFY(writeSourceFile(other, "int irrelevant;\n", false).isEmpty());
    WorkspaceIndex index;
    QSignalSpy ready(&index, &WorkspaceIndex::ready);
    index.open(dir.path());
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 10000);
    auto r = request(path, "count");
    r.paths.append(other);
    r.workspaceRoot = dir.path();
    r.options.wholeWords = true;
    const QByteArray live = "int count;\n";
    r.overlays.append({path, live, analyzeSource(live, path)});
    const auto result = run(r);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QCOMPARE(result.hits.size(), 1);
    QCOMPARE(result.filesSearched, 1);
    QCOMPARE(result.hits.first().location.path, path);
}

void ReferenceTests::realSemanticOverloadsAndMembers() {
#ifndef CODECAIRN_TEST_CLANGD
    QSKIP("clangd is not configured.");
#else
    QTemporaryDir dir;
#ifndef Q_OS_WIN
    QTemporaryDir aliases;
    const auto alias = aliases.filePath("workspace");
    QVERIFY(QFile::link(dir.path(), alias));
    const auto path = QDir(alias).filePath("a.cpp");
#else
    const auto path = dir.filePath("a.cpp");
#endif
    const QByteArray source =
        "int pick(int x) { return x; }\n"
        "int pick(double x) { return int(x); }\n"
        "struct A { int value; }; struct B { int value; };\n"
        "int caller(A a, B b) { return pick(1) + pick(2.0) + a.value + b.value; }\n";
    QVERIFY(writeSourceFile(path, source, false).isEmpty());
    LanguageServerClient client;
    QSignalSpy ready(&client, &LanguageServerClient::ready);
    QSignalSpy locations(&client, &LanguageServerClient::locationsReady);
    client.start(dir.path(), {}, QString::fromUtf8(CODECAIRN_TEST_CLANGD));
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 20000);
    client.locations(path, source, 1, source.indexOf("pick(int"), 1, "references", true, false);
    QTRY_VERIFY_WITH_TIMEOUT(!locations.isEmpty(), 20000);
    auto reply = locations.takeFirst();
    QVERIFY2(reply[3].toString().isEmpty(), qPrintable(reply[3].toString()));
    const auto targets = qvariant_cast<QVector<LspLocation>>(reply[2]);
    const auto results = resolveSemanticReferences(targets, {path}, "pick",
                                                   {{path, source, analyzeSource(source, path)}},
                                                   std::make_shared<std::atomic_bool>(false));
    QCOMPARE(results.hits.size(), 1);
    QCOMPARE(results.hits.first().location.start, source.indexOf("pick(1)"));
    client.locations(path, source, 1, source.indexOf("int value") + 4, 2, "references", true,
                     false);
    QTRY_VERIFY_WITH_TIMEOUT(!locations.isEmpty(), 20000);
    reply = locations.takeFirst();
    QVERIFY2(reply[3].toString().isEmpty(), qPrintable(reply[3].toString()));
    const auto memberTargets = qvariant_cast<QVector<LspLocation>>(reply[2]);
    const auto members = resolveSemanticReferences(memberTargets, {path}, "value",
                                                   {{path, source, analyzeSource(source, path)}},
                                                   std::make_shared<std::atomic_bool>(false));
    QCOMPARE(members.hits.size(), 1);
    QCOMPARE(members.hits.first().location.start, source.indexOf("a.value") + 2);
#endif
}

void ReferenceTests::continuationRejectsChangedSources() {
    QTemporaryDir dir;
    const auto path = dir.filePath("a.cpp");
    QVERIFY(writeSourceFile(path, "count count count", false).isEmpty());
    auto r = request(path, "count");
    r.limit = 1;
    auto first = run(r);
    QVERIFY(first.truncated);
    r.offset = 1;
    r.expectedHashes = first.sourceHashes;
    QVERIFY(writeSourceFile(path, "other count count", false).isEmpty());
    const auto changed = run(r);
    QVERIFY(changed.error.contains("changed"));
    QVERIFY(changed.hits.isEmpty());

    const auto other = dir.filePath("b.cpp");
    QVERIFY(writeSourceFile(path, "int absent;\n", false).isEmpty());
    QVERIFY(writeSourceFile(other, "int count; int f() { return count; }\n", false).isEmpty());
    WorkspaceIndex index;
    QSignalSpy ready(&index, &WorkspaceIndex::ready);
    index.open(dir.path());
    QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 10000);
    r = request(path, "count");
    r.paths.append(other);
    r.workspaceRoot = dir.path();
    r.options.wholeWords = true;
    r.limit = 1;
    first = run(r);
    QVERIFY2(first.error.isEmpty(), qPrintable(first.error));
    QVERIFY(first.truncated);
    r.expectedHashes = first.sourceHashes;
    r.offset = 1;
    QVERIFY(writeSourceFile(path, "int count;\n", false).isEmpty());
    const auto formerlyNegative = run(r);
    QVERIFY(formerlyNegative.error.contains("changed"));
    QVERIFY(formerlyNegative.hits.isEmpty());
}

void ReferenceTests::workspaceRestoresResultRows() {
    QTemporaryDir dir, personal;
    const auto path = dir.filePath("a.cpp");
    const auto config = personal.filePath("settings.json");
    QVERIFY(writeSourceFile(path, "int count;\n", false).isEmpty());
    {
        MainWindow window(true, nullptr, config, personal.path());
        window.show();
        window.openProject(dir.path());
        QVERIFY(window.openFile(path));
        window.activeEditor()->selectRange(4, 9);
        window.activeEditor()->setFocus();
        window.findChild<QAction *>("lookupReferences")->trigger();
        auto *dialog = window.findChild<QDialog *>("lookupReferencesDialog");
        setReferenceMethod(dialog, SearchMethod::Occurrences);
        dialog->findChild<QPushButton *>("runReferenceLookup")->click();
        auto *tree = window.findChild<QTreeWidget *>("referenceResults");
        QVERIFY(tree);
        QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 1, 10000);
        QCOMPARE(tree->topLevelItem(0)->childCount(), 1);
        QVERIFY(window.close());
    }
    MainWindow restored(true, nullptr, config, personal.path());
    restored.show();
    restored.openProject(dir.path());
    auto *tree = restored.findChild<QTreeWidget *>("referenceResults");
    QVERIFY(tree);
    QCOMPARE(tree->topLevelItemCount(), 1);
    QCOMPARE(tree->topLevelItem(0)->childCount(), 1);
    auto *row = tree->topLevelItem(0)->child(0);
    tree->setCurrentItem(row);
    tree->itemActivated(row, 0);
    QTRY_VERIFY_WITH_TIMEOUT(restored.activeEditor(), 10000);
    QCOMPARE(restored.activeEditor()->send(SCI_GETSELECTIONSTART), 0);
    QVERIFY(restored.close());
}

void ReferenceTests::clickableReferenceIconsAndToolbar() {
    QTemporaryDir dir;
    const auto path = dir.filePath("a.cpp");
    const QByteArray source = "int count;\nint caller() { return count; }\n";
    QVERIFY(writeSourceFile(path, source, false).isEmpty());
    MainWindow window(false);
    window.show();
    QVERIFY(window.openFile(path));
    auto *editor = window.activeEditor();
    editor->selectRange(4, 9);
    editor->setFocus();
    window.findChild<QAction *>("lookupReferences")->trigger();
    auto *dialog = window.findChild<QDialog *>("lookupReferencesDialog");
    setReferenceMethod(dialog, SearchMethod::Occurrences);
    dialog->findChild<QPushButton *>("runReferenceLookup")->click();
    auto *tree = window.findChild<NavigationTree *>("referenceResults");
    QVERIFY(tree);
    QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 1, 10000);
    auto *row = tree->topLevelItem(0)->child(1);
    QTRY_VERIFY(!tree->itemIconRect(row).isEmpty());
    QTest::mouseClick(tree->viewport(), Qt::LeftButton, Qt::NoModifier,
                      tree->itemIconRect(row).center());
    QTRY_COMPARE(window.activeEditor(), editor);
    QCOMPARE(editor->send(SCI_GETSELECTIONSTART), source.indexOf("int caller"));
    QCOMPARE(editor->send(SCI_GETSELECTIONEND), source.size() - 1);
    QVERIFY(editor->send(SCI_GETMARGINWIDTHN, 1) > 0);
    QVERIFY(editor->send(SCI_MARKERGET, 1) != 0);
    const int y = editor->send(SCI_POINTYFROMPOSITION, 0, source.lastIndexOf("count")) +
                  editor->lineHeight() / 2;
    const int x = editor->send(SCI_GETMARGINWIDTHN, 0) + 10;
    QTest::mouseClick(editor->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(x, y));
    auto *tabs = window.findChild<QTabWidget *>("sourceTabs");
    QTRY_COMPARE(tabs->currentWidget(), window.findChild<QWidget *>("searchResultsPage"));
    QCOMPARE(tree->currentItem(), row);
    auto *toolbar = window.findChild<QToolBar *>("mainToolbar");
    auto *previous = qobject_cast<QToolButton *>(
        toolbar->widgetForAction(window.findChild<QAction *>("previousReference")));
    auto *next = qobject_cast<QToolButton *>(
        toolbar->widgetForAction(window.findChild<QAction *>("nextReference")));
    QVERIFY(previous);
    QVERIFY(next);
    QTest::mouseClick(previous, Qt::LeftButton);
    QTRY_COMPARE(window.activeEditor(), editor);
    QCOMPARE(editor->send(SCI_GETSELECTIONSTART), 0);
    QTest::mouseClick(next, Qt::LeftButton);
    QCOMPARE(editor->send(SCI_GETSELECTIONSTART), source.indexOf("int caller"));
}

void ReferenceTests::closingResultsClearsLinksAndStartsFresh() {
    QTemporaryDir dir;
    const auto path = dir.filePath("a.cpp");
    const QByteArray source = "int count;\n";
    QVERIFY(writeSourceFile(path, source, false).isEmpty());
    MainWindow window(false);
    window.show();
    QVERIFY(window.openFile(path));
    auto *editor = window.activeEditor();
    editor->selectRange(4, 9);
    editor->setFocus();
    window.findChild<QAction *>("lookupReferences")->trigger();
    auto *dialog = window.findChild<QDialog *>("lookupReferencesDialog");
    setReferenceMethod(dialog, SearchMethod::Occurrences);
    dialog->findChild<QPushButton *>("runReferenceLookup")->click();
    auto *tree = window.findChild<QTreeWidget *>("referenceResults");
    QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 1, 10000);
    window.findChild<QAction *>("closeFile")->trigger();
    QVERIFY(!window.findChild<QWidget *>("searchResultsPage"));
    QVERIFY(!window.findChild<QAction *>("showSearchResults")->isEnabled());
    QCOMPARE(editor->send(SCI_MARKERGET, 0), 0);
    QCOMPARE(editor->send(SCI_GETMARGINWIDTHN, 1), 0);
    QVERIFY(!window.findChild<QAction *>("nextReference")->isEnabled());
    window.activateWindow();
    QTest::qWait(20);
    editor->setFocus();
    QTRY_VERIFY(editor->hasFocus());
    QTest::keyClick(editor, Qt::Key_L, Qt::ControlModifier);
    QCOMPARE(editor->bytes(), source);
    editor->sends(SCI_INSERTTEXT, 0, "// extra\n");
    QCOMPARE(editor->send(SCI_MARKERGET, 1), 0);
    editor->selectRange(13, 18);
    editor->setFocus();
    window.findChild<QAction *>("lookupReferences")->trigger();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    dialog = window.findChild<QDialog *>("lookupReferencesDialog");
    QVERIFY(dialog);
    QCOMPARE(dialog->findChild<QComboBox *>("referenceQueryHistory")->count(), 0);
    setReferenceMethod(dialog, SearchMethod::Occurrences);
    dialog->findChild<QPushButton *>("runReferenceLookup")->click();
    tree = window.findChild<QTreeWidget *>("referenceResults");
    QVERIFY(tree);
    QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), 1, 10000);
    QCOMPARE(tree->topLevelItem(0)->child(0)->text(2), "2");
    window.findChild<QAction *>("jumpReference")->trigger();
    QTRY_COMPARE(window.activeEditor(), editor);
    QCOMPARE(editor->send(SCI_GETSELECTIONSTART), 9);
    window.findChild<QAction *>("jumpReference")->trigger();
    window.findChild<QAction *>("clearReferenceResults")->trigger();
    QCOMPARE(editor->send(SCI_GETMARGINWIDTHN, 1), 0);
    QCOMPARE(editor->send(SCI_MARKERGET, 1), 0);
    QVERIFY(!window.findChild<QAction *>("nextReference")->isEnabled());
}

void ReferenceTests::searchedSymbolsPreviewAndDelete() {
    QTemporaryDir dir;
    const auto path = dir.filePath("a.cpp");
    const QByteArray source = "int first;\nint second;\nint caller() { return first + second; }\n";
    QVERIFY(writeSourceFile(path, source, false).isEmpty());
    MainWindow window(false);
    window.resize(1446, 768);
    window.show();
    QVERIFY(window.openFile(path));
    auto *editor = window.activeEditor();
    for (const auto &query : {QByteArray("first"), QByteArray("second")}) {
        window.findChild<QTabWidget *>("sourceTabs")->setCurrentWidget(editor);
        editor->selectRange(source.indexOf(query), source.indexOf(query) + query.size());
        editor->setFocus();
        window.findChild<QAction *>("lookupReferences")->trigger();
        auto *dialog = window.findChild<QDialog *>("lookupReferencesDialog");
        QVERIFY(dialog);
        setReferenceMethod(dialog, SearchMethod::Occurrences);
        dialog->findChild<QPushButton *>("runReferenceLookup")->click();
        auto *tree = window.findChild<QTreeWidget *>("referenceResults");
        QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItemCount(), query == "first" ? 1 : 2, 10000);
    }

    auto *results = window.findChild<QTreeWidget *>("referenceResults");
    auto *outline = window.findChild<QTreeWidget *>("outline");
    QCOMPARE(window.findChild<QLabel *>("outlineTitle")->text(), "Search Results");
    QCOMPARE(outline->topLevelItemCount(), 2);
    QCOMPARE(outline->topLevelItem(0)->text(0), "first");
    QCOMPARE(outline->topLevelItem(1)->text(0), "second");
    QVERIFY(results->isHeaderHidden());
    QCOMPARE(results->indentation(), 0);
    QVERIFY(results->isColumnHidden(1) && results->isColumnHidden(2) && results->isColumnHidden(3));
    const auto size = window.findChild<QToolBar *>("mainToolbar")->iconSize();
    QCOMPARE(results->iconSize(), size);
    QCOMPARE(editor->send(SCI_GETMARGINWIDTHN, 1), size.width() + 4);
    const auto point = outline->visualItemRect(outline->topLevelItem(0)).center();
    QTest::mouseClick(outline->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    auto *context = window.contextEditor();
    QTRY_COMPARE_WITH_TIMEOUT(context->bytes(), source, 10000);
    QTRY_COMPARE(context->send(SCI_GETSELECTIONSTART), 0);
    QCOMPARE(context->send(SCI_GETSELECTIONEND), source.indexOf('\n'));
    QTest::keyClick(outline, Qt::Key_Down);
    QTRY_COMPARE(context->send(SCI_GETSELECTIONSTART), source.indexOf("int second"));
    QCOMPARE(context->send(SCI_GETSELECTIONEND),
             source.indexOf('\n', source.indexOf("int second")));
    QCOMPARE(static_cast<quint32>(
                 context->send(SCI_GETELEMENTCOLOUR, SC_ELEMENT_SELECTION_INACTIVE_BACK)),
             quint32(0xff9e9750));
    QTest::qWait(300);
    QCOMPARE(context->send(SCI_GETSELECTIONSTART), source.indexOf("int second"));
    QVERIFY(window.grab().save(
        QDir(QCoreApplication::applicationDirPath()).filePath("compact-reference-results.png")));
    QTest::keyClick(outline, Qt::Key_Delete);
    QCOMPARE(results->topLevelItemCount(), 1);
    QCOMPARE(outline->topLevelItemCount(), 1);
    QCOMPARE(outline->topLevelItem(0)->text(0), "first");
    QCOMPARE(context->send(SCI_GETSELECTIONSTART), 0);
    window.findChild<QAction *>("closeFile")->trigger();
    window.findChild<QAction *>("showSearchResults")->trigger();
    QVERIFY(!window.findChild<QTreeWidget *>("referenceResults"));
    QVERIFY(!window.findChild<QAction *>("showSearchResults")->isEnabled());
    QCOMPARE(editor->send(SCI_GETMARGINWIDTHN, 1), 0);
}

void ReferenceTests::resultsTextCaretAndHover() {
    QTemporaryDir dir;
    const auto path = dir.filePath("a.cpp");
    const QByteArray source = "int count;\nint caller() { return count; }\n";
    QVERIFY(writeSourceFile(path, source, false).isEmpty());
    MainWindow window(false);
    window.show();
    window.activateWindow();
    QVERIFY(window.openFile(path));
    auto *editor = window.activeEditor();
    editor->selectRange(4, 9);
    editor->setFocus();
    window.findChild<QAction *>("lookupReferences")->trigger();
    auto *dialog = window.findChild<QDialog *>("lookupReferencesDialog");
    setReferenceMethod(dialog, SearchMethod::Occurrences);
    dialog->findChild<QPushButton *>("runReferenceLookup")->click();
    auto *view = window.findChild<ReferenceResultsView *>("referenceResults");
    QVERIFY(view);
    QTRY_COMPARE_WITH_TIMEOUT(view->topLevelItemCount(), 1, 10000);
    window.activateWindow();
    QTest::qWait(20);
    auto *row = view->topLevelItem(0)->child(0);
    const auto rect = view->visualItemRect(row);
    const QPoint point(rect.left() + view->iconSize().width() + 10 +
                           view->fontMetrics().horizontalAdvance(row->text(0).left(4)),
                       rect.center().y());
    QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    view->setFocus();
    QTRY_VERIFY(view->hasFocus());
    QCOMPARE(view->textCursor().positionInBlock(), 4);
    QTest::keyClick(view, Qt::Key_Right);
    QCOMPARE(view->textCursor().positionInBlock(), 5);
    QTest::keyClick(view, Qt::Key_Left);
    QCOMPARE(view->textCursor().positionInBlock(), 4);
    QTest::keyClick(view, Qt::Key_Home);
    QCOMPARE(view->textCursor().positionInBlock(), 0);
    QTest::keyClick(view, Qt::Key_Up);
    QCOMPARE(view->textCursor().blockNumber(), 0);
    QCOMPARE(view->currentItem(), view->topLevelItem(0));
    QTest::keyClick(view, Qt::Key_Down);
    QCOMPARE(view->textCursor().blockNumber(), 1);
    QCOMPARE(view->currentItem(), row);
    QTest::keyClick(view, Qt::Key_Right, Qt::ShiftModifier);
    QCOMPARE(view->textCursor().selectedText(), row->text(0).left(1));
    QVERIFY(window.findChild<QAction *>("copy")->isEnabled());
    window.findChild<QAction *>("copy")->trigger();
    QCOMPARE(QApplication::clipboard()->text(), row->text(0).left(1));
    const auto before = view->textCursor().document()->toPlainText();
    QTest::keyClicks(view, "typing must not edit results");
    QTest::keyClick(view, Qt::Key_Delete);
    QCOMPARE(view->textCursor().document()->toPlainText(), before);
    QTest::mouseMove(view->viewport(), point);
    QCOMPARE(view->viewport()->cursor().shape(), Qt::IBeamCursor);
    const auto icon = view->itemIconRect(row);
    QTest::mouseMove(view->viewport(), icon.center());
    const auto index = view->model()->index(0, 0, view->model()->index(0, 0));
    QTRY_VERIFY(view->iconHovered(index));
    QCOMPARE(view->viewport()->cursor().shape(), Qt::PointingHandCursor);
    const auto image = view->viewport()->grab().toImage();
    const auto dpr = image.devicePixelRatio();
    int red = 0;
    for (int x = icon.left() + 1; x < icon.right(); ++x)
        if (image.pixelColor(qRound(x * dpr), qRound(icon.top() * dpr)).red() > 200 &&
            image.pixelColor(qRound(x * dpr), qRound(icon.top() * dpr)).green() < 100)
            ++red;
    QVERIFY(red >= icon.width() / 2);
    QTest::mouseMove(view->viewport(), point);
    QVERIFY(!view->iconHovered(index));
    QTest::mouseClick(view->viewport(), Qt::LeftButton, Qt::NoModifier, icon.center());
    QTRY_COMPARE(window.activeEditor(), editor);
    const int y = editor->send(SCI_POINTYFROMPOSITION, 0, 0) + editor->lineHeight() / 2;
    QTest::mouseMove(editor->viewport(), QPoint(10, y));
    QTRY_VERIFY(editor->send(SCI_MARKERGET, 0) & (1 << 23));
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(editor, &leave);
    QCOMPARE(editor->send(SCI_MARKERGET, 0) & (1 << 23), 0);
    QVERIFY(editor->send(SCI_MARKERGET, 0) & (1 << 24));
    QCOMPARE(readSourceFile(path).bytes, source);
}

void ReferenceTests::referenceIconsHaveLogicalPixelSize() {
    for (int size : {12, 16, 24}) {
        const auto image = UiAppearance::referenceLinkImage(size);
        QCOMPARE(image.size(), QSize(size, size));
        QCOMPARE(image.devicePixelRatio(), 1.0);
        const auto hover = UiAppearance::referenceLinkImage(size, true);
        QCOMPARE(hover.size(), image.size());
        QCOMPARE(hover.pixelColor(0, 0), QColor("#ed3030"));
    }
}

void ReferenceTests::lookupDialogLayoutAndOptions() {
    QTemporaryDir dir;
    const auto path = dir.filePath("a.cpp");
    QVERIFY(writeSourceFile(path, "int count;\n// count\n", false).isEmpty());
    MainWindow window(false);
    window.show();
    QVERIFY(window.openFile(path));
    window.activeEditor()->selectRange(4, 9);
    window.activeEditor()->setFocus();
    window.findChild<QAction *>("lookupReferences")->trigger();
    auto *dialog = window.findChild<QDialog *>("lookupReferencesDialog");
    QVERIFY(dialog);
    auto *history = dialog->findChild<QComboBox *>("referenceQueryHistory");
    auto *query = dialog->findChild<QLineEdit *>("referenceQuery");
    auto *scope = dialog->findChild<QComboBox *>("referenceScope");
    auto *patterns = dialog->findChild<QLineEdit *>("referenceFilePatterns");
    auto *options = dialog->findChild<QGroupBox *>("referenceOptions");
    auto *search = dialog->findChild<QPushButton *>("runReferenceLookup");
    auto *close = dialog->findChild<QPushButton *>("closeReferenceLookup");
    auto *mode = dialog->findChild<QComboBox *>("referenceMethod");
    QVERIFY(history && query && scope && patterns && options && search && close && mode);
    QVERIFY(!dialog->findChild<QPushButton *>("referenceHelp"));
    QVERIFY(history->isEditable());
    QCOMPARE(query->text(), "count");
    QCOMPARE(scope->currentText(), "All Project Files");
    QCOMPARE(mode->itemText(0), "Simple String");
    QCOMPARE(mode->itemText(1), "Regular Expression");
    QCOMPARE(mode->itemText(2), "Keyword Expression");
    QCOMPARE(mode->itemData(3).toInt(), static_cast<int>(SearchMethod::References));
    const auto location = [dialog](QWidget *widget) {
        return widget->mapTo(dialog, QPoint{});
    };
    QVERIFY(location(history).y() < location(scope).y());
    QVERIFY(location(scope).y() < location(patterns).y());
    QVERIFY(location(patterns).y() < location(options).y());
    QVERIFY(location(search).x() >= location(options).x() + options->width());
    QCOMPARE(location(search).x(), location(close).x());
    QVERIFY(location(search).y() < location(close).y());
    QVERIFY(search->isDefault());
    auto *skip = dialog->findChild<QCheckBox *>("referenceSkipComments");
    auto *only = dialog->findChild<QCheckBox *>("referenceOnlyComments");
    auto *whole = dialog->findChild<QCheckBox *>("referenceWholeWords");
    QVERIFY(skip && only && whole);
    QVERIFY(!skip->isEnabled());
    QVERIFY(whole->isChecked() && !whole->isEnabled());
    setReferenceMethod(dialog, SearchMethod::Literal);
    QVERIFY(skip->isEnabled());
    whole->setChecked(false);
    setReferenceMethod(dialog, SearchMethod::References);
    QVERIFY(!dialog->findChild<QCheckBox *>("referenceReplaceResults")->isChecked());
    QVERIFY(whole->isChecked() && !whole->isEnabled());
    setReferenceMethod(dialog, SearchMethod::Literal);
    QVERIFY(!whole->isChecked());
    skip->setChecked(true);
    only->setChecked(true);
    QVERIFY(!skip->isChecked());
    skip->setChecked(true);
    QVERIFY(!only->isChecked());
    skip->setChecked(false);
    QVERIFY(!only->isChecked());
    setReferenceMethod(dialog, SearchMethod::KeywordExpression);
    QVERIFY(dialog->findChild<QSpinBox *>("referenceContextLines")->isVisible());
    setReferenceMethod(dialog, SearchMethod::Literal);
    QVERIFY(!dialog->findChild<QSpinBox *>("referenceContextLines")->isVisible());
    QVERIFY(!dialog->findChild<QCheckBox *>("referenceTouchFiles")->isChecked());
    QTest::keyClick(dialog, Qt::Key_F1);
    QVERIFY(!dialog->findChild<QDialog *>("referenceHelpDialog"));
    QVERIFY(dialog->isVisible());
    QVERIFY(dialog->grab().save(
        QDir(QCoreApplication::applicationDirPath()).filePath("lookup-reference-dialog.png")));
    patterns->setText("*.cpp");
    query->setFocus();
    QTest::keyClick(query, Qt::Key_Return);
    auto *results = window.findChild<QTreeWidget *>("referenceResults");
    QVERIFY(results);
    QTRY_COMPARE_WITH_TIMEOUT(results->topLevelItemCount(), 1, 10000);
    QCOMPARE(results->topLevelItem(0)->childCount(), 2);
    QCOMPARE(window.configuration().references.search.comments, CommentSearch::Include);
    QCOMPARE(window.configuration().references.search.filePatterns, "*.cpp");
    window.findChild<QAction *>("lookupReferences")->trigger();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    dialog = window.findChild<QDialog *>("lookupReferencesDialog");
    QVERIFY(dialog);
    history = dialog->findChild<QComboBox *>("referenceQueryHistory");
    QVERIFY(history->findText("count") >= 0);
    dialog->findChild<QPushButton *>("closeReferenceLookup")->click();
    QVERIFY(!dialog->isVisible());
}

void ReferenceTests::dialogTouchRequiresConfirmation() {
    QTemporaryDir dir;
    const auto path = dir.filePath("a.cpp");
    const QByteArray source = "int count;\n";
    QVERIFY(writeSourceFile(path, source, false).isEmpty());
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadWrite));
    const auto oldTime = QDateTime::currentDateTimeUtc().addDays(-2);
    QVERIFY(file.setFileTime(oldTime, QFileDevice::FileModificationTime));
    file.close();
    MainWindow window(false);
    window.show();
    QVERIFY(window.openFile(path));
    window.activeEditor()->selectRange(4, 9);
    window.activeEditor()->setFocus();
    window.findChild<QAction *>("lookupReferences")->trigger();
    auto *dialog = window.findChild<QDialog *>("lookupReferencesDialog");
    setReferenceMethod(dialog, SearchMethod::Occurrences);
    dialog->findChild<QCheckBox *>("referenceTouchFiles")->setChecked(true);
    int confirmations = 0;
    QTimer cancelConfirmation;
    connect(&cancelConfirmation, &QTimer::timeout, &window, [&] {
        if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            message && message->text().contains("matching files to now?")) {
            ++confirmations;
            message->button(QMessageBox::Cancel)->click();
            cancelConfirmation.stop();
        }
    });
    cancelConfirmation.start(10);
    dialog->findChild<QPushButton *>("runReferenceLookup")->click();
    QTRY_COMPARE_WITH_TIMEOUT(confirmations, 1, 10000);
    QCOMPARE(QFileInfo(path).lastModified().toUTC(), oldTime);
    QCOMPARE(readSourceFile(path).bytes, source);
}

void ReferenceTests::closedResultsStayClearedAfterFailedSearch() {
    QTemporaryDir dir, personal;
    const auto path = dir.filePath("a.cpp");
    const auto config = personal.filePath("settings.json");
    const QByteArray source = "int count;\nint other;\n";
    QVERIFY(writeSourceFile(path, source, false).isEmpty());
    {
        MainWindow window(true, nullptr, config, personal.path());
        window.show();
        QVERIFY(window.openFile(path));
        auto *editor = window.activeEditor();
        for (const auto &query : {QByteArray("count"), QByteArray("other")}) {
            window.findChild<QTabWidget *>("sourceTabs")->setCurrentWidget(editor);
            editor->selectRange(source.indexOf(query), source.indexOf(query) + query.size());
            editor->setFocus();
            window.findChild<QAction *>("lookupReferences")->trigger();
            auto *dialog = window.findChild<QDialog *>("lookupReferencesDialog");
            QVERIFY(dialog);
            setReferenceMethod(dialog, SearchMethod::Occurrences);
            dialog->findChild<QPushButton *>("runReferenceLookup")->click();
            auto *results = window.findChild<QTreeWidget *>("referenceResults");
            QTRY_COMPARE_WITH_TIMEOUT(results->topLevelItemCount(), query == "count" ? 1 : 2,
                                      10000);
        }
        window.findChild<QAction *>("closeFile")->trigger();
        QVERIFY(!window.findChild<QWidget *>("searchResultsPage"));
        ApplicationStateStore saved(personal.path());
        const auto cleared = saved.loadSession(dir.path());
        QVERIFY2(cleared.session.has_value(), qPrintable(cleared.error));
        QVERIFY(cleared.session->references.isEmpty());
        auto *service = window.findChild<LanguageServerClient *>();
        QVERIFY(service);
        service->stop();
        editor->selectRange(4, 9);
        editor->setFocus();
        window.findChild<QAction *>("lookupReferences")->trigger();
        auto *dialog = window.findChild<QDialog *>("lookupReferencesDialog");
        QVERIFY(dialog);
        setReferenceMethod(dialog, SearchMethod::References);
        dialog->findChild<QCheckBox *>("referenceReplaceResults")->setChecked(true);
        dialog->findChild<QPushButton *>("runReferenceLookup")->click();
        auto *results = window.findChild<QTreeWidget *>("referenceResults");
        QVERIFY(results);
        QTRY_VERIFY(
            window.findChild<QLabel *>("referenceResultsStatus")->text().contains("unavailable"));
        QCOMPARE(results->topLevelItemCount(), 0);
        QCOMPARE(window.findChild<QTreeWidget *>("outline")->topLevelItemCount(), 0);
        QVERIFY(window.close());
    }
    MainWindow restored(true, nullptr, config, personal.path());
    restored.show();
    restored.openStartupPaths({});
    QVERIFY(!restored.findChild<QTreeWidget *>("referenceResults"));
    QVERIFY(!restored.findChild<QAction *>("showSearchResults")->isEnabled());
    QVERIFY(restored.close());
}

void ReferenceTests::replacementCommitsAndPersists() {
    QTemporaryDir dir, personal;
    const auto path = dir.filePath("a.cpp");
    const auto config = personal.filePath("settings.json");
    const QByteArray source = "int old;\nint other;\nint fresh;\n";
    QVERIFY(writeSourceFile(path, source, false).isEmpty());
    {
        MainWindow window(true, nullptr, config, personal.path());
        window.show();
        QVERIFY(window.openFile(path));
        auto *editor = window.activeEditor();
        const auto start = [&](const QString &query, bool replace) {
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
            window.findChild<QTabWidget *>("sourceTabs")->setCurrentWidget(editor);
            editor->setFocus();
            window.findChild<QAction *>("lookupReferences")->trigger();
            auto *dialog = window.findChild<QDialog *>("lookupReferencesDialog");
            QVERIFY(dialog);
            setReferenceMethod(dialog, SearchMethod::Occurrences);
            auto *box = dialog->findChild<QCheckBox *>("referenceReplaceResults");
            QVERIFY(box);
            QVERIFY(!box->isChecked());
            box->setChecked(replace);
            dialog->findChild<QLineEdit *>("referenceQuery")->setText(query);
            dialog->findChild<QPushButton *>("runReferenceLookup")->click();
        };
        start("old", false);
        auto *results = window.findChild<QTreeWidget *>("referenceResults");
        QTRY_COMPARE_WITH_TIMEOUT(results->topLevelItemCount(), 1, 10000);
        start("other", false);
        QTRY_COMPARE_WITH_TIMEOUT(results->topLevelItemCount(), 2, 10000);
        start("fresh", true);
        QTRY_COMPARE_WITH_TIMEOUT(results->topLevelItemCount(), 1, 10000);
        QCOMPARE(results->topLevelItem(0)->data(0, ReferenceItemRole::Query).toString(), "fresh");
        QCOMPARE(results->topLevelItem(0)->childCount(), 1);
        ApplicationStateStore saved(personal.path());
        const auto session = saved.loadSession(dir.path());
        QVERIFY2(session.session.has_value(), qPrintable(session.error));
        QCOMPARE(session.session->references.size(), 1);
        QCOMPARE(session.session->references.first().query, "fresh");
        start("missing", true);
        QTRY_COMPARE_WITH_TIMEOUT(
            results->topLevelItem(0)->data(0, ReferenceItemRole::Query).toString(),
            QString("missing"), 10000);
        QCOMPARE(results->topLevelItemCount(), 1);
        QCOMPARE(results->topLevelItem(0)->childCount(), 0);
        QVERIFY(window.close());
    }
    MainWindow restored(true, nullptr, config, personal.path());
    restored.show();
    restored.openStartupPaths({});
    auto *results = restored.findChild<QTreeWidget *>("referenceResults");
    QVERIFY(results);
    QCOMPARE(results->topLevelItemCount(), 1);
    QCOMPARE(results->topLevelItem(0)->data(0, ReferenceItemRole::Query).toString(), "missing");
    QCOMPARE(results->topLevelItem(0)->childCount(), 0);
    QVERIFY(restored.close());
}

void ReferenceTests::incompleteReplacementKeepsHistory() {
    QTemporaryDir dir;
    const auto path = dir.filePath("a.cpp");
    const auto many = dir.filePath("many.txt");
    QVERIFY(writeSourceFile(path, "int old;\n", false).isEmpty());
    QVERIFY(writeSourceFile(many, QByteArray("count\n").repeated(10003), false).isEmpty());
    MainWindow window(false);
    window.show();
    QVERIFY(window.openFile(path));
    auto *oldEditor = window.activeEditor();
    oldEditor->selectRange(4, 7);
    oldEditor->setFocus();
    window.findChild<QAction *>("lookupReferences")->trigger();
    auto *dialog = window.findChild<QDialog *>("lookupReferencesDialog");
    setReferenceMethod(dialog, SearchMethod::Occurrences);
    dialog->findChild<QPushButton *>("runReferenceLookup")->click();
    auto *results = window.findChild<QTreeWidget *>("referenceResults");
    QTRY_COMPARE_WITH_TIMEOUT(results->topLevelItemCount(), 1, 10000);
    QVERIFY(window.openFile(many));
    auto *editor = window.activeEditor();
    const auto startReplacement = [&] {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        window.findChild<QTabWidget *>("sourceTabs")->setCurrentWidget(editor);
        editor->selectRange(0, 5);
        editor->setFocus();
        window.findChild<QAction *>("lookupReferences")->trigger();
        auto *lookup = window.findChild<QDialog *>("lookupReferencesDialog");
        QVERIFY(lookup);
        setReferenceMethod(lookup, SearchMethod::Occurrences);
        lookup->findChild<QComboBox *>("referenceScope")->setCurrentIndex(1);
        lookup->findChild<QCheckBox *>("referenceSkipComments")->setChecked(false);
        lookup->findChild<QCheckBox *>("referenceStrings")->setChecked(true);
        lookup->findChild<QCheckBox *>("referenceReplaceResults")->setChecked(true);
        lookup->findChild<QPushButton *>("runReferenceLookup")->click();
    };
    startReplacement();
    auto *more = window.findChild<QAction *>("moreReferenceResults");
    QTRY_VERIFY_WITH_TIMEOUT(more->isEnabled(), 10000);
    QCOMPARE(results->topLevelItemCount(), 1);
    QCOMPARE(results->topLevelItem(0)->data(0, ReferenceItemRole::Query).toString(), "old");
    auto *cancel = window.findChild<QAction *>("cancelReferenceSearch");
    QVERIFY(cancel->isEnabled());
    cancel->trigger();
    QVERIFY(!more->isEnabled());
    QCOMPARE(results->topLevelItemCount(), 1);
    startReplacement();
    QTRY_VERIFY_WITH_TIMEOUT(more->isEnabled(), 10000);
    window.findChild<QAction *>("closeFile")->trigger();
    window.findChild<QAction *>("showSearchResults")->trigger();
    QVERIFY(!window.findChild<QTreeWidget *>("referenceResults"));
    startReplacement();
    results = window.findChild<QTreeWidget *>("referenceResults");
    more = window.findChild<QAction *>("moreReferenceResults");
    QTRY_VERIFY_WITH_TIMEOUT(more->isEnabled(), 10000);
    more->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(
        results->topLevelItemCount() > 0 && !more->isEnabled() &&
            results->topLevelItem(0)->data(0, ReferenceItemRole::Query).toString() == "count",
        15000);
    QCOMPARE(results->topLevelItemCount(), 2);
    QCOMPARE(results->topLevelItem(0)->childCount() + results->topLevelItem(1)->childCount(),
             10003);
    QCOMPARE(oldEditor->send(SCI_MARKERGET, 0), 0);
}

void ReferenceTests::replacementSaveFailureKeepsHistory() {
    QTemporaryDir dir, personal;
    const auto path = dir.filePath("a.cpp");
    const auto config = personal.filePath("settings.json");
    const QByteArray source = "int old;\nint fresh;\n";
    QVERIFY(writeSourceFile(path, source, false).isEmpty());
    MainWindow window(true, nullptr, config, personal.path());
    window.show();
    QVERIFY(window.openFile(path));
    auto *editor = window.activeEditor();
    editor->selectRange(4, 7);
    editor->setFocus();
    window.findChild<QAction *>("lookupReferences")->trigger();
    auto *dialog = window.findChild<QDialog *>("lookupReferencesDialog");
    setReferenceMethod(dialog, SearchMethod::Occurrences);
    dialog->findChild<QPushButton *>("runReferenceLookup")->click();
    auto *results = window.findChild<QTreeWidget *>("referenceResults");
    QTRY_COMPARE_WITH_TIMEOUT(results->topLevelItemCount(), 1, 10000);
    QVERIFY(window.close());
    window.show();
    ApplicationStateStore store(personal.path());
    QLockFile lock(store.sessionPath(dir.path()) + ".lock");
    QVERIFY(lock.tryLock(0));
    window.findChild<QTabWidget *>("sourceTabs")->setCurrentWidget(editor);
    editor->selectRange(source.indexOf("fresh"), source.indexOf("fresh") + 5);
    editor->setFocus();
    window.findChild<QAction *>("lookupReferences")->trigger();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    dialog = window.findChild<QDialog *>("lookupReferencesDialog");
    setReferenceMethod(dialog, SearchMethod::Occurrences);
    dialog->findChild<QCheckBox *>("referenceReplaceResults")->setChecked(true);
    dialog->findChild<QPushButton *>("runReferenceLookup")->click();
    QTRY_VERIFY_WITH_TIMEOUT(window.findChild<QLabel *>("referenceResultsStatus")
                                 ->text()
                                 .contains("Could not save the replacement"),
                             10000);
    QCOMPARE(results->topLevelItemCount(), 1);
    QCOMPARE(results->topLevelItem(0)->data(0, ReferenceItemRole::Query).toString(), "old");
    const auto session = store.loadSession(dir.path());
    QVERIFY2(session.session.has_value(), qPrintable(session.error));
    QCOMPARE(session.session->references.size(), 1);
    QCOMPARE(session.session->references.first().query, "old");
    lock.unlock();
    QVERIFY(window.close());
}

void ReferenceTests::closeResultsWithOpenMenuAndSearchAgain() {
    QTemporaryDir dir;
    const auto path = dir.filePath("a.cpp");
    const QByteArray source = "int first;\nint second;\n";
    QVERIFY(writeSourceFile(path, source, false).isEmpty());
    MainWindow window(false);
    window.show();
    QVERIFY(window.openFile(path));
    auto *editor = window.activeEditor();
    editor->selectRange(4, 9);
    editor->setFocus();
    window.findChild<QAction *>("lookupReferences")->trigger();
    auto *dialog = window.findChild<QDialog *>("lookupReferencesDialog");
    setReferenceMethod(dialog, SearchMethod::Occurrences);
    dialog->findChild<QPushButton *>("runReferenceLookup")->click();
    auto *results = window.findChild<QTreeWidget *>("referenceResults");
    QTRY_COMPARE_WITH_TIMEOUT(results->topLevelItemCount(), 1, 10000);
    bool restarted = false;
    QTimer::singleShot(0, &window, [&] {
        auto *tabs = window.findChild<QTabWidget *>("sourceTabs");
        auto *bar = window.findChild<QTabBar *>("documentBar");
        bar->tabCloseRequested(tabs->indexOf(window.findChild<QWidget *>("searchResultsPage")));
        editor->selectRange(source.indexOf("second"), source.indexOf("second") + 6);
        editor->setFocus();
        window.findChild<QAction *>("lookupReferences")->trigger();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        auto *lookup = window.findChild<QDialog *>("lookupReferencesDialog");
        QVERIFY(lookup);
        setReferenceMethod(lookup, SearchMethod::Occurrences);
        lookup->findChild<QPushButton *>("runReferenceLookup")->click();
        restarted = true;
    });
    results->customContextMenuRequested(QPoint(10, 10));
    QTRY_VERIFY_WITH_TIMEOUT(restarted, 10000);
    results = window.findChild<QTreeWidget *>("referenceResults");
    QVERIFY(results);
    QTRY_COMPARE_WITH_TIMEOUT(results->topLevelItemCount(), 1, 10000);
    QCOMPARE(results->topLevelItem(0)->data(0, ReferenceItemRole::Query).toString(), "second");
}

void ReferenceTests::closingPendingSearchIgnoresLateResults() {
    QTemporaryDir dir;
    const auto path = dir.filePath("a.cpp");
    const QByteArray source = "int first;\nint second;\n";
    QVERIFY(writeSourceFile(path, source, false).isEmpty());
    MainWindow window(false);
    window.show();
    QVERIFY(window.openFile(path));
    auto *editor = window.activeEditor();
    const auto start = [&](const QByteArray &word) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        window.findChild<QTabWidget *>("sourceTabs")->setCurrentWidget(editor);
        editor->selectRange(source.indexOf(word), source.indexOf(word) + word.size());
        editor->setFocus();
        window.findChild<QAction *>("lookupReferences")->trigger();
        auto *lookup = window.findChild<QDialog *>("lookupReferencesDialog");
        QVERIFY(lookup);
        setReferenceMethod(lookup, SearchMethod::Occurrences);
        lookup->findChild<QPushButton *>("runReferenceLookup")->click();
    };
    start("first");
    auto *results = window.findChild<QTreeWidget *>("referenceResults");
    QTRY_COMPARE_WITH_TIMEOUT(results->topLevelItemCount(), 1, 10000);
    for (int attempt = 0; attempt < 5; ++attempt) {
        start("first");
        QPointer<ReferenceResultsView> previous =
            window.findChild<ReferenceResultsView *>("referenceResults");
        previous->topLevelItem(0)->child(0)->setText(0, "Schedule a pending view rebuild");
        auto *tabs = window.findChild<QTabWidget *>("sourceTabs");
        window.findChild<QTabBar *>("documentBar")
            ->tabCloseRequested(tabs->indexOf(window.findChild<QWidget *>("searchResultsPage")));
        QVERIFY(!previous);
        QVERIFY(!window.findChild<QAction *>("showSearchResults")->isEnabled());
        start("second");
        results = window.findChild<QTreeWidget *>("referenceResults");
        QVERIFY(results);
        QTRY_COMPARE_WITH_TIMEOUT(results->topLevelItemCount(), 1, 10000);
        QCOMPARE(results->topLevelItem(0)->data(0, ReferenceItemRole::Query).toString(), "second");
        QTest::qWait(20);
        QCOMPARE(results->topLevelItemCount(), 1);
        QCOMPARE(results->topLevelItem(0)->data(0, ReferenceItemRole::Query).toString(), "second");
    }
    QCOMPARE(readSourceFile(path).bytes, source);
}

void ReferenceTests::searchDialogSurvivesResultsClose() {
    QTemporaryDir dir;
    const auto path = dir.filePath("a.cpp");
    const QByteArray source = "int first;\nint second;\n";
    QVERIFY(writeSourceFile(path, source, false).isEmpty());
    MainWindow window(false);
    window.show();
    QVERIFY(window.openFile(path));
    auto *editor = window.activeEditor();
    editor->selectRange(4, 9);
    editor->setFocus();
    window.findChild<QAction *>("lookupReferences")->trigger();
    auto *lookup = window.findChild<QDialog *>("lookupReferencesDialog");
    setReferenceMethod(lookup, SearchMethod::Occurrences);
    lookup->findChild<QPushButton *>("runReferenceLookup")->click();
    auto *results = window.findChild<QTreeWidget *>("referenceResults");
    QTRY_COMPARE_WITH_TIMEOUT(results->topLevelItemCount(), 1, 10000);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    window.findChild<QAction *>("lookupReferences")->trigger();
    lookup = window.findChild<QDialog *>("lookupReferencesDialog");
    QVERIFY(lookup);
    setReferenceMethod(lookup, SearchMethod::Occurrences);
    lookup->findChild<QLineEdit *>("referenceQuery")->setText("second");
    auto *tabs = window.findChild<QTabWidget *>("sourceTabs");
    window.findChild<QTabBar *>("documentBar")
        ->tabCloseRequested(tabs->indexOf(window.findChild<QWidget *>("searchResultsPage")));
    QVERIFY(lookup->isVisible());
    lookup->findChild<QPushButton *>("runReferenceLookup")->click();
    results = window.findChild<QTreeWidget *>("referenceResults");
    QVERIFY(results);
    QTRY_COMPARE_WITH_TIMEOUT(results->topLevelItemCount(), 1, 10000);
    QCOMPARE(results->topLevelItem(0)->data(0, ReferenceItemRole::Query).toString(), "second");
}

void ReferenceTests::closingResultsCancelsTouchConfirmation() {
    QTemporaryDir dir;
    const auto path = dir.filePath("a.cpp");
    const QByteArray source = "int count;\n";
    QVERIFY(writeSourceFile(path, source, false).isEmpty());
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadWrite));
    const auto oldTime = QDateTime::currentDateTimeUtc().addDays(-2);
    QVERIFY(file.setFileTime(oldTime, QFileDevice::FileModificationTime));
    file.close();
    MainWindow window(false);
    window.show();
    QVERIFY(window.openFile(path));
    window.activeEditor()->selectRange(4, 9);
    window.activeEditor()->setFocus();
    window.findChild<QAction *>("lookupReferences")->trigger();
    auto *lookup = window.findChild<QDialog *>("lookupReferencesDialog");
    setReferenceMethod(lookup, SearchMethod::Occurrences);
    lookup->findChild<QPushButton *>("runReferenceLookup")->click();
    auto *results = window.findChild<QTreeWidget *>("referenceResults");
    QTRY_COMPARE_WITH_TIMEOUT(results->topLevelItemCount(), 1, 10000);
    bool closed = false;
    QTimer::singleShot(0, &window, [&] {
        auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        QVERIFY(message);
        window.findChild<QAction *>("closeFile")->trigger();
        closed = !window.findChild<QWidget *>("searchResultsPage");
        message->button(QMessageBox::Yes)->click();
    });
    window.findChild<QAction *>("touchReferenceFiles")->trigger();
    QVERIFY(closed);
    QVERIFY(!window.findChild<QWidget *>("searchResultsPage"));
    QCOMPARE(QFileInfo(path).lastModified().toUTC(), oldTime);
    QCOMPARE(readSourceFile(path).bytes, source);
}

QTEST_MAIN(ReferenceTests)
#include "ReferenceTests.moc"

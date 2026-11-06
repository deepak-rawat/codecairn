#include "CodeEditor.h"
#include "FileIO.h"
#include "MainWindow.h"
#include "SourceAnalysis.h"
#include "UiAppearance.h"
#include "TestFixtures.h"
#include "TestStorage.h"
#include "WorkspaceOptionsStore.h"

#include <QCheckBox>
#include <QAction>
#include <QFile>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QStatusBar>
#include <QMenuBar>
#include <QScrollBar>
#include <QFontDatabase>
#include <QSignalSpy>
#include <QSplitter>
#include <QTabWidget>
#include <QTabBar>
#include <QToolBar>
#include <QTreeWidgetItemIterator>
#include <QTemporaryDir>
#include <QTest>
#include <QTreeWidget>

class PrototypeTests : public QObject {
    Q_OBJECT
    TestStorage storage_;
private slots:
    void initTestCase();
    void extractsCDeclarations();
    void extractsCppHierarchy();
    void recoversIncompleteSyntax();
    void usesUtf8ByteRanges();
    void rejectsUnsupportedAndOversizedAnalysis();
    void fileRoundTripAndEncodingErrors();
    void stylesDoNotEditDocument();
    void referenceSyntaxRoles_data();
    void referenceSyntaxRoles();
    void referenceBindingsRespectScopes();
    void uniformLineHeightIsExplicit();
    void readOnlyPreviewIsIndependent();
    void windowNavigationAndPinning();
    void latestUnsavedAnalysisWins();
    void savesOriginalLineEndings();
    void scansFilesWithoutOpeningTabs();
    void referenceLayoutAndTypography();
    void outlineIncludesAndConditionals();
    void outlineStateSurvivesEditsAndTabs();
    void browserCategoriesAndFolders();
    void symbolCategoryTree();
    void globalTabsAndSearch();
    void contextHistoryAndOpen();
    void sourceBackgroundIsUniform();
    void capturesTypographyPrototype();
};

void PrototypeTests::initTestCase() {
#ifdef Q_OS_WIN
    QApplication::setFont(QFont("Segoe UI", 9));
#endif
}

void PrototypeTests::extractsCDeclarations() {
    const QByteArray source =
        "#define LIMIT 10\n"
        "typedef void (*Callback)(unsigned rate);\n"
        "typedef struct { int minimum; int maximum; } Config;\n"
        "int update_rate(const Config *config);\n"
        "int update_rate(const Config *config) { int local = 1; return local; }\n";
    const auto result = analyzeSource(source, "fixture.c");
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QVERIFY(result.supported);
    QVERIFY(!result.syntaxErrors);
    QCOMPARE(result.styles.size(), source.size());
    QStringList names;
    for (const auto &symbol : result.symbols) {
        if (!symbol.local)
            names << symbol.name;
        QCOMPARE(QString::fromUtf8(source.mid(symbol.nameStart, symbol.nameEnd - symbol.nameStart)),
                 symbol.name);
        if (symbol.name == "Callback")
            QCOMPARE(symbol.kind, "Type");
        if (symbol.name == "minimum") {
            QVERIFY(symbol.parent >= 0);
            QCOMPARE(result.symbols[symbol.parent].name, "Config");
        }
    }
    QVERIFY(names.contains("LIMIT"));
    QVERIFY(names.contains("Config"));
    QVERIFY(names.contains("Callback"));
    QCOMPARE(names.count("update_rate"), 2);
    QVERIFY(!names.contains("local"));
}

void PrototypeTests::extractsCppHierarchy() {
    const QByteArray source =
        "namespace demo {\n"
        "class Worker { public: int run(int n) { return n; } private: int count; };\n"
        "enum class State { Ready, Busy };\n"
        "using Size = unsigned;\n"
        "int first, second;\n"
        "void (*callback)(int);\n"
        "}\n";
    const auto result = analyzeSource(source, "fixture.hpp");
    QVERIFY(result.error.isEmpty());
    QVERIFY(!result.syntaxErrors);
    QStringList names;
    for (const auto &symbol : result.symbols) {
        names << symbol.name;
        if (symbol.name == "run") {
            QCOMPARE(symbol.kind, "Function");
            QCOMPARE(result.symbols[symbol.parent].name, "Worker");
        }
        if (symbol.name == "callback")
            QCOMPARE(symbol.kind, "Variable");
    }
    for (const auto &name : {"demo", "Worker", "run", "count", "State", "Ready", "Busy", "Size",
                             "first", "second", "callback"})
        QVERIFY2(names.contains(name), name);
}

void PrototypeTests::recoversIncompleteSyntax() {
    const auto result = analyzeSource("struct Config { int field;\n", "partial.cpp");
    QVERIFY(result.error.isEmpty());
    QVERIFY(result.syntaxErrors);
    QVERIFY(!result.styles.isEmpty());
}

void PrototypeTests::usesUtf8ByteRanges() {
    const QByteArray source =
        "// " + QByteArray::fromHex("e4bda0e5a5bd") + "\r\nint calculate();\r\n";
    const auto result = analyzeSource(source, "utf8.cpp");
    QCOMPARE(result.symbols.size(), 1);
    QCOMPARE(result.symbols[0].nameStart, source.indexOf("calculate"));
    QCOMPARE(result.symbols[0].line, 1);
    QCOMPARE(result.styles.size(), source.size());
}

void PrototypeTests::rejectsUnsupportedAndOversizedAnalysis() {
    const auto plain = analyzeSource("Some notes\n", "notes.txt");
    QVERIFY(!plain.supported);
    QVERIFY(plain.error.isEmpty());
    QVERIFY(plain.symbols.isEmpty());
    const auto large = analyzeSource(QByteArray(MaximumDocumentBytes + 1, 'x'), "large.cpp");
    QVERIFY(!large.error.isEmpty());
}

void PrototypeTests::fileRoundTripAndEncodingErrors() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = dir.filePath("roundtrip.cpp");
    const QByteArray text = "int value;\r\n\t// " + QByteArray::fromHex("c3a9") + "\r\n";
    QVERIFY(writeSourceFile(path, text, true).isEmpty());
    const auto file = readSourceFile(path);
    QVERIFY(file.error.isEmpty());
    QVERIFY(file.utf8Bom);
    QCOMPARE(file.bytes, text);
    for (const QByteArray &invalid :
         {QByteArray("a\0b", 3), QByteArray::fromHex("c3"), QByteArray::fromHex("ff")}) {
        QVERIFY(writeSourceFile(path, invalid, false).isEmpty());
        QVERIFY(!readSourceFile(path).error.isEmpty());
    }
    QVERIFY(!readSourceFile(dir.filePath("missing.cpp")).error.isEmpty());
    QVERIFY(!writeSourceFile(QDir(dir.filePath("missing")).filePath("child.cpp"), "test", false)
                 .isEmpty());
}

void PrototypeTests::stylesDoNotEditDocument() {
    CodeEditor editor;
    const QByteArray text = "int calculate(int n) {\r\n\treturn n + 1;\r\n}\r\n";
    editor.loadBytes(text);
    editor.applyStyles(analyzeSource(text, "test.cpp").styles);
    editor.selectRange(4, 13);
    const auto start = editor.send(SCI_GETSELECTIONSTART);
    const auto end = editor.send(SCI_GETSELECTIONEND);
    editor.applyTypography(
        {QFontDatabase::systemFont(QFontDatabase::FixedFont).family(), 11, 1.4, false, true});
    QCOMPARE(editor.bytes(), text);
    QCOMPARE(editor.send(SCI_GETSELECTIONSTART), start);
    QCOMPARE(editor.send(SCI_GETSELECTIONEND), end);
    QVERIFY(!editor.isModified());
    QVERIFY(!editor.send(SCI_CANUNDO));
    editor.sends(SCI_APPENDTEXT, 3, "//x");
    QVERIFY(editor.isModified());
    editor.applyTypography(
        {QFontDatabase::systemFont(QFontDatabase::FixedFont).family(), 12, 1.0, true, false});
    editor.send(SCI_UNDO);
    QCOMPARE(editor.bytes(), text);
}

void PrototypeTests::referenceSyntaxRoles_data() {
    QTest::addColumn<QString>("fileName");
    QTest::newRow("c") << QString("transport.c");
    QTest::newRow("cpp") << QString("transport.cpp");
    QTest::newRow("header") << QString("transport.h");
}

void PrototypeTests::referenceSyntaxRoles() {
    QFETCH(QString, fileName);
    const QByteArray source =
        "#define ENABLED 1\n"
        "typedef struct Transport { int timer; } Transport;\n"
        "enum Mode { Ready, RTC_ICE_MODE_FULL };\n"
        "static Transport *rtc_transport_create(Transport *worker, const Transport *cfg) {\n"
        "    Transport *transport = (Transport *)calloc(1, sizeof(*transport));\n"
        "    if (!worker || !cfg) return NULL;\n"
        "    transport->timer = RTC_WORKER_TIMER_INVALID;\n"
        "    int rc = rtc_init(transport, cfg->timer);\n"
        "    int mode = Ready;\n"
        "    free(transport);\n"
        "    // worker transport RTC_OK\n"
        "    const char *message = \"worker RTC_OK\";\n"
        "    return transport;\n"
        "}\n";
    const auto analysis = analyzeSource(source, fileName);
    QVERIFY2(analysis.error.isEmpty(), qPrintable(analysis.error));
    QVERIFY(!analysis.syntaxErrors);
    CodeEditor editor;
    editor.loadBytes(source);
    editor.applyStyles(analysis.styles);
    const auto check = [&](const QByteArray &context, const QByteArray &token, TextStyle expected) {
        const auto start = source.indexOf(context);
        QVERIFY2(start >= 0, context.constData());
        const auto offset = context.indexOf(token);
        QVERIFY2(offset >= 0, token.constData());
        for (qsizetype i = 0; i < token.size(); ++i) {
            const auto position = start + offset + i;
            QCOMPARE(static_cast<TextStyle>(analysis.styles[position]), expected);
            QCOMPARE(editor.send(SCI_GETSTYLEAT, position), static_cast<int>(expected));
        }
    };
    check("#define ENABLED", "#define", TextStyle::Preprocessor);
    check("static Transport", "static", TextStyle::Primitive);
    check("rtc_transport_create(", "rtc_transport_create", TextStyle::Function);
    check("Transport *worker", "worker", TextStyle::Parameter);
    check("!worker", "worker", TextStyle::Parameter);
    check("const Transport *cfg", "cfg", TextStyle::Parameter);
    check("cfg->timer", "cfg", TextStyle::Parameter);
    check("Transport *transport =", "transport", TextStyle::VariableDeclaration);
    check("return transport;", "transport", TextStyle::Variable);
    check("transport->timer", "timer", TextStyle::Field);
    check("RTC_WORKER_TIMER_INVALID", "RTC_WORKER_TIMER_INVALID", TextStyle::Constant);
    check("return NULL", "NULL", TextStyle::Constant);
    check("mode = Ready", "Ready", TextStyle::Constant);
    check("calloc(1", "1", TextStyle::Number);
    check("calloc(1", "calloc", TextStyle::LibraryFunction);
    check("free(transport)", "free", TextStyle::LibraryFunction);
    check("sizeof(*transport)", "sizeof", TextStyle::Primitive);
    check("rtc_init(transport", "rtc_init", TextStyle::FunctionReference);
    check("if (!worker", "if", TextStyle::Keyword);
    check("!worker ||", "||", TextStyle::Operator);
    check("cfg->timer", "->", TextStyle::Operator);
    check("free(transport);", ";", TextStyle::Punctuation);
    check("// worker transport RTC_OK", "RTC_OK", TextStyle::Comment);
    check("\"worker RTC_OK\"", "RTC_OK", TextStyle::String);
    QVERIFY(!editor.isModified());
}

void PrototypeTests::referenceBindingsRespectScopes() {
    const QByteArray source = "void first(int value, void (*callback)(int nested)) {\n"
                              "    callback(value);\n"
                              "    { int value = 1; use(value); }\n"
                              "    use(value);\n"
                              "    use(nested);\n"
                              "    for (int index = 0; index < 1; ++index) use(index);\n"
                              "    use(index);\n"
                              "    int RTC_OK = 1; use(RTC_OK);\n"
                              "}\n"
                              "void second(void) { int value = 2; use(value); }\n"
                              "void third(void) { use(value); }\n";
    const auto analysis = analyzeSource(source, "scopes.cpp");
    QVERIFY(!analysis.syntaxErrors);
    const auto styleAt = [&](const QByteArray &context, const QByteArray &token) {
        return static_cast<TextStyle>(
            analysis.styles[source.indexOf(context) + context.indexOf(token)]);
    };
    QCOMPARE(styleAt("callback(value)", "callback"), TextStyle::Parameter);
    QCOMPARE(styleAt("callback(value)", "value"), TextStyle::Parameter);
    QCOMPARE(styleAt("use(value); }", "value"), TextStyle::Variable);
    QCOMPARE(styleAt("    use(value);", "value"), TextStyle::Parameter);
    QCOMPARE(styleAt("use(nested)", "nested"), TextStyle::Plain);
    QCOMPARE(styleAt("++index", "index"), TextStyle::Variable);
    QCOMPARE(styleAt("    use(index);", "index"), TextStyle::Plain);
    QCOMPARE(styleAt("use(RTC_OK)", "RTC_OK"), TextStyle::Variable);
    QCOMPARE(styleAt("third(void) { use(value)", "value"), TextStyle::Plain);
}

void PrototypeTests::uniformLineHeightIsExplicit() {
    CodeEditor editor;
    const auto family = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    editor.loadBytes("int function();\n// normal comment\n");
    editor.applyTypography({family, 12, 1.6, false, false});
    const int emphasizedHeight = editor.lineHeight();
    QCOMPARE(editor.send(SCI_TEXTHEIGHT, 0), editor.send(SCI_TEXTHEIGHT, 1));
    editor.applyTypography({family, 12, 1.6, true, false});
    QVERIFY(editor.lineHeight() < emphasizedHeight);
}

void PrototypeTests::readOnlyPreviewIsIndependent() {
    CodeEditor editor;
    CodeEditor preview;
    editor.loadBytes("int original;\n");
    preview.setReadOnly(true);
    preview.loadBytes(editor.bytes());
    preview.selectRange(4, 12);
    preview.sends(SCI_REPLACESEL, 0, "changed");
    QCOMPARE(preview.bytes(), editor.bytes());
    editor.selectRange(4, 12);
    editor.sends(SCI_REPLACESEL, 0, "updated");
    QVERIFY(editor.bytes() != preview.bytes());
    QVERIFY(preview.send(SCI_GETREADONLY));
    QVERIFY(!preview.isModified());
}

void PrototypeTests::windowNavigationAndPinning() {
    MainWindow window(false);
    window.show();
    QSignalSpy ready(&window, &MainWindow::analysisReady);
    QVERIFY(openTypographyFixture(window));
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() > 0, 10000);
    QCOMPARE(window.documentCount(), 1);
    auto *outline = window.findChild<QTreeWidget *>("outline");
    QVERIFY(outline && outline->topLevelItemCount() > 0);
    auto *editor = window.activeEditor();
    editor->setFocus();
    QTest::qWait(100);
    QWidget *focused = QApplication::focusWidget();
    window.previewSymbol(0);
    QCOMPARE(QApplication::focusWidget(), focused);
    QCOMPARE(window.contextEditor()->bytes(), editor->bytes());
    QVERIFY(window.contextEditor()->send(SCI_GETFIRSTVISIBLELINE) <=
            window.contextEditor()->send(SCI_LINEFROMPOSITION,
                                         window.contextEditor()->send(SCI_GETSELECTIONSTART)));
    QCOMPARE(window.documentCount(), 1);
    auto *pin = window.findChild<QAction *>("pinContext");
    pin->setChecked(true);
    const QByteArray snapshot = window.contextEditor()->bytes();
    editor->sends(SCI_APPENDTEXT, 17, "\nint added_name;\n");
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() > 1, 10000);
    QCOMPARE(window.contextEditor()->bytes(), snapshot);
    pin->setChecked(false);
    QTRY_COMPARE(window.contextEditor()->bytes(), editor->bytes());
    QVERIFY(window.contextEditor()->send(SCI_GETREADONLY));
    const auto splitters = window.findChildren<QSplitter *>();
    QCOMPARE(splitters.size(), 2);
    for (auto *splitter : splitters)
        QVERIFY(!splitter->childrenCollapsible());
}

void PrototypeTests::latestUnsavedAnalysisWins() {
    MainWindow window(false);
    QSignalSpy ready(&window, &MainWindow::analysisReady);
    QVERIFY(openTypographyFixture(window));
    auto *editor = window.activeEditor();
    editor->send(SCI_SELECTALL);
    editor->sends(SCI_REPLACESEL, 0, "int superseded();\n");
    editor->send(SCI_SELECTALL);
    editor->sends(SCI_REPLACESEL, 0, "int latest_version();\n");
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() > 0, 10000);
    auto *outline = window.findChild<QTreeWidget *>("outline");
    QTRY_COMPARE_WITH_TIMEOUT(outline->topLevelItemCount(), 1, 10000);
    QCOMPARE(outline->topLevelItem(0)->text(0), "latest_version");
    QCOMPARE(window.contextEditor()->bytes(), editor->bytes());
}

void PrototypeTests::savesOriginalLineEndings() {
    QTemporaryDir dir;
    const auto path = dir.filePath("source.cpp");
    const QByteArray original = "int count;\r\n";
    QVERIFY(writeSourceFile(path, original, true).isEmpty());
    MainWindow window(false);
    QSignalSpy analyzed(&window, &MainWindow::analysisReady);
    QSignalSpy indexed(&window, &MainWindow::indexReady);
    QVERIFY(window.openFile(path));
    // Startup readers can prevent atomic replacement on Windows.
    QTRY_VERIFY_WITH_TIMEOUT(analyzed.count() > 0, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(indexed.count() > 0, 10000);
    auto *editor = window.activeEditor();
    editor->sends(SCI_APPENDTEXT, 13, "// appended\r\n");
    QVERIFY(window.saveCurrent());
    const auto saved = readSourceFile(path);
    QCOMPARE(saved.bytes, original + "// appended\r\n");
    QVERIFY(saved.utf8Bom);
    QVERIFY(!editor->isModified());
}

void PrototypeTests::scansFilesWithoutOpeningTabs() {
    QTemporaryDir dir;
    QVERIFY(writeSourceFile(dir.filePath("one.cpp"), "int one;\n", false).isEmpty());
    QVERIFY(QDir(dir.path()).mkdir("build"));
    QVERIFY(writeSourceFile(dir.filePath("build/ignored.cpp"), "int ignored;\n", false).isEmpty());
    MainWindow window(false);
    window.openProject(dir.path());
    auto *files = window.findChild<QTreeWidget *>("fileList");
    QTRY_COMPARE_WITH_TIMEOUT(files->topLevelItemCount(), 1, 10000);
    QCOMPARE(window.documentCount(), 0);
    files->setCurrentItem(files->topLevelItem(0));
    QTRY_COMPARE_WITH_TIMEOUT(window.contextEditor()->bytes(), QByteArray("int one;\n"), 10000);
    QCOMPARE(window.documentCount(), 0);
}

void PrototypeTests::referenceLayoutAndTypography() {
    MainWindow window(false);
    window.resize(1446, 742);
    window.show();
    QSignalSpy ready(&window, &MainWindow::analysisReady);
    QVERIFY(openTypographyFixture(window));
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() > 0, 10000);
    auto *outline = window.findChild<QTreeWidget *>("outline");
    auto *browser = window.findChild<QTabWidget *>("browserTabs");
    auto *tabs = window.findChild<QTabBar *>("documentBar");
    auto *main = window.findChild<QSplitter *>("mainSplitter");
    auto *right = window.findChild<QSplitter *>("rightSplitter");
    auto *toolbar = window.findChild<QToolBar *>("mainToolbar");
    QVERIFY(outline->isHeaderHidden());
    QCOMPARE(outline->columnCount(), 1);
    QVERIFY(!outline->alternatingRowColors());
    QCOMPARE(browser->count(), 4);
    QCOMPARE(browser->tabText(0), "Project Files");
    QCOMPARE(browser->tabText(1), "Project Symbols");
    QCOMPARE(browser->tabText(2), "Folders");
    QCOMPARE(browser->tabText(3), "Symbol Categories");
    QCOMPARE(tabs->count(), 1);
    QCOMPARE(tabs->mapToParent(tabs->tabRect(0).topLeft()).x(), 0);
    QCOMPARE(toolbar->height(), UiAppearance::bandHeight(defaultAppSettings()));
    QVERIFY(!toolbar->isMovable());
    QVERIFY(toolbar->findChildren<QLineEdit *>().isEmpty());
    QCOMPARE(window.findChild<QTreeWidget *>("fileList")->columnCount(), 3);
    const int rowHeight = outline->visualItemRect(outline->topLevelItem(0)).height();
    QVERIFY2(rowHeight >= 13 && rowHeight <= 16, qPrintable(QString::number(rowHeight)));
    QVERIFY(qAbs(main->sizes()[0] - 252) <= 4);
    QVERIFY(qAbs(main->sizes()[1] - 660) <= 4);
    QVERIFY(qAbs(right->sizes()[0] - right->sizes()[1]) <= 16);
    auto *editor = window.activeEditor();
    QCOMPARE(editor->send(SCI_GETMARGINWIDTHN, 0), 0);
    QCOMPARE(editor->send(SCI_STYLEGETBACK, STYLE_DEFAULT), 0x202020);
    QCOMPARE(editor->send(SCI_STYLEGETFORE, static_cast<int>(TextStyle::Type)), 0x00ffff);
    QCOMPARE(editor->send(SCI_STYLEGETFORE, static_cast<int>(TextStyle::Function)), 0xffffff);
    QCOMPARE(editor->send(SCI_STYLEGETSIZEFRACTIONAL, STYLE_DEFAULT),
             qRound(825.0 * 96.0 / editor->logicalDpiY()));
    QCOMPARE(editor->send(SCI_TEXTHEIGHT, 0), editor->send(SCI_TEXTHEIGHT, 1));
    const auto sourceBefore = editor->bytes();
    auto *numbers = window.findChild<QAction *>("lineNumbers");
    numbers->trigger();
    QVERIFY(editor->send(SCI_GETMARGINWIDTHN, 0) > 0);
    QCOMPARE(editor->send(SCI_GETMARGINWIDTHN, 0),
             window.contextEditor()->send(SCI_GETMARGINWIDTHN, 0));
    numbers->trigger();
    QCOMPARE(editor->send(SCI_GETMARGINWIDTHN, 0), 0);
    window.findChild<QAction *>("sourceAppearanceAction")->trigger();
    QVERIFY(window.findChild<QDialog *>("sourceAppearance")->isVisible());
    window.findChild<QDialog *>("sourceAppearance")->reject();
    QCOMPARE(editor->bytes(), sourceBefore);
    QVERIFY(!editor->isModified());
}

void PrototypeTests::outlineIncludesAndConditionals() {
    const QByteArray text = "#ifndef HEADER_H\n#define HEADER_H\n#include \"support.h\"\n"
                            "#if ENABLED\nint first();\n#else\nint second();\n#endif\n#endif\n";
    const auto result = analyzeSource(text, "outline.h");
    QVERIFY(result.error.isEmpty());
    int includes = 0, conditionals = 0;
    for (const auto &symbol : result.symbols) {
        if (symbol.kind == "Include") {
            ++includes;
            QVERIFY(symbol.outlineOnly);
            QVERIFY(symbol.name.contains("support.h"));
            QVERIFY(symbol.parent >= 0);
        }
        if (symbol.kind == "Conditional") {
            ++conditionals;
            QVERIFY(symbol.outlineOnly);
        }
        if (symbol.name == "first" || symbol.name == "second") {
            QVERIFY(!symbol.outlineOnly);
            QVERIFY(symbol.parent >= 0);
        }
    }
    QCOMPARE(includes, 1);
    QVERIFY(conditionals >= 5);
}

void PrototypeTests::outlineStateSurvivesEditsAndTabs() {
    QTemporaryDir dir;
    const auto path = dir.filePath("one.cpp");
    const auto second = dir.filePath("two.cpp");
    QVERIFY(
        writeSourceFile(path, "struct A { int one; int two; };\nint zebra();\n", false).isEmpty());
    QVERIFY(writeSourceFile(second, "int another();\n", false).isEmpty());
    MainWindow window(false);
    window.show();
    QSignalSpy ready(&window, &MainWindow::analysisReady);
    QVERIFY(window.openFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() > 0, 10000);
    auto *outline = window.findChild<QTreeWidget *>("outline");
    auto *editor = window.activeEditor();
    auto *type = outline->topLevelItem(0);
    QCOMPARE(type->text(0), "A");
    QVERIFY(type->isExpanded());
    type->setExpanded(false);
    outline->setCurrentItem(type);
    editor->sends(SCI_INSERTTEXT, 0, "// new first line\n");
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() > 1, 10000);
    QVERIFY(outline->currentItem());
    QCOMPARE(outline->currentItem()->text(0), "A");
    QVERIFY(!outline->topLevelItem(0)->isExpanded());
    window.findChild<QAction *>("sortName")->trigger();
    QVERIFY(!outline->topLevelItem(0)->isExpanded());
    QVERIFY(window.openFile(second));
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() > 2, 10000);
    QVERIFY(window.openFile(path));
    QCOMPARE(window.activeEditor(), editor);
    QVERIFY(outline->currentItem());
    QCOMPARE(outline->currentItem()->text(0), "A");
    QVERIFY(!outline->topLevelItem(0)->isExpanded());
    window.findChild<QAction *>("expandOutline")->trigger();
    QVERIFY(outline->topLevelItem(0)->isExpanded());
    editor->markSaved();
}

void PrototypeTests::browserCategoriesAndFolders() {
    QTemporaryDir dir;
    QVERIFY(QDir(dir.path()).mkdir("include"));
    const auto path = dir.filePath("include/types.h");
    QVERIFY(writeSourceFile(
                path, "#include <stddef.h>\nstruct Widget { int count; };\nint run();\n", false)
                .isEmpty());
    MainWindow window(false);
    window.show();
    window.openProject(dir.path());
    auto *files = window.findChild<QTreeWidget *>("fileList");
    QTRY_COMPARE_WITH_TIMEOUT(files->topLevelItemCount(), 1, 10000);
    QCOMPARE(files->topLevelItem(0)->text(1), "include");
    QCOMPARE(files->topLevelItem(0)->text(2).toLongLong(), QFileInfo(path).size());
    auto *folders = window.findChild<QTreeWidget *>("folderTree");
    QCOMPARE(folders->topLevelItemCount(), 1);
    auto *folder = folders->topLevelItem(0)->child(0);
    QCOMPARE(folder->text(0), "include");
    QCOMPARE(folder->child(0)->text(0), "types.h");
    QSignalSpy ready(&window, &MainWindow::analysisReady);
    QVERIFY(window.openFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() > 0, 10000);
    auto *symbols = window.findChild<QTreeWidget *>("symbolList");
    QCOMPARE(symbols->topLevelItemCount(), 3);
    for (int i = 0; i < symbols->topLevelItemCount(); ++i)
        QVERIFY(!symbols->topLevelItem(i)->text(0).contains("#include"));
    auto *categories = window.findChild<QTreeWidget *>("categoryTree");
    QCOMPARE(categories->headerItem()->text(0), "Symbol Type");
    QCOMPARE(categories->headerItem()->text(1), "File Name");
    QVERIFY(categories->rootIsDecorated());
    QCOMPARE(categories->topLevelItemCount(), 3);
    QCOMPARE(categories->topLevelItem(0)->text(0), "Structures (1)");
    QCOMPARE(categories->topLevelItem(1)->text(0), "Function Prototypes (1)");
    QCOMPARE(categories->topLevelItem(2)->text(0), "Data Members (1)");
    for (int i = 0; i < categories->topLevelItemCount(); ++i) {
        const auto *category = categories->topLevelItem(i);
        QCOMPARE(category->text(1), QString{});
        QCOMPARE(category->childCount(), 1);
        QVERIFY(!category->isExpanded());
        QCOMPARE(category->child(0)->text(1), "types.h");
    }
    QTreeWidgetItem *functions = nullptr;
    for (int i = 0; i < categories->topLevelItemCount(); ++i)
        if (categories->topLevelItem(i)->data(0, Qt::UserRole).toString() == "Function Prototypes")
            functions = categories->topLevelItem(i);
    QVERIFY(functions);
    functions->setExpanded(true);
    categories->itemActivated(functions, 0);
    QTRY_COMPARE_WITH_TIMEOUT(symbols->topLevelItemCount(), 1, 10000);
    QCOMPARE(window.findChild<QTabWidget *>("browserTabs")->currentIndex(), 1);
    QCOMPARE(categories->topLevelItemCount(), 3);
    QVERIFY(categories->topLevelItem(1)->isExpanded());
    auto *prototype = categories->topLevelItem(1)->child(0);
    QCOMPARE(prototype->text(0), "run");
    categories->setCurrentItem(prototype);
    categories->itemActivated(prototype, 0);
    const auto source = window.activeEditor()->bytes();
    QTRY_COMPARE_WITH_TIMEOUT(window.activeEditor()->send(SCI_GETSELECTIONSTART),
                              source.indexOf("int run"), 10000);
    QCOMPARE(window.activeEditor()->send(SCI_GETSELECTIONEND),
             source.indexOf("int run") + qstrlen("int run();"));
    window.findChild<QAction *>("clearCategory")->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(symbols->topLevelItemCount(), 3, 10000);
    QVERIFY(categories->topLevelItem(1)->isExpanded());
    QCOMPARE(categories->currentItem()->text(0), "run");
    for (int i = 0; i < symbols->topLevelItemCount(); ++i)
        QVERIFY(!symbols->topLevelItem(i)->isHidden());
    QVERIFY(window.findChild<QAction *>("refreshFiles")->isEnabled());
}

void PrototypeTests::symbolCategoryTree() {
    QTemporaryDir dir;
    QVERIFY(writeSourceFile(dir.filePath("categories.c"),
                            "#define LIMIT 42\n"
                            "#define APPLY(x) ((x) + LIMIT)\n"
                            "struct Record { int field; };\n"
                            "typedef int Number;\n"
                            "typedef enum { Ready } State;\n"
                            "int global;\n"
                            "void declared(void);\n"
                            "void defined(void) {}\n",
                            false)
                .isEmpty());
    MainWindow window(false);
    window.show();
    window.openProject(dir.path());
    auto *categories = window.findChild<QTreeWidget *>("categoryTree");
    QTRY_COMPARE_WITH_TIMEOUT(categories->topLevelItemCount(), 10, 10000);
    const auto order = symbolCategoryOrder();
    for (int i = 0; i < order.size(); ++i) {
        const auto *category = categories->topLevelItem(i);
        QCOMPARE(category->text(0), order[i] + " (1)");
        QCOMPARE(category->text(1), QString{});
        QCOMPARE(category->childCount(), 1);
        QVERIFY(!category->isExpanded());
        QVERIFY(!category->icon(0).isNull());
        QCOMPARE(category->child(0)->text(1), "categories.c");
    }
    window.findChild<QTabWidget *>("browserTabs")->setCurrentIndex(3);
    QVERIFY(categories->grab().save("symbol-categories.png"));
}

void PrototypeTests::globalTabsAndSearch() {
    QTemporaryDir dir;
    const auto first = dir.filePath("first.cpp");
    const auto second = dir.filePath("second.cpp");
    QVERIFY(writeSourceFile(first, "int first();\n", false).isEmpty());
    QVERIFY(writeSourceFile(second, "int second();\n", false).isEmpty());
    MainWindow window(false);
    window.show();
    window.activateWindow();
    QTRY_VERIFY(window.isActiveWindow());
    QVERIFY(window.openFile(first));
    auto *firstEditor = window.activeEditor();
    QVERIFY(window.openFile(second));
    auto *bar = window.findChild<QTabBar *>("documentBar");
    QCOMPARE(bar->count(), 2);
    bar->setCurrentIndex(0);
    QCOMPARE(window.activeEditor(), firstEditor);
    window.findChild<QAction *>("nextDocument")->trigger();
    QCOMPARE(bar->currentIndex(), 1);
    window.findChild<QAction *>("activateFiles")->trigger();
    QCOMPARE(QApplication::focusWidget(), window.findChild<QLineEdit *>("fileFilter"));
    window.findChild<QAction *>("closeFile")->trigger();
    QCOMPARE(window.documentCount(), 1);
    QCOMPARE(bar->count(), 1);
    QCOMPARE(window.activeEditor(), firstEditor);
}

void PrototypeTests::contextHistoryAndOpen() {
    QTemporaryDir dir;
    const auto path = dir.filePath("context.cpp");
    QVERIFY(writeSourceFile(path, "int first();\nint second();\n", false).isEmpty());
    MainWindow window(false);
    window.show();
    QSignalSpy ready(&window, &MainWindow::analysisReady);
    QVERIFY(window.openFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() > 0, 10000);
    window.previewSymbol(0);
    window.previewSymbol(1);
    auto *title = window.findChild<QLabel *>("contextTitle");
    QVERIFY(title->text().startsWith("second"));
    auto *back = window.findChild<QAction *>("contextBack");
    QVERIFY(back->isEnabled());
    back->trigger();
    QVERIFY(title->text().startsWith("first"));
    window.findChild<QAction *>("contextForward")->trigger();
    QVERIFY(title->text().startsWith("second"));
    window.findChild<QAction *>("openContext")->trigger();
    QCOMPARE(window.documentCount(), 1);
    QCOMPARE(window.activeEditor()->send(SCI_GETSELECTIONSTART), 13);
}

void PrototypeTests::sourceBackgroundIsUniform() {
    MainWindow window(false);
    window.show();
    QSignalSpy ready(&window, &MainWindow::analysisReady);
    QVERIFY(openTypographyFixture(window));
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() > 0, 10000);
    QTest::qWait(50);
    auto *editor = window.activeEditor();
    const auto image = editor->viewport()->grab().toImage();
    const auto scale = image.devicePixelRatio();
    const int x = image.width() - 10;
    const int logicalX = static_cast<int>(x / scale);
    const int lineHeight = static_cast<int>(editor->send(SCI_TEXTHEIGHT, 0));
    const int top = qRound(180 * scale);
    int sampled = 0;
    for (int y = top; y < image.height() - 10; ++y) {
        const auto position =
            editor->send(SCI_POSITIONFROMPOINTCLOSE, logicalX, static_cast<int>(y / scale));
        const auto line = editor->send(SCI_LINEFROMPOSITION, position);
        const auto end = editor->send(SCI_GETLINEENDPOSITION, line);
        if (logicalX <= editor->send(SCI_POINTXFROMPOSITION, 0, end) + lineHeight)
            continue;
        const auto color = image.pixelColor(x, y);
        QVERIFY2(qAbs(color.red() - 32) <= 1 && qAbs(color.green() - 32) <= 1 &&
                     qAbs(color.blue() - 32) <= 1,
                 qPrintable(
                     QString("Unexpected background %1 at %2,%3").arg(color.name()).arg(x).arg(y)));
        ++sampled;
    }
    QVERIFY(sampled >= qRound(lineHeight * scale));
}

void PrototypeTests::capturesTypographyPrototype() {
#ifndef CODECAIRN_TEST_CLANGD
    QSKIP("The documentation capture requires clangd and a configured C++ workspace.");
#else
    QTemporaryDir dir;
    QTemporaryDir compilation;
    QVERIFY(dir.isValid());
    QVERIFY(compilation.isValid());
    const auto fixture = readSourceFile(":/fixtures/typography.cpp");
    const auto header = readSourceFile(":/fixtures/typography.h");
    QVERIFY(fixture.error.isEmpty());
    QVERIFY(header.error.isEmpty());
    const auto path = dir.filePath("typography.cpp");
    QVERIFY(writeSourceFile(path, fixture.bytes, false).isEmpty());
    QVERIFY(writeSourceFile(dir.filePath("typography.h"), header.bytes, false).isEmpty());
    const QJsonArray commands{QJsonObject{
        {"directory", dir.path()},
        {"file", path},
        {"arguments", QJsonArray{"clang++", "-std=c++20", "-I" + dir.path(), "-c", path}}}};
    QVERIFY(writeSourceFile(compilation.filePath("compile_commands.json"),
                            QJsonDocument(commands).toJson(), false)
                .isEmpty());
    QVERIFY(WorkspaceOptionsStore::initialize(dir.path()).isEmpty());
    QVERIFY(WorkspaceOptionsStore::saveOptions(dir.path(), compilation.path(), {}).isEmpty());
    MainWindow window(false);
    window.resize(1446, 742);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *service = window.findChild<LanguageServerClient *>();
    auto *index = window.findChild<WorkspaceIndex *>();
    QVERIFY(service && index);
    QSignalSpy statuses(service, &LanguageServerClient::status);
    QSignalSpy indexed(index, &WorkspaceIndex::ready);
    QSignalSpy symbolPages(index, &WorkspaceIndex::symbolsReady);
    QSignalSpy ready(&window, &MainWindow::analysisReady);
    window.openProject(dir.path());
    QVERIFY(window.openFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(ready.count() > 0, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(service->available(), 15000);
    QVERIFY(service->hasCompilationDatabase());
    QTRY_VERIFY_WITH_TIMEOUT(!indexed.isEmpty(), 15000);
    const auto result = qvariant_cast<IndexResult>(indexed.last().first());
    QVERIFY2(result.warnings.isEmpty(), qPrintable(result.warnings.join('\n')));
    auto *files = window.findChild<QTreeWidget *>("fileList");
    QTRY_COMPARE_WITH_TIMEOUT(files->topLevelItemCount(), 2, 10000);
    auto *outline = window.findChild<QTreeWidget *>("outline");
    for (QTreeWidgetItemIterator it(outline); *it; ++it)
        if ((*it)->text(0) == "RateConfig")
            outline->setCurrentItem(*it);
    QVERIFY(outline->currentItem());
    window.findChild<QTabWidget *>("browserTabs")->setCurrentIndex(1);
    auto *filter = window.findChild<QLineEdit *>("symbolFilter");
    auto *symbols = window.findChild<QTreeWidget *>("symbolList");
    filter->setFocus();
    symbolPages.clear();
    filter->setText("stream");
    QTRY_VERIFY_WITH_TIMEOUT(!symbolPages.isEmpty(), 10000);
    QTreeWidgetItem *selected = nullptr;
    for (int row = 0; row < symbols->topLevelItemCount(); ++row)
        if (symbols->topLevelItem(row)->text(0) == "stream::RateConfig")
            selected = symbols->topLevelItem(row);
    QVERIFY(selected);
    symbols->setFocus();
    symbols->setCurrentItem(nullptr);
    symbols->setCurrentItem(selected);
    symbols->scrollToItem(selected);
    QTRY_COMPARE_WITH_TIMEOUT(window.contextEditor()->bytes(), fixture.bytes, 10000);
    QTRY_VERIFY(window.findChild<QLabel *>("contextTitle")->text().contains("RateConfig"));
    window.activeEditor()->send(SCI_SETFIRSTVISIBLELINE, 8);
    QTest::qWait(750);
    const QRegularExpression error("(diagnostic|error|failed|unavailable|warning)",
                                   QRegularExpression::CaseInsensitiveOption);
    for (const auto &status : statuses)
        QVERIFY2(!error.match(status.first().toString()).hasMatch(),
                 qPrintable(status.first().toString()));
    QVERIFY2(!error.match(window.statusBar()->currentMessage()).hasMatch(),
             qPrintable(window.statusBar()->currentMessage()));
    QVERIFY(window.grab().save("prototype.png"));
    auto *main = window.findChild<QSplitter *>("mainSplitter");
    auto *right = window.findChild<QSplitter *>("rightSplitter");
    QJsonObject measurements{
        {"viewportWidth", window.width()},
        {"viewportHeight", window.height()},
        {"devicePixelRatio", window.devicePixelRatioF()},
        {"outlineWidth", main->sizes()[0]},
        {"editorWidth", main->sizes()[1]},
        {"rightWidth", main->sizes()[2]},
        {"contextHeight", right->sizes()[0]},
        {"outlineRowHeight", outline->visualItemRect(outline->topLevelItem(0)).height()},
        {"fileRowHeight", files->visualItemRect(files->topLevelItem(0)).height()},
        {"sourceLineHeight", window.activeEditor()->lineHeight()}};
    QFile metrics("ui-measurements.json");
    QVERIFY(metrics.open(QIODevice::WriteOnly));
    const auto json = QJsonDocument(measurements).toJson();
    QCOMPARE(metrics.write(json), json.size());
#endif
}

QTEST_MAIN(PrototypeTests)
#include "PrototypeTests.moc"

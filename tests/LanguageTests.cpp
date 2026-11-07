#include "FileIO.h"
#include "CompilerSymbolRecovery.h"
#include "WorkspaceIndex.h"
#include "LanguageRegistry.h"
#include "NavigationHistory.h"
#include "ReferenceSearch.h"
#include "TestStorage.h"
#include <QDirIterator>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <QtConcurrentMap>
#include <algorithm>

class LanguageTests : public QObject {
    Q_OBJECT
    TestStorage storage_;
private slots:
    void examples_data();
    void examples();
    void multiFileExamples_data();
    void multiFileExamples();
    void updatedGrammarSyntax_data();
    void updatedGrammarSyntax();
    void compilerRegressions();
    void compilerBuiltinHeaders();
    void forwardTagsInTypedefs();
    void anonymousCompilerRecordsKeepOnlyNamedSymbols();
    void compilerAndSyntaxCategoriesAgree();
    void anonymousEnumTypedefsAreEnumerations();
    void includedMacrosGenerateFunctionDefinitions();
    void headerAndShebangDetection();
    void workspaceLanguagesAndCancellation();
    void textReferencesRespectBoundaries();
    void semanticReferenceContextAndDeduplication();
    void navigationHistoryBranches();
    void externalWorkspaceReadOnly();
    void externalWorkspaceCompilerCoverage();
};

void LanguageTests::examples_data() {
    QTest::addColumn<QString>("file");
    QTest::addColumn<QStringList>("expected");
    QTest::newRow("C") << QString("c/counter.c")
                       << QStringList{"increment", "current_counter", "reset_counter"};
    QTest::newRow("C-header") << QString("c/api.h")
                              << QStringList{"CounterTag", "Counter", "value", "increment"};
    QTest::newRow("C-main") << QString("c/main.c") << QStringList{"main"};
    QTest::newRow("C++") << QString("cpp/counter.cpp") << QStringList{"sample", "increment"};
    QTest::newRow("C++-header") << QString("cpp/counter.hpp")
                                << QStringList{"sample", "Counter", "increment", "value_"};
    QTest::newRow("C++-main") << QString("cpp/main.cpp") << QStringList{"main"};
    QTest::newRow("Python") << QString("python/counter.py")
                            << QStringList{"Counter", "__init__", "increment"};
    QTest::newRow("Bash") << QString("bash/greetings.sh") << QStringList{"greet", "name"};
    QTest::newRow("Bash-main") << QString("bash/tasks.sh") << QStringList{"main"};
    QTest::newRow("JavaScript") << QString("javascript/counter.js")
                                << QStringList{"Counter", "value", "increment", "createCounter"};
    QTest::newRow("TypeScript") << QString("typescript/counter.ts")
                                << QStringList{"CounterState", "value", "increment", "createState"};
    QTest::newRow("TSX") << QString("typescript/view.tsx") << QStringList{"CounterView", "state"};
    QTest::newRow("Go") << QString("go/counter.go")
                        << QStringList{"Counter", "Value", "Increment", "New"};
    QTest::newRow("Rust") << QString("rust/counter.rs")
                          << QStringList{"Counter", "value", "new", "increment",
                                         "State",   "Ready", "Busy"};
    QTest::newRow("JSON") << QString("config/settings.json")
                          << QStringList{"counter", "initialValue", "step"};
    QTest::newRow("YAML") << QString("config/tasks.yaml")
                          << QStringList{"tasks", "build", "command", "test"};
    QTest::newRow("CMake") << QString("cmake/CMakeLists.txt")
                           << QStringList{"BUILD_COUNTER", "counter_demo"};
    QTest::newRow("CMake-module") << QString("cmake/CounterTargets.cmake")
                                  << QStringList{"add_counter"};
}

void LanguageTests::examples() {
    QFETCH(QString, file);
    QFETCH(QStringList, expected);
    const auto path = QDir(CODECAIRN_EXAMPLES_PATH).filePath(file);
    const auto source = readSourceFile(path);
    QVERIFY2(source.error.isEmpty(), qPrintable(source.error));
    const auto result = analyzeSource(source.bytes, path);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QVERIFY(result.supported);
    QStringList names;
    for (int i = 0; i < result.symbols.size(); ++i) {
        const auto &symbol = result.symbols[i];
        names << symbol.name;
        QVERIFY(symbol.nameStart >= 0);
        QVERIFY(symbol.nameEnd <= source.bytes.size());
        QVERIFY(symbol.start <= symbol.nameStart);
        QVERIFY(symbol.end >= symbol.nameEnd);
        QVERIFY(symbol.parent < result.symbols.size());
        QVERIFY(symbol.parent != i);
        if (!symbol.generated && !symbol.outlineOnly)
            QCOMPARE(QString::fromUtf8(
                         source.bytes.mid(symbol.nameStart, symbol.nameEnd - symbol.nameStart)),
                     symbol.name);
        if (symbol.name == "createState" || symbol.name == "createCounter")
            QVERIFY(symbol.definition);
        if (file == "go/counter.go" && symbol.name == "Increment")
            QCOMPARE(symbol.qualifiedName, "Counter::Increment");
    }
    for (const auto &name : expected)
        QVERIFY2(names.contains(name), qPrintable(name + " missing; got: " + names.join(", ")));
}

void LanguageTests::updatedGrammarSyntax_data() {
    QTest::addColumn<QByteArray>("source");
    QTest::addColumn<QString>("path");
    QTest::addColumn<QStringList>("names");
    QTest::newRow("cpp-explicit-object")
        << QByteArray("struct Box { int value; int get(this Box &self) { return self.value; } };\n")
        << QString(":/updated.cpp") << QStringList{"Box", "get", "value"};
    QTest::newRow("cpp-module")
        << QByteArray("export module sample;\nexport int answer() { return 42; }\n")
        << QString(":/updated.cpp") << QStringList{"answer"};
    QTest::newRow("python-expression-statement")
        << QByteArray("value = 1\nprint(value)\ndef answer():\n    return value\n")
        << QString(":/updated.py") << QStringList{"value", "answer"};
    QTest::newRow("json-signed-exponents")
        << QByteArray("{\"values\": [1e10, 1e+10, 1E+10, 1e-10, 1.5e+10, -1.5E+10]}\n")
        << QString(":/updated.json") << QStringList{"values"};
    QTest::newRow("yaml-unindented-escaped-newlines")
        << QByteArray("root: \"a\\\nb\\\nc\"\n") << QString(":/updated.yaml")
        << QStringList{"root"};
}

void LanguageTests::updatedGrammarSyntax() {
    QFETCH(QByteArray, source);
    QFETCH(QString, path);
    QFETCH(QStringList, names);
    const auto result = analyzeSource(source, path);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QVERIFY(result.supported);
    QVERIFY2(!result.syntaxErrors, qPrintable(result.diagnostics.join('\n')));
    QCOMPARE(result.styles.size(), source.size());
    QStringList actual;
    for (const auto &symbol : result.symbols) {
        actual.append(symbol.name);
        QVERIFY(symbol.nameStart >= 0);
        QVERIFY(symbol.nameEnd <= source.size());
        QCOMPARE(QString::fromUtf8(source.mid(symbol.nameStart, symbol.nameEnd - symbol.nameStart)),
                 symbol.name);
    }
    for (const auto &name : names)
        QVERIFY2(actual.contains(name), qPrintable(name + " missing; got: " + actual.join(", ")));
}

void LanguageTests::multiFileExamples_data() {
    QTest::addColumn<QString>("entry");
    QTest::addColumn<QString>("dependency");
    QTest::addColumn<QString>("reference");
    QTest::addColumn<QString>("definition");
    struct Example {
        const char *name;
        const char *entry;
        const char *dependency;
        const char *reference;
        const char *definition;
    };
    const Example examples[] = {
        {"C", "c/main.c", "c/counter.c", "increment", "increment"},
        {"C++", "cpp/main.cpp", "cpp/counter.cpp", "increment", "increment"},
        {"Python", "python/app.py", "python/counter.py", "from counter import", "Counter"},
        {"Bash", "bash/tasks.sh", "bash/greetings.sh", "greetings.sh", "greet"},
        {"JavaScript", "javascript/app.js", "javascript/counter.js", "./counter.js",
         "createCounter"},
        {"TypeScript", "typescript/app.ts", "typescript/counter.ts", "./counter", "createState"},
        {"TSX", "typescript/view.tsx", "typescript/counter.ts", "./counter", "createState"},
        {"Go", "go/cmd/demo/main.go", "go/counter.go", "codecairn.example/counter", "New"},
        {"Rust", "rust/main.rs", "rust/counter.rs", "mod counter;", "Counter"},
        {"JSON", "config/settings.json", "config/defaults.json", "defaults.json", "counter"},
        {"YAML", "config/tasks.yaml", "config/defaults.yaml", "defaults.yaml", "working_directory"},
        {"CMake", "cmake/CMakeLists.txt", "cmake/CounterTargets.cmake", "CounterTargets.cmake",
         "add_counter"}};
    for (const auto &example : examples)
        QTest::newRow(example.name) << QString(example.entry) << QString(example.dependency)
                                    << QString(example.reference) << QString(example.definition);
}

void LanguageTests::multiFileExamples() {
    QFETCH(QString, entry);
    QFETCH(QString, dependency);
    QFETCH(QString, reference);
    QFETCH(QString, definition);
    const QDir root(CODECAIRN_EXAMPLES_PATH);
    const auto caller = readSourceFile(root.filePath(entry));
    const auto target = readSourceFile(root.filePath(dependency));
    QVERIFY2(caller.error.isEmpty(), qPrintable(caller.error));
    QVERIFY2(target.error.isEmpty(), qPrintable(target.error));
    QVERIFY(entry != dependency);
    QVERIFY(caller.bytes.contains(reference.toUtf8()));
    const auto callerAnalysis = analyzeSource(caller.bytes, root.filePath(entry));
    const auto targetAnalysis = analyzeSource(target.bytes, root.filePath(dependency));
    QVERIFY(callerAnalysis.supported && targetAnalysis.supported);
    QVERIFY2(callerAnalysis.error.isEmpty(), qPrintable(callerAnalysis.error));
    QVERIFY2(targetAnalysis.error.isEmpty(), qPrintable(targetAnalysis.error));
    bool found = false;
    for (const auto &symbol : targetAnalysis.symbols)
        if (symbol.name == definition && symbol.definition)
            found = true;
    QVERIFY2(found, qPrintable("Missing companion definition: " + dependency + ": " + definition));
}

void LanguageTests::compilerRegressions() {
    QTemporaryDir directory;
    const auto path = directory.filePath("regression.c");
    const QByteArray source = "typedef struct WorkerTag { int count; } Worker;\n"
                              "static _Thread_local Worker *current_worker;\n"
                              "struct Clock { int count; };\n"
                              "int run(void) { struct Clock local; return 0; }\n"
                              "#define DEFINE_CALLBACK(name) void on_##name(int value) {}\n"
                              "DEFINE_CALLBACK(packet)\n";
    QVERIFY(writeSourceFile(path, source, false).isEmpty());
    const auto result = analyzeSource(source, path);
    QVERIFY(result.compilerEnriched);
    int clocks = 0;
    bool current = false, generated = false, local = false;
    for (const auto &symbol : result.symbols) {
        if (symbol.name == "Clock") {
            ++clocks;
            QCOMPARE(symbol.qualifiedName, "Clock");
        }
        if (symbol.name == "WorkerTag")
            QCOMPARE(symbol.qualifiedName, "WorkerTag");
        if (symbol.name == "current_worker") {
            current = true;
            QCOMPARE(symbol.kind, "Variable");
            QVERIFY(symbol.definition);
        }
        if (symbol.name == "local") {
            local = true;
            QVERIFY(symbol.local);
        }
        if (symbol.name == "on_packet") {
            generated = true;
            QVERIFY(symbol.generated);
            QVERIFY(symbol.definition);
            QCOMPARE(symbol.provider, "compiler");
            QCOMPARE(source.mid(symbol.nameStart, symbol.nameEnd - symbol.nameStart),
                     "DEFINE_CALLBACK");
        }
    }
    QCOMPARE(clocks, 1);
    QVERIFY(current && generated && local);
}

void LanguageTests::headerAndShebangDetection() {
    QCOMPARE(languageForFile("script", "#!/usr/bin/env python3\n").id, LanguageId::Python);
    QCOMPARE(languageForFile("run", "#!/bin/bash\n").id, LanguageId::Bash);
    QCOMPARE(languageForFile("sample.C").id, LanguageId::Cpp);
    QCOMPARE(analyzeSource("class Counter { public: int run(); };", "counter.h").language,
             LanguageId::Cpp);
    QCOMPARE(analyzeSource("struct Counter { int value; };", "counter.h").language, LanguageId::C);
}

void LanguageTests::compilerBuiltinHeaders() {
    QTemporaryDir directory;
    const auto path = directory.filePath("builtins.c");
    const QByteArray source = "#include <stdbool.h>\n#include <stdatomic.h>\n"
                              "#define DEFINE_FLAG(name) bool flag_##name(void) { return true; }\n"
                              "DEFINE_FLAG(ready)\nstatic _Thread_local atomic_int counter;\n";
    QVERIFY(writeSourceFile(path, source, false).isEmpty());
    const auto result = analyzeSource(source, path);
    QVERIFY(result.compilerEnriched);
    for (const auto &diagnostic : result.diagnostics)
        QVERIFY2(!diagnostic.contains("error:") &&
                     !diagnostic.contains("Compiler enrichment unavailable"),
                 qPrintable(diagnostic));
    const auto generated =
        std::find_if(result.symbols.cbegin(), result.symbols.cend(),
                     [](const auto &symbol) { return symbol.name == "flag_ready"; });
    QVERIFY(generated != result.symbols.cend());
    QVERIFY(generated->definition);
    QVERIFY(generated->generated);
}

void LanguageTests::forwardTagsInTypedefs() {
    const QByteArray source = "typedef struct Connection ConnectionHandle;\n"
                              "typedef union Storage StorageHandle;\n"
                              "typedef enum State StateHandle;\n"
                              "struct Plain;\n"
                              "struct Connection *connection;\n"
                              "struct Plain *plain;\n"
                              "union Storage *storage;\n"
                              "enum State *state;\n";
    const auto result = analyzeSource(source, "forward.c");
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QVERIFY(!result.syntaxErrors);
    QVERIFY(!result.compilerEnriched);
    for (const auto &name : {"Connection", "Storage", "State", "Plain"}) {
        const auto tag = std::find_if(result.symbols.cbegin(), result.symbols.cend(),
                                      [&](const auto &symbol) { return symbol.name == name; });
        QVERIFY2(tag != result.symbols.cend(), name);
        QVERIFY(!tag->definition);
        QCOMPARE(std::count_if(result.symbols.cbegin(), result.symbols.cend(),
                               [&](const auto &symbol) { return symbol.name == name; }),
                 1);
    }
    for (const auto &name : {"ConnectionHandle", "StorageHandle", "StateHandle"}) {
        const auto alias = std::find_if(result.symbols.cbegin(), result.symbols.cend(),
                                        [&](const auto &symbol) { return symbol.name == name; });
        QVERIFY2(alias != result.symbols.cend(), name);
        QVERIFY(alias->definition);
        QCOMPARE(alias->category, "Alias");
    }
}

void LanguageTests::anonymousCompilerRecordsKeepOnlyNamedSymbols() {
    QTemporaryDir directory;
    const auto path = directory.filePath("anonymous.c");
    const QByteArray source = "struct Container { struct { int value; } nested; };\n"
                              "typedef struct { int count; } Alias;\n"
                              "#define RECORD(name) struct name { int member; };\n"
                              "RECORD(Generated)\n"
                              "#define FIXED struct Fixed { int fixed_member; };\n"
                              "FIXED\n";
    QVERIFY(writeSourceFile(path, source, false).isEmpty());
    SourceAnalysis compiler;
    compiler.language = LanguageId::C;
    recoverCompilerSymbols(compiler, source, path);
    QVERIFY(compiler.compilerEnriched);
    QSet<QString> names;
    for (const auto &symbol : compiler.symbols) {
        QVERIFY2(!symbol.name.contains("(unnamed"), qPrintable(symbol.name));
        names.insert(symbol.name);
    }
    for (const auto &name : {"Container", "value", "nested", "Alias", "count", "Generated",
                             "member", "Fixed", "fixed_member"})
        QVERIFY2(names.contains(name), name);
    QCOMPARE(std::count_if(compiler.symbols.cbegin(), compiler.symbols.cend(),
                           [](const auto &symbol) { return symbol.name == "Alias"; }),
             1);
}

void LanguageTests::compilerAndSyntaxCategoriesAgree() {
    QTemporaryDir directory;
    const auto path = directory.filePath("categories.cpp");
    const QByteArray source = "struct Record { int field; void method(); };\n"
                              "typedef int Number;\nusing Alias = Number;\n"
                              "enum State { Ready };\nint global;\n"
                              "int run(int);\nint run(int x) { return x; }\n";
    QVERIFY(writeSourceFile(path, source, false).isEmpty());
    const auto syntax = analyzeSource(source, path);
    QVERIFY(!syntax.syntaxErrors);
    SourceAnalysis compiler;
    compiler.language = syntax.language;
    recoverCompilerSymbols(compiler, source, path);
    QVERIFY(compiler.compilerEnriched);
    QCOMPARE(compiler.symbols.size(), syntax.symbols.size());
    for (const auto &symbol : syntax.symbols) {
        const auto found = std::find_if(
            compiler.symbols.cbegin(), compiler.symbols.cend(), [&](const auto &other) {
                return other.name == symbol.name && other.nameStart == symbol.nameStart;
            });
        QVERIFY2(found != compiler.symbols.cend(), qPrintable(symbol.name));
        QCOMPARE(symbolCategory(*found), symbolCategory(symbol));
    }
}

void LanguageTests::anonymousEnumTypedefsAreEnumerations() {
    QTemporaryDir directory;
    const auto path = directory.filePath("enums.c");
    const QByteArray source = "typedef enum { Ready, Busy } State, *StatePointer;\n"
                              "typedef State StateAlias;\n"
                              "typedef enum Tag { Tagged } TagAlias;\n"
                              "typedef struct { int count; } Record;\n";
    QVERIFY(writeSourceFile(path, source, false).isEmpty());
    const auto syntax = analyzeSource(source, path);
    QVERIFY(!syntax.syntaxErrors);
    const QByteArray recoverySource = source + "#define CONCAT(a,b) a##b\n";
    const auto recovered = analyzeSource(recoverySource, path);
    QVERIFY(recovered.compilerEnriched);
    for (const auto &symbol : syntax.symbols)
        if (symbol.name == "Ready" || symbol.name == "Busy") {
            QVERIFY(symbol.parent >= 0);
            QCOMPARE(syntax.symbols[symbol.parent].name, "State");
        }
    SourceAnalysis compiler;
    compiler.language = syntax.language;
    recoverCompilerSymbols(compiler, source, path);
    QVERIFY(compiler.compilerEnriched);
    const QMap<QString, QString> expected{
        {"State", "Enumerations"},          {"StatePointer", "Type Definitions"},
        {"StateAlias", "Type Definitions"}, {"Tag", "Enumerations"},
        {"TagAlias", "Type Definitions"},   {"Record", "Type Definitions"},
        {"Ready", "Enum Constants"},        {"Busy", "Enum Constants"},
        {"Tagged", "Enum Constants"}};
    const SourceAnalysis *analyses[]{&syntax, &compiler, &recovered};
    for (const auto *analysis : analyses) {
        for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
            const auto found =
                std::find_if(analysis->symbols.cbegin(), analysis->symbols.cend(),
                             [&](const auto &symbol) { return symbol.name == it.key(); });
            QVERIFY2(found != analysis->symbols.cend(), qPrintable(it.key()));
            QVERIFY2(symbolCategory(*found) == it.value(),
                     qPrintable(it.key() + " (" + found->provider + "): " + symbolCategory(*found) +
                                " instead of " + it.value()));
            QCOMPARE(std::count_if(analysis->symbols.cbegin(), analysis->symbols.cend(),
                                   [&](const auto &symbol) { return symbol.name == it.key(); }),
                     1);
        }
    }
}

void LanguageTests::includedMacrosGenerateFunctionDefinitions() {
    QTemporaryDir directory;
    QVERIFY(writeSourceFile(directory.filePath("harness.h"),
                            "#define TEST(name) static void test_##name(void)\n", false)
                .isEmpty());
    const auto path = directory.filePath("tests.c");
    const QByteArray source = "#include \"harness.h\"\n"
                              "TEST(first) { int local = 0; }\n"
                              "TEST(second) { }\n";
    QVERIFY(writeSourceFile(path, source, false).isEmpty());
    const auto result = analyzeSource(source, path);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QVERIFY(!result.syntaxErrors);
    QVERIFY(result.compilerEnriched);
    for (const auto &name : {"test_first", "test_second"}) {
        const auto symbol = std::find_if(result.symbols.cbegin(), result.symbols.cend(),
                                         [&](const auto &entry) { return entry.name == name; });
        QVERIFY2(symbol != result.symbols.cend(), name);
        QVERIFY(symbol->definition);
        QVERIFY(symbol->generated);
        QCOMPARE(symbol->provider, "compiler");
        QCOMPARE(source.mid(symbol->nameStart, symbol->nameEnd - symbol->nameStart), "TEST");
    }
    for (const auto &entry : result.symbols)
        if (entry.kind == "Function")
            QVERIFY(entry.name != "first" && entry.name != "second");
    const auto built = buildWorkspaceIndex(directory.path(), false, true, {},
                                           std::make_shared<std::atomic_bool>(false));
    QVERIFY2(built.error.isEmpty(), qPrintable(built.error));
    SymbolQuery query;
    query.text = "test_";
    const auto page = queryWorkspaceSymbols(directory.path(), query);
    QVERIFY2(page.error.isEmpty(), qPrintable(page.error));
    QCOMPARE(page.total, 2);
    for (const auto &entry : page.symbols) {
        QVERIFY(entry.symbol.generated);
        QVERIFY(entry.symbol.definition);
    }
}

void LanguageTests::workspaceLanguagesAndCancellation() {
    QTemporaryDir directory;
    QVERIFY(writeSourceFile(directory.filePath("example.py"),
                            "def count():\n    local = 1\n    return local\n", false)
                .isEmpty());
    QVERIFY(
        writeSourceFile(directory.filePath("run"), "#!/bin/bash\ngreet() { echo hello; }\n", false)
            .isEmpty());
    const auto result = buildWorkspaceIndex(directory.path(), false, true, {},
                                            std::make_shared<std::atomic_bool>(false));
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QCOMPARE(result.parsed, 2);
    const auto page = queryWorkspaceSymbols(directory.path(), {});
    QVERIFY2(page.error.isEmpty(), qPrintable(page.error));
    QCOMPARE(page.total, 2);
    for (const auto &entry : page.symbols)
        QVERIFY(!entry.symbol.local);
    SymbolQuery query;
    query.cancel = std::make_shared<std::atomic_bool>(true);
    const auto cancelled = queryWorkspaceSymbols(directory.path(), query);
    QVERIFY(cancelled.cancelled);
    QVERIFY(cancelled.error.isEmpty());
}

void LanguageTests::textReferencesRespectBoundaries() {
    QTemporaryDir directory;
    const auto path = directory.filePath("calls.cpp");
    const QByteArray source = "int count();\nint counter;\n// count\nconst char *s = "
                              "\"count\";\nint run() { return count(); }\n";
    QVERIFY(writeSourceFile(path, source, false).isEmpty());
    auto cancel = std::make_shared<std::atomic_bool>(false);
    const auto code = findTextOccurrences({path}, "count", {}, false, false, cancel);
    QCOMPARE(code.hits.size(), 2);
    QCOMPARE(code.hits[0].line, 0);
    QCOMPARE(code.hits[1].line, 4);
    QCOMPARE(code.hits[1].container, "run");
    QCOMPARE(findTextOccurrences({path}, "count", {}, true, true, cancel).hits.size(), 4);
    const QByteArray modified = "int count();\n";
    QCOMPARE(findTextOccurrences({path}, "count", {{path, modified, analyzeSource(modified, path)}},
                                 true, true, cancel)
                 .hits.size(),
             1);
    const auto before = readSourceFile(path).bytes;
    cancel->store(true);
    QVERIFY(findTextOccurrences({path}, "count", {}, true, true, cancel).cancelled);
    QCOMPARE(readSourceFile(path).bytes, before);
}

void LanguageTests::semanticReferenceContextAndDeduplication() {
    QTemporaryDir directory;
    const auto path = directory.filePath("calls.cpp");
    const QByteArray source = "int callee();\nint caller() { return callee(); }\n";
    QVERIFY(writeSourceFile(path, source, false).isEmpty());
    const auto position = lspPosition(source, static_cast<int>(source.lastIndexOf("callee")));
    LspLocation target;
    target.path = path;
    target.line = target.endLine = position.value("line").toInt();
    target.character = position.value("character").toInt();
    target.endCharacter = target.character + 6;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    const auto results = resolveSemanticReferences({target, target}, {path}, "callee", {}, cancel);
    QCOMPARE(results.hits.size(), 1);
    QCOMPARE(results.hits.first().container, "caller");
    QCOMPARE(results.hits.first().line, 1);
    QVERIFY(results.hits.first().text.contains("return callee()"));
    QVERIFY(!results.hits.first().location.expectedHash.isEmpty());
    QVERIFY(results.warnings.isEmpty());
    QCOMPARE(resolveSemanticReferences({target}, {}, "callee", {}, cancel).hits.size(), 0);
    auto changed = target;
    changed.character = 0;
    QVERIFY(!resolveSemanticReferences({changed}, {path}, "callee", {}, cancel).warnings.isEmpty());
    cancel->store(true);
    QVERIFY(resolveSemanticReferences({target}, {path}, "callee", {}, cancel).cancelled);
}

void LanguageTests::navigationHistoryBranches() {
    NavigationHistory history;
    history.record({"one", 1, 2}, {"two", 3, 4});
    QVERIFY(history.canBack());
    QCOMPARE(history.move(-1).path, "one");
    QVERIFY(history.canForward());
    history.record({"one", 1, 2}, {"three", 5, 6});
    QVERIFY(!history.canForward());
    QCOMPARE(history.move(-1).path, "one");
    QCOMPARE(history.move(1).path, "three");
}

void LanguageTests::externalWorkspaceReadOnly() {
    const auto root = qEnvironmentVariable("CODECAIRN_VALIDATE_WORKSPACE");
    if (root.isEmpty())
        QSKIP("Set CODECAIRN_VALIDATE_WORKSPACE for an optional read-only real-workspace "
              "extraction check.");
    QVERIFY(QFileInfo(root).isDir());
    const auto expected =
        qEnvironmentVariable("CODECAIRN_VALIDATE_SYMBOLS").split(',', Qt::SkipEmptyParts);
    QSet<QString> names;
    QMap<QString, int> categories;
    QDirIterator files(root, {"*.c", "*.h", "*.cpp", "*.hpp"}, QDir::Files,
                       QDirIterator::Subdirectories);
    int parsed = 0, partial = 0;
    while (files.hasNext()) {
        const auto path = files.next();
        if (path.contains("/build/") || path.contains("/.git/"))
            continue;
        const auto source = readSourceFile(path);
        QVERIFY2(source.error.isEmpty(), qPrintable(path + ": " + source.error));
        const auto result = analyzeSource(source.bytes, path);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        ++parsed;
        if (result.syntaxErrors)
            ++partial;
        for (const auto &symbol : result.symbols) {
            if (!symbol.local && !symbol.outlineOnly)
                ++categories[symbolCategory(symbol)];
            if (expected.contains(symbol.name))
                qInfo().noquote() << QFileInfo(path).fileName() << symbol.name << symbol.kind
                                  << (symbol.definition ? "definition" : "declaration")
                                  << symbol.provider;
            if (symbol.definition)
                names.insert(symbol.name);
        }
    }
    for (const auto &name : expected)
        QVERIFY2(names.contains(name), qPrintable("Missing definition: " + name));
    qInfo() << parsed << "files analyzed read-only;" << partial << "partial syntax;" << names.size()
            << "distinct definition names";
    for (auto it = categories.cbegin(); it != categories.cend(); ++it)
        qInfo().noquote() << it.key() << it.value();
}

void LanguageTests::externalWorkspaceCompilerCoverage() {
    const auto root = qEnvironmentVariable("CODECAIRN_VALIDATE_WORKSPACE");
    if (root.isEmpty())
        QSKIP("Set CODECAIRN_VALIDATE_WORKSPACE for independent compiler symbol coverage.");
    QDirIterator files(root, {"*.c", "*.h", "*.cpp", "*.hpp"}, QDir::Files,
                       QDirIterator::Subdirectories);
    QStringList paths;
    while (files.hasNext()) {
        const auto path = files.next();
        if (path.contains("/build/") || path.contains("/.git/"))
            continue;
        paths.append(path);
    }
    struct Coverage {
        QString path;
        QString error;
        QByteArray source;
        SourceAnalysis syntax;
        SourceAnalysis compiler;
    };
    QThreadPool pool;
    pool.setMaxThreadCount(4);
    const auto results = QtConcurrent::blockingMapped(&pool, paths, [](const QString &path) {
        Coverage result;
        result.path = path;
        const auto source = readSourceFile(path);
        result.error = source.error;
        if (!result.error.isEmpty())
            return result;
        result.source = source.bytes;
        result.syntax = analyzeSource(source.bytes, path);
        result.error = result.syntax.error;
        if (!result.error.isEmpty())
            return result;
        result.compiler.language = result.syntax.language;
        recoverCompilerSymbols(result.compiler, source.bytes, path);
        if (!result.compiler.compilerEnriched)
            result.error = "Compiler analysis failed.";
        return result;
    });
    QStringList missing, mismatched, duplicates, missingMacros, unexpectedMacros;
    int expected = 0, diagnostics = 0, macros = 0;
    const QRegularExpression defines("^[\\t ]*#[\\t ]*define[\\t ]+([A-Za-z_][A-Za-z_0-9]*)",
                                     QRegularExpression::MultilineOption);
    for (const auto &result : results) {
        QVERIFY2(result.error.isEmpty(), qPrintable(result.path + ": " + result.error));
        if (!result.compiler.diagnostics.isEmpty())
            ++diagnostics;
        QSet<QString> locations;
        QSet<QString> macroLocations;
        for (const auto &symbol : result.syntax.symbols) {
            if (symbol.local || symbol.outlineOnly)
                continue;
            const auto key = symbol.name + ':' + QString::number(symbol.nameStart);
            if (locations.contains(key))
                duplicates.append(result.path + ':' + QString::number(symbol.line + 1) + ": " +
                                  symbol.name);
            locations.insert(key);
            if (symbol.kind == "Macro")
                macroLocations.insert(key);
        }
        auto definitions = defines.globalMatch(QString::fromLatin1(result.source));
        while (definitions.hasNext()) {
            const auto definition = definitions.next();
            const auto offset = definition.capturedStart(1);
            const auto style = static_cast<TextStyle>(result.syntax.styles[offset]);
            if (style == TextStyle::Comment || style == TextStyle::String)
                continue;
            ++macros;
            const auto key = definition.captured(1) + ':' + QString::number(offset);
            if (!macroLocations.remove(key))
                missingMacros.append(result.path + ": " + key);
        }
        for (const auto &key : macroLocations)
            unexpectedMacros.append(result.path + ": " + key);
        for (const auto &symbol : result.compiler.symbols) {
            if (symbol.local || symbol.outlineOnly)
                continue;
            ++expected;
            const auto found = std::find_if(result.syntax.symbols.cbegin(),
                                            result.syntax.symbols.cend(), [&](const auto &entry) {
                                                return !entry.local && !entry.outlineOnly &&
                                                       entry.name == symbol.name &&
                                                       entry.nameStart == symbol.nameStart;
                                            });
            const auto location =
                result.path + ':' + QString::number(symbol.line + 1) + ": " + symbol.name;
            if (found == result.syntax.symbols.cend())
                missing.append(location);
            else if (found->kind != symbol.kind || found->definition != symbol.definition ||
                     symbolCategory(*found) != symbolCategory(symbol))
                mismatched.append(location + ": " + symbolCategory(*found) + " / " +
                                  symbolCategory(symbol));
        }
    }
    qInfo() << results.size() << "files;" << expected << "compiler symbols;" << missing.size()
            << "missing;" << mismatched.size() << "classification mismatches;" << duplicates.size()
            << "duplicate locations;" << diagnostics << "files with compiler diagnostics";
    qInfo() << macros << "macro definitions;" << missingMacros.size() << "missing;"
            << unexpectedMacros.size() << "unexpected macro locations";
    QVERIFY2(missing.isEmpty(), qPrintable(missing.mid(0, 30).join('\n')));
    QVERIFY2(mismatched.isEmpty(), qPrintable(mismatched.mid(0, 30).join('\n')));
    QVERIFY2(duplicates.isEmpty(), qPrintable(duplicates.mid(0, 30).join('\n')));
    QVERIFY2(missingMacros.isEmpty(), qPrintable(missingMacros.mid(0, 30).join('\n')));
    QVERIFY2(unexpectedMacros.isEmpty(), qPrintable(unexpectedMacros.mid(0, 30).join('\n')));
}

QTEST_MAIN(LanguageTests)
#include "LanguageTests.moc"

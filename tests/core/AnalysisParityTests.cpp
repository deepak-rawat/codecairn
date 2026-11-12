#include "SourceAnalysis.h"
#include "FileIO.h"
#include "TestStorage.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <iterator>

namespace {
struct Fixture {
    const char *name;
    const char *path;
    const char *source;
};

const Fixture fixtures[] = {
    {"plain", ":/parity.txt", "plain text\n"},
    {"c-header-tie", ":/parity.h", "struct Box { int value; };\n"},
    {"cpp-header", ":/parity.h", "namespace N { class Box { public: int get(); }; }\n"},
    {"partial-header", ":/parity.h", "class Box { public: int get(;\n"},
    {"conditional", ":/parity.c",
     "#ifdef ENABLED\n#include \"box.h\"\n#define FLAG(x) (x)\nint value;\n#else\nint "
     "other;\n#endif\n"},
    {"bindings", ":/parity.c",
     "int f(int x) { int y = x; { int x = y; y = x; } return x + y; }\n"
     "int g(void) { return f(1) + puts(\"ok\"); }\n"},
    {"c-declarations", ":/parity.c",
     "#define LIMIT 7\n"
     "typedef struct { int field; } Box;\n"
     "enum E { FIRST, SECOND = LIMIT };\n"
     "extern int outside;\n"
     "int (*callback)(int);\n"
     "int declared(int p);\n"},
    {"qualified-cpp", ":/parity.cpp",
     "namespace N { struct Box { int field; int get() { return field; } }; }\n"
     "int N::Box::get();\n"},
    {"partial-c", ":/parity.c", "int f(int x) { return x + ; }\n"},
    {"partial-python", ":/parity.py", "def broken(:\n    return 1\n"},
    {"macro-syntax", ":/parity.c", "#define MAKE(n) int made_##n(void) { return 1; }\nMAKE(one)\n"},
    {"macro-recovery", "$ROOT/analysis-parity-recovery.c",
     "#define MAKE(n) int made_##n(void) { return 1; }\nMAKE(one)\n"},
    {"python", ":/parity.py",
     "class Box:\n    def get(self):\n        value = 1\n        return value\n"},
    {"utf8", ":/parity.py", "# caf\xc3\xa9\nname = \"caf\xc3\xa9\"\n"},
    {"bash", ":/parity.sh", "#!/bin/sh\ngreet() { local name=\"world\"; echo \"$name\"; }\n"},
    {"javascript", ":/parity.js", "class Box { value = 1; get() { return this.value; } }\n"},
    {"typescript", ":/parity.ts",
     "interface Box { value: number; }\nconst make = (value: number) => ({value});\n"},
    {"tsx", ":/parity.tsx", "function View() { return <div title=\"hi\">Hello</div>; }\n"},
    {"go", ":/parity.go",
     "package p\ntype Box struct { Value int }\nfunc (b Box) Get() int { return b.Value }\n"},
    {"rust", ":/parity.rs",
     "struct Box { value: i32 }\nimpl Box { fn get(&self) -> i32 { self.value } }\n"},
    {"json", ":/parity.json", "{\"name\": \"Box\", \"count\": 2}\n"},
    {"yaml", ":/parity.yaml", "box:\n  value: 2\n"},
    {"cmake", ":/CMakeLists.txt",
     "set(VALUE 2)\nfunction(greet name)\n  message(\"${name}\")\nendfunction()\n"},
};

struct Expected {
    const char *name;
    const char *styles;
    const char *snapshotHash;
};

// Captured from the original analyzer before extracting its stages. The snapshot
// hashes cover every SourceSymbol field in traversal order and all analysis metadata.
const Expected expected[] = {
    {"plain", "0000000000000000000000",
     "3e3d487c43a80cc0d0836013426e31cdfcf3915afe43607a6c4e187703408cc8"},
    {"c-header-tie", "020202020202000505050012000b0b0b0007070707071200121200",
     "1538ad80f6efe895ed4ab5b5278983353840c685da6763aeb58245539f2fe72b"},
    {"cpp-header",
     "020202020202020202000700120002020202020005050500120002020202020212000b0b0b0006060612121200121"
     "2001200",
     "9f846ecc55c23d9462aec04e93fc42de6c005c347d905833c51218e2d323b19c"},
    {"partial-header", "0202020202000a0a0a00120002020202020212000b0b0b00070707121200",
     "29dec18ba3b9bb3262fc0aaf2ab64ac743c9f50c22f15b85e8b6aa69039f5fcf"},
    {"conditional",
     "080808080808000f0f0f0f0f0f0f00080808080808080800030303030303030008080808080808000808080812001"
     "200000000000b0b0b000e0e0e0e0e12000808080808000b0b0b000e0e0e0e0e120008080808080800",
     "ad75e73c3492725c48fc56c6b9d3320918f1a1a8898e5cb3b55f338eeae6276e"},
    {"bindings",
     "0b0b0b0006120b0b0b000c120012000b0b0b000e0009000c120012000b0b0b000e0009000d12000d0009000d12001"
     "200020202020202000c0009000d120012000b0b0b0006120b0b0b0b12001200020202020202001012041200090011"
     "11111112030303031212001200",
     "7ad8db06bd5033ce3dd92beb5219a6f874b96741b0b6e1e9137276ae4df388b9"},
    {"c-declarations",
     "0808080808080800080808080800000002020202020202000202020202020012000b0b0b000707070707120012000"
     "5050512000202020200050012000f0f0f0f0f12000f0f0f0f0f0f0009000f0f0f0f0f00121200020202020202000b"
     "0b0b000e0e0e0e0e0e0e12000b0b0b0012090e0e0e0e0e0e0e0e12120b0b0b1212000b0b0b0006060606060606061"
     "20b0b0b000c121200",
     "192d2c1e01448d1e7273ea2a6f5d2918800e99ca4581c07291cde630b1bcf716"},
    {"qualified-cpp",
     "0202020202020202020007001200020202020202000505050012000b0b0b00070707070712000b0b0b00060606121"
     "20012000202020202020000000000001200120012120012000b0b0b00000909000000090906060612121200",
     "e2c5e70409bef29fe45bc21854d6a4c36f79ab8dc020dfcc07800fd5e75875a7"},
    {"partial-c", "0b0b0b0006120b0b0b000c12001200020202020202000c00090012001200",
     "a4f836e7a7adcf6dece1f52aa73545df0a95d044ceabea67f348c98685810483"},
    {"partial-python", "0202020006060606060609090000000000020202020202000000",
     "025c3df187be7ce169a00cedc80df05b1f3a66050d07b74d45e3bed1a3b6f9cd"},
    {"macro-syntax",
     "080808080808080008080808120012000000000000000000000000000000000000000000000000000000000000000"
     "000000f0f0f0f120a0a0a1200",
     "980582d156ae19c2cada6731edc533b3e912baa862f8bcc4bb696a6a9a4c91ac"},
    {"macro-recovery",
     "080808080808080008080808120012000000000000000000000000000000000000000000000000000000000000000"
     "000000f0f0f0f120a0a0a1200",
     "4823df1f015d657f873ae0d326b84a00ee19e966e758ad6a3ff4cabb3cc6c6c5"},
    {"python",
     "020202020200050505090000000000020202000606060900000000090900000000000000000007070707070009000"
     "000000000000000000002020202020200000000000000",
     "00ec408126136ccbacd42915cfa5a25eabc4319ca6ddffcd0a244619c0bc7fde"},
    {"utf8", "0101010101010100070707070009000303030303030300",
     "ea2ed00dccf1d5235433e8efd291114c94b98b473a2e964b4cbc08fd7f13e126"},
    {"bash",
     "010101010101010101000606060606090900090002020202020007070707090303030303030309000000000000030"
     "3030303030309000900",
     "aa1712452819ec3fd7ed19124f113283c8b4611f182b4d9ed8c554fc3641c693"},
    {"javascript",
     "020202020200050505000900070707070700090000090006060609090009000202020202020002020202090000000"
     "000090009000900",
     "37aa1eeff1e1b211845783fa405cea016c0a42c1dec6c3129f6e1831ff53d1bc"},
    {"typescript",
     "020202020202020202000505050009000707070707090009090909090909000900020202020200060606060009000"
     "90000000000090009090909090909000909000909000000000009090900",
     "a5d92d37e11aed8933a46f8be2ba40cd5fd0361008c3efafdf4164d9f34a44ce"},
    {"tsx",
     "020202020202020200060606060909000900020202020202000900000000000000000009030303030900000000000"
     "9090000000909000900",
     "193198ed5b7319612d450da753183242bb11285fe6c3fd41840f5ae5a1e9d51c"},
    {"go",
     "020202020202020000000909090900050505000202020202020009000707070707000a0a0a0009000202020200090"
     "0000a0a0a09000606060909000a0a0a0009000202020202020000090707070707000900",
     "ef35c2ebb1fe11507226ed086f22b07ac8c2e20963c37f3942eaadd2d6465658"},
    {"rust",
     "02020202020200050505000900070707070709000b0b0b00090002020202000a0a0a0009000202000606060909020"
     "2020209000909000b0b0b000900020202020907070707070009000900",
     "b02d60469b247a725db7bb0a7dfad678bce63b34328b877d22b259d46774b26c"},
    {"json", "09030707070703090003030303030900030707070707030900000900",
     "df73395766edc57e3a0a7b394f98d69e8c488ec86a2130d2afc8c1dfb4eac36e"},
    {"yaml", "07070709000000070707070709000000",
     "02168d7354ae903d430ab19557ee446581efd605346bdcf48eb2b0f344990e66"},
    {"cmake",
     "000000090707070707000009000202020202020202090606060606000000000009000000000000000000000903030"
     "303030303030309000000000000000000000000090900",
     "d77f15cb947b2bff90286d2b86aaa8af242fc1ebf45bdb801f410bdbce1f6d4a"},
};

QString fixturePath(const Fixture &fixture) {
    const auto path = QString::fromUtf8(fixture.path);
    return path.startsWith("$ROOT/") ? QDir::root().filePath(path.mid(6)) : path;
}

QByteArray snapshot(const SourceAnalysis &analysis) {
    QJsonArray symbols;
    for (const auto &symbol : analysis.symbols)
        symbols.append(QJsonObject{{"name", symbol.name},
                                   {"kind", symbol.kind},
                                   {"start", symbol.start},
                                   {"end", symbol.end},
                                   {"nameStart", symbol.nameStart},
                                   {"nameEnd", symbol.nameEnd},
                                   {"line", symbol.line},
                                   {"parent", symbol.parent},
                                   {"outlineOnly", symbol.outlineOnly},
                                   {"qualifiedName", symbol.qualifiedName},
                                   {"signature", symbol.signature},
                                   {"definition", symbol.definition},
                                   {"category", symbol.category},
                                   {"local", symbol.local},
                                   {"generated", symbol.generated},
                                   {"provider", symbol.provider}});
    return QJsonDocument(
               QJsonObject{{"symbols", symbols},
                           {"error", analysis.error},
                           {"syntaxErrors", analysis.syntaxErrors},
                           {"supported", analysis.supported},
                           {"language", static_cast<int>(analysis.language)},
                           {"diagnostics", QJsonArray::fromStringList(analysis.diagnostics)},
                           {"compilerEnriched", analysis.compilerEnriched}})
        .toJson(QJsonDocument::Compact);
}
}

class AnalysisParityTests : public QObject {
    Q_OBJECT
    TestStorage storage_;
private slots:
    void parity_data();
    void parity();
    void sizeLimit();
    void maximumSizeAccepted();
    void signatureLimit();
    void diagnosticLimit();
};

void AnalysisParityTests::parity_data() {
    QTest::addColumn<QByteArray>("source");
    QTest::addColumn<QString>("path");
    QTest::addColumn<QByteArray>("expectedStyles");
    QTest::addColumn<QByteArray>("expectedSnapshotHash");
    static_assert(std::size(fixtures) == std::size(expected));
    for (size_t i = 0; i < std::size(fixtures); ++i) {
        QCOMPARE(QByteArray(fixtures[i].name), QByteArray(expected[i].name));
        QTest::newRow(fixtures[i].name)
            << QByteArray(fixtures[i].source) << fixturePath(fixtures[i])
            << QByteArray(expected[i].styles) << QByteArray(expected[i].snapshotHash);
    }
}

void AnalysisParityTests::parity() {
    QFETCH(QByteArray, source);
    QFETCH(QString, path);
    QFETCH(QByteArray, expectedStyles);
    QFETCH(QByteArray, expectedSnapshotHash);
    const auto actual = analyzeSource(source, path);
    QCOMPARE(actual.styles.toHex(), expectedStyles);
    const auto actualSnapshot = snapshot(actual);
    const auto actualHash =
        QCryptographicHash::hash(actualSnapshot, QCryptographicHash::Sha256).toHex();
    QVERIFY2(actualHash == expectedSnapshotHash, actualSnapshot.constData());
}

void AnalysisParityTests::sizeLimit() {
    const auto actual = analyzeSource(QByteArray(MaximumDocumentBytes + 1, 'x'), ":/parity.c");
    QCOMPARE(actual.styles, QByteArray{});
    QVERIFY(actual.symbols.isEmpty());
    QCOMPARE(actual.error, "Analysis requires a document no larger than 4 MiB in this prototype.");
    QVERIFY(!actual.supported);
    QVERIFY(!actual.syntaxErrors);
    QCOMPARE(actual.language, LanguageId::Plain);
    QVERIFY(actual.diagnostics.isEmpty());
    QVERIFY(!actual.compilerEnriched);
}

void AnalysisParityTests::maximumSizeAccepted() {
    const auto actual = analyzeSource(QByteArray(MaximumDocumentBytes, 'x'), ":/parity.txt");
    QCOMPARE(actual.styles, QByteArray(MaximumDocumentBytes, static_cast<char>(TextStyle::Plain)));
    QCOMPARE(
        snapshot(actual),
        QByteArray("{\"compilerEnriched\":false,\"diagnostics\":[],\"error\":\"\","
                   "\"language\":0,\"supported\":false,\"symbols\":[],\"syntaxErrors\":false}"));
}

void AnalysisParityTests::signatureLimit() {
    const auto source = QByteArray("int f(") + QByteArray(600, ' ') + "int parameter);\n";
    const auto actual = analyzeSource(source, ":/parity.c");
    QVERIFY(actual.error.isEmpty());
    QVERIFY(!actual.syntaxErrors);
    QCOMPARE(actual.symbols.size(), 1);
    QCOMPARE(actual.symbols[0].name, "f");
    QCOMPARE(actual.symbols[0].signature, "int f(");
    QVERIFY(!actual.symbols[0].definition);
}

void AnalysisParityTests::diagnosticLimit() {
    const auto actual =
        analyzeSource(QByteArray("def broken(:\n    return 1\n").repeated(20), ":/parity.py");
    QStringList expectedDiagnostics;
    for (int i = 0; i < 12; ++i)
        expectedDiagnostics.append(QString("Missing token at %1:12").arg(2 * i + 1));
    QCOMPARE(actual.diagnostics, expectedDiagnostics);
    QVERIFY(actual.syntaxErrors);
    QVERIFY(!actual.compilerEnriched);
}

QTEST_GUILESS_MAIN(AnalysisParityTests)
#include "AnalysisParityTests.moc"

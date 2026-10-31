#include "FileIO.h"
#include "ReferenceCandidateIndex.h"
#include "SymbolIndexStore.h"
#include "TestStorage.h"
#include "WorkspaceIndex.h"

#include <QDateTime>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTest>
#include <QUuid>

using namespace WorkspaceIndexDetail;

namespace {
class Fixture {
public:
    Fixture() {
        root = workspacePath(QDir::current().absoluteFilePath(
            "reference-candidates-" + QUuid::createUuid().toString(QUuid::WithoutBraces)));
        if (!QDir().mkpath(root))
            qFatal("Cannot create reference candidate fixture");
    }
    ~Fixture() { QDir(root).removeRecursively(); }
    QString path(const QString &name) const { return workspacePath(QDir(root).filePath(name)); }
    void write(const QString &name, const QByteArray &bytes) const {
        QFile file(path(name));
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
            qFatal("Cannot write reference candidate fixture");
    }
    IndexResult build(bool force = false) const {
        return buildWorkspaceIndex(root, force, true, defaultWorkspaceExcludes(),
                                   std::make_shared<std::atomic_bool>(false));
    }
    QString root;
};
}

class ReferenceCandidateIndexTests : public QObject {
    Q_OBJECT
    TestStorage storage_;
private slots:
    void allTextAndExactCase();
    void freshnessUsesContentsNotMetadata();
    void incrementalRefreshAndDeletion();
    void missingLegacyAndVersionedPostings();
    void unsupportedAndUnreadableContent();
    void unchangedInvalidUtf8RefreshIsQuiet();
    void cancellationAndAtomicRefresh();
};

void ReferenceCandidateIndexTests::allTextAndExactCase() {
    Fixture fixture;
    fixture.write("comments.cpp", "// Needle appears only in a comment\nint other;\n");
    fixture.write("strings.cpp", "const char *s = \"Needle\";\n");
    fixture.write("lower.cpp", "int needle;\n");
    fixture.write("prefix.cpp", "int NeedleSuffix; int _Needle; int xNeedle;\n");
    fixture.write("notes.txt", "Needle in a language without a parser\n");
    fixture.write("broken.cpp", "void broken( { // Needle\n");
    const auto built = fixture.build();
    QVERIFY2(built.error.isEmpty(), qPrintable(built.error));
    const QStringList paths{fixture.path("comments.cpp"), fixture.path("strings.cpp"),
                            fixture.path("lower.cpp"),    fixture.path("prefix.cpp"),
                            fixture.path("notes.txt"),    fixture.path("broken.cpp")};
    auto result = referenceCandidateFiles(fixture.root, paths, "Needle", true);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QCOMPARE(result.files, (QStringList{paths[0], paths[1], paths[4], paths[5]}));
    QCOMPARE(result.sourceHashes.size(), 2);
    QVERIFY(result.sourceHashes.contains(paths[2]));
    QVERIFY(result.sourceHashes.contains(paths[3]));
    QVERIFY(!result.sourceHashes.contains(paths[0]));
    result = referenceCandidateFiles(fixture.root, paths, "NEEDLE", false);
    QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
    QCOMPARE(result.files, (QStringList{paths[0], paths[1], paths[2], paths[4], paths[5]}));
    QCOMPARE(referenceCandidateFiles(fixture.root, paths, "Missing", true).files, QStringList{});
    for (const auto &query :
         QStringList{"", "Needle.*", "scope::Needle", "9Needle", QString::fromUtf8("\xc3\xa9")})
        QCOMPARE(referenceCandidateFiles(fixture.root, paths, query, true).files, paths);
    const QStringList repeated{paths[0], paths[3], paths[0]};
    QCOMPARE(referenceCandidateFiles(fixture.root, repeated, "Needle", true).files,
             (QStringList{paths[0], paths[0]}));
}

void ReferenceCandidateIndexTests::freshnessUsesContentsNotMetadata() {
    Fixture fixture;
    fixture.write("a.cpp", "// absent\n");
    const auto built = fixture.build();
    QVERIFY2(built.error.isEmpty(), qPrintable(built.error));
    const QStringList paths{fixture.path("a.cpp")};
    auto result = referenceCandidateFiles(fixture.root, paths, "Needle", true);
    QCOMPARE(result.files, QStringList{});
    QCOMPARE(result.sourceHashes.value(workspacePath(paths[0])),
             QCryptographicHash::hash("// absent\n", QCryptographicHash::Sha256).toHex());
    const auto originalTime = QFileInfo(paths[0]).lastModified();
    fixture.write("a.cpp", "// Needle\n");
    QFile file(paths[0]);
    QVERIFY(file.open(QIODevice::ReadWrite));
    QVERIFY(file.setFileTime(originalTime, QFileDevice::FileModificationTime));
    file.close();
    QCOMPARE(QFileInfo(paths[0]).lastModified(), originalTime);
    result = referenceCandidateFiles(fixture.root, paths, "Needle", true);
    QCOMPARE(result.files, paths);
    QCOMPARE(result.sourceHashes.value(workspacePath(paths[0])),
             QCryptographicHash::hash("// Needle\n", QCryptographicHash::Sha256).toHex());
    fixture.write("new.cpp", "// Needle\n");
    const QStringList newPaths{paths[0], fixture.path("new.cpp")};
    result = referenceCandidateFiles(fixture.root, newPaths, "Needle", true);
    QCOMPARE(result.files, newPaths);
    QVERIFY(!result.sourceHashes.contains(newPaths[1]));
    const QByteArray utf8 = "// \xc3\xa9 Needle\n";
    fixture.write("a.cpp", utf8);
    result = referenceCandidateFiles(fixture.root, paths, "Needle", true);
    QCOMPARE(result.files, paths);
    QCOMPARE(result.sourceHashes.value(workspacePath(paths[0])),
             QCryptographicHash::hash(utf8, QCryptographicHash::Sha256).toHex());
    fixture.write("a.cpp", QByteArray("\xff", 1));
    result = referenceCandidateFiles(fixture.root, paths, "Needle", true);
    QCOMPARE(result.files, paths);
    QVERIFY(result.sourceHashes.isEmpty());
    QVERIFY(QFile::remove(paths[0]));
    result = referenceCandidateFiles(fixture.root, paths, "Needle", true);
    QCOMPARE(result.files, paths);
    QVERIFY(result.sourceHashes.isEmpty());
}

void ReferenceCandidateIndexTests::incrementalRefreshAndDeletion() {
    Fixture fixture;
    fixture.write("a.cpp", "// Needle\n");
    fixture.write("b.txt", "Needle\n");
    auto built = fixture.build();
    QVERIFY2(built.error.isEmpty(), qPrintable(built.error));
    const QStringList paths{fixture.path("a.cpp"), fixture.path("b.txt")};
    fixture.write("a.cpp", "// absent\n");
    fixture.write("b.txt", "absent\n");
    built = fixture.build();
    QVERIFY2(built.error.isEmpty(), qPrintable(built.error));
    QCOMPARE(referenceCandidateFiles(fixture.root, paths, "Needle", true).files, QStringList{});
    fixture.write("a.cpp", "// Needle\n");
    built = fixture.build();
    QVERIFY2(built.error.isEmpty(), qPrintable(built.error));
    QCOMPARE(referenceCandidateFiles(fixture.root, paths, "Needle", true).files,
             QStringList{paths[0]});
    QVERIFY(QFile::remove(paths[0]));
    built = fixture.build();
    QVERIFY2(built.error.isEmpty(), qPrintable(built.error));
    SymbolIndexStore database(workspaceDatabase(fixture.root), false);
    IndexStatement count(database,
                         "SELECT (SELECT count(*) FROM reference_occurrence_files WHERE path=?) + "
                         "(SELECT count(*) FROM reference_occurrences WHERE path=?)");
    count.bind(1, paths[0]);
    count.bind(2, paths[0]);
    QVERIFY(count.next());
    QCOMPARE(count.integer(0), 0);
}

void ReferenceCandidateIndexTests::missingLegacyAndVersionedPostings() {
    Fixture fixture;
    fixture.write("a.cpp", "int other;\n");
    const QStringList paths{fixture.path("a.cpp")};
    QCOMPARE(referenceCandidateFiles(fixture.root, paths, "Needle", true).files, paths);
    auto built = fixture.build();
    QVERIFY2(built.error.isEmpty(), qPrintable(built.error));
    {
        SymbolIndexStore database(workspaceDatabase(fixture.root), true);
        database.exec("UPDATE reference_occurrence_files SET version=0;");
    }
    QCOMPARE(referenceCandidateFiles(fixture.root, paths, "Needle", true).files, paths);
    built = fixture.build();
    QVERIFY2(built.error.isEmpty(), qPrintable(built.error));
    QVERIFY(built.reused > 0);
    QCOMPARE(referenceCandidateFiles(fixture.root, paths, "Needle", true).files, QStringList{});
    {
        SymbolIndexStore database(workspaceDatabase(fixture.root), true);
        database.exec("DROP TABLE reference_occurrences; DROP TABLE reference_occurrence_files;");
    }
    auto result = referenceCandidateFiles(fixture.root, paths, "Needle", true);
    QVERIFY(!result.error.isEmpty());
    QCOMPARE(result.files, paths);
    built = fixture.build();
    QVERIFY2(built.error.isEmpty(), qPrintable(built.error));
    QVERIFY(built.reused > 0);
    QCOMPARE(referenceCandidateFiles(fixture.root, paths, "Needle", true).files, QStringList{});
    {
        SymbolIndexStore database(workspaceDatabase(fixture.root), true);
        database.exec("PRAGMA foreign_keys=ON; DELETE FROM reference_occurrence_files;");
    }
    QCOMPARE(referenceCandidateFiles(fixture.root, paths, "Needle", true).files, paths);
}

void ReferenceCandidateIndexTests::unsupportedAndUnreadableContent() {
    Fixture fixture;
    fixture.write("binary.dat", QByteArray("a\0b", 3));
    fixture.write("invalid.txt", QByteArray("\xff", 1));
    fixture.write("large.txt", QByteArray(MaximumDocumentBytes + 1, 'x'));
    fixture.write("unicode.txt", QByteArray("// \xe2\x84\xaa"
                                            "ey\n"));
    fixture.write("empty.txt", {});
    fixture.write("bom.txt", QByteArray("\xef\xbb\xbf// Needle\n"));
    const auto built = fixture.build();
    QVERIFY2(built.error.isEmpty(), qPrintable(built.error));
    const QStringList unreadable{fixture.path("binary.dat"), fixture.path("invalid.txt"),
                                 fixture.path("large.txt")};
    QCOMPARE(referenceCandidateFiles(fixture.root, unreadable, "Needle", true).files, unreadable);
    const QStringList unicode{fixture.path("unicode.txt")};
    QCOMPARE(referenceCandidateFiles(fixture.root, unicode, "key", false).files, unicode);
    QCOMPARE(referenceCandidateFiles(fixture.root, unicode, "Needle", true).files, QStringList{});
    const QStringList empty{fixture.path("empty.txt")};
    QCOMPARE(referenceCandidateFiles(fixture.root, empty, "Needle", true).files, QStringList{});
    const QStringList bom{fixture.path("bom.txt")};
    QCOMPARE(referenceCandidateFiles(fixture.root, bom, "Needle", true).files, bom);
    QCOMPARE(referenceCandidateFiles(fixture.root, bom, "Absent", true).files, QStringList{});
    fixture.write("empty.txt", QByteArray("a\0b", 3));
    const auto refreshed = fixture.build();
    QVERIFY2(refreshed.error.isEmpty(), qPrintable(refreshed.error));
    QCOMPARE(referenceCandidateFiles(fixture.root, empty, "Needle", true).files, empty);
    SymbolIndexStore database(workspaceDatabase(fixture.root), false);
    IndexStatement invalidated(database,
                               "SELECT count(*) FROM reference_occurrence_files WHERE path=?");
    invalidated.bind(1, empty[0]);
    QVERIFY(invalidated.next());
    QCOMPARE(invalidated.integer(0), 0);
}

void ReferenceCandidateIndexTests::unchangedInvalidUtf8RefreshIsQuiet() {
    Fixture fixture;
    fixture.write("a.cpp", "int other;\n");
    auto built = fixture.build();
    QVERIFY2(built.error.isEmpty(), qPrintable(built.error));
    fixture.write("a.cpp", QByteArray(1, '\xff'));
    built = fixture.build();
    QVERIFY2(built.error.isEmpty(), qPrintable(built.error));
    QVERIFY(built.changed);
    built = fixture.build();
    QVERIFY2(built.error.isEmpty(), qPrintable(built.error));
    QVERIFY(!built.changed);
}

void ReferenceCandidateIndexTests::cancellationAndAtomicRefresh() {
    Fixture fixture;
    fixture.write("a.cpp", "// absent\n");
    const auto built = fixture.build();
    QVERIFY2(built.error.isEmpty(), qPrintable(built.error));
    const QStringList paths{fixture.path("a.cpp")};
    auto cancel = std::make_shared<std::atomic_bool>(true);
    auto result = referenceCandidateFiles(fixture.root, paths, "Needle", true, cancel);
    QVERIFY(result.cancelled);
    QCOMPARE(result.files, paths);
    fixture.write("a.cpp", "// Needle\n");
    const auto cancelledBuild =
        buildWorkspaceIndex(fixture.root, false, true, defaultWorkspaceExcludes(), cancel);
    QVERIFY(cancelledBuild.cancelled);
    QCOMPARE(referenceCandidateFiles(fixture.root, paths, "Needle", true).files, paths);
    cancel->store(false);
    {
        SymbolIndexStore database(workspaceDatabase(fixture.root), true);
        database.exec("PRAGMA foreign_keys=ON; BEGIN IMMEDIATE;");
        updateReferenceOccurrences(database, paths[0], "// Needle\n", "uncommitted", cancel);
        // A concurrent reader sees the old, complete snapshot, not half-written postings.
        QCOMPARE(referenceCandidateFiles(fixture.root, paths, "Needle", true).files, paths);
        // Closing without COMMIT must roll back the postings and their completeness marker.
    }
    {
        SymbolIndexStore database(workspaceDatabase(fixture.root), false);
        IndexStatement posting(database, "SELECT count(*) FROM reference_occurrences "
                                         "WHERE identifier='Needle'");
        QVERIFY(posting.next());
        QCOMPARE(posting.integer(0), 0);
    }
}

QTEST_GUILESS_MAIN(ReferenceCandidateIndexTests)
#include "ReferenceCandidateIndexTests.moc"

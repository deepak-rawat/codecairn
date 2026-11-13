#include <codecairn/Document.h>
#include <codecairn/FileIO.h>
#include <codecairn/WorkspacePaths.h>
#include <QSignalSpy>
#include <QTest>

class DocumentAnalysisTests : public QObject {
    Q_OBJECT
private slots:
    void suppliedBytesKeepSavedAndLiveIdentitiesSeparate();
    void boundSnapshotsAreAcquiredOncePerRevision();
    void snapshotsRetainTheirContentsAndPosition();
    void unbindingRetainsCurrentContents();
    void pathsUseNormalizedIdentity();
    void externalChangesPreserveDirtyBuffers();
};

void DocumentAnalysisTests::suppliedBytesKeepSavedAndLiveIdentitiesSeparate() {
    const QByteArray original = "def before(): pass\n";
    codecairn::Document document("sample.py", original, true);
    QSignalSpy changed(&document, &codecairn::Document::contentsChanged);
    const auto initial = document.snapshot();
    QVERIFY(initial.bytes.constData() == original.constData());
    QCOMPARE(document.savedContentHash(), initial.hash);
    QVERIFY(!document.modified());
    const QByteArray replacement = "def after(): pass\n";
    document.updateContents(replacement);
    QCOMPARE(changed.size(), 1);
    QCOMPARE(document.revision(), quint64(1));
    const auto updated = document.snapshot();
    QCOMPARE(updated.bytes, replacement);
    QVERIFY(updated.bytes.constData() == replacement.constData());
    QCOMPARE(document.savedBytes(), original);
    QCOMPARE(document.savedContentHash(), initial.hash);
    QVERIFY(updated.hash != initial.hash);
    QVERIFY(document.modified());
    QVERIFY(document.hasUtf8Bom());
    document.setSavedContents(replacement, false);
    QCOMPARE(document.savedContentHash(), updated.hash);
    QVERIFY(!document.modified());
    QCOMPARE(document.revision(), quint64(1));
    QVERIFY(!document.hasUtf8Bom());
    document.setSavedContents(original, true);
    QCOMPARE(document.snapshot().bytes, replacement);
    QVERIFY(document.modified());
}

void DocumentAnalysisTests::boundSnapshotsAreAcquiredOncePerRevision() {
    codecairn::Document document("sample.py", "def before(): pass\n");
    QByteArray liveBytes = document.savedBytes();
    int reads = 0;
    document.bindContents([&] {
        ++reads;
        return liveBytes;
    });
    QCOMPARE(reads, 0);
    const auto initial = document.snapshot();
    QCOMPARE(initial.bytes, liveBytes);
    QCOMPARE(reads, 1);
    const auto repeated = document.snapshot();
    QVERIFY(repeated.bytes.constData() == initial.bytes.constData());
    QCOMPARE(repeated.hash, initial.hash);
    QCOMPARE(reads, 1);
    liveBytes = "def superseded(): pass\n";
    document.contentChanged();
    liveBytes = "def after(): pass\n";
    document.contentChanged();
    QCOMPARE(reads, 1);
    const auto updated = document.snapshot();
    QCOMPARE(updated.bytes, liveBytes);
    QCOMPARE(updated.revision, quint64(2));
    QCOMPARE(reads, 2);
    QCOMPARE(updated.hash, sourceContentHash(liveBytes));
    QVERIFY(updated.hash != initial.hash);
}

void DocumentAnalysisTests::snapshotsRetainTheirContentsAndPosition() {
    codecairn::Document document("sample.py", "before\n");
    const auto before = document.position(3);
    document.updateContents("after\n");
    QCOMPARE(before.snapshot.bytes, QByteArray("before\n"));
    QCOMPARE(before.snapshot.hash, sourceContentHash("before\n"));
    QCOMPARE(before.snapshot.revision, quint64(0));
    QCOMPARE(before.byteOffset, 3);
    QCOMPARE(document.position(-1).byteOffset, 0);
    QCOMPARE(document.position(100).byteOffset, 6);
    QCOMPARE(document.position(2).snapshot.revision, quint64(1));
}

void DocumentAnalysisTests::unbindingRetainsCurrentContents() {
    codecairn::Document document("sample.py", "before\n");
    QByteArray liveBytes = "after\n";
    int reads = 0;
    document.bindContents([&] {
        ++reads;
        return liveBytes;
    });
    document.contentChanged();
    document.bindContents({});
    QCOMPARE(reads, 1);
    liveBytes.clear();
    QCOMPARE(document.snapshot().bytes, QByteArray("after\n"));
    QCOMPARE(document.revision(), quint64(1));
    QCOMPARE(reads, 1);
    document.updateContents("standalone\n");
    QCOMPARE(document.snapshot().bytes, QByteArray("standalone\n"));
}

void DocumentAnalysisTests::pathsUseNormalizedIdentity() {
    codecairn::Document relative("./src/../sample.py", {});
    QCOMPARE(relative.path(), workspacePath("sample.py"));
    QCOMPARE(relative.snapshot().path, relative.path());
    codecairn::Document resource(":/examples/sample.py", {});
    QCOMPARE(resource.path(), QString(":/examples/sample.py"));
}

void DocumentAnalysisTests::externalChangesPreserveDirtyBuffers() {
    codecairn::Document document("sample.py", "def sample(): pass\n");
    using Change = codecairn::Document::ExternalChange;
    QCOMPARE(document.externalChange(document.savedContentHash(), true, false), Change::None);
    QCOMPARE(document.externalChange({}, true, false), Change::None);
    QCOMPARE(document.externalChange("changed", false, true), Change::Removed);
    QCOMPARE(document.externalChange("changed", true, true), Change::UnsavedBuffer);
    QCOMPARE(document.externalChange("changed", true, true), Change::None);
    QCOMPARE(document.externalChange("newer", true, true), Change::UnsavedBuffer);
    QCOMPARE(document.externalChange("newer", true, false), Change::Reload);
    QCOMPARE(document.savedBytes(), QByteArray("def sample(): pass\n"));
    QCOMPARE(document.revision(), quint64(0));
    document.setSavedContents(document.savedBytes(), false);
    QCOMPARE(document.externalChange("newer", true, true), Change::UnsavedBuffer);
}

QTEST_GUILESS_MAIN(DocumentAnalysisTests)
#include "DocumentAnalysisTests.moc"

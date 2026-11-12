#include "DocumentAnalysis.h"
#include "FileIO.h"
#include <QCryptographicHash>
#include <QSignalSpy>
#include <QTest>

namespace {
class SnapshotDocument final : public DocumentAnalysis {
public:
    SnapshotDocument()
        : DocumentAnalysis("sample.py", "def before(): pass\n", false, 10), liveBytes(savedBytes) {}

    QByteArray liveBytes;
    mutable int snapshotReads = 0;

protected:
    QByteArray readSnapshotBytes() const override {
        ++snapshotReads;
        return liveBytes;
    }
};
}

class DocumentAnalysisTests : public QObject {
    Q_OBJECT
private slots:
    void suppliedBytesKeepSavedAndLiveIdentitiesSeparate();
    void boundSnapshotsAreAcquiredOncePerRevision();
    void staleAnalysisUsesLatestBytes();
    void compilationDirectoryInvalidatesAnalysis();
    void externalChangesPreserveDirtyBuffers();
    void oversizedUpdateRejectsStaleAnalysis();
    void stoppingDisconnectsCompletion();
};

void DocumentAnalysisTests::suppliedBytesKeepSavedAndLiveIdentitiesSeparate() {
    const QByteArray original = "def before(): pass\n";
    DocumentAnalysis document("sample.py", original, true, 10);
    QVERIFY(document.analyzedRevision != document.revision);
    QVERIFY(document.snapshotBytes().constData() == original.constData());
    const auto originalHash = document.snapshotHash;
    QCOMPARE(document.savedContentHash(), originalHash);
    const QByteArray replacement = "def after(): pass\n";
    document.setBytes(replacement);
    QCOMPARE(document.revision, quint64(1));
    QCOMPARE(document.snapshotBytes(), replacement);
    QVERIFY(document.snapshotBytes().constData() == replacement.constData());
    QCOMPARE(document.savedBytes, original);
    QCOMPARE(document.savedContentHash(), originalHash);
    QVERIFY(document.snapshotHash != originalHash);
    QVERIFY(document.hasUtf8Bom);
    document.setSavedContents(document.snapshotBytes(), false);
    QCOMPARE(document.savedContentHash(), document.snapshotHash);
    QVERIFY(!document.hasUtf8Bom);
}

void DocumentAnalysisTests::boundSnapshotsAreAcquiredOncePerRevision() {
    SnapshotDocument document;
    QCOMPARE(document.snapshotReads, 0);
    QCOMPARE(document.snapshotBytes(), document.liveBytes);
    QCOMPARE(document.snapshotReads, 1);
    const auto *identity = document.snapshotBytes().constData();
    const auto hash = document.snapshotHash;
    document.snapshotBytes();
    QVERIFY(document.snapshotBytes().constData() == identity);
    QCOMPARE(document.snapshotHash, hash);
    QCOMPARE(document.snapshotReads, 1);
    document.liveBytes = "def after(): pass\n";
    ++document.revision;
    QCOMPARE(document.snapshotReads, 1);
    QCOMPARE(document.snapshotBytes(), document.liveBytes);
    QCOMPARE(document.snapshotReads, 2);
    QVERIFY(document.snapshotHash != hash);
}

void DocumentAnalysisTests::staleAnalysisUsesLatestBytes() {
    DocumentAnalysis document("sample.py", "def before(): pass\n", false, 10);
    QSignalSpy completed(&document, &DocumentAnalysis::analysisCompleted);
    document.startAnalysis();
    document.setBytes("def superseded(): pass\n");
    document.setBytes("def after(): pass\n");
    document.startAnalysis();
    QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 10000);
    QCOMPARE(document.analyzedRevision, document.revision);
    QCOMPARE(document.analysis.symbols.size(), 1);
    QCOMPARE(document.analysis.symbols.first().name, QString("after"));
    QCOMPARE(
        document.snapshotHash,
        QCryptographicHash::hash(document.snapshotBytes(), QCryptographicHash::Sha256).toHex());
}

void DocumentAnalysisTests::compilationDirectoryInvalidatesAnalysis() {
    DocumentAnalysis document("sample.py", "def sample(): pass\n", false, 0);
    QSignalSpy completed(&document, &DocumentAnalysis::analysisCompleted);
    document.startAnalysis();
    QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 1, 10000);
    document.startAnalysis();
    document.setCompilationDirectory("compile-directory");
    QVERIFY(document.analyzedRevision != document.revision);
    QTRY_COMPARE_WITH_TIMEOUT(completed.size(), 2, 10000);
    QCOMPARE(document.analyzedRevision, document.revision);
    document.setCompilationDirectory("compile-directory");
    QCOMPARE(document.analyzedRevision, document.revision);
}

void DocumentAnalysisTests::externalChangesPreserveDirtyBuffers() {
    DocumentAnalysis document("sample.py", "def sample(): pass\n", false, 10);
    using Change = DocumentAnalysis::ExternalChange;
    QCOMPARE(document.externalChange(document.savedContentHash(), true, false), Change::None);
    QCOMPARE(document.externalChange({}, true, false), Change::None);
    QCOMPARE(document.externalChange("changed", false, true), Change::Removed);
    QCOMPARE(document.externalChange("changed", true, true), Change::UnsavedBuffer);
    QCOMPARE(document.externalChange("changed", true, true), Change::None);
    QCOMPARE(document.externalChange("newer", true, true), Change::UnsavedBuffer);
    QCOMPARE(document.externalChange("newer", true, false), Change::Reload);
    QCOMPARE(document.savedBytes, QByteArray("def sample(): pass\n"));
    QCOMPARE(document.revision, quint64(0));
}

void DocumentAnalysisTests::oversizedUpdateRejectsStaleAnalysis() {
    DocumentAnalysis document("sample.py", "def sample(): pass\n", false, 10);
    QSignalSpy completed(&document, &DocumentAnalysis::analysisCompleted);
    QSignalSpy rejected(&document, &DocumentAnalysis::analysisRejected);
    document.startAnalysis();
    document.setBytes(QByteArray(MaximumDocumentBytes + 1, 'x'));
    QTRY_COMPARE_WITH_TIMEOUT(rejected.size(), 1, 10000);
    QCOMPARE(completed.size(), 0);
    QCOMPARE(document.analyzedRevision, document.revision);
    QVERIFY(document.analysis.error.contains("4 MiB"));
    QVERIFY(!document.isAnalysisRunning());
}

void DocumentAnalysisTests::stoppingDisconnectsCompletion() {
    DocumentAnalysis document("sample.py", "def sample(): pass\n", false, 10);
    QSignalSpy completed(&document, &DocumentAnalysis::analysisCompleted);
    document.startAnalysis();
    document.stopBackgroundWork();
    QCoreApplication::processEvents();
    QCOMPARE(completed.size(), 0);
}

QTEST_GUILESS_MAIN(DocumentAnalysisTests)
#include "DocumentAnalysisTests.moc"

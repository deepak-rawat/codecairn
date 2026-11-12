#include "DocumentAnalysis.h"
#include "FileIO.h"
#include "Diagnostics.h"
#include <QCryptographicHash>
#include <QtConcurrentRun>

DocumentAnalysis::DocumentAnalysis(const QString &path, const QByteArray &bytes, bool hasUtf8Bom,
                                   int analysisDebounceMs, QObject *parent)
    : QObject(parent), path(path), analysisTimer_(this), analysisWatcher_(this), snapshot_(bytes) {
    setSavedContents(bytes, hasUtf8Bom);
    analysisTimer_.setSingleShot(true);
    analysisTimer_.setInterval(analysisDebounceMs);
    connect(&analysisTimer_, &QTimer::timeout, this, &DocumentAnalysis::startAnalysis);
    connect(&analysisWatcher_, &QFutureWatcher<SourceAnalysis>::finished, this, [this] {
        analysisBusy_ = false;
        if (runningRevision_ != revision || runningCompilationDirectory_ != compilationDirectory_) {
            qCDebug(analysisLog) << "analysis.stale" << "path=" << this->path
                                 << "completedRevision=" << runningRevision_
                                 << "currentRevision=" << revision;
            startAnalysis();
            return;
        }
        analysis = analysisWatcher_.result();
        analyzedRevision = revision;
        qCDebug(analysisLog) << "analysis.accepted" << "path=" << this->path
                             << "revision=" << revision;
        emit analysisCompleted();
    });
}

DocumentAnalysis::~DocumentAnalysis() {
    stopBackgroundWork();
}

void DocumentAnalysis::setBytes(const QByteArray &bytes) {
    snapshot_ = bytes;
    ++revision;
    snapshotRevision_ = std::numeric_limits<quint64>::max();
}

QByteArray DocumentAnalysis::readSnapshotBytes() const {
    return snapshot_;
}

const QByteArray &DocumentAnalysis::snapshotBytes() {
    if (snapshotRevision_ != revision) {
        snapshot_ = readSnapshotBytes();
        snapshotHash = QCryptographicHash::hash(snapshot_, QCryptographicHash::Sha256).toHex();
        snapshotRevision_ = revision;
    }
    return snapshot_;
}

void DocumentAnalysis::setSavedContents(const QByteArray &bytes, bool bom) {
    savedBytes = bytes;
    hasUtf8Bom = bom;
    savedContentHash_ = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

void DocumentAnalysis::startAnalysis() {
    if (analysisBusy_)
        return;
    analysisTimer_.stop();
    const auto bytes = snapshotBytes();
    if (bytes.size() > MaximumDocumentBytes) {
        analysis = {};
        analysis.error = "Live analysis paused: document exceeds the prototype's 4 MiB limit.";
        qCWarning(analysisLog) << "analysis.rejected" << "path=" << path << "revision=" << revision
                               << "error=" << analysis.error;
        analyzedRevision = revision;
        emit analysisRejected(analysis.error);
        return;
    }
    runningRevision_ = revision;
    runningCompilationDirectory_ = compilationDirectory_;
    analysisBusy_ = true;
    analysisWatcher_.setFuture(
        QtConcurrent::run([bytes, filePath = path, directory = compilationDirectory_] {
            return analyzeSource(bytes, filePath, SourceAnalysisMode::Editor, directory);
        }));
}

void DocumentAnalysis::setCompilationDirectory(const QString &directory) {
    if (compilationDirectory_ == directory)
        return;
    compilationDirectory_ = directory;
    analyzedRevision = std::numeric_limits<quint64>::max();
    analysisTimer_.start();
}

void DocumentAnalysis::stopBackgroundWork() {
    analysisTimer_.stop();
    disconnect(&analysisWatcher_, nullptr, this, nullptr);
    analysisWatcher_.waitForFinished();
}

DocumentAnalysis::ExternalChange DocumentAnalysis::externalChange(const QByteArray &hash,
                                                                  bool fileExists, bool modified) {
    if (!fileExists)
        return ExternalChange::Removed;
    if (hash.isEmpty() || hash == savedContentHash())
        return ExternalChange::None;
    if (modified) {
        if (lastReportedExternalHash == hash)
            return ExternalChange::None;
        lastReportedExternalHash = hash;
        return ExternalChange::UnsavedBuffer;
    }
    return ExternalChange::Reload;
}

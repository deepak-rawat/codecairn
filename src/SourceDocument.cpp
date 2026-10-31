#include "SourceDocument.h"
#include "CodeEditor.h"
#include "FileIO.h"
#include "Diagnostics.h"
#include <QCryptographicHash>
#include <QFileInfo>
#include <QtConcurrentRun>

SourceDocument::SourceDocument(const QString &path, const QByteArray &bytes, bool hasUtf8Bom,
                               int analysisDebounceMs, QObject *parent)
    : QObject(parent), editor(new CodeEditor), path(path), analysisTimer_(this),
      analysisWatcher_(this), reloadWatcher_(this) {
    setSavedContents(bytes, hasUtf8Bom);
    editor->loadBytes(bytes);
    connect(editor, &CodeEditor::contentEdited, this, [this] { ++revision; });
    analysisTimer_.setSingleShot(true);
    analysisTimer_.setInterval(analysisDebounceMs);
    connect(&analysisTimer_, &QTimer::timeout, this, &SourceDocument::startAnalysis);
    connect(&analysisWatcher_, &QFutureWatcher<SourceAnalysis>::finished, this, [this] {
        analysisBusy_ = false;
        if (runningRevision_ != revision) {
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

SourceDocument::~SourceDocument() {
    stopBackgroundWork();
}

const QByteArray &SourceDocument::snapshotBytes() {
    if (snapshotRevision_ != revision) {
        snapshot_ = editor->bytes();
        snapshotHash = QCryptographicHash::hash(snapshot_, QCryptographicHash::Sha256).toHex();
        snapshotRevision_ = revision;
    }
    return snapshot_;
}

void SourceDocument::setSavedContents(const QByteArray &bytes, bool bom) {
    savedBytes = bytes;
    hasUtf8Bom = bom;
    savedContentHash_ = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

void SourceDocument::startAnalysis() {
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
    analysisBusy_ = true;
    analysisWatcher_.setFuture(
        QtConcurrent::run([bytes, filePath = path] { return analyzeSource(bytes, filePath); }));
}

void SourceDocument::stopBackgroundWork() {
    analysisTimer_.stop();
    disconnect(&analysisWatcher_, nullptr, this, nullptr);
    analysisWatcher_.waitForFinished();
    disconnect(&reloadWatcher_, nullptr, this, nullptr);
    reloadWatcher_.waitForFinished();
}

void SourceDocument::reconcileDiskHash(const QByteArray &hash) {
    if (!QFileInfo::exists(path)) {
        emit statusMessage("File removed externally; keeping the open buffer: " + path, 8000);
        return;
    }
    if (hash.isEmpty() || hash == savedContentHash())
        return;
    if (editor->isModified()) {
        if (lastReportedExternalHash != hash) {
            lastReportedExternalHash = hash;
            emit statusMessage("File changed externally; unsaved buffer preserved: " + path, 8000);
        }
        return;
    }
    if (reloading_)
        return;
    reloading_ = true;
    const auto requestedRevision = revision;
    const auto requestedPath = path;
    disconnect(&reloadWatcher_, &QFutureWatcher<FileContents>::finished, this, nullptr);
    connect(
        &reloadWatcher_, &QFutureWatcher<FileContents>::finished, this,
        [this, requestedRevision, requestedPath, hash] {
            reloading_ = false;
            const auto file = reloadWatcher_.result();
            if (revision != requestedRevision || editor->isModified() || path != requestedPath)
                return;
            if (!file.error.isEmpty()) {
                emit backgroundError(file.error);
                return;
            }
            if (QCryptographicHash::hash(file.bytes, QCryptographicHash::Sha256).toHex() != hash) {
                emit statusMessage(
                    "File is still changing externally; refresh again when writes finish.", 5000);
                return;
            }
            const auto position = editor->caretPosition();
            editor->loadBytes(file.bytes);
            const int boundedPosition = qMin(position, static_cast<int>(file.bytes.size()));
            editor->selectRange(boundedPosition, boundedPosition);
            setSavedContents(file.bytes, file.utf8Bom);
            ++revision;
            emit diskReloaded();
            startAnalysis();
            emit statusMessage("Reloaded externally changed file: " + path, 6000);
        });
    reloadWatcher_.setFuture(
        QtConcurrent::run([requestedPath] { return readSourceFile(requestedPath); }));
}

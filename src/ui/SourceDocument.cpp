#include "SourceDocument.h"
#include "CodeEditor.h"
#include <codecairn/FileIO.h>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QtConcurrentRun>

SourceDocument::SourceDocument(const QString &path, const QByteArray &bytes, bool hasUtf8Bom,
                               int analysisDebounceMs, QObject *parent)
    : DocumentAnalysis(path, bytes, hasUtf8Bom, analysisDebounceMs, parent), editor(new CodeEditor),
      reloadWatcher_(this) {
    editor->loadBytes(bytes);
    connect(editor, &CodeEditor::contentEdited, this, [this] { ++revision; });
}

SourceDocument::~SourceDocument() {
    stopBackgroundWork();
}

QByteArray SourceDocument::readSnapshotBytes() const {
    return editor->bytes();
}

void SourceDocument::stopBackgroundWork() {
    DocumentAnalysis::stopBackgroundWork();
    disconnect(&reloadWatcher_, nullptr, this, nullptr);
    reloadWatcher_.waitForFinished();
}

void SourceDocument::reconcileDiskHash(const QByteArray &hash) {
    switch (externalChange(hash, QFileInfo::exists(path), editor->isModified())) {
    case ExternalChange::Removed:
        emit statusMessage("File removed externally; keeping the open buffer: " + path, 8000);
        return;
    case ExternalChange::None:
        return;
    case ExternalChange::UnsavedBuffer:
        emit statusMessage("File changed externally; unsaved buffer preserved: " + path, 8000);
        return;
    case ExternalChange::Reload:
        break;
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

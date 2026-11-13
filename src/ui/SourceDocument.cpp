#include "SourceDocument.h"
#include "CodeEditor.h"
#include <codecairn/Diagnostics.h>
#include <codecairn/Project.h>
#include <QFileInfo>
#include <QtConcurrentRun>

SourceDocument::SourceDocument(codecairn::Document *source, codecairn::Engine *engine,
                               int analysisDebounceMs, QObject *parent)
    : QObject(parent), editor(new CodeEditor), source_(source), engine_(engine),
      analysisTimer_(this), reloadWatcher_(this) {
    Q_ASSERT(source);
    editor->loadBytes(source->contents());
    const QPointer<CodeEditor> boundEditor(editor);
    source->bindContents(
        [boundEditor] { return boundEditor ? boundEditor->bytes() : QByteArray{}; });
    connect(editor, &CodeEditor::contentEdited, this, [this] {
        if (source_ && !stopped_)
            source_->contentChanged();
    });
    connect(editor, &QObject::destroyed, this, [this] {
        editor = nullptr;
        stopBackgroundWork();
    });
    connect(source, &QObject::destroyed, this, &SourceDocument::stopBackgroundWork);
    analysisTimer_.setSingleShot(true);
    analysisTimer_.setInterval(analysisDebounceMs);
    connect(&analysisTimer_, &QTimer::timeout, this, &SourceDocument::startAnalysis);
    if (engine_)
        connect(engine_, &codecairn::Engine::analysisReady, this, &SourceDocument::acceptAnalysis);
}

SourceDocument::~SourceDocument() {
    stopBackgroundWork();
}

void SourceDocument::setSavedContents(const QByteArray &bytes, bool hasUtf8Bom) {
    source_->setSavedContents(bytes, hasUtf8Bom);
}

void SourceDocument::scheduleAnalysis() {
    if (!stopped_)
        analysisTimer_.start();
}

void SourceDocument::setAnalysisDebounceMs(int milliseconds) {
    analysisTimer_.setInterval(milliseconds);
}

void SourceDocument::setProject(codecairn::Project *project) {
    if (project_ == project)
        return;
    cancelAnalysis();
    disconnect(projectAnalysisConnection_);
    disconnect(contextAnalysisConnection_);
    disconnect(projectDestroyedConnection_);
    project_ = project;
    if (project_) {
        projectAnalysisConnection_ = connect(project_, &codecairn::Project::analysisReady, this,
                                             &SourceDocument::acceptAnalysis);
        contextAnalysisConnection_ =
            connect(project_, &codecairn::Project::contextChanged, this, [this] {
                cancelAnalysis();
                analyzedRevision = std::numeric_limits<quint64>::max();
                scheduleAnalysis();
            });
        projectDestroyedConnection_ = connect(project_, &QObject::destroyed, this, [this] {
            cancelAnalysis();
            analyzedRevision = std::numeric_limits<quint64>::max();
            scheduleAnalysis();
        });
    }
    analyzedRevision = std::numeric_limits<quint64>::max();
    scheduleAnalysis();
}

void SourceDocument::startAnalysis() {
    if (stopped_ || !source_ || analysisBusy_)
        return;
    analysisTimer_.stop();
    const auto current = source_->snapshot();
    if (current.bytes.size() > MaximumDocumentBytes) {
        analysis = {};
        analysis.error = "Live analysis paused: document exceeds the prototype's 4 MiB limit.";
        analyzedRevision = current.revision;
        emit analysisRejected(analysis.error);
        return;
    }
    if (!engine_ && !project_) {
        emit analysisRejected("Live analysis is unavailable: the engine has been closed.");
        return;
    }
    runningProject_ = project_;
    analysisRequest_ = project_ ? project_->analyze(current) : engine_->analyze(current);
    analysisBusy_ = static_cast<bool>(analysisRequest_);
    if (!analysisBusy_)
        emit analysisRejected("Live analysis could not be started.");
}

void SourceDocument::acceptAnalysis(codecairn::RequestId request,
                                    const codecairn::AnalysisResult &result) {
    if (stopped_ || !source_ || !analysisBusy_ || request != analysisRequest_)
        return;
    analysisBusy_ = false;
    analysisRequest_ = {};
    if (result.source.revision != source_->revision() || result.source.path != source_->path() ||
        runningProject_ != project_) {
        qCDebug(analysisLog) << "analysis.stale" << "path=" << path()
                             << "completedRevision=" << result.source.revision
                             << "currentRevision=" << revision();
        startAnalysis();
        return;
    }
    analysisTimer_.stop();
    analysis = result.analysis;
    analyzedRevision = result.source.revision;
    qCDebug(analysisLog) << "analysis.accepted" << "path=" << path() << "revision=" << revision();
    emit analysisCompleted();
}

void SourceDocument::cancelAnalysis() {
    if (analysisRequest_) {
        if (runningProject_)
            runningProject_->cancel(analysisRequest_);
        else if (engine_)
            engine_->cancel(analysisRequest_);
    }
    analysisRequest_ = {};
    analysisBusy_ = false;
    runningProject_.clear();
}

void SourceDocument::stopBackgroundWork() {
    if (stopped_)
        return;
    stopped_ = true;
    analysisTimer_.stop();
    cancelAnalysis();
    disconnect(projectAnalysisConnection_);
    disconnect(contextAnalysisConnection_);
    disconnect(projectDestroyedConnection_);
    disconnect(&reloadWatcher_, nullptr, this, nullptr);
    reloadWatcher_.waitForFinished();
    if (source_)
        source_->bindContents({});
}

void SourceDocument::reconcileDiskHash(const QByteArray &hash) {
    if (!source_ || !editor)
        return;
    using Change = codecairn::Document::ExternalChange;
    switch (source_->externalChange(hash, QFileInfo::exists(path()), editor->isModified())) {
    case Change::Removed:
        emit statusMessage("File removed externally; keeping the open buffer: " + path(), 8000);
        return;
    case Change::None:
        return;
    case Change::UnsavedBuffer:
        emit statusMessage("File changed externally; unsaved buffer preserved: " + path(), 8000);
        return;
    case Change::Reload:
        break;
    }
    if (stopped_ || reloading_)
        return;
    reloading_ = true;
    const auto requestedRevision = revision();
    const auto requestedPath = path();
    disconnect(&reloadWatcher_, &QFutureWatcher<FileContents>::finished, this, nullptr);
    connect(&reloadWatcher_, &QFutureWatcher<FileContents>::finished, this,
            [this, requestedRevision, requestedPath, hash] {
                reloading_ = false;
                const auto file = reloadWatcher_.result();
                if (!source_ || revision() != requestedRevision || editor->isModified() ||
                    path() != requestedPath)
                    return;
                if (!file.error.isEmpty()) {
                    emit backgroundError(file.error);
                    return;
                }
                if (sourceContentHash(file.bytes) != hash) {
                    emit statusMessage(
                        "File is still changing externally; refresh again when writes finish.",
                        5000);
                    return;
                }
                const auto position = editor->caretPosition();
                editor->loadBytes(file.bytes);
                const int boundedPosition = qMin(position, static_cast<int>(file.bytes.size()));
                editor->selectRange(boundedPosition, boundedPosition);
                source_->setSavedContents(file.bytes, file.utf8Bom);
                source_->contentChanged();
                emit diskReloaded();
                startAnalysis();
                emit statusMessage("Reloaded externally changed file: " + path(), 6000);
            });
    reloadWatcher_.setFuture(
        QtConcurrent::run([requestedPath] { return readSourceFile(requestedPath); }));
}

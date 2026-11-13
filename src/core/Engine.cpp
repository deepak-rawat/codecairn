#include <codecairn/Engine.h>
#include <codecairn/Project.h>
#include <codecairn/Diagnostics.h>
#include <codecairn/FileIO.h>
#include <codecairn/WorkspacePaths.h>
#include <QFutureWatcher>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QtConcurrentRun>
#include <atomic>

namespace codecairn {

namespace {
QString documentKey(QString key) {
#ifdef Q_OS_WIN
    key = key.toCaseFolded();
#endif
    return key;
}
}

PreparedProject prepareProject(const ProjectSpec &spec) {
    if (spec.path.trimmed().isEmpty()) {
        PreparedProject failed;
        failed.error = "Project path must not be empty.";
        qCWarning(storageLog) << "project.prepare_failed" << failed.error;
        return failed;
    }
    if (spec.kind == ProjectSpec::Kind::Folder)
        return ::prepareProject(spec.path);
    const auto loaded = WorkspaceOptionsStore::loadProject(spec.path);
    if (!loaded.error.isEmpty()) {
        PreparedProject failed;
        failed.error = loaded.error;
        return failed;
    }
    return ::prepareProject(loaded.root, workspacePath(spec.path));
}

struct Engine::State {
    QHash<QString, Document *> documents;
    QSet<Project *> projects;
    QHash<quint64, Project *> analyses;
    struct Read {
        RequestId id;
        SourceLocation location;
        std::optional<IndexedSymbol> symbol;
    };
    QQueue<Read> reads;
    QSet<quint64> activeReads;
    QFutureWatcher<SourceRead> reader;
    RequestId runningRead;
    bool reading = false;
};

Engine::Engine(QObject *parent) : QObject(parent), state_(std::make_unique<State>()) {
    connect(&state_->reader, &QFutureWatcher<SourceRead>::finished, this, [this] {
        const auto id = state_->runningRead;
        state_->reading = false;
        if (state_->activeReads.remove(id.value))
            emit sourceReady(id, state_->reader.result());
        startRead();
    });
}

Engine::~Engine() {
    for (auto *project : std::as_const(state_->projects))
        delete project;
    state_->projects.clear();
    state_->analyses.clear();
    state_->activeReads.clear();
    disconnect(&state_->reader, nullptr, this, nullptr);
    state_->reader.waitForFinished();
    for (auto *watcher : findChildren<QFutureWatcherBase *>()) {
        disconnect(watcher, nullptr, this, nullptr);
        watcher->waitForFinished();
    }
    qDeleteAll(state_->documents);
    state_->documents.clear();
}

RequestId Engine::nextRequest() {
    static std::atomic<quint64> sequence{0};
    return {sequence.fetch_add(1, std::memory_order_relaxed) + 1};
}

Project *Engine::createProject() {
    auto *project = new Project(this);
    state_->projects.insert(project);
    return project;
}

Result<Project *> Engine::openProject(const ProjectSpec &spec, const ProjectOptions &options) {
    return openProject(prepareProject(spec), options);
}

Result<Project *> Engine::openProject(const PreparedProject &prepared,
                                      const ProjectOptions &options) {
    auto *project = createProject();
    const auto opened = project->open(prepared, options);
    if (!opened) {
        state_->projects.remove(project);
        delete project;
        return Result<Project *>::failure(opened.error().code, opened.error().message);
    }
    return Result<Project *>::success(project);
}

Result<Document *> Engine::createDocument(const QString &path, const QByteArray &contents,
                                          bool hasUtf8Bom) {
    if (path.isEmpty()) {
        qCWarning(fileLog) << "document.open_failed" << "Empty source path.";
        return Result<Document *>::failure(ErrorCode::InvalidArgument, "Empty source path.");
    }
    const auto identity = path.startsWith(":/") ? path : workspacePath(path);
    if (auto *existing = state_->documents.value(documentKey(identity)))
        return Result<Document *>::success(existing);
    auto *document = new Document(identity, contents, hasUtf8Bom, this);
    state_->documents.insert(documentKey(identity), document);
    return Result<Document *>::success(document);
}

Result<Document *> Engine::openDocument(const QString &path) {
    if (auto *existing = document(path))
        return Result<Document *>::success(existing);
    const auto source = readSourceFile(path);
    if (!source.error.isEmpty())
        return Result<Document *>::failure(ErrorCode::Io, source.error);
    return createDocument(path, source.bytes, source.utf8Bom);
}

Document *Engine::document(const QString &path) const {
    if (path.isEmpty() || state_->documents.isEmpty())
        return nullptr;
    if (auto *known = state_->documents.value(documentKey(path)))
        return known;
    return state_->documents.value(documentKey(path.startsWith(":/") ? path : workspacePath(path)));
}

Result<void> Engine::renameDocument(Document *document, const QString &path) {
    if (!document || state_->documents.value(documentKey(document->path())) != document ||
        path.isEmpty()) {
        qCWarning(fileLog) << "document.rename_failed" << "Invalid document or path.";
        return Result<void>::failure(ErrorCode::InvalidArgument, "Invalid document or path.");
    }
    const auto identity = path.startsWith(":/") ? path : workspacePath(path);
    if (identity == document->path())
        return Result<void>::success();
    if (auto *existing = state_->documents.value(documentKey(identity));
        existing && existing != document) {
        qCWarning(fileLog) << "document.rename_failed" << "Target is already open:" << identity;
        return Result<void>::failure(ErrorCode::Conflict, "Target is already open: " + identity);
    }
    const auto old = document->path();
    state_->documents.remove(documentKey(old));
    document->setPath(identity);
    state_->documents.insert(documentKey(identity), document);
    emit documentClosed(old);
    return Result<void>::success();
}

Result<void> Engine::closeDocument(Document *document) {
    if (!document || state_->documents.value(documentKey(document->path())) != document) {
        qCWarning(fileLog) << "document.close_failed" << "Document belongs to another engine.";
        return Result<void>::failure(ErrorCode::InvalidArgument,
                                     "Document belongs to another engine.");
    }
    const auto path = document->path();
    state_->documents.remove(documentKey(path));
    emit documentClosed(path);
    delete document;
    return Result<void>::success();
}

Result<void> Engine::closeProject(Project *project) {
    if (!state_->projects.remove(project)) {
        qCWarning(storageLog) << "project.close_failed" << "Project belongs to another engine.";
        return Result<void>::failure(ErrorCode::InvalidArgument,
                                     "Project belongs to another engine.");
    }
    delete project;
    return Result<void>::success();
}

RequestId Engine::analyze(const SourceSnapshot &snapshot, AnalysisDetail detail) {
    return analyzeFor(nullptr, snapshot, detail);
}

RequestId Engine::analyzeFor(Project *project, const SourceSnapshot &snapshot,
                             AnalysisDetail detail) {
    const auto id = nextRequest();
    state_->analyses.insert(id.value, project);
    const auto directory =
        project && project->contains(snapshot.path)
            ? workspaceClangdDirectory(project->info().root, project->info().projectFile)
            : QString{};
    auto *watcher = new QFutureWatcher<AnalysisResult>(this);
    connect(watcher, &QFutureWatcher<AnalysisResult>::finished, this,
            [this, watcher, id, context = QPointer<Project>(project)] {
                const auto result = watcher->result();
                watcher->deleteLater();
                if (!state_->analyses.contains(id.value))
                    return;
                state_->analyses.remove(id.value);
                if (context)
                    emit context->analysisReady(id, result);
                else
                    emit analysisReady(id, result);
            });
    const auto mode =
        detail == AnalysisDetail::Symbols ? SourceAnalysisMode::Index : SourceAnalysisMode::Editor;
    watcher->setFuture(QtConcurrent::run([snapshot, directory, mode] {
        return AnalysisResult{snapshot,
                              analyzeSource(snapshot.bytes, snapshot.path, mode, directory)};
    }));
    return id;
}

QVector<SymbolOverlay> Engine::overlays(const Project *project) const {
    QVector<SymbolOverlay> result;
    result.reserve(state_->documents.size());
    for (auto *document : state_->documents) {
        if (project && !project->info().root.isEmpty() && !project->contains(document->path()))
            continue;
        const auto snapshot = document->snapshot();
        const auto analysis = project ? project->cachedAnalysis(snapshot) : SourceAnalysis{};
        result.append({snapshot.path, snapshot.bytes, analysis, snapshot.hash});
    }
    return result;
}

void Engine::cancelFor(Project *project) {
    for (auto it = state_->analyses.begin(); it != state_->analyses.end();) {
        if (it.value() == project)
            it = state_->analyses.erase(it);
        else
            ++it;
    }
}

void Engine::cancel(RequestId request) {
    state_->analyses.remove(request.value);
    state_->activeReads.remove(request.value);
}

RequestId Engine::readSource(const SourceLocation &location,
                             const std::optional<IndexedSymbol> &symbol) {
    const auto id = nextRequest();
    state_->reads.enqueue({id, location, symbol});
    state_->activeReads.insert(id.value);
    startRead();
    return id;
}

void Engine::startRead() {
    if (state_->reading)
        return;
    while (!state_->reads.isEmpty()) {
        const auto request = state_->reads.dequeue();
        if (!state_->activeReads.contains(request.id.value))
            continue;
        const auto *live = document(request.location.path);
        const auto snapshot = live ? std::optional<SourceSnapshot>(live->snapshot()) : std::nullopt;
        state_->runningRead = request.id;
        state_->reading = true;
        state_->reader.setFuture(QtConcurrent::run([request, snapshot] {
            SourceRead result;
            result.location = request.location;
            result.file = snapshot ? FileContents{snapshot->bytes, {}, false}
                                   : readSourceFile(request.location.path);
            if (result.file.error.isEmpty())
                result.range =
                    resolveNavigationTarget(request.location, result.file.bytes, request.symbol);
            return result;
        }));
        return;
    }
}

}

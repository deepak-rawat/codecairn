#include <codecairn/Project.h>
#include <codecairn/Diagnostics.h>
#include "DefinitionQueryExecutor.h"
#include "SearchExecutor.h"
#include "ProjectContext.h"
#include "ProjectAnalysisCache.h"
#include "ProjectIndexer.h"
#include "ProjectSymbolQuery.h"
#include <QFileInfo>
#include <QTimer>
#include <QSet>
#include <utility>
#include <algorithm>

namespace codecairn {

struct Project::State {
    explicit State(Engine *engine)
        : engine(engine), definitions(&languages),
          searches(&languages, [engine](const QString &path) -> std::optional<QByteArray> {
              if (auto *document = engine->document(path))
                  return document->contents();
              return {};
          }) {}
    Engine *engine;
    ProjectIndexer indexer;
    detail::ProjectSymbolQuery symbols;
    detail::ProjectLanguages languages;
    detail::ProjectContext context;
    detail::DefinitionQueryExecutor definitions;
    RequestId indexRequest;
    RequestId symbolRequest;
    bool synchronizeLanguages = false;
    detail::SearchExecutor searches;
    detail::ProjectAnalysisCache analyses;
};

Project::Project(Engine *engine) : QObject(engine), state_(std::make_unique<State>(engine)) {
    state_->indexer.setParent(this);
    state_->symbols.setParent(this);
    state_->languages.setParent(this);
    state_->searches.setParent(this);
    state_->definitions.setParent(this);
    auto *index = &state_->indexer;
    connect(index, &ProjectIndexer::catalogReady, this, &Project::catalogReady);
    connect(index, &ProjectIndexer::batchReady, this, &Project::indexBatchReady);
    connect(index, &ProjectIndexer::busyChanged, this, &Project::indexingChanged);
    connect(index, &ProjectIndexer::busyChanged, &state_->symbols,
            &detail::ProjectSymbolQuery::setIndexing);
    connect(index, &ProjectIndexer::failed, this, &Project::failed);
    connect(index, &ProjectIndexer::status, this, &Project::status);
    connect(index, &ProjectIndexer::ready, this, [this](const codecairn::IndexResult &result) {
        if (result.changed)
            state_->context.clearMembership();
        const auto request = std::exchange(state_->indexRequest, {});
        emit indexReady(request, result);
    });
    connect(&state_->symbols, &detail::ProjectSymbolQuery::ready, this,
            [this](const codecairn::SymbolPage &page) {
                const auto request = std::exchange(state_->symbolRequest, {});
                emit symbolsReady(request, page);
            });
    const auto observe = [this](LanguageServerClient *service) {
        connect(service, &LanguageServerClient::status, this, &Project::status);
        connect(service, &LanguageServerClient::ready, this, &Project::languageReady);
    };
    observe(state_->languages.cpp());
    connect(&state_->languages, &detail::ProjectLanguages::serviceCreated, this, observe);
    connect(engine, &Engine::documentClosed, this, [this](const QString &path) {
        state_->analyses.forget(path);
        state_->context.forgetDocument(path);
        state_->languages.closeDocument(path);
    });
    connect(this, &Project::analysisReady, this, [this](RequestId, const AnalysisResult &result) {
        auto *live = state_->engine->document(result.source.path);
        if (live && live->revision() == result.source.revision &&
            live->snapshot().hash == result.source.hash &&
            live->snapshot().bytes == result.source.bytes) {
            state_->analyses.store(result);
            synchronizeDocument(result.source);
        }
    });
    connect(&state_->searches, &detail::SearchExecutor::ready, this,
            [this](RequestId request, detail::SearchKind kind, const SearchPage &page) {
                if (kind == detail::SearchKind::References)
                    emit referencesReady(request, page);
                else
                    emit textMatchesReady(request, page);
            });
    state_->definitions.setSnapshotProvider(
        [this](const QString &path) -> std::optional<QByteArray> {
            if (auto *document = state_->engine->document(path))
                return document->snapshot().bytes;
            return {};
        });
    connect(&state_->definitions, &detail::DefinitionQueryExecutor::ready, this,
            &Project::definitionsReady);
}

Project::~Project() {
    close();
}

const ProjectInfo &Project::info() const {
    return state_->context.info();
}

Result<void> Project::open(const PreparedProject &prepared, const ProjectOptions &options) {
    const auto changed = state_->context.open(prepared);
    if (!changed)
        return Result<void>::failure(changed.error().code, changed.error().message);
    const auto change = changed.value();
    if (change.analysis || options.indexing == IndexStartup::Automatic ||
        options.languages == LanguageStartup::Automatic)
        invalidateOperations();
    if (change.analysis)
        state_->languages.stop();
    state_->synchronizeLanguages = options.languages == LanguageStartup::Automatic;
    if (change.identity || options.indexing == IndexStartup::Automatic) {
        state_->symbols.open(info().root, info().projectFile);
        state_->indexer.open(info().root, info().projectFile,
                             options.indexing == IndexStartup::Automatic
                                 ? ProjectIndexer::OpenMode::IndexAndWatch
                                 : ProjectIndexer::OpenMode::QueryOnly);
    }
    if (options.languages == LanguageStartup::Automatic)
        state_->languages.start(info().root, info().configuration.compilationDirectory,
                                info().projectFile);
    if (change.analysis)
        emit this->contextChanged();
    return Result<void>::success();
}

void Project::close() {
    invalidateOperations();
    state_->symbols.open({}, {});
    state_->indexer.close();
    state_->languages.stop();
    state_->context.clear();
}

void Project::invalidateOperations() {
    state_->engine->cancelFor(this);
    state_->definitions.cancelAll();
    state_->searches.cancelAll();
    state_->analyses.clear();
    state_->context.clearMembership();
    state_->indexRequest = {};
    state_->symbolRequest = {};
}

Result<void> Project::reloadOptions() {
    const auto changed = state_->context.reload();
    if (!changed)
        return Result<void>::failure(changed.error().code, changed.error().message);
    if (info().configuration.excludedDirectories)
        state_->indexer.setExcludes(*info().configuration.excludedDirectories);
    if (changed.value().analysis) {
        invalidateOperations();
        if (state_->synchronizeLanguages)
            state_->languages.start(info().root, info().configuration.compilationDirectory,
                                    info().projectFile);
        else
            state_->languages.stop();
        emit contextChanged();
    }
    return Result<void>::success();
}

bool Project::contains(const QString &path) const {
    if (path.isEmpty())
        return false;
    const auto *document = state_->engine->document(path);
    const auto identity = document ? document->path() : canonicalPath(path);
    return state_->context.includes(identity, document != nullptr);
}
bool Project::indexing() const {
    return state_->indexer.busy();
}
QStringList Project::files() const {
    return state_->indexer.files();
}
QStringList Project::excludedDirectories() const {
    return state_->indexer.excludes();
}
void Project::setExcludedDirectories(const QStringList &names) {
    const auto updated = state_->context.setExcludedDirectories(names);
    if (!updated) {
        emit failed(updated.error().message);
        return;
    }
    state_->indexer.setExcludes(names);
}
void Project::setRefreshIntervalMinutes(int minutes) {
    state_->indexer.setRefreshIntervalMinutes(minutes);
}
RequestId Project::refreshIndex(codecairn::IndexUpdate mode) {
    state_->indexRequest = state_->engine->nextRequest();
    state_->indexer.refresh(mode);
    return state_->indexRequest;
}
RequestId Project::findSymbols(const SymbolQuery &query) {
    ::codecairn::SymbolCatalogQuery captured;
    captured.text = query.text;
    captured.category = query.category;
    captured.offset = query.offset;
    captured.limit = query.pageSize;
    captured.exact = query.exact;
    captured.includeCategories = query.includeCategories;
    captured.includeCategorySymbols = query.includeCategorySymbols;
    captured.includeCounts = query.includeCounts;
    captured.categoryLimit = query.categoryPageSize;
    captured.anchor = query.anchor;
    captured.allowPendingSchema = indexing();
    captured.projectFile = info().projectFile;
    captured.overlays = state_->engine->overlays(this);
    captured.overlays.removeIf([this](const auto &overlay) {
        auto *document = state_->engine->document(overlay.path);
        return document && !document->modified() && !overlay.analysis.supported;
    });
    state_->symbolRequest = state_->engine->nextRequest();
    state_->symbols.search(captured);
    return state_->symbolRequest;
}
void Project::cancelIndexing() {
    state_->indexer.cancel();
}
void Project::cancelSymbolQuery() {
    state_->symbols.cancel();
}

codecairn::SourceAnalysis Project::cachedAnalysis(const SourceSnapshot &snapshot) const {
    return state_->analyses.find(snapshot);
}

RequestId Project::findDefinitions(const DefinitionQuery &query) {
    const auto id = state_->engine->nextRequest();
    auto sources = captureSources();
    detail::DefinitionInputs inputs;
    inputs.query = query;
    inputs.root = info().root;
    inputs.projectFile = info().projectFile;
    inputs.originAnalysis = cachedAnalysis(query.at.snapshot);
    inputs.overlays = std::move(sources.overlays);
    inputs.versions = std::move(sources.versions);
    state_->definitions.submit(inputs, id);
    return id;
}

RequestId Project::analyze(const SourceSnapshot &snapshot, AnalysisDetail detail) {
    return state_->engine->analyzeFor(this, snapshot, detail);
}

void Project::synchronizeDocument(const SourceSnapshot &snapshot) {
    if (info().root.isEmpty() || !state_->synchronizeLanguages)
        return;
    if (auto *service = state_->languages.serviceFor(snapshot.path, snapshot.bytes))
        service->syncDocument(snapshot.path, snapshot.bytes, static_cast<int>(snapshot.revision));
}

void Project::restartLanguageServices() {
    state_->synchronizeLanguages = true;
    state_->definitions.cancelAll();
    state_->searches.cancelAll();
    state_->analyses.clear();
    state_->engine->cancelFor(this);
    state_->context.invalidateAnalysis();
    state_->languages.start(info().root, info().configuration.compilationDirectory,
                            info().projectFile);
    for (const auto &overlay : state_->engine->overlays(this))
        if (auto *doc = state_->engine->document(overlay.path))
            synchronizeDocument(doc->snapshot());
    emit contextChanged();
}

Result<void> Project::recreateIndex() {
    state_->symbols.cancel();
    state_->symbols.waitForFinished();
    const auto error = state_->indexer.recreateDatabase();
    if (!error.isEmpty()) {
        qCWarning(storageLog) << "project.index_recreate_failed" << error;
        return Result<void>::failure(ErrorCode::Io, error);
    }
    return Result<void>::success();
}

void Project::cancel(RequestId request) {
    if (!request)
        return;
    state_->engine->cancelFor(this, request);
    if (request == state_->indexRequest)
        cancelIndexing();
    if (request == state_->symbolRequest)
        cancelSymbolQuery();
    state_->definitions.cancel(request);
    state_->searches.cancel(request);
}

QStringList Project::files(const SearchFiles &scope) const {
    return state_->searches.files(files(), captureSources(), scope);
}

RequestId Project::searchText(const TextQuery &query) {
    const auto request = state_->engine->nextRequest();
    state_->searches.searchText(info(), files(), captureSources(), query, request);
    return request;
}

RequestId Project::findReferences(const ReferenceQuery &query) {
    const auto request = state_->engine->nextRequest();
    state_->searches.findReferences(info(), files(), captureSources(), query, request);
    return request;
}

RequestId Project::continueSearch(SearchCursor cursor) {
    const auto request = state_->engine->nextRequest();
    state_->searches.continueSearch(cursor, request);
    return request;
}

detail::QuerySources Project::captureSources() const {
    detail::QuerySources sources;
    sources.overlays = state_->engine->overlays(this);
    sources.versions.reserve(sources.overlays.size());
    for (const auto &overlay : sources.overlays)
        if (auto *document = state_->engine->document(overlay.path))
            sources.versions.insert(overlay.path, static_cast<int>(document->revision()));
    return sources;
}

quint64 Project::analysisGeneration() const {
    return state_->context.generation();
}

QString Project::analysisDirectory(const QString &path) const {
    return contains(path) ? state_->context.analysisDirectory() : QString{};
}

}

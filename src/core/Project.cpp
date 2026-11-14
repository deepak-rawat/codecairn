#include <codecairn/Project.h>
#include <codecairn/Diagnostics.h>
#include "NavigationService.h"
#include "ReferenceSession.h"
#include "ProjectIndexer.h"
#include "ProjectSymbolQuery.h"
#include <QFileInfo>
#include <QTimer>
#include <QSet>
#include <utility>
#include <algorithm>

namespace codecairn {

struct Project::State {
    explicit State(Engine *engine) : engine(engine), navigation(&languages) {}
    Engine *engine;
    ProjectIndexer indexer;
    detail::ProjectSymbolQuery symbols;
    detail::ProjectLanguages languages;
    ProjectInfo info;
    NavigationService navigation;
    RequestId indexRequest;
    RequestId symbolRequest;
    QHash<quint64, bool> definitions;
    QSet<quint64> deliveries;
    bool dispatching = false;
    bool synchronizeLanguages = false;
    struct Search {
        ReferenceSession *session;
        bool references;
        RequestId request;
        int offset;
        bool canContinue = false;
    };
    QHash<quint64, Search> searches;
    QHash<QString, AnalysisResult> analyses;
    mutable QHash<QString, bool> membership;
};

Project::Project(Engine *engine) : QObject(engine), state_(std::make_unique<State>(engine)) {
    state_->indexer.setParent(this);
    state_->symbols.setParent(this);
    state_->languages.setParent(this);
    state_->navigation.setParent(this);
    auto *index = &state_->indexer;
    connect(index, &ProjectIndexer::catalogReady, this, &Project::catalogReady);
    connect(index, &ProjectIndexer::batchReady, this, &Project::indexBatchReady);
    connect(index, &ProjectIndexer::busyChanged, this, &Project::indexingChanged);
    connect(index, &ProjectIndexer::busyChanged, &state_->symbols,
            &detail::ProjectSymbolQuery::setIndexing);
    connect(index, &ProjectIndexer::failed, this, &Project::failed);
    connect(index, &ProjectIndexer::status, this, &Project::status);
    connect(index, &ProjectIndexer::ready, this, [this](const IndexResult &result) {
        if (result.changed)
            state_->membership.clear();
        const auto request = std::exchange(state_->indexRequest, {});
        emit indexReady(request, result);
    });
    connect(&state_->symbols, &detail::ProjectSymbolQuery::ready, this,
            [this](const SymbolPage &page) {
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
        state_->analyses.remove(path);
        state_->membership.remove(path);
        state_->languages.closeDocument(path);
    });
    connect(this, &Project::analysisReady, this, [this](RequestId, const AnalysisResult &result) {
        auto *live = state_->engine->document(result.source.path);
        if (live && live->revision() == result.source.revision &&
            live->snapshot().hash == result.source.hash &&
            live->snapshot().bytes == result.source.bytes) {
            state_->analyses.insert(result.source.path, result);
            synchronizeDocument(result.source);
        }
    });
    state_->navigation.setSnapshotProvider(
        [this](const QString &path) -> std::optional<QByteArray> {
            if (auto *document = state_->engine->document(path))
                return document->snapshot().bytes;
            return {};
        });
    connect(&state_->navigation, &NavigationService::definitionReady, this,
            [this](quint64 token, const QString &, const ::DefinitionResult &result) {
                if (!state_->definitions.contains(token))
                    return;
                DefinitionPage page;
                page.semantic = result.semantic;
                page.provider = result.provider;
                page.error = result.error;
                page.warning = result.warning;
                if (result.semantic) {
                    page.total = result.locations.size();
                    page.definitions.reserve(result.locations.size());
                    for (const auto &location : result.locations) {
                        SourceLocation target;
                        target.path = location.path;
                        target.line = location.line;
                        target.character = location.character;
                        target.endLine = location.endLine;
                        target.endCharacter = location.endCharacter;
                        page.definitions.append({target, {}});
                    }
                } else {
                    page.total = result.candidates.total;
                    page.complete = result.candidates.complete;
                    page.definitions.reserve(result.candidates.targets.size());
                    for (const auto &symbol : result.candidates.targets)
                        page.definitions.append({{symbol.path, symbol.symbol.nameStart,
                                                  symbol.symbol.nameEnd, -1, symbol.hash},
                                                 symbol});
                }
                const auto deliver = [this, token, page] {
                    if (state_->definitions.remove(token))
                        emit definitionsReady({token}, page);
                };
                if (state_->dispatching)
                    QTimer::singleShot(0, this, deliver);
                else
                    deliver();
            });
}

Project::~Project() {
    close();
    for (auto *session : findChildren<ReferenceSession *>(QString{}, Qt::FindDirectChildrenOnly))
        delete session;
}

const ProjectInfo &Project::info() const {
    return state_->info;
}

Result<void> Project::open(const PreparedProject &prepared, const ProjectOptions &options) {
    if (!prepared.error.isEmpty() || prepared.root.isEmpty() || !QFileInfo(prepared.root).isDir() ||
        !QFileInfo(prepared.root).isReadable()) {
        const auto error = prepared.error.isEmpty()
                               ? "Project folder is missing or unreadable: " + prepared.root
                               : prepared.error;
        qCWarning(storageLog) << "project.open_failed" << error;
        return Result<void>::failure(ErrorCode::InvalidArgument, error);
    }
    const bool switching =
        info().root != prepared.root || info().projectFile != prepared.projectFile;
    const bool contextChanged =
        info().root != prepared.root || info().projectFile != prepared.projectFile ||
        info().configuration.compilationDirectory != prepared.options.compilationDirectory ||
        info().configuration.projectDigest != prepared.options.projectDigest;
    if (switching || options.indexing == IndexStartup::Automatic ||
        options.languages == LanguageStartup::Automatic)
        invalidateOperations();
    if (switching)
        state_->languages.stop();
    state_->info = {prepared.root, prepared.projectFile, prepared.name, prepared.options};
    state_->synchronizeLanguages = options.languages == LanguageStartup::Automatic;
    if (switching || options.indexing == IndexStartup::Automatic) {
        state_->symbols.open(info().root, info().projectFile);
        state_->indexer.open(info().root, info().projectFile,
                             options.indexing == IndexStartup::Automatic
                                 ? ProjectIndexer::OpenMode::IndexAndWatch
                                 : ProjectIndexer::OpenMode::QueryOnly);
    }
    if (options.languages == LanguageStartup::Automatic)
        state_->languages.start(info().root, info().configuration.compilationDirectory,
                                info().projectFile);
    if (contextChanged)
        emit this->contextChanged();
    return Result<void>::success();
}

void Project::close() {
    invalidateOperations();
    state_->symbols.open({}, {});
    state_->indexer.close();
    state_->languages.stop();
    state_->info = {};
}

void Project::invalidateOperations() {
    state_->engine->cancelFor(this);
    state_->navigation.cancelLookups();
    state_->navigation.cancelRead();
    state_->definitions.clear();
    state_->deliveries.clear();
    for (const auto &search : std::as_const(state_->searches))
        search.session->retire();
    state_->searches.clear();
    state_->analyses.clear();
    state_->membership.clear();
    state_->indexRequest = {};
    state_->symbolRequest = {};
}

Result<void> Project::reloadOptions() {
    const auto previous = info().configuration;
    const auto loaded = WorkspaceOptionsStore::loadOptions(info().root, info().projectFile);
    if (!loaded.error.isEmpty()) {
        qCWarning(storageLog) << "project.options_failed" << loaded.error;
        return Result<void>::failure(ErrorCode::Io, loaded.error);
    }
    state_->info.configuration = loaded;
    if (loaded.excludedDirectories)
        state_->indexer.setExcludes(*loaded.excludedDirectories);
    state_->membership.clear();
    if (previous.compilationDirectory != info().configuration.compilationDirectory ||
        previous.projectDigest != info().configuration.projectDigest) {
        state_->engine->cancelFor(this);
        state_->analyses.clear();
        emit contextChanged();
    }
    return Result<void>::success();
}

bool Project::contains(const QString &path) const {
    if (path.isEmpty())
        return false;
    const auto known = state_->membership.constFind(path);
    if (known != state_->membership.cend())
        return *known;
    const auto *document = state_->engine->document(path);
    const auto identity = document ? document->path() : workspacePath(path);
    const auto canonical = state_->membership.constFind(identity);
    if (canonical != state_->membership.cend())
        return *canonical;
    const bool included = projectIncludesPath(info().root, identity, info().configuration);
    if (document)
        state_->membership.insert(identity, included);
    return included;
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
    state_->info.configuration.excludedDirectories = names;
    state_->membership.clear();
    state_->indexer.setExcludes(names);
}
void Project::setRefreshIntervalMinutes(int minutes) {
    state_->indexer.setRefreshIntervalMinutes(minutes);
}
RequestId Project::refreshIndex(IndexRefresh mode) {
    state_->indexRequest = state_->engine->nextRequest();
    state_->indexer.refresh(mode);
    return state_->indexRequest;
}
RequestId Project::findSymbols(const SymbolQuery &query) {
    ::SymbolQuery captured;
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

SourceAnalysis Project::cachedAnalysis(const SourceSnapshot &snapshot) const {
    const auto cached = state_->analyses.constFind(snapshot.path);
    if (cached != state_->analyses.cend() && cached->source.revision == snapshot.revision &&
        cached->source.hash == snapshot.hash)
        return cached->analysis;
    return {};
}

RequestId Project::findDefinitions(const DefinitionQuery &query) {
    const auto id = state_->engine->nextRequest();
    DefinitionRequest request;
    request.root = info().root;
    request.projectFile = info().projectFile;
    request.path = query.at.snapshot.path;
    request.source = query.at.snapshot.bytes;
    request.version = static_cast<int>(query.at.snapshot.revision);
    request.position = query.at.byteOffset;
    request.offset = query.offset;
    request.policy =
        query.resolution == DefinitionResolution::SyntaxOnly     ? DefinitionPolicy::SyntaxOnly
        : query.resolution == DefinitionResolution::SemanticOnly ? DefinitionPolicy::SemanticOnly
                                                                 : DefinitionPolicy::PreferSemantic;
    request.analysis = cachedAnalysis(query.at.snapshot);

    if (std::none_of(request.overlays.cbegin(), request.overlays.cend(),
                     [&](const auto &entry) { return entry.path == request.path; }))
        request.overlays.append(
            {request.path, request.source, request.analysis, query.at.snapshot.hash});
    for (const auto &overlay : request.overlays)
        if (auto *doc = state_->engine->document(overlay.path))
            request.versions.insert(overlay.path, static_cast<int>(doc->revision()));
    if (query.origin == DefinitionOrigin::ExcludeUnresolvedSelf) {
        const auto selection = contextIdentifier(request.source, request.position);
        request.identifier = selection.name;
        request.identifierStart = selection.start;
        request.identifierEnd = selection.end;
        request.context = true;
    }
    state_->definitions.insert(id.value, true);
    state_->dispatching = true;
    state_->navigation.lookup(request, id.value, QString::number(id.value));
    state_->dispatching = false;
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
    state_->navigation.cancelLookups();
    state_->definitions.clear();
    state_->analyses.clear();
    state_->engine->cancelFor(this);
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
    state_->engine->cancel(request);
    if (request == state_->indexRequest)
        cancelIndexing();
    if (request == state_->symbolRequest)
        cancelSymbolQuery();
    state_->definitions.remove(request.value);
    state_->deliveries.remove(request.value);
    state_->navigation.cancelLookup(QString::number(request.value));
    for (auto it = state_->searches.begin(); it != state_->searches.end(); ++it)
        if (it->request == request || it.key() == request.value) {
            auto *session = it->session;
            state_->searches.erase(it);
            session->retire();
            return;
        }
}

RequestId Project::startSearch(ReferenceSearchRequest request,
                               const std::optional<SourcePosition> &origin, bool references,
                               bool declarations) {
    const auto id = state_->engine->nextRequest();
    auto *session = new ReferenceSession(&state_->languages, this);
    session->setSnapshotProvider([this](const QString &path) -> std::optional<QByteArray> {
        if (auto *doc = state_->engine->document(path))
            return doc->snapshot().bytes;
        return {};
    });
    request.workspaceRoot = info().root;
    request.projectFile = info().projectFile;
    request.overlays = state_->engine->overlays(this);
    if (origin &&
        std::none_of(request.overlays.cbegin(), request.overlays.cend(),
                     [&](const auto &entry) { return entry.path == origin->snapshot.path; }))
        request.overlays.append({origin->snapshot.path, origin->snapshot.bytes,
                                 cachedAnalysis(origin->snapshot), origin->snapshot.hash});
    state_->searches.insert(id.value, {session, references, id, request.offset, false});
    connect(session, &ReferenceSession::pageReady, this,
            [this, search = id.value](quint64 token, const ReferenceResults &result, bool) {
                auto it = state_->searches.find(search);
                if (it == state_->searches.end() || it->request.value != token)
                    return;
                SearchPage page{result.hits,      result.warnings,      result.error,
                                result.cancelled, result.filesSearched, {}};
                it->offset += result.hits.size();
                it->canContinue = result.truncated && result.error.isEmpty() && !result.cancelled;
                if (it->canContinue)
                    page.next = SearchCursor(search, it->offset, it->references);
                const bool references = it->references;
                if (!it->canContinue) {
                    it->session->deleteLater();
                    state_->searches.erase(it);
                }
                state_->deliveries.insert(token);
                const auto deliver = [this, token, references, page] {
                    if (!state_->deliveries.remove(token))
                        return;
                    if (references)
                        emit referencesReady({token}, page);
                    else
                        emit textMatchesReady({token}, page);
                };
                if (state_->dispatching)
                    QTimer::singleShot(0, this, deliver);
                else
                    deliver();
            });
    std::optional<SemanticReferenceOrigin> semantic;
    if (origin) {
        semantic = SemanticReferenceOrigin{origin->snapshot.path,
                                           origin->snapshot.bytes,
                                           static_cast<int>(origin->snapshot.revision),
                                           origin->byteOffset,
                                           declarations,
                                           {}};
        for (const auto &overlay : request.overlays)
            if (auto *doc = state_->engine->document(overlay.path))
                semantic->versions.insert(overlay.path, static_cast<int>(doc->revision()));
    }
    state_->dispatching = true;
    session->start(request, id.value, semantic);
    state_->dispatching = false;
    return id;
}

namespace {
QStringList scopePaths(const SearchFiles &scope, const ProjectInfo &project,
                       const QStringList &catalog, const QVector<SymbolOverlay> &overlays) {
    ReferenceInputs inputs;
    inputs.scope = scope.scope == SearchScope::CurrentFile     ? ReferenceScope::CurrentFile
                   : scope.scope == SearchScope::SelectedFiles ? ReferenceScope::SelectedFiles
                                                               : ReferenceScope::Project;
    inputs.currentFile = scope.currentFile;
    inputs.selectedFiles = scope.selectedFiles;
    inputs.catalog = catalog;
    inputs.overlays = overlays;
    inputs.root = project.root;
    inputs.options = project.configuration;
    inputs.filePatterns = scope.patterns;
    return resolveReferenceScope(inputs);
}
}

QStringList Project::files(const SearchFiles &scope) const {
    return scopePaths(scope, info(), files(), state_->engine->overlays(this));
}

RequestId Project::searchText(const TextQuery &query) {
    ReferenceSearchRequest request;
    request.query = query.text;
    request.overlays = state_->engine->overlays(this);
    request.paths = scopePaths(query.files, info(), files(), request.overlays);
    request.offset = query.offset;
    request.limit = query.pageSize;
    request.options.method =
        query.method == TextMatch::Literal             ? SearchMethod::Literal
        : query.method == TextMatch::RegularExpression ? SearchMethod::RegularExpression
        : query.method == TextMatch::KeywordExpression ? SearchMethod::KeywordExpression
                                                       : SearchMethod::Occurrences;
    request.options.smartMatching = false;
    request.options.caseSensitive = query.caseSensitive;
    request.options.wholeWords = query.wholeWords;
    request.options.comments = query.comments;
    request.options.strings = query.strings;
    request.options.skipInactive = query.skipInactive;
    request.options.contextLines = query.contextLines;
    request.options.variations = query.variations;
    request.options.fragments = query.fragments;
    request.options.output = query.output;
    return startSearch(request, {}, false, false);
}

RequestId Project::findReferences(const ReferenceQuery &query) {
    ReferenceSearchRequest request;
    request.query = sourceIdentifier(query.symbol.snapshot.bytes, query.symbol.byteOffset);
    request.overlays = state_->engine->overlays(this);
    request.paths = scopePaths(query.files, info(), files(), request.overlays);
    request.offset = query.offset;
    request.limit = query.pageSize;
    request.options.method = SearchMethod::References;
    request.options.smartMatching = query.resolution == ReferenceResolution::Semantic;
    request.options.comments = query.comments;
    request.options.strings = query.strings;
    request.options.skipInactive = query.skipInactive;
    request.options.output = query.output;
    return startSearch(request, query.symbol, true, query.includeDeclaration);
}

RequestId Project::continueSearch(SearchCursor cursor) {
    const auto id = state_->engine->nextRequest();
    auto it = state_->searches.find(cursor.search_);
    if (it == state_->searches.end() || !it->canContinue || it->offset != cursor.offset_) {
        qCWarning(navigationLog) << "search.invalid_cursor";
        const SearchPage page{{}, {}, "Search cursor is no longer valid.", false, 0, {}};
        state_->deliveries.insert(id.value);
        QTimer::singleShot(0, this, [this, id, page, references = cursor.references_] {
            if (!state_->deliveries.remove(id.value))
                return;
            if (references)
                emit referencesReady(id, page);
            else
                emit textMatchesReady(id, page);
        });
        return id;
    }
    it->request = id;
    it->canContinue = false;
    state_->dispatching = true;
    it->session->nextPage(id.value);
    state_->dispatching = false;
    return id;
}

}

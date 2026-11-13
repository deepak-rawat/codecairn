#include "NavigationService.h"
#include "DefinitionResolver.h"
#include <codecairn/Diagnostics.h>

#include <QtConcurrentRun>
#include <algorithm>
#include <utility>

NavigationService::NavigationService(WorkspaceServices *services, QObject *parent)
    : QObject(parent), services_(services), reader_(this) {
    connect(&reader_, &QFutureWatcher<NavigationRead>::finished, this, [this] {
        reading_ = false;
        if (runningReadGeneration_ == readGeneration_)
            emit readReady(runningReadToken_, reader_.result());
        startRead();
    });
}

NavigationService::~NavigationService() {
    cancelLookups();
    cancelRead();
    for (auto *watcher : findChildren<QFutureWatcherBase *>()) {
        disconnect(watcher, nullptr, this, nullptr);
        watcher->waitForFinished();
    }
}

void NavigationService::setSnapshotProvider(
    std::function<std::optional<QByteArray>(const QString &)> provider) {
    snapshots_ = std::move(provider);
}

bool NavigationService::fresh(const DefinitionRequest &request) const {
    if (snapshots_)
        if (const auto bytes = snapshots_(request.path))
            return *bytes == request.source;
    return true;
}

void NavigationService::cancelLookup(const QString &channel) {
    const auto previous = lookups_.take(channel);
    disconnect(previous.connection);
    if (auto *resolver = resolvers_.take(channel))
        resolver->discard();
    if (previous.service && !previous.request.context)
        previous.service->cancelLocations(channel);
}

void NavigationService::cancelLookups() {
    const auto channels = lookups_.keys();
    for (const auto &channel : channels)
        cancelLookup(channel);
}

void NavigationService::lookup(const DefinitionRequest &request, quint64 token,
                               const QString &channel) {
    cancelLookup(channel);
    const auto generation = ++generation_;
    lookups_.insert(channel, {request, token, generation, {}, {}, {}});
    if (request.path.isEmpty() || request.position < 0 ||
        request.position > request.source.size() ||
        (request.identifier.isEmpty() &&
         sourceIdentifier(request.source, request.position).isEmpty())) {
        DefinitionResult result;
        result.error = "Select an identifier at a valid source position.";
        complete(channel, generation, result);
        return;
    }
    if (request.policy == DefinitionPolicy::SyntaxOnly) {
        syntaxLookup(channel, generation, {});
        return;
    }
    auto *service = services_->languageService(request.root, request.path, request.source);
    if (!service) {
        semanticReady(channel, generation, {}, "No semantic provider for this language.");
        return;
    }
    auto &lookup = lookups_[channel];
    lookup.service = service;
    for (const auto &overlay : request.overlays)
        if (services_->languageService(request.root, overlay.path, overlay.source) == service)
            service->syncDocument(overlay.path, overlay.source,
                                  request.versions.value(overlay.path, 0));
    if (request.context) {
        lookup.connection = connect(
            service, &LanguageServerClient::definitionReady, this,
            [this, channel, token, generation](
                quint64 replyToken, const QVector<LspLocation> &locations, const QString &error) {
                if (replyToken == token)
                    semanticReady(channel, generation, locations, error);
            });
        service->definition(request.path, request.source, request.version, request.position, token);
    } else {
        lookup.connection =
            connect(service, &LanguageServerClient::locationsReady, this,
                    [this, channel, token,
                     generation](quint64 replyToken, const QString &replyChannel,
                                 const QVector<LspLocation> &locations, const QString &error) {
                        if (replyToken == token && replyChannel == channel)
                            semanticReady(channel, generation, locations, error);
                    });
        service->locations(request.path, request.source, request.version, request.position, token,
                           channel);
    }
}

void NavigationService::semanticReady(const QString &channel, quint64 generation,
                                      const QVector<LspLocation> &locations, const QString &error) {
    const auto it = lookups_.constFind(channel);
    if (it == lookups_.cend() || it->generation != generation)
        return;
    const auto lookup = *it;
    disconnect(lookup.connection);
    auto targets = locations;
    if (lookup.request.context) {
        const auto &request = lookup.request;
        targets.removeIf([&](const auto &target) {
            if (workspacePath(target.path) != workspacePath(request.path))
                return false;
            const auto position = sourceByteOffset(request.source, target.line, target.character);
            const bool self =
                position >= request.identifierStart && position < request.identifierEnd;
            const bool declaration =
                std::any_of(request.analysis.symbols.cbegin(), request.analysis.symbols.cend(),
                            [&](const auto &symbol) {
                                return !symbol.outlineOnly && symbol.nameStart == position &&
                                       symbol.name == request.identifier &&
                                       (symbol.provider == "syntax" || symbol.definition);
                            });
            return self && !declaration;
        });
    }
    if ((!error.isEmpty() || targets.isEmpty()) &&
        lookup.request.policy == DefinitionPolicy::PreferSemantic) {
        syntaxLookup(channel, generation, error);
        return;
    }
    DefinitionResult result;
    result.semantic = true;
    result.locations = targets;
    result.error = error;
    if (lookup.service) {
        result.provider = lookup.service->name() + " definition";
        if (lookup.service == services_->cppService() && !lookup.service->hasCompilationDatabase())
            result.provider += " (fallback compile flags)";
    }
    complete(channel, generation, result);
}

void NavigationService::syntaxLookup(const QString &channel, quint64 generation,
                                     const QString &warning) {
    const auto it = lookups_.constFind(channel);
    if (it == lookups_.cend() || it->generation != generation)
        return;
    const auto request = it->request;
    SymbolQuery query;
    query.projectFile = request.projectFile;
    query.text = request.identifier.isEmpty() ? sourceIdentifier(request.source, request.position)
                                              : request.identifier;
    query.exact = true;
    query.limit = RuntimeLimits::DefinitionCandidateLimit;
    query.includeCategories = false;
    query.offset = request.offset;
    query.overlays = request.overlays;
    lookups_[channel].warning = warning;
    auto *resolver = resolvers_.value(channel);
    if (!resolver) {
        resolver = new DefinitionResolver(this);
        resolvers_.insert(channel, resolver);
        connect(resolver, &DefinitionResolver::candidatesReady, this,
                [this, channel](quint64 generation, const DefinitionCandidates &candidates) {
                    const auto pending = lookups_.constFind(channel);
                    if (pending == lookups_.cend() || pending->generation != generation)
                        return;
                    DefinitionResult result;
                    result.candidates = candidates;
                    result.error = result.candidates.error;
                    result.warning = pending->warning;
                    result.provider = "Syntax-index candidate (not compiler verified)";
                    complete(channel, generation, result);
                });
    }
    resolver->request(request.root, query, generation);
}

void NavigationService::complete(const QString &channel, quint64 generation,
                                 DefinitionResult result) {
    const auto it = lookups_.constFind(channel);
    if (it == lookups_.cend() || it->generation != generation)
        return;
    const auto lookup = *it;
    disconnect(lookup.connection);
    lookups_.remove(channel);
    if (auto *resolver = resolvers_.take(channel))
        resolver->discard();
    if (!fresh(lookup.request)) {
        result = {};
        result.error = "Source changed during lookup; select the symbol again.";
    }
    if (!result.error.isEmpty())
        qCWarning(navigationLog) << "navigation.lookup_failed" << result.error;
    emit definitionReady(lookup.token, channel, result);
}

void NavigationService::read(const NavigationLocation &location, quint64 token,
                             const std::optional<IndexedSymbol> &indexedTarget) {
    pendingRead_ = ReadRequest{location, token, ++readGeneration_, indexedTarget};
    startRead();
}

void NavigationService::cancelRead() {
    ++readGeneration_;
    pendingRead_.reset();
}

void NavigationService::startRead() {
    if (reading_ || !pendingRead_)
        return;
    const auto request = *pendingRead_;
    pendingRead_.reset();
    reading_ = true;
    runningReadToken_ = request.token;
    runningReadGeneration_ = request.generation;
    const auto live = snapshots_ ? snapshots_(request.location.path) : std::optional<QByteArray>{};
    reader_.setFuture(QtConcurrent::run([request, live] {
        NavigationRead result;
        result.location = request.location;
        result.file = live ? FileContents{*live, {}, false} : readSourceFile(request.location.path);
        if (result.file.error.isEmpty())
            result.range =
                resolveNavigationTarget(request.location, result.file.bytes, request.indexedTarget);
        return result;
    }));
}

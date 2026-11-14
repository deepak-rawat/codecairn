#include "DefinitionQueryExecutor.h"
#include "DefinitionResolver.h"
#include <codecairn/Diagnostics.h>
#include <codecairn/Paths.h>

#include <QTimer>
#include <algorithm>
#include <utility>

namespace codecairn::detail {

DefinitionQueryExecutor::DefinitionQueryExecutor(ProjectLanguages *languages, QObject *parent)
    : QObject(parent), languages_(languages) {
}

DefinitionQueryExecutor::~DefinitionQueryExecutor() {
    cancelAll();
    for (auto *watcher : findChildren<QFutureWatcherBase *>()) {
        disconnect(watcher, nullptr, this, nullptr);
        watcher->waitForFinished();
    }
}

void DefinitionQueryExecutor::setSnapshotProvider(
    std::function<std::optional<QByteArray>(const QString &)> provider) {
    snapshots_ = std::move(provider);
}

bool DefinitionQueryExecutor::fresh(const SourceSnapshot &snapshot) const {
    if (snapshots_)
        if (const auto bytes = snapshots_(snapshot.path))
            return *bytes == snapshot.bytes;
    return true;
}

void DefinitionQueryExecutor::cancel(RequestId request) {
    pending_.cancel(request);
    const auto lookup = lookups_.take(request.value);
    disconnect(lookup.connection);
    if (lookup.resolver)
        lookup.resolver->discard();
    if (lookup.service)
        lookup.service->cancelLocations(lookup.channel);
}

void DefinitionQueryExecutor::cancelAll() {
    pending_.clear();
    const auto requests = lookups_.keys();
    for (const auto request : requests)
        cancel({request});
}

void DefinitionQueryExecutor::submit(const DefinitionInputs &inputs, RequestId request) {
    cancel(request);
    if (!request)
        return;
    pending_.add(request);
    const auto generation = ++generation_;
    Lookup lookup;
    lookup.inputs = inputs;
    lookup.generation = generation;
    lookup.channel = QString("definition-%1-%2").arg(request.value).arg(generation);
    lookups_.insert(request.value, lookup);
    const auto &query = inputs.query;
    const auto &source = query.at.snapshot;
    const auto selection = query.origin == DefinitionOrigin::ExcludeUnresolvedSelf
                               ? contextIdentifier(source.bytes, query.at.byteOffset)
                               : IdentifierSelection{};
    if (source.path.isEmpty() || query.at.byteOffset < 0 ||
        query.at.byteOffset > source.bytes.size() ||
        (selection.name.isEmpty() &&
         sourceIdentifier(source.bytes, query.at.byteOffset).isEmpty())) {
        DefinitionPage page;
        page.error = "Select an identifier at a valid source position.";
        complete(request, generation, std::move(page));
        return;
    }
    if (query.resolution == DefinitionResolution::SyntaxOnly) {
        syntaxLookup(request, generation, {});
        return;
    }
    auto *service = languages_->serviceFor(source.path, source.bytes);
    if (!service) {
        semanticReady(request, generation, {}, "No semantic provider for this language.");
        return;
    }
    lookups_[request.value].service = service;
    for (const auto &overlay : inputs.overlays)
        if (languages_->serviceFor(overlay.path, overlay.source) == service)
            service->syncDocument(overlay.path, overlay.source,
                                  inputs.versions.value(overlay.path, 0));
    const auto channel = lookups_[request.value].channel;
    lookups_[request.value].connection =
        connect(service, &LanguageServerClient::locationsReady, this,
                [this, request, channel,
                 generation](quint64 replyToken, const QString &replyChannel,
                             const QVector<LspLocation> &locations, const QString &error) {
                    if (replyToken == request.value && replyChannel == channel)
                        semanticReady(request, generation, locations, error);
                });
    service->locations(source.path, source.bytes, static_cast<int>(source.revision),
                       query.at.byteOffset, request.value, channel);
}

void DefinitionQueryExecutor::semanticReady(RequestId request, quint64 generation,
                                            const QVector<LspLocation> &locations,
                                            const QString &error) {
    const auto it = lookups_.constFind(request.value);
    if (it == lookups_.cend() || it->generation != generation)
        return;
    const auto lookup = *it;
    disconnect(lookup.connection);
    auto targets = locations;
    const auto &query = lookup.inputs.query;
    if (query.origin == DefinitionOrigin::ExcludeUnresolvedSelf) {
        const auto &source = query.at.snapshot;
        const auto selection = contextIdentifier(source.bytes, query.at.byteOffset);
        targets.removeIf([&](const auto &target) {
            if (canonicalPath(target.path) != canonicalPath(source.path))
                return false;
            const auto position = sourceByteOffset(source.bytes, target.line, target.character);
            const bool self = position >= selection.start && position < selection.end;
            const auto &symbols = lookup.inputs.originAnalysis.symbols;
            const bool declaration =
                std::any_of(symbols.cbegin(), symbols.cend(), [&](const auto &symbol) {
                    return !symbol.outlineOnly && symbol.nameStart == position &&
                           symbol.name == selection.name &&
                           (symbol.provider == "syntax" || symbol.definition);
                });
            return self && !declaration;
        });
    }
    if ((!error.isEmpty() || targets.isEmpty()) &&
        query.resolution == DefinitionResolution::SemanticPreferred) {
        syntaxLookup(request, generation, error);
        return;
    }
    DefinitionPage page;
    page.semantic = true;
    page.error = error;
    page.total = targets.size();
    page.definitions.reserve(targets.size());
    for (const auto &location : targets) {
        SourceLocation target;
        target.path = location.path;
        target.line = location.line;
        target.character = location.character;
        target.endLine = location.endLine;
        target.endCharacter = location.endCharacter;
        page.definitions.append({target, {}});
    }
    if (lookup.service) {
        page.provider = lookup.service->name() + " definition";
        if (lookup.service == languages_->cpp() && !lookup.service->hasCompilationDatabase())
            page.provider += " (fallback compile flags)";
    }
    complete(request, generation, std::move(page));
}

void DefinitionQueryExecutor::syntaxLookup(RequestId request, quint64 generation,
                                           const QString &warning) {
    const auto it = lookups_.find(request.value);
    if (it == lookups_.end() || it->generation != generation)
        return;
    const auto &inputs = it->inputs;
    const auto &source = inputs.query.at.snapshot;
    ::codecairn::SymbolCatalogQuery query;
    query.projectFile = inputs.projectFile;
    const auto selection = inputs.query.origin == DefinitionOrigin::ExcludeUnresolvedSelf
                               ? contextIdentifier(source.bytes, inputs.query.at.byteOffset)
                               : IdentifierSelection{};
    query.text = selection.name.isEmpty()
                     ? sourceIdentifier(source.bytes, inputs.query.at.byteOffset)
                     : selection.name;
    query.exact = true;
    query.limit = RuntimeLimits::DefinitionCandidateLimit;
    query.includeCategories = false;
    query.offset = inputs.query.offset;
    query.overlays = inputs.overlays;
    it->warning = warning;
    auto *resolver = new DefinitionResolver(this);
    it->resolver = resolver;
    connect(resolver, &DefinitionResolver::candidatesReady, this,
            [this, request](quint64 generation, const DefinitionCandidates &candidates) {
                const auto pending = lookups_.constFind(request.value);
                if (pending == lookups_.cend() || pending->generation != generation)
                    return;
                DefinitionPage page;
                page.total = candidates.total;
                page.complete = candidates.complete;
                page.error = candidates.error;
                page.warning = pending->warning;
                page.provider = "Syntax-index candidate (not compiler verified)";
                page.definitions.reserve(candidates.targets.size());
                for (const auto &symbol : candidates.targets)
                    page.definitions.append({{symbol.path, symbol.symbol.nameStart,
                                              symbol.symbol.nameEnd, -1, symbol.hash},
                                             symbol});
                complete(request, generation, std::move(page));
            });
    resolver->request(inputs.root, query, generation);
}

void DefinitionQueryExecutor::complete(RequestId request, quint64 generation, DefinitionPage page) {
    const auto it = lookups_.constFind(request.value);
    if (it == lookups_.cend() || it->generation != generation)
        return;
    const auto lookup = lookups_.take(request.value);
    disconnect(lookup.connection);
    if (lookup.resolver)
        lookup.resolver->discard();
    const auto source = lookup.inputs.query.at.snapshot;
    QTimer::singleShot(0, this, [this, request, source, page = std::move(page)]() mutable {
        if (!pending_.finish(request))
            return;
        if (!fresh(source)) {
            page = {};
            page.error = "Source changed during lookup; select the symbol again.";
        }
        if (!page.error.isEmpty())
            qCWarning(navigationLog) << "navigation.lookup_failed" << page.error;
        emit ready(request, page);
    });
}

}

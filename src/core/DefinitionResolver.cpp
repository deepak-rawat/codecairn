#include "DefinitionResolver.h"
#include <codecairn/Indexing.h>
#include <QtConcurrentRun>
#include <algorithm>
#include <utility>

DefinitionCandidates definitionCandidates(const codecairn::SymbolPage &page, int offset) {
    DefinitionCandidates candidates{page.symbols, page.total,
                                    page.total <= offset + page.symbols.size(), page.error};
    const bool hasDefinitions =
        std::any_of(candidates.targets.cbegin(), candidates.targets.cend(),
                    [](const auto &target) { return target.symbol.definition; });
    if (hasDefinitions)
        candidates.targets.erase(
            std::remove_if(candidates.targets.begin(), candidates.targets.end(),
                           [](const auto &target) { return !target.symbol.definition; }),
            candidates.targets.end());
    return candidates;
}

DefinitionResolver::DefinitionResolver(QObject *parent) : QObject(parent), watcher_(this) {
    connect(&watcher_, &QFutureWatcher<codecairn::SymbolPage>::finished, this, [this] {
        busy_ = false;
        if (discarded_) {
            deleteLater();
            return;
        }
        emit candidatesReady(runningToken_,
                             definitionCandidates(watcher_.result(), runningOffset_));
        startPending();
    });
}

void DefinitionResolver::discard() {
    pending_.reset();
    discarded_ = true;
    if (runningCancel_)
        runningCancel_->store(true);
    if (!busy_)
        deleteLater();
}

DefinitionResolver::~DefinitionResolver() {
    disconnect(&watcher_, nullptr, this, nullptr);
    if (runningCancel_)
        runningCancel_->store(true);
    watcher_.waitForFinished();
}

void DefinitionResolver::request(const QString &root, const codecairn::SymbolCatalogQuery &query,
                                 quint64 token) {
    pending_ = Request{root, query, token};
    startPending();
}

void DefinitionResolver::startPending() {
    if (busy_ || !pending_)
        return;
    auto request = std::move(*pending_);
    pending_.reset();
    busy_ = true;
    runningToken_ = request.token;
    runningOffset_ = request.query.offset;
    if (!request.query.cancel)
        request.query.cancel = std::make_shared<std::atomic_bool>(false);
    runningCancel_ = request.query.cancel;
    watcher_.setFuture(QtConcurrent::run(
        [root = std::move(request.root), query = std::move(request.query)]() mutable {
            for (auto &overlay : query.overlays) {
                if (query.cancel->load())
                    return codecairn::SymbolPage{};
                if (!overlay.analysis.supported && overlay.analysis.error.isEmpty() &&
                    overlay.analysis.symbols.isEmpty())
                    overlay.analysis = analyzeSource(
                        overlay.source, overlay.path, SourceAnalysisMode::Index,
                        root.isEmpty() ? QString{}
                                       : projectCompilationDirectory(root, query.projectFile));
            }
            return queryProjectSymbols(root, query);
        }));
}

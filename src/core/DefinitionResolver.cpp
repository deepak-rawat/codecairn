#include "DefinitionResolver.h"
#include <codecairn/WorkspaceIndex.h>
#include <QtConcurrentRun>
#include <algorithm>
#include <utility>

DefinitionCandidates definitionCandidates(const SymbolPage &page, int offset) {
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
    connect(&watcher_, &QFutureWatcher<SymbolPage>::finished, this, [this] {
        busy_ = false;
        emit candidatesReady(runningToken_,
                             definitionCandidates(watcher_.result(), runningOffset_));
        startPending();
    });
}

DefinitionResolver::~DefinitionResolver() {
    disconnect(&watcher_, nullptr, this, nullptr);
    watcher_.waitForFinished();
}

void DefinitionResolver::request(const QString &root, const SymbolQuery &query, quint64 token) {
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
    watcher_.setFuture(
        QtConcurrent::run([root = std::move(request.root), query = std::move(request.query)] {
            return queryWorkspaceSymbols(root, query);
        }));
}

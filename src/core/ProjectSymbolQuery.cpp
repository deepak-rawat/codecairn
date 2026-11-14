#include "ProjectSymbolQuery.h"
#include <codecairn/Indexing.h>
#include <QSignalBlocker>
#include <QtConcurrentRun>

namespace codecairn::detail {

ProjectSymbolQuery::ProjectSymbolQuery(QObject *parent) : QObject(parent), job_(this) {
    pool_.setMaxThreadCount(1);
    connect(&job_, &QFutureWatcher<SymbolPage>::finished, this, [this] {
        busy_ = false;
        if (runningGeneration_ == generation_)
            emit ready(job_.result());
        startPending();
    });
}

ProjectSymbolQuery::~ProjectSymbolQuery() {
    cancel();
    disconnect(&job_, nullptr, this, nullptr);
    job_.waitForFinished();
}

void ProjectSymbolQuery::open(const QString &root, const QString &projectFile) {
    cancel();
    root_ = root;
    projectFile_ = projectFile;
}

void ProjectSymbolQuery::cancel() {
    ++generation_;
    queued_ = false;
    if (cancelled_)
        cancelled_->store(true);
}

void ProjectSymbolQuery::waitForFinished() {
    const QSignalBlocker blocker(&job_);
    job_.waitForFinished();
    busy_ = false;
}

void ProjectSymbolQuery::search(const ::SymbolQuery &query) {
    const auto sameOverlays = [&] {
        if (query.overlays.size() != pending_.overlays.size())
            return false;
        for (qsizetype i = 0; i < query.overlays.size(); ++i) {
            const auto &a = query.overlays[i];
            const auto &b = pending_.overlays[i];
            if (a.path != b.path || a.hash != b.hash || a.source != b.source ||
                a.analysis.symbols.size() != b.analysis.symbols.size())
                return false;
        }
        return true;
    };
    const auto sameAnchor = [&] {
        if (query.anchor.has_value() != pending_.anchor.has_value())
            return false;
        return !query.anchor ||
               (query.anchor->path == pending_.anchor->path &&
                query.anchor->symbol.nameStart == pending_.anchor->symbol.nameStart &&
                query.anchor->symbol.name == pending_.anchor->symbol.name);
    };
    const bool same = busy_ && runningGeneration_ == generation_ && query.text == pending_.text &&
                      query.category == pending_.category && query.offset == pending_.offset &&
                      query.limit == pending_.limit && query.exact == pending_.exact &&
                      query.includeCategorySymbols == pending_.includeCategorySymbols &&
                      query.categoryLimit == pending_.categoryLimit &&
                      query.includeCounts == pending_.includeCounts &&
                      query.includeCategories == pending_.includeCategories && sameOverlays() &&
                      sameAnchor();
    if (!same) {
        cancel();
        cancelled_ = std::make_shared<std::atomic_bool>(false);
    }
    pending_ = query;
    pending_.projectFile = projectFile_;
    pending_.cancel = cancelled_;
    queued_ = true;
    startPending();
}

void ProjectSymbolQuery::startPending() {
    if (busy_ || !queued_)
        return;
    queued_ = false;
    busy_ = true;
    runningGeneration_ = generation_;
    auto query = pending_;
    query.allowPendingSchema = indexing_;
    job_.setFuture(QtConcurrent::run(
        &pool_, [root = root_, query] { return queryWorkspaceSymbols(root, query); }));
}

}

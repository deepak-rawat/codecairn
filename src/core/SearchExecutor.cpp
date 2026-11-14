#include "SearchExecutor.h"
#include <codecairn/Diagnostics.h>
#include <codecairn/FileIO.h>
#include <codecairn/Paths.h>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <QtConcurrentRun>
#include <algorithm>
#include <utility>

namespace codecairn::detail {

class SearchExecutor::Operation final : public QObject {
public:
    Operation(SearchExecutor *owner, RequestId id, SearchKind kind, const ProjectInfo &project,
              QuerySources sources)
        : QObject(owner), owner(owner), id(id), request(id), kind(kind), watcher(this),
          versions(std::move(sources.versions)) {
        captured.workspaceRoot = project.root;
        captured.projectFile = project.projectFile;
        captured.overlays = std::move(sources.overlays);
        connect(&watcher, &QFutureWatcher<ReferenceResults>::finished, this, [this] {
            busy = false;
            if (retired) {
                deleteLater();
                return;
            }
            this->owner->accept(this, watcher.result());
        });
    }
    ~Operation() override {
        cancel();
        disconnect(&watcher, nullptr, this, nullptr);
        watcher.waitForFinished();
    }

    void cancel() {
        disconnect(connection);
        if (service)
            service->cancelLocations(channel);
        if (cancelled)
            cancelled->store(true);
    }

    void fail(const QString &message) {
        qCWarning(navigationLog) << "search.failed" << message;
        ReferenceResults result;
        result.error = message;
        owner->accept(this, result);
    }

    bool freshOverlays() const {
        for (const auto &overlay : captured.overlays)
            if (const auto current = owner->snapshots_(overlay.path);
                current && *current != overlay.source)
                return false;
        return true;
    }

    void start() {
        const auto validation = validateReferenceSearch(captured.query, captured.options);
        if (!validation.isEmpty()) {
            fail(validation);
            return;
        }
        if (captured.paths.isEmpty()) {
            fail("No files in the selected scope and file types.");
            return;
        }
        if (!semantic) {
            page();
            return;
        }
        if (!origin) {
            fail("Select a source symbol for semantic reference matching.");
            return;
        }
        service = owner->languages_->serviceFor(origin->snapshot.path, origin->snapshot.bytes);
        if (!service) {
            fail("No semantic provider is available. Select a text search explicitly.");
            return;
        }
        channel = "references-" + QString::number(id.value);
        connection = connect(
            service, &LanguageServerClient::locationsReady, this,
            [this](quint64 token, const QString &replyChannel,
                   const QVector<LspLocation> &locations, const QString &error) {
                if (retired || token != id.value || replyChannel != channel)
                    return;
                disconnect(connection);
                if (!error.isEmpty()) {
                    fail(error);
                    return;
                }
                const auto current = owner->snapshots_(origin->snapshot.path);
                const auto file = current ? FileContents{*current, {}, false}
                                          : readSourceFile(origin->snapshot.path);
                if (!file.error.isEmpty() || file.bytes != origin->snapshot.bytes) {
                    fail("Source changed or became unreadable during semantic lookup. Rerun it.");
                    return;
                }
                if (!freshOverlays()) {
                    fail("Source changed since the search started. Previous results were kept; "
                         "rerun the search.");
                    return;
                }
                targets = locations;
                page();
            });
        busy = true;
        for (const auto &overlay : captured.overlays)
            if (owner->languages_->serviceFor(overlay.path, overlay.source) == service)
                service->syncDocument(overlay.path, overlay.source,
                                      versions.value(overlay.path, 0));
        service->locations(origin->snapshot.path, origin->snapshot.bytes,
                           static_cast<int>(origin->snapshot.revision), origin->byteOffset,
                           id.value, channel, true, declarations);
    }

    void page() {
        cancelled = std::make_shared<std::atomic_bool>(false);
        busy = true;
        const auto input = captured;
        const auto cancelFlag = cancelled;
        if (semantic) {
            const auto locations = targets;
            watcher.setFuture(QtConcurrent::run([input, locations, cancelFlag] {
                return resolveSemanticReferences(
                    locations, input.paths, input.query, input.overlays, cancelFlag, input.offset,
                    input.limit, input.options.skipInactive, input.expectedHashes,
                    input.workspaceRoot.isEmpty()
                        ? QString{}
                        : projectCompilationDirectory(input.workspaceRoot, input.projectFile));
            }));
        } else
            watcher.setFuture(QtConcurrent::run(searchReferences, input, cancelFlag));
    }

    SearchExecutor *owner;
    RequestId id;
    RequestId request;
    SearchKind kind;
    QFutureWatcher<ReferenceResults> watcher;
    ReferenceSearchRequest captured;
    std::optional<SourcePosition> origin;
    QHash<QString, int> versions;
    QVector<LspLocation> targets;
    QPointer<LanguageServerClient> service;
    QMetaObject::Connection connection;
    QString channel;
    std::shared_ptr<std::atomic_bool> cancelled;
    bool semantic = false;
    bool declarations = true;
    bool busy = false;
    bool canContinue = false;
    bool retired = false;
};

SearchExecutor::SearchExecutor(ProjectLanguages *languages, SnapshotProvider snapshots,
                               QObject *parent)
    : QObject(parent), languages_(languages), snapshots_(std::move(snapshots)) {
}

SearchExecutor::~SearchExecutor() {
    cancelAll();
    const auto owned = owned_.values();
    for (auto *operation : owned)
        delete operation;
}

void SearchExecutor::own(Operation *operation) {
    owned_.insert(operation);
    connect(operation, &QObject::destroyed, this, [this, operation] { owned_.remove(operation); });
}

QStringList SearchExecutor::files(const QStringList &catalog, const QuerySources &sources,
                                  const SearchFiles &scope) const {
    QStringList paths;
    if (scope.scope == SearchScope::CurrentFile) {
        if (!scope.currentFile.isEmpty())
            paths.append(scope.currentFile);
    } else if (scope.scope == SearchScope::SelectedFiles)
        paths = scope.selectedFiles;
    else {
        paths = catalog;
        for (const auto &source : sources.overlays)
            if (!paths.contains(source.path))
                paths.append(source.path);
    }
    paths.removeDuplicates();
    const auto filters = scope.patterns.split(QRegularExpression("[;\\s]+"), Qt::SkipEmptyParts);
    if (!filters.isEmpty())
        paths.removeIf(
            [&](const auto &path) { return !QDir::match(filters, QFileInfo(path).fileName()); });
    return paths;
}

void SearchExecutor::searchText(const ProjectInfo &project, const QStringList &catalog,
                                QuerySources sources, const TextQuery &query, RequestId request) {
    const auto paths = files(catalog, sources, query.files);
    auto *operation = new Operation(this, request, SearchKind::Text, project, std::move(sources));
    own(operation);
    auto &input = operation->captured;
    input.paths = paths;
    input.query = query.text;
    input.offset = query.offset;
    input.limit = query.pageSize;
    input.options.method =
        query.method == TextMatch::Literal             ? SearchMethod::Literal
        : query.method == TextMatch::RegularExpression ? SearchMethod::RegularExpression
        : query.method == TextMatch::KeywordExpression ? SearchMethod::KeywordExpression
                                                       : SearchMethod::Occurrences;
    input.options.smartMatching = false;
    input.options.caseSensitive = query.caseSensitive;
    input.options.wholeWords = query.wholeWords;
    input.options.comments = query.comments;
    input.options.strings = query.strings;
    input.options.skipInactive = query.skipInactive;
    input.options.contextLines = query.contextLines;
    input.options.variations = query.variations;
    input.options.fragments = query.fragments;
    input.options.output = query.output;
    operations_.insert(request.value, operation);
    deliveries_.add(request);
    dispatching_ = true;
    operation->start();
    dispatching_ = false;
}

void SearchExecutor::findReferences(const ProjectInfo &project, const QStringList &catalog,
                                    QuerySources sources, const ReferenceQuery &query,
                                    RequestId request) {
    const auto paths = files(catalog, sources, query.files);
    auto *operation =
        new Operation(this, request, SearchKind::References, project, std::move(sources));
    own(operation);
    auto &input = operation->captured;
    input.paths = paths;
    input.query = sourceIdentifier(query.symbol.snapshot.bytes, query.symbol.byteOffset);
    input.offset = query.offset;
    input.limit = query.pageSize;
    input.options.method = SearchMethod::References;
    operation->semantic = query.resolution == ReferenceResolution::Semantic;
    input.options.smartMatching = operation->semantic;
    input.options.comments = query.comments;
    input.options.strings = query.strings;
    input.options.skipInactive = query.skipInactive;
    input.options.output = query.output;
    operation->origin = query.symbol;
    operation->declarations = query.includeDeclaration;
    if (std::none_of(input.overlays.cbegin(), input.overlays.cend(), [&](const auto &overlay) {
            return overlay.path == query.symbol.snapshot.path;
        }))
        input.overlays.append({query.symbol.snapshot.path,
                               query.symbol.snapshot.bytes,
                               {},
                               query.symbol.snapshot.hash});
    operations_.insert(request.value, operation);
    deliveries_.add(request);
    dispatching_ = true;
    operation->start();
    dispatching_ = false;
}

void SearchExecutor::accept(Operation *operation, const ReferenceResults &result) {
    if (operation->retired || operations_.value(operation->id.value) != operation)
        return;
    SearchPage page{result.hits,      result.warnings,      result.error,
                    result.cancelled, result.filesSearched, {}};
    operation->canContinue = result.truncated && result.error.isEmpty() && !result.cancelled;
    if (result.error.isEmpty() && !result.cancelled) {
        operation->captured.offset += result.hits.size();
        for (auto hash = result.sourceHashes.cbegin(); hash != result.sourceHashes.cend(); ++hash)
            operation->captured.expectedHashes.insert(hash.key(), hash.value());
    }
    if (operation->canContinue)
        page.next = SearchCursor(operation->id.value, operation->captured.offset,
                                 operation->kind == SearchKind::References);
    const auto request = operation->request;
    const auto kind = operation->kind;
    if (!operation->canContinue)
        retire(operation);
    deliver(request, kind, page);
}

void SearchExecutor::deliver(RequestId request, SearchKind kind, SearchPage page) {
    const auto delivery = [this, request, kind, page = std::move(page)] {
        if (deliveries_.finish(request))
            emit ready(request, kind, page);
    };
    if (dispatching_)
        QTimer::singleShot(0, this, delivery);
    else
        delivery();
}

void SearchExecutor::continueSearch(SearchCursor cursor, RequestId request) {
    deliveries_.add(request);
    dispatching_ = true;
    auto *operation = operations_.value(cursor.search_);
    if (!operation || !operation->canContinue || operation->busy ||
        operation->captured.offset != cursor.offset_) {
        qCWarning(navigationLog) << "search.invalid_cursor";
        deliver(request, cursor.references_ ? SearchKind::References : SearchKind::Text,
                {{}, {}, "Search cursor is no longer valid.", false, 0, {}});
        dispatching_ = false;
        return;
    }
    operation->request = request;
    operation->canContinue = false;
    if (!operation->freshOverlays()) {
        operation->fail("Source changed since the search started. Previous results were kept; "
                        "rerun the search.");
    } else
        operation->page();
    dispatching_ = false;
}

void SearchExecutor::retire(Operation *operation) {
    operations_.remove(operation->id.value);
    operation->retired = true;
    operation->cancel();
    if (!operation->watcher.isRunning())
        operation->deleteLater();
}

void SearchExecutor::cancel(RequestId request) {
    deliveries_.cancel(request);
    for (auto *operation : std::as_const(operations_))
        if (operation->id == request || operation->request == request) {
            deliveries_.cancel(operation->request);
            retire(operation);
            return;
        }
}

void SearchExecutor::cancelAll() {
    deliveries_.clear();
    const auto operations = operations_.values();
    for (auto *operation : operations)
        retire(operation);
}

}

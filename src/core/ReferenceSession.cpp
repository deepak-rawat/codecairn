#include "ReferenceSession.h"
#include "FileIO.h"
#include "Diagnostics.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QtConcurrentRun>

QVector<SymbolOverlay> captureDocumentOverlays(const QVector<OpenSource> &documents,
                                               const QString &root,
                                               const WorkspaceOptions &options) {
    QVector<SymbolOverlay> overlays;
    for (const auto &open : documents) {
        auto *doc = open.document;
        if (!root.isEmpty() && !projectIncludesPath(root, doc->path, options))
            continue;
        if (doc->analyzedRevision != doc->revision && !open.modified)
            continue;
        const auto &bytes = doc->snapshotBytes();
        overlays.append({doc->path, bytes,
                         doc->analyzedRevision == doc->revision ? doc->analysis : SourceAnalysis{},
                         doc->snapshotHash});
    }
    return overlays;
}

QStringList resolveReferenceScope(const ReferenceInputs &inputs) {
    QStringList paths;
    if (inputs.scope == ReferenceScope::CurrentFile) {
        if (!inputs.currentFile.isEmpty())
            paths.append(inputs.currentFile);
    } else if (inputs.scope == ReferenceScope::SelectedFiles)
        paths = inputs.selectedFiles;
    else
        return referenceSearchPaths(inputs.catalog, inputs.overlays, inputs.root, inputs.options,
                                    inputs.filePatterns);
    return referenceSearchPaths(paths, {}, inputs.root, inputs.options, inputs.filePatterns);
}

QStringList referenceSearchPaths(const QStringList &catalog, const QVector<SymbolOverlay> &overlays,
                                 const QString &root, const WorkspaceOptions &options,
                                 const QString &filePatterns) {
    auto paths = catalog;
    for (const auto &overlay : overlays)
        if ((root.isEmpty() || projectIncludesPath(root, overlay.path, options)) &&
            !paths.contains(overlay.path))
            paths.append(overlay.path);
    paths.removeDuplicates();
    const auto filters = filePatterns.split(QRegularExpression("[;\\s]+"), Qt::SkipEmptyParts);
    if (!filters.isEmpty())
        paths.removeIf(
            [&](const auto &file) { return !QDir::match(filters, QFileInfo(file).fileName()); });
    return paths;
}

ReferenceSession::ReferenceSession(WorkspaceServices *services, QObject *parent)
    : QObject(parent), services_(services) {
}

ReferenceSession::~ReferenceSession() {
    cancel();
    for (auto *watcher : findChildren<QFutureWatcherBase *>()) {
        disconnect(watcher, nullptr, this, nullptr);
        watcher->waitForFinished();
    }
}

void ReferenceSession::setSnapshotProvider(
    std::function<std::optional<QByteArray>(const QString &)> provider) {
    snapshotProvider_ = std::move(provider);
}

std::optional<QByteArray> ReferenceSession::liveSource(const QString &path) const {
    if (snapshotProvider_)
        return snapshotProvider_(path);
    const auto it = overlaySources_.constFind(path);
    if (it != overlaySources_.cend())
        return *it;
    return {};
}

void ReferenceSession::cancel() {
    ++generation_;
    busy_ = false;
    disconnect(semanticConnection_);
    if (service_)
        service_->cancelLocations("references");
    service_.clear();
    if (cancelled_)
        cancelled_->store(true);
}

void ReferenceSession::fail(quint64 token, const QString &error) {
    busy_ = false;
    qCWarning(navigationLog) << "references.failed" << error;
    ReferenceResults results;
    results.error = error;
    emit pageReady(token, results, semantic_);
}

void ReferenceSession::start(const ReferenceSearchRequest &request, quint64 token,
                             const std::optional<SemanticReferenceOrigin> &origin) {
    cancel();
    request_ = request;
    overlaySources_.clear();
    for (const auto &overlay : request.overlays)
        if (!overlaySources_.contains(overlay.path))
            overlaySources_.insert(overlay.path, overlay.source);
    targets_.clear();
    semantic_ = request.options.method == SearchMethod::References && request.options.smartMatching;
    const auto validation = validateReferenceSearch(request.query, request.options);
    if (!validation.isEmpty()) {
        fail(token, validation);
        return;
    }
    if (request.paths.isEmpty()) {
        fail(token, "No files in the selected scope and file types.");
        return;
    }
    if (!semantic_) {
        searchPage(token);
        return;
    }
    if (!origin) {
        fail(token, "Select a source symbol for semantic reference matching.");
        return;
    }
    service_ = services_->languageService(request.workspaceRoot, origin->path, origin->source);
    if (!service_) {
        fail(token, "No semantic provider is available. Select a text search explicitly.");
        return;
    }
    const auto generation = generation_;
    semanticConnection_ = connect(
        service_, &LanguageServerClient::locationsReady, this,
        [this, token, generation, origin = *origin](quint64 replyToken, const QString &channel,
                                                    const QVector<LspLocation> &targets,
                                                    const QString &error) {
            if (generation != generation_ || replyToken != token || channel != "references")
                return;
            disconnect(semanticConnection_);
            busy_ = false;
            if (!error.isEmpty()) {
                fail(token, error);
                return;
            }
            const auto live = liveSource(origin.path);
            const auto source = live ? FileContents{*live, {}, false} : readSourceFile(origin.path);
            if (!source.error.isEmpty() || source.bytes != origin.source) {
                fail(token,
                     "Source changed or became unreadable during semantic lookup. Rerun it.");
                return;
            }
            targets_ = targets;
            nextPage(token);
        });
    busy_ = true;
    for (const auto &overlay : request.overlays)
        if (services_->languageService(request.workspaceRoot, overlay.path, overlay.source) ==
            service_)
            service_->syncDocument(overlay.path, overlay.source,
                                   origin->versions.value(overlay.path, 0));
    service_->locations(origin->path, origin->source, origin->version, origin->position, token,
                        "references", true, origin->declarations);
}

void ReferenceSession::nextPage(quint64 token) {
    if (busy_)
        return;
    for (const auto &captured : request_.overlays)
        if (const auto current = liveSource(captured.path);
            current && captured.source != *current) {
            fail(token, "Source changed since the search started. Previous results were kept; "
                        "rerun the search.");
            return;
        }
    searchPage(token);
}

void ReferenceSession::searchPage(quint64 token) {
    cancelled_ = std::make_shared<std::atomic_bool>(false);
    busy_ = true;
    const auto generation = generation_;
    auto *watcher = new QFutureWatcher<ReferenceResults>(this);
    connect(watcher, &QFutureWatcher<ReferenceResults>::finished, this,
            [this, watcher, token, generation] {
                const auto results = watcher->result();
                watcher->deleteLater();
                if (generation != generation_)
                    return;
                busy_ = false;
                if (results.error.isEmpty() && !results.cancelled) {
                    request_.offset += results.hits.size();
                    for (auto it = results.sourceHashes.cbegin(); it != results.sourceHashes.cend();
                         ++it)
                        request_.expectedHashes.insert(it.key(), it.value());
                }
                emit pageReady(token, results, semantic_);
            });
    const auto request = request_;
    const auto targets = targets_;
    const auto cancel = cancelled_;
    if (semantic_)
        watcher->setFuture(QtConcurrent::run([request, targets, cancel] {
            return resolveSemanticReferences(
                targets, request.paths, request.query, request.overlays, cancel, request.offset,
                request.limit, request.options.skipInactive, request.expectedHashes,
                request.workspaceRoot.isEmpty()
                    ? QString{}
                    : workspaceClangdDirectory(request.workspaceRoot, request.projectFile));
        }));
    else
        watcher->setFuture(QtConcurrent::run(searchReferences, request, cancel));
}

#include <codecairn/WorkspaceIndex.h>
#include "WorkspaceScanner.h"
#include <codecairn/Diagnostics.h>
#include <codecairn/WorkspaceOptionsStore.h>

#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QSet>
#include <QSignalBlocker>
#include <QUuid>
#include <QtConcurrentRun>
#include <utility>

using namespace WorkspaceIndexDetail;

void WorkspaceIndex::logResources(const char *stage) {
    if (!progress_ || !resourceElapsed_.isValid() || !indexLog().isInfoEnabled())
        return;
    const auto memory = Diagnostics::processMemory();
    qCInfo(indexLog) << "index.resources" << "stage=" << stage << "root=" << runningRoot_
                     << "generation=" << runningGeneration_
                     << "discovered=" << progress_->discovered.load()
                     << "parsed=" << progress_->parsed.load()
                     << "committed=" << progress_->committed.load()
                     << "rssBytes=" << memory.residentBytes << "rssError=" << memory.error
                     << "elapsedMs=" << resourceElapsed_.elapsed();
}

WorkspaceIndex::WorkspaceIndex(QObject *parent) : QObject(parent) {
    indexPool_.setMaxThreadCount(1);
    queryPool_.setMaxThreadCount(1);
    indexPool_.setThreadPriority(QThread::LowPriority);
    refreshTimer_.setSingleShot(true);
    refreshTimer_.setInterval(700);
    pollTimer_.setTimerType(Qt::VeryCoarseTimer);
    pollTimer_.setInterval(RuntimeLimits::DefaultWorkspaceRefreshMinutes * 60000);
    progressTimer_.setInterval(250);
    resourceTimer_.setInterval(5000);
    connect(&resourceTimer_, &QTimer::timeout, this, [this] {
        if (busy_)
            logResources("progress");
    });
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this] {
        if (!paused_)
            refreshTimer_.start();
    });
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, [this] {
        if (!paused_) {
            automaticFileChanged_ = true;
            refreshTimer_.start();
        }
    });
    connect(&refreshTimer_, &QTimer::timeout, this, [this] {
        if (!paused_)
            requestRefresh(false, true, true, std::exchange(automaticFileChanged_, false));
    });
    connect(&pollTimer_, &QTimer::timeout, this, [this] {
        if (!paused_ && !busy_)
            requestRefresh(false, true, true, true);
    });
    connect(&progressTimer_, &QTimer::timeout, this, [this] {
        const int parsed = progress_->parsed.load();
        if (runningAutomatic_ && parsed == 0)
            return;
        emit status(progress_->phase.load() == 2
                        ? QString("Symbols ready; preparing reference acceleration...")
                    : parsed > 0 ? QString("Indexing: %1 files discovered, %2 parsed, %3 committed")
                                       .arg(progress_->discovered.load())
                                       .arg(parsed)
                                       .arg(progress_->committed.load())
                                 : QString("Checking workspace: %1 files discovered")
                                       .arg(progress_->discovered.load()));
    });
    connect(&indexJob_, &QFutureWatcher<IndexResult>::finished, this, [this] {
        resourceTimer_.stop();
        logResources("complete");
        busy_ = false;
        progressTimer_.stop();
        emit busyChanged(false);
        const auto result = indexJob_.result();
        qCInfo(indexLog) << "index.completed" << "root=" << result.root
                         << "generation=" << runningGeneration_ << "files=" << result.files.size()
                         << "parsed=" << result.parsed << "cached=" << result.reused
                         << "cancelled=" << result.cancelled << "skipped=" << result.skipped
                         << "discoveryMs=" << result.discoveryMilliseconds
                         << "symbolsMs=" << result.symbolMilliseconds
                         << "referencesMs=" << result.referenceMilliseconds
                         << "suppressedWarnings=" << result.suppressedWarnings
                         << "peakParsers=" << progress_->peakParsers.load();
        for (const auto &warning : result.warnings)
            qCWarning(indexLog) << "index.warning" << "root=" << result.root << warning;
        if (runningGeneration_ == generation_) {
            if (!result.error.isEmpty())
                emit failed(result.error);
            else if (result.cancelled) {
                emit batchReady();
                emit status("Indexing cancelled; committed files preserved. Refresh to resume.");
            } else if (!result.skipped) {
                files_ = result.files;
                if (!result.directorySnapshots.isEmpty())
                    directorySnapshots_ = result.directorySnapshots;
                QStringList paths = result.directories;
                if (paths.isEmpty())
                    paths << root_;
                // Bound native watches; polling/manual refresh covers platforms with lower limits.
                paths.append(files_.mid(0, qMax(0, 2048 - static_cast<int>(paths.size()))));
                if (paths.size() > 2048)
                    paths = paths.mid(0, 2048);
                const auto watched = watcher_.directories() + watcher_.files();
                const QSet<QString> current(watched.cbegin(), watched.cend());
                const QSet<QString> desired(paths.cbegin(), paths.cend());
                QStringList removed, added;
                for (const auto &path : watched)
                    if (!desired.contains(path))
                        removed.append(path);
                for (const auto &path : paths)
                    if (!current.contains(path))
                        added.append(path);
                if (!removed.isEmpty())
                    watcher_.removePaths(removed);
                const auto failedPaths = added.isEmpty() ? QStringList{} : watcher_.addPaths(added);
                if (!failedPaths.isEmpty())
                    emit status(
                        "Some filesystem watches unavailable; periodic refresh remains active.");
                emit ready(result);
            }
        }
        startPending();
        startQuery();
    });
    connect(&queryJob_, &QFutureWatcher<SymbolPage>::finished, this, [this] {
        queryBusy_ = false;
        if (runningQueryGeneration_ == queryGeneration_)
            emit symbolsReady(queryJob_.result());
        startQuery();
    });
}

WorkspaceIndex::~WorkspaceIndex() {
    resourceTimer_.stop();
    cancel();
    invalidateSearch();
    indexJob_.waitForFinished();
    if (busy_)
        logResources("complete");
    queryJob_.waitForFinished();
}

void WorkspaceIndex::open(const QString &root, const QString &projectFile) {
    root_ = workspacePath(root);
    projectFile_ = projectFile;
    ++generation_;
    invalidateSearch();
    files_.clear();
    directorySnapshots_.clear();
    automaticFileChanged_ = false;
    refreshTimer_.stop();
    progressTimer_.stop();
    const auto options = WorkspaceOptionsStore::loadOptions(root_, projectFile_);
    excludes_ = options.excludedDirectories.value_or(defaultWorkspaceExcludes());
    if (cancelled_)
        cancelled_->store(true);
    if (!watcher_.directories().isEmpty())
        watcher_.removePaths(watcher_.directories());
    if (!watcher_.files().isEmpty())
        watcher_.removePaths(watcher_.files());
    pollTimer_.start();
    refresh();
}

void WorkspaceIndex::setExcludes(const QStringList &names) {
    if (excludes_ == names)
        return;
    excludes_ = names;
    refresh();
}

void WorkspaceIndex::close() {
    cancel();
    invalidateSearch();
    ++generation_;
    pollTimer_.stop();
    root_.clear();
    projectFile_.clear();
    files_.clear();
    directorySnapshots_.clear();
    if (!watcher_.directories().isEmpty())
        watcher_.removePaths(watcher_.directories());
    if (!watcher_.files().isEmpty())
        watcher_.removePaths(watcher_.files());
}

void WorkspaceIndex::setRefreshIntervalMinutes(int minutes) {
    if (minutes < 1 || minutes > RuntimeLimits::MaximumWorkspaceRefreshMinutes) {
        qCWarning(indexLog) << "index.invalid_refresh_interval" << "root=" << root_
                            << "minutes=" << minutes;
        emit failed(QString("Workspace refresh interval must be between 1 and %1 minutes.")
                        .arg(RuntimeLimits::MaximumWorkspaceRefreshMinutes));
        return;
    }
    if (pollTimer_.interval() != minutes * 60000)
        pollTimer_.setInterval(minutes * 60000);
}

void WorkspaceIndex::refresh(IndexRefresh mode) {
    const bool force =
        mode == IndexRefresh::ReparseKnownFiles || mode == IndexRefresh::RebuildFilesAndSymbols;
    const bool rescan =
        mode == IndexRefresh::DiscoverChanges || mode == IndexRefresh::RebuildFilesAndSymbols;
    requestRefresh(force, rescan, false, true);
}

void WorkspaceIndex::requestRefresh(bool force, bool rescan, bool automatic, bool verifyContents) {
    if (root_.isEmpty())
        return;
    paused_ = false;
    pendingAutomatic_ = pending_ ? pendingAutomatic_ && automatic : automatic;
    force_ = pending_ ? force_ || force : force;
    rescan_ = pending_ ? rescan_ || rescan : rescan;
    verifyContents_ = pending_ ? verifyContents_ || verifyContents : verifyContents;
    pending_ = true;
    startPending();
}

void WorkspaceIndex::cancel() {
    paused_ = true;
    pending_ = false;
    force_ = false;
    rescan_ = true;
    pendingAutomatic_ = false;
    verifyContents_ = false;
    automaticFileChanged_ = false;
    refreshTimer_.stop();
    progressTimer_.stop();
    if (cancelled_)
        cancelled_->store(true);
}

QString WorkspaceIndex::recreateDatabase() {
    if (root_.isEmpty())
        return "No workspace is open.";
    cancel();
    invalidateSearch();
    ++generation_;
    {
        const QSignalBlocker indexSignals(&indexJob_);
        const QSignalBlocker querySignals(&queryJob_);
        indexJob_.waitForFinished();
        queryJob_.waitForFinished();
    }
    resourceTimer_.stop();
    if (busy_)
        logResources("complete");
    busy_ = false;
    queryBusy_ = false;
    emit busyChanged(false);
    const auto database = workspaceDatabase(root_, projectFile_);
    const auto metadata = workspaceStorageDirectory(root_, {}, projectFile_);
    QLockFile lock(QDir(metadata).filePath("index.lock"));
    if (!lock.tryLock(0))
        return "Another CodeCairn process is using the workspace index; cannot recreate it.";
    const auto suffix = ".backup-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QStringList moved;
    for (const auto &journal : {"", "-wal", "-shm"}) {
        const auto path = database + journal;
        if (QFileInfo(path).isSymLink())
            return "Refusing to archive a symlinked workspace database or journal.";
    }
    for (const auto &journal : {"", "-wal", "-shm"}) {
        const auto path = database + journal;
        if (!QFileInfo::exists(path))
            continue;
        if (!QFile::rename(path, path + suffix)) {
            QString error = "Cannot archive workspace index: " + path;
            for (const auto &previous : moved)
                if (!QFile::rename(previous + suffix, previous))
                    error += "\nCannot restore archived index: " + previous + suffix;
            return error;
        }
        moved.append(path);
    }
    lock.unlock();
    files_.clear();
    directorySnapshots_.clear();
    if (!watcher_.directories().isEmpty())
        watcher_.removePaths(watcher_.directories());
    if (!watcher_.files().isEmpty())
        watcher_.removePaths(watcher_.files());
    emit status("Previous index archived; rebuilding workspace index.");
    pollTimer_.start();
    refresh();
    return {};
}

void WorkspaceIndex::startPending() {
    if (busy_ || !pending_)
        return;
    pending_ = false;
    busy_ = true;
    runningGeneration_ = generation_;
    cancelled_ = std::make_shared<std::atomic_bool>(false);
    progress_ = std::make_shared<IndexProgress>();
    const auto generation = runningGeneration_;
    progress_->catalogReady = [this, generation](const QStringList &paths) {
        QMetaObject::invokeMethod(
            this,
            [this, generation, paths] {
                if (generation != generation_)
                    return;
                files_ = paths;
                emit catalogReady(paths);
                startQuery();
            },
            Qt::QueuedConnection);
    };
    progress_->batchReady = [this, generation] {
        QMetaObject::invokeMethod(
            this,
            [this, generation] {
                if (generation == generation_)
                    emit batchReady();
            },
            Qt::QueuedConnection);
    };
    runningAutomatic_ = std::exchange(pendingAutomatic_, false);
    emit busyChanged(true);
    if (!runningAutomatic_)
        emit status("Discovering source files...");
    progressTimer_.start();
    const auto root = root_;
    const auto projectFile = projectFile_;
    const auto excludes = excludes_;
    const auto cancel = cancelled_;
    const auto progress = progress_;
    const auto snapshots = directorySnapshots_;
    const bool automatic = runningAutomatic_;
    const bool verifyContents = std::exchange(verifyContents_, false);
    const bool force = std::exchange(force_, false), rescan = std::exchange(rescan_, false);
    qCInfo(indexLog) << "index.started" << "root=" << root << "generation=" << runningGeneration_
                     << "automatic=" << automatic << "force=" << force << "rescan=" << rescan;
    runningRoot_ = root;
    resourceElapsed_.start();
    logResources("start");
    resourceTimer_.start();
    indexJob_.setFuture(QtConcurrent::run(&indexPool_, [root, projectFile, excludes, cancel,
                                                        progress, snapshots, automatic,
                                                        verifyContents, force, rescan] {
        if (automatic && !force && !verifyContents && !snapshots.isEmpty()) {
            bool unchanged = true;
            for (auto it = snapshots.cbegin(); it != snapshots.cend(); ++it) {
                const QFileInfo directory(it.key());
                if (!directory.isDir() || !directory.isReadable() ||
                    directorySnapshot(directoryEntries(it.key()), excludes) != it.value()) {
                    unchanged = false;
                    break;
                }
                if (cancel->load())
                    break;
            }
            if (unchanged || cancel->load()) {
                IndexResult result;
                result.root = root;
                result.automatic = true;
                result.skipped = unchanged;
                result.cancelled = cancel->load();
                return result;
            }
        }
        const auto mode =
            force
                ? (rescan ? IndexRefresh::RebuildFilesAndSymbols : IndexRefresh::ReparseKnownFiles)
                : (rescan ? IndexRefresh::DiscoverChanges : IndexRefresh::CheckKnownFiles);
        auto result = buildWorkspaceIndex(root, mode, excludes, cancel, progress, projectFile);
        result.automatic = automatic;
        return result;
    }));
}

void WorkspaceIndex::search(const SymbolQuery &query) {
    const auto sameOverlays = [&] {
        if (query.overlays.size() != pendingQuery_.overlays.size())
            return false;
        for (qsizetype i = 0; i < query.overlays.size(); ++i) {
            const auto &a = query.overlays[i];
            const auto &b = pendingQuery_.overlays[i];
            if (a.path != b.path || a.hash != b.hash || a.source != b.source ||
                a.analysis.symbols.size() != b.analysis.symbols.size())
                return false;
        }
        return true;
    };
    const auto sameAnchor = [&] {
        if (query.anchor.has_value() != pendingQuery_.anchor.has_value())
            return false;
        return !query.anchor ||
               (query.anchor->path == pendingQuery_.anchor->path &&
                query.anchor->symbol.nameStart == pendingQuery_.anchor->symbol.nameStart &&
                query.anchor->symbol.name == pendingQuery_.anchor->symbol.name);
    };
    const bool same = queryBusy_ && runningQueryGeneration_ == queryGeneration_ &&
                      query.text == pendingQuery_.text &&
                      query.category == pendingQuery_.category &&
                      query.offset == pendingQuery_.offset && query.limit == pendingQuery_.limit &&
                      query.exact == pendingQuery_.exact &&
                      query.includeCategorySymbols == pendingQuery_.includeCategorySymbols &&
                      query.categoryLimit == pendingQuery_.categoryLimit &&
                      query.includeCounts == pendingQuery_.includeCounts &&
                      query.includeCategories == pendingQuery_.includeCategories &&
                      sameOverlays() && sameAnchor();
    if (!same) {
        invalidateSearch();
        queryCancelled_ = std::make_shared<std::atomic_bool>(false);
    }
    pendingQuery_ = query;
    pendingQuery_.projectFile = projectFile_;
    pendingQuery_.cancel = queryCancelled_;
    queryPending_ = true;
    startQuery();
}

void WorkspaceIndex::startQuery() {
    if (queryBusy_ || !queryPending_)
        return;
    queryPending_ = false;
    queryBusy_ = true;
    runningQueryGeneration_ = queryGeneration_;
    const auto root = root_;
    auto query = pendingQuery_;
    query.allowPendingSchema = busy_;
    queryJob_.setFuture(QtConcurrent::run(
        &queryPool_, [root, query] { return queryWorkspaceSymbols(root, query); }));
}

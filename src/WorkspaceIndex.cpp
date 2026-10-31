#include "WorkspaceIndex.h"
#include "AppSettings.h"
#include "WorkspaceScanner.h"
#include "Diagnostics.h"

#include <QFileInfo>
#include <QSet>
#include <QtConcurrentRun>
#include <utility>

using namespace WorkspaceIndexDetail;

WorkspaceIndex::WorkspaceIndex(QObject *parent) : QObject(parent) {
    indexPool_.setMaxThreadCount(1);
    indexPool_.setThreadPriority(QThread::LowPriority);
    refreshTimer_.setSingleShot(true);
    refreshTimer_.setInterval(700);
    pollTimer_.setTimerType(Qt::VeryCoarseTimer);
    pollTimer_.setInterval(defaultAppSettings().behavior.workspaceRefreshMinutes * 60000);
    progressTimer_.setInterval(250);
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
        emit status(parsed > 0 ? QString("Indexing: %1 files discovered, %2 parsed")
                                     .arg(progress_->discovered.load())
                                     .arg(parsed)
                               : QString("Checking workspace: %1 files discovered")
                                     .arg(progress_->discovered.load()));
    });
    connect(&indexJob_, &QFutureWatcher<IndexResult>::finished, this, [this] {
        busy_ = false;
        progressTimer_.stop();
        emit busyChanged(false);
        const auto result = indexJob_.result();
        qCInfo(indexLog) << "index.completed" << "root=" << result.root
                         << "generation=" << runningGeneration_ << "files=" << result.files.size()
                         << "parsed=" << result.parsed << "cached=" << result.reused
                         << "cancelled=" << result.cancelled << "skipped=" << result.skipped;
        for (const auto &warning : result.warnings)
            qCWarning(indexLog) << "index.warning" << "root=" << result.root << warning;
        if (runningGeneration_ == generation_) {
            if (!result.error.isEmpty())
                emit failed(result.error);
            else if (result.cancelled)
                emit status("Indexing cancelled; previous completed database preserved.");
            else if (!result.skipped) {
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
    cancel();
    invalidateSearch();
    indexJob_.waitForFinished();
    queryJob_.waitForFinished();
}

void WorkspaceIndex::open(const QString &root) {
    root_ = workspacePath(root);
    ++generation_;
    ++queryGeneration_;
    files_.clear();
    directorySnapshots_.clear();
    automaticFileChanged_ = false;
    refreshTimer_.stop();
    progressTimer_.stop();
    excludes_ = defaultWorkspaceExcludes();
    if (cancelled_)
        cancelled_->store(true);
    if (!watcher_.directories().isEmpty())
        watcher_.removePaths(watcher_.directories());
    if (!watcher_.files().isEmpty())
        watcher_.removePaths(watcher_.files());
    pollTimer_.start();
    refresh(false, true);
}

void WorkspaceIndex::setExcludes(const QStringList &names) {
    if (excludes_ == names)
        return;
    excludes_ = names;
    refresh(false, true);
}

void WorkspaceIndex::close() {
    cancel();
    invalidateSearch();
    ++generation_;
    pollTimer_.stop();
    root_.clear();
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

void WorkspaceIndex::refresh(bool force, bool rescan) {
    requestRefresh(force, rescan, false, true);
}

void WorkspaceIndex::requestRefresh(bool force, bool rescan, bool automatic, bool verifyContents) {
    if (root_.isEmpty())
        return;
    paused_ = false;
    pendingAutomatic_ = pending_ ? pendingAutomatic_ && automatic : automatic;
    pending_ = true;
    force_ = force_ || force;
    rescan_ = rescan_ || rescan;
    verifyContents_ = verifyContents_ || verifyContents;
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

void WorkspaceIndex::startPending() {
    if (busy_ || !pending_)
        return;
    pending_ = false;
    busy_ = true;
    runningGeneration_ = generation_;
    cancelled_ = std::make_shared<std::atomic_bool>(false);
    progress_ = std::make_shared<IndexProgress>();
    runningAutomatic_ = std::exchange(pendingAutomatic_, false);
    emit busyChanged(true);
    if (!runningAutomatic_)
        emit status("Discovering source files...");
    progressTimer_.start();
    const auto root = root_;
    const auto excludes = excludes_;
    const auto cancel = cancelled_;
    const auto progress = progress_;
    const auto snapshots = directorySnapshots_;
    const bool automatic = runningAutomatic_;
    const bool verifyContents = std::exchange(verifyContents_, false);
    const bool force = std::exchange(force_, false), rescan = std::exchange(rescan_, false);
    qCInfo(indexLog) << "index.started" << "root=" << root << "generation=" << runningGeneration_
                     << "automatic=" << automatic << "force=" << force << "rescan=" << rescan;
    indexJob_.setFuture(QtConcurrent::run(&indexPool_, [root, excludes, cancel, progress, snapshots,
                                                        automatic, verifyContents, force, rescan] {
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
        auto result = buildWorkspaceIndex(root, force, rescan, excludes, cancel, progress);
        result.automatic = automatic;
        return result;
    }));
}

void WorkspaceIndex::search(const SymbolQuery &query) {
    invalidateSearch();
    queryCancelled_ = std::make_shared<std::atomic_bool>(false);
    pendingQuery_ = query;
    pendingQuery_.cancel = queryCancelled_;
    queryPending_ = true;
    startQuery();
}

void WorkspaceIndex::startQuery() {
    if (queryBusy_ || !queryPending_ || (busy_ && files_.isEmpty()))
        return;
    queryPending_ = false;
    queryBusy_ = true;
    runningQueryGeneration_ = queryGeneration_;
    const auto root = root_;
    const auto query = pendingQuery_;
    queryJob_.setFuture(
        QtConcurrent::run([root, query] { return queryWorkspaceSymbols(root, query); }));
}

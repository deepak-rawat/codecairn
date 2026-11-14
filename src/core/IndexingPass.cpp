#include <codecairn/Indexing.h>
#include <codecairn/Diagnostics.h>
#include <codecairn/FileIO.h>
#include "ReferenceCandidateIndex.h"
#include "ProjectIndexStore.h"
#include "ProjectFileScanner.h"
#include <codecairn/WorkspaceOptionsStore.h>

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLockFile>
#include <QThreadPool>
#include <QScopeGuard>
#include <QtConcurrentRun>
#include <algorithm>
#include <stdexcept>
#include <utility>

using namespace codecairn::detail;

namespace {
struct PreparedFile {
    QString path;
    QFileInfo info;
    SourceAnalysis analysis;
    QByteArray hash;
    QString error;
    QString oldState;
    bool found = false;
    bool supported = false;
    bool reused = false;
    bool changed = false;
    bool parsed = false;
};

void warning(IndexResult &result, const QString &message) {
    if (result.warnings.size() < 100)
        result.warnings.append(message);
    else
        ++result.suppressedWarnings;
}

PreparedFile prepareFile(PreparedFile file, const QString &oldHash, const QString &oldParser,
                         bool force, const std::shared_ptr<std::atomic_bool> &cancel,
                         const std::shared_ptr<IndexProgress> &progress,
                         const QString &compilationDirectory) {
    if (cancel->load())
        return file;
    file.supported = sourceLanguage(file.path);
    if (!file.supported)
        return file;
    const auto content = readSourceFile(file.path, SourceReadContext::Document);
    file.error = content.error;
    if (!file.error.isEmpty() || cancel->load())
        return file;
    file.hash = sourceContentHash(content.bytes);
    file.reused = !force && file.found && oldHash == QString::fromLatin1(file.hash) &&
                  oldParser == ParserVersion &&
                  (file.oldState == "indexed" || file.oldState == "partial");
    if (!file.reused) {
        {
            if (progress) {
                const int active = ++progress->activeParsers;
                int peak = progress->peakParsers.load();
                while (active > peak &&
                       !progress->peakParsers.compare_exchange_weak(peak, active)) {
                }
            }
            const auto finished = qScopeGuard([&] {
                if (progress)
                    --progress->activeParsers;
            });
            file.analysis = analyzeSource(content.bytes, file.path, SourceAnalysisMode::Index,
                                          compilationDirectory);
        }
        file.analysis.symbols.erase(
            std::remove_if(file.analysis.symbols.begin(), file.analysis.symbols.end(),
                           [](const auto &symbol) {
                               return symbol.outlineOnly || symbol.local || symbol.name.isEmpty() ||
                                      symbol.nameEnd <= symbol.nameStart ||
                                      symbol.end <= symbol.start;
                           }),
            file.analysis.symbols.end());
        file.parsed = true;
        file.error = file.analysis.error;
        file.changed = true;
    }
    const QFileInfo after(file.path);
    if (file.info.lastModified() != after.lastModified() || file.info.size() != after.size())
        file.error = "File changed while being parsed; refresh to retry.";
    return file;
}
}

namespace codecairn::detail {

class IndexingPass {
public:
    IndexingPass(ProjectIndexStore &database, const QString &root, IndexRefresh mode,
                 const QStringList &excludes, const std::shared_ptr<std::atomic_bool> &cancel,
                 const std::shared_ptr<IndexProgress> &progress, const WorkspaceOptions &options,
                 const QString &compilationDirectory, IndexResult &result)
        : database_(database), writer_(database), root_(root), excludes_(excludes), cancel_(cancel),
          progress_(progress), options_(options), compilationDirectory_(compilationDirectory),
          result_(result), force_(mode == IndexRefresh::ReparseKnownFiles ||
                                  mode == IndexRefresh::RebuildFilesAndSymbols),
          rescan_(mode == IndexRefresh::DiscoverChanges ||
                  mode == IndexRefresh::RebuildFilesAndSymbols) {
        pool_.setMaxThreadCount(workers_);
        pool_.setThreadPriority(QThread::LowPriority);
    }

    void run() {
        QElapsedTimer timer;
        timer.start();
        discover();
        result_.discoveryMilliseconds = timer.elapsed();
        if (stopped())
            return;
        reconcile();
        if (stopped())
            return;
        timer.restart();
        symbols();
        result_.symbolMilliseconds = timer.elapsed();
        if (stopped())
            return;
        timer.restart();
        references();
        result_.referenceMilliseconds = timer.elapsed();
        stopped();
    }

private:
    bool stopped() {
        if (cancel_->load())
            result_.cancelled = true;
        return result_.cancelled;
    }

    void discover() {
        paths_ = scanProjectFiles(database_, root_, rescan_, excludes_, cancel_, progress_, result_,
                                  options_);
        if (!cancel_->load())
            result_.files = paths_;
    }

    void reconcile() {
        // Discovery must finish before deleting anything. Parsing never owns catalog membership.
        result_.changed = writer_.reconcile(paths_, cancel_) || result_.changed;
        if (!stopped() && progress_) {
            progress_->phase.store(IndexPhase::Symbols);
            if (progress_->catalogReady)
                progress_->catalogReady(paths_);
        }
    }

    void symbols() {
        QElapsedTimer notifications;
        notifications.start();
        bool pendingNotification = false;
        bool publishedBatch = false;
        // Only this bounded window owns parse results; the coordinator is the sole SQLite writer.
        for (qsizetype start = 0; start < paths_.size() && !cancel_->load();
             start += workers_ * 2) {
            QVector<QFuture<PreparedFile>> jobs;
            const auto end = qMin(paths_.size(), start + workers_ * 2);
            for (qsizetype i = start; i < end && !cancel_->load(); ++i) {
                PreparedFile input;
                input.path = paths_[i];
                input.info = QFileInfo(input.path);
                const auto previous = writer_.metadata(input.path);
                input.found = previous.found;
                input.oldState = previous.state;
                input.changed = !input.found || previous.size != input.info.size() ||
                                previous.mtime != input.info.lastModified().toMSecsSinceEpoch();
                jobs.append(QtConcurrent::run(
                    &pool_,
                    [input = std::move(input), previous, force = force_, cancel = cancel_,
                     progress = progress_, compilationDirectory = compilationDirectory_]() mutable {
                        return prepareFile(std::move(input), previous.hash, previous.parser, force,
                                           cancel, progress, compilationDirectory);
                    }));
            }
            bool committed = false;
            for (auto &job : jobs) {
                auto prepared = job.result();
                job = {};
                if (cancel_->load())
                    break;
                if (!prepared.error.isEmpty()) {
                    warning(result_, prepared.path + ": " + prepared.error);
                    // A read/parse failure must not discard the last usable file snapshot.
                    if (prepared.found)
                        continue;
                }
                const auto &analysis = prepared.analysis;
                if (analysis.syntaxErrors || (prepared.reused && prepared.oldState == "partial"))
                    warning(result_, prepared.path +
                                         ": incomplete syntax; symbols are best effort. " +
                                         analysis.diagnostics.join("; "));
                const bool unchangedUnsupported =
                    !force_ && prepared.found && !prepared.supported &&
                    prepared.oldState == "unsupported" && !prepared.changed;
                const FileIndexRecord record{
                    prepared.path,
                    QString::fromLatin1(prepared.hash),
                    prepared.info.size(),
                    prepared.info.lastModified().toMSecsSinceEpoch(),
                    !prepared.error.isEmpty() ? "error"
                    : !prepared.supported     ? "unsupported"
                    : analysis.syntaxErrors   ? "partial"
                                              : "indexed",
                    prepared.error.isEmpty() ? analysis.diagnostics.join('\n') : prepared.error};
                if (prepared.reused) {
                    if (prepared.changed)
                        writer_.updateMetadata(record);
                } else if (!unchangedUnsupported) {
                    if (!writer_.replace(record, analysis.symbols, cancel_))
                        break;
                }
                committed = true;
                result_.changed = result_.changed || prepared.changed;
                if (!prepared.hash.isEmpty())
                    result_.sourceHashes.insert(prepared.path, prepared.hash);
                if (prepared.reused)
                    ++result_.reused;
                if (prepared.parsed)
                    ++result_.parsed;
                if (progress_) {
                    if (prepared.parsed)
                        ++progress_->parsed;
                    ++progress_->committed;
                }
            }
            pendingNotification = pendingNotification || committed;
            if (pendingNotification && progress_ && progress_->batchReady &&
                (!publishedBatch || notifications.elapsed() >= 150)) {
                progress_->batchReady();
                publishedBatch = true;
                pendingNotification = false;
                notifications.restart();
            }
        }
        pool_.waitForDone();
        if (pendingNotification && progress_ && progress_->batchReady)
            progress_->batchReady();
    }

    void references() {
        // Optional postings are a later phase. Missing/stale postings retain the full-search fallback.
        if (progress_)
            progress_->phase.store(IndexPhase::References);
        ReferenceIndexWriter writer(database_);
        for (qsizetype start = 0; start < paths_.size() && !cancel_->load();
             start += workers_ * 2) {
            QVector<QFuture<ReferenceContent>> jobs;
            const auto end = qMin(paths_.size(), start + workers_ * 2);
            for (qsizetype i = start; i < end && !cancel_->load(); ++i) {
                const auto path = paths_[i];
                const auto previous = writer.previous(path);
                jobs.append(QtConcurrent::run(&pool_, [path, previous, cancel = cancel_] {
                    if (cancel->load()) {
                        ReferenceContent result;
                        result.cancelled = true;
                        return result;
                    }
                    const auto content =
                        readSourceFile(path, sourceLanguage(path) ? SourceReadContext::Document
                                                                  : SourceReadContext::Catalog);
                    QByteArray hash;
                    if (content.error.isEmpty() && !cancel->load())
                        hash = sourceContentHash(content.bytes);
                    return prepareReferenceContent(path, content.bytes, hash, previous, cancel);
                }));
            }
            for (auto &job : jobs) {
                auto content = job.result();
                job = {};
                if (cancel_->load())
                    break;
                result_.changed = writer.replace(content, cancel_) || result_.changed;
            }
        }
        pool_.waitForDone();
    }

    ProjectIndexStore &database_;
    ProjectIndexWriter writer_;
    const QString &root_;
    const QStringList &excludes_;
    const std::shared_ptr<std::atomic_bool> &cancel_;
    const std::shared_ptr<IndexProgress> &progress_;
    const WorkspaceOptions &options_;
    const QString &compilationDirectory_;
    IndexResult &result_;
    const bool force_;
    const bool rescan_;
    QStringList paths_;
    const int workers_ = qBound(1, QThread::idealThreadCount(), 8);
    QThreadPool pool_;
};

}

IndexResult buildWorkspaceIndex(const QString &requestedRoot, IndexRefresh mode,
                                const QStringList &excludes,
                                const std::shared_ptr<std::atomic_bool> &cancel,
                                const std::shared_ptr<IndexProgress> &progress,
                                const QString &projectFile) {
    IndexResult result;
    const auto root =
        QDir::isAbsolutePath(requestedRoot) ? workspacePath(requestedRoot) : requestedRoot;
    result.root = root;
    try {
        if (!QFileInfo(root).isDir())
            throw std::runtime_error("Workspace folder does not exist.");
        const auto storageError = WorkspaceOptionsStore::initialize(root, projectFile);
        if (!storageError.isEmpty())
            throw std::runtime_error(storageError.toStdString());
        const auto options = WorkspaceOptionsStore::loadOptions(root, projectFile);
        if (!options.error.isEmpty())
            throw std::runtime_error(options.error.toStdString());
        const auto metadata = workspaceStorageDirectory(root, {}, projectFile);
        const auto database = workspaceDatabase(root, projectFile);
        const auto compilationDirectory =
            projectFile.isEmpty() ? QString{} : workspaceClangdDirectory(root, projectFile);
        for (const auto &suffix : {"", "-wal", "-shm"})
            if (QFileInfo(database + suffix).isSymLink())
                throw std::runtime_error("Refusing a symlinked workspace database or journal.");
        QLockFile lock(QDir(metadata).filePath("index.lock"));
        if (!lock.tryLock(0))
            throw std::runtime_error("Another CodeCairn process is indexing this workspace.");
        ProjectIndexStore db(database, true);
        db.ensureSchema();
        const auto cancellation = cancel ? cancel : std::make_shared<std::atomic_bool>(false);
        IndexingPass pass(db, root, mode, excludes, cancellation, progress, options,
                          compilationDirectory, result);
        pass.run();
    } catch (const std::runtime_error &error) {
        result.error = QString::fromUtf8(error.what());
        qCWarning(indexLog) << "index.failed" << "root=" << root << "error=" << result.error;
    }
    return result;
}

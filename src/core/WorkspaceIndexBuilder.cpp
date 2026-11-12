#include "WorkspaceIndex.h"
#include "Diagnostics.h"
#include "FileIO.h"
#include "ReferenceCandidateIndex.h"
#include "SymbolIndexStore.h"
#include "WorkspaceScanner.h"
#include "WorkspaceOptionsStore.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLockFile>
#include <QThreadPool>
#include <QScopeGuard>
#include <QtConcurrentRun>
#include <algorithm>
#include <sqlite3.h>
#include <stdexcept>

using namespace WorkspaceIndexDetail;

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

IndexResult buildWorkspaceIndex(const QString &requestedRoot, bool force, bool rescan,
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
        SymbolIndexStore db(database, true);
        db.ensureSchema();
        QElapsedTimer timer;
        timer.start();
        const auto paths =
            scanWorkspaceFiles(db, root, rescan, excludes, cancel, progress, result, options);
        result.discoveryMilliseconds = timer.elapsed();
        if (cancel->load()) {
            result.cancelled = true;
            return result;
        }
        result.files = paths;
        // Discovery must finish before deleting anything. Parsing never owns catalog membership.
        db.exec("BEGIN IMMEDIATE; CREATE TEMP TABLE seen(path TEXT PRIMARY KEY);");
        {
            IndexStatement seen(db, "INSERT OR IGNORE INTO seen VALUES(?)");
            for (const auto &path : paths) {
                if (cancel->load()) {
                    result.cancelled = true;
                    return result;
                }
                seen.bind(1, path);
                seen.next();
                seen.reset();
            }
        }
        db.exec("DELETE FROM files WHERE path NOT IN (SELECT path FROM seen);");
        result.changed = sqlite3_changes(db.handle()) > 0;
        db.exec("COMMIT;");
        if (progress) {
            progress->phase.store(1);
            if (progress->catalogReady)
                progress->catalogReady(paths);
        }

        timer.restart();
        QThreadPool pool;
        const int workers = qBound(1, QThread::idealThreadCount(), 8);
        pool.setMaxThreadCount(workers);
        pool.setThreadPriority(QThread::LowPriority);
        IndexStatement previous(db, "SELECT hash,parser,state,size,mtime FROM files WHERE path=?");
        IndexStatement file(
            db, "INSERT INTO files VALUES(?,?,?,?,?,?,?) ON CONFLICT(path) DO UPDATE SET "
                "hash=excluded.hash,size=excluded.size,mtime=excluded.mtime,state=excluded."
                "state,error=excluded.error,parser=excluded.parser");
        IndexStatement remove(db, "DELETE FROM symbols WHERE path=?");
        IndexStatement updateMetadata(db, "UPDATE files SET size=?,mtime=? WHERE path=?");
        IndexStatement insert(db, "INSERT INTO symbols VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        QElapsedTimer notifications;
        notifications.start();
        bool pendingNotification = false;
        bool publishedBatch = false;
        // Only this bounded window owns parse results; the coordinator is the sole SQLite writer.
        for (qsizetype start = 0; start < paths.size() && !cancel->load(); start += workers * 2) {
            QVector<QFuture<PreparedFile>> jobs;
            const auto end = qMin(paths.size(), start + workers * 2);
            for (qsizetype i = start; i < end && !cancel->load(); ++i) {
                PreparedFile input;
                input.path = paths[i];
                input.info = QFileInfo(input.path);
                previous.bind(1, input.path);
                input.found = previous.next();
                const auto oldHash = input.found ? previous.text(0) : QString{};
                const auto oldParser = input.found ? previous.text(1) : QString{};
                input.oldState = input.found ? previous.text(2) : QString{};
                input.changed =
                    !input.found || previous.integer(3) != input.info.size() ||
                    previous.integer(4) != input.info.lastModified().toMSecsSinceEpoch();
                previous.reset();
                jobs.append(QtConcurrent::run(&pool, [input, oldHash, oldParser, force, cancel,
                                                      progress, compilationDirectory] {
                    return prepareFile(input, oldHash, oldParser, force, cancel, progress,
                                       compilationDirectory);
                }));
            }
            bool committed = false;
            for (auto &job : jobs) {
                auto prepared = job.result();
                if (cancel->load())
                    break;
                if (!prepared.error.isEmpty()) {
                    warning(result, prepared.path + ": " + prepared.error);
                    // A read/parse failure must not discard the last usable file snapshot.
                    if (prepared.found)
                        continue;
                }
                const auto &analysis = prepared.analysis;
                if (analysis.syntaxErrors || (prepared.reused && prepared.oldState == "partial"))
                    warning(result, prepared.path +
                                        ": incomplete syntax; symbols are best effort. " +
                                        analysis.diagnostics.join("; "));
                const bool write = !prepared.reused || prepared.changed;
                const bool unchangedUnsupported = !force && prepared.found && !prepared.supported &&
                                                  prepared.oldState == "unsupported" &&
                                                  !prepared.changed;
                if (write && !unchangedUnsupported)
                    db.exec("BEGIN IMMEDIATE;");
                if (prepared.reused) {
                    if (prepared.changed) {
                        updateMetadata.bind(1, prepared.info.size());
                        updateMetadata.bind(2, prepared.info.lastModified().toMSecsSinceEpoch());
                        updateMetadata.bind(3, prepared.path);
                        updateMetadata.next();
                        updateMetadata.reset();
                    }
                } else if (!unchangedUnsupported) {
                    file.bind(1, prepared.path);
                    file.bind(2, QString::fromLatin1(prepared.hash));
                    file.bind(3, prepared.info.size());
                    file.bind(4, prepared.info.lastModified().toMSecsSinceEpoch());
                    file.bind(5, !prepared.error.isEmpty() ? "error"
                                 : !prepared.supported     ? "unsupported"
                                 : analysis.syntaxErrors   ? "partial"
                                                           : "indexed");
                    file.bind(6, prepared.error.isEmpty() ? analysis.diagnostics.join('\n')
                                                          : prepared.error);
                    file.bind(7, ParserVersion);
                    file.next();
                    file.reset();
                    remove.bind(1, prepared.path);
                    remove.next();
                    remove.reset();
                    for (const auto &symbol : analysis.symbols) {
                        if (symbol.outlineOnly || symbol.local)
                            continue;
                        insert.bind(1, prepared.path);
                        insert.bind(2, symbol.name);
                        insert.bind(3, symbol.kind);
                        insert.bind(4, symbol.qualifiedName);
                        insert.bind(5, symbol.signature);
                        insert.bind(6, symbol.definition ? 1 : 0);
                        insert.bind(7, symbol.start);
                        insert.bind(8, symbol.end);
                        insert.bind(9, symbol.nameStart);
                        insert.bind(10, symbol.nameEnd);
                        insert.bind(11, symbol.line);
                        insert.bind(12, symbol.category);
                        insert.bind(13, symbol.generated ? 1 : 0);
                        insert.bind(14, symbol.provider);
                        insert.next();
                        insert.reset();
                    }
                }
                if (write && !unchangedUnsupported)
                    db.exec("COMMIT;");
                committed = true;
                result.changed = result.changed || prepared.changed;
                if (!prepared.hash.isEmpty())
                    result.sourceHashes.insert(prepared.path, prepared.hash);
                if (prepared.reused)
                    ++result.reused;
                if (prepared.parsed)
                    ++result.parsed;
                if (progress) {
                    if (prepared.parsed)
                        ++progress->parsed;
                    ++progress->committed;
                }
            }
            pendingNotification = pendingNotification || committed;
            if (pendingNotification && progress && progress->batchReady &&
                (!publishedBatch || notifications.elapsed() >= 150)) {
                progress->batchReady();
                publishedBatch = true;
                pendingNotification = false;
                notifications.restart();
            }
        }
        pool.waitForDone();
        if (pendingNotification && progress && progress->batchReady)
            progress->batchReady();
        result.symbolMilliseconds = timer.elapsed();
        if (cancel->load()) {
            result.cancelled = true;
            return result;
        }

        // Optional postings are a later phase. Missing/stale postings retain the full-search fallback.
        timer.restart();
        if (progress)
            progress->phase.store(2);
        for (const auto &path : paths) {
            if (cancel->load()) {
                result.cancelled = true;
                break;
            }
            const auto content =
                readSourceFile(path, sourceLanguage(path) ? SourceReadContext::Document
                                                          : SourceReadContext::Catalog);
            QByteArray hash;
            if (content.error.isEmpty())
                hash = sourceContentHash(content.bytes);
            db.exec("BEGIN IMMEDIATE;");
            result.changed =
                updateReferenceOccurrences(db, path, content.bytes, hash, cancel) || result.changed;
            if (cancel->load()) {
                db.exec("ROLLBACK;");
                result.cancelled = true;
                break;
            }
            db.exec("COMMIT;");
        }
        result.referenceMilliseconds = timer.elapsed();
    } catch (const std::runtime_error &error) {
        result.error = QString::fromUtf8(error.what());
        qCWarning(indexLog) << "index.failed" << "root=" << root << "error=" << result.error;
    }
    return result;
}

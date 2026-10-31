#include "WorkspaceIndex.h"
#include "Diagnostics.h"
#include "FileIO.h"
#include "ReferenceCandidateIndex.h"
#include "SymbolIndexStore.h"
#include "WorkspaceScanner.h"
#include "WorkspaceOptionsStore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QLockFile>
#include <sqlite3.h>
#include <stdexcept>

using namespace WorkspaceIndexDetail;

IndexResult buildWorkspaceIndex(const QString &root, bool force, bool rescan,
                                const QStringList &excludes,
                                const std::shared_ptr<std::atomic_bool> &cancel,
                                const std::shared_ptr<IndexProgress> &progress) {
    IndexResult result;
    result.root = root;
    try {
        if (!QFileInfo(root).isDir())
            throw std::runtime_error("Workspace folder does not exist.");
        const auto storageError = WorkspaceOptionsStore::initialize(root);
        if (!storageError.isEmpty())
            throw std::runtime_error(storageError.toStdString());
        const auto metadata = workspaceStorageDirectory(root);
        for (const auto &suffix : {"", "-wal", "-shm"})
            if (QFileInfo(workspaceDatabase(root) + suffix).isSymLink())
                throw std::runtime_error("Refusing a symlinked workspace database or journal.");
        QLockFile lock(QDir(metadata).filePath("index.lock"));
        if (!lock.tryLock(0))
            throw std::runtime_error("Another CodeCairn process is indexing this workspace.");
        SymbolIndexStore db(workspaceDatabase(root), true);
        db.ensureSchema();
        const auto paths = scanWorkspaceFiles(db, root, rescan, excludes, cancel, progress, result);
        if (cancel->load()) {
            result.cancelled = true;
            return result;
        }
        db.exec("BEGIN IMMEDIATE; CREATE TEMP TABLE seen(path TEXT PRIMARY KEY);");
        IndexStatement seen(db, "INSERT OR IGNORE INTO seen VALUES(?)");
        IndexStatement previous(db, "SELECT hash,parser,state,size,mtime FROM files WHERE path=?");
        IndexStatement file(
            db, "INSERT INTO files VALUES(?,?,?,?,?,?,?) ON CONFLICT(path) DO UPDATE SET "
                "hash=excluded.hash,size=excluded.size,mtime=excluded.mtime,state=excluded."
                "state,error=excluded.error,parser=excluded.parser");
        IndexStatement remove(db, "DELETE FROM symbols WHERE path=?");
        IndexStatement updateMetadata(db, "UPDATE files SET size=?,mtime=? WHERE path=?");
        IndexStatement insert(db, "INSERT INTO symbols VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        for (const auto &path : paths) {
            if (cancel->load()) {
                result.cancelled = true;
                return result;
            }
            if (!QFileInfo::exists(path))
                continue;
            seen.bind(1, path);
            seen.next();
            seen.reset();
            previous.bind(1, path);
            const bool found = previous.next();
            const QString oldHash = found ? previous.text(0) : QString{};
            const QString oldParser = found ? previous.text(1) : QString{};
            const QString oldState = found ? previous.text(2) : QString{};
            const qint64 oldSize = found ? previous.integer(3) : -1;
            const qint64 oldMtime = found ? previous.integer(4) : -1;
            previous.reset();
            const QFileInfo info(path);
            if (!found || oldSize != info.size() ||
                oldMtime != info.lastModified().toMSecsSinceEpoch())
                result.changed = true;
            SourceAnalysis analysis;
            QString error;
            QByteArray hash;
            const bool parseSymbols = sourceLanguage(path);
            const auto content = readSourceFile(path, parseSymbols ? SourceReadContext::Document
                                                                   : SourceReadContext::Catalog);
            const QByteArray occurrenceHash =
                content.error.isEmpty()
                    ? QCryptographicHash::hash(content.bytes, QCryptographicHash::Sha256).toHex()
                    : QByteArray{};
            const auto refreshOccurrences = [&] {
                result.changed =
                    updateReferenceOccurrences(db, path, content.bytes, occurrenceHash, cancel) ||
                    result.changed;
            };
            QString state = "unsupported";
            if (parseSymbols) {
                error = content.error;
                if (error.isEmpty()) {
                    hash = occurrenceHash;
                    result.sourceHashes.insert(path, hash);
                    if (!force && found && oldHash == QString::fromLatin1(hash) &&
                        oldParser == ParserVersion &&
                        (oldState == "indexed" || oldState == "partial")) {
                        if (oldState == "partial")
                            result.warnings.append(path +
                                                   ": incomplete syntax; symbols are best effort.");
                        if (oldSize != info.size() ||
                            oldMtime != info.lastModified().toMSecsSinceEpoch()) {
                            updateMetadata.bind(1, info.size());
                            updateMetadata.bind(2, info.lastModified().toMSecsSinceEpoch());
                            updateMetadata.bind(3, path);
                            updateMetadata.next();
                            updateMetadata.reset();
                        }
                        ++result.reused;
                        result.files.append(path);
                        refreshOccurrences();
                        continue;
                    }
                    analysis = analyzeSource(content.bytes, path);
                    result.changed = true;
                    error = analysis.error;
                    const QFileInfo after(path);
                    if (info.lastModified() != after.lastModified() ||
                        info.size() != after.size()) {
                        error = "File changed while being parsed; refresh to retry.";
                        qCWarning(indexLog) << "index.source_changed" << "root=" << root
                                            << "path=" << path << "error=" << error;
                    }
                    state = analysis.syntaxErrors ? "partial" : "indexed";
                    if (analysis.syntaxErrors)
                        result.warnings.append(path + ": incomplete syntax" +
                                               (analysis.compilerEnriched
                                                    ? "; compiler supplement applied. "
                                                    : "; symbols are best effort. ") +
                                               analysis.diagnostics.join("; "));
                    ++result.parsed;
                    if (progress)
                        ++progress->parsed;
                }
                if (!error.isEmpty())
                    state = "error";
            }
            if (!force && found && state == "unsupported" && oldState == state &&
                oldSize == info.size() && oldMtime == info.lastModified().toMSecsSinceEpoch()) {
                result.files.append(path);
                refreshOccurrences();
                continue;
            }
            file.bind(1, path);
            file.bind(2, QString::fromLatin1(hash));
            file.bind(3, info.size());
            file.bind(4, info.lastModified().toMSecsSinceEpoch());
            file.bind(5, state);
            file.bind(6, error.isEmpty() ? analysis.diagnostics.join('\n') : error);
            file.bind(7, ParserVersion);
            file.next();
            file.reset();
            refreshOccurrences();
            remove.bind(1, path);
            remove.next();
            remove.reset();
            if (error.isEmpty()) {
                for (const auto &symbol : analysis.symbols) {
                    if (symbol.outlineOnly || symbol.local)
                        continue;
                    insert.bind(1, path);
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
            } else
                result.warnings.append(path + ": " + error);
            result.files.append(path);
        }
        if (cancel->load()) {
            result.cancelled = true;
            return result;
        }
        db.exec("DELETE FROM files WHERE path NOT IN (SELECT path FROM seen);");
        result.changed = result.changed || sqlite3_changes(db.handle()) > 0;
        db.exec("COMMIT;");
    } catch (const std::runtime_error &error) {
        result.error = QString::fromUtf8(error.what());
        qCWarning(indexLog) << "index.failed" << "root=" << root << "error=" << result.error;
    }
    return result;
}

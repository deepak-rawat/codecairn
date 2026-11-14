#include "ReferenceCandidateIndex.h"
#include <codecairn/Diagnostics.h>

#include <codecairn/FileIO.h>
#include "ProjectIndexStore.h"
#include <codecairn/Paths.h>

#include <QDateTime>
#include <QFileInfo>
#include <QSet>
#include <stdexcept>

using namespace codecairn::detail;

namespace {
bool identifierCharacter(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool acceleratedIdentifier(const QString &identifier) {
    if (identifier.isEmpty() || (identifier.front() >= '0' && identifier.front() <= '9'))
        return false;
    for (const auto c : identifier)
        if (c.unicode() > 127 || !identifierCharacter(static_cast<unsigned char>(c.unicode())))
            return false;
    return true;
}

bool cancelled(const std::shared_ptr<std::atomic_bool> &cancel) {
    return cancel && cancel->load();
}
}

ReferenceContent codecairn::detail::prepareReferenceContent(
    const QString &path, const QByteArray &bytes, const QByteArray &hash,
    const ReferenceFileState &previous, const std::shared_ptr<std::atomic_bool> &cancel) {
    ReferenceContent result;
    result.path = path;
    result.hash = hash;
    result.previousFound = previous.found;
    result.unchanged = !hash.isEmpty() && previous.hash == hash;
    if (cancelled(cancel)) {
        result.cancelled = true;
        return result;
    }
    if (result.unchanged || hash.isEmpty())
        return result;
    qsizetype start = 0;
    for (qsizetype i = 0; i <= bytes.size(); ++i) {
        if ((i & 4095) == 0 && cancelled(cancel)) {
            result.cancelled = true;
            return result;
        }
        const auto c = i < bytes.size() ? static_cast<unsigned char>(bytes.at(i)) : 0;
        if (c >= 128)
            result.asciiOnly = false;
        if (i < bytes.size() && identifierCharacter(c))
            continue;
        if (i > start)
            result.identifiers.insert(bytes.mid(start, i - start));
        start = i + 1;
    }
    return result;
}

ReferenceIndexWriter::ReferenceIndexWriter(ProjectIndexStore &database)
    : database_(database),
      previous_(database, "SELECT hash,version FROM reference_occurrence_files WHERE path=?"),
      remove_(database, "DELETE FROM reference_occurrence_files WHERE path=?"),
      file_(database, "INSERT INTO reference_occurrence_files VALUES(?,?,?,?)"),
      posting_(database, "INSERT INTO reference_occurrences VALUES(?,?,?)"),
      complete_(database, "UPDATE reference_occurrence_files SET version=? WHERE path=?") {
}

ReferenceFileState ReferenceIndexWriter::previous(const QString &path) {
    previous_.bind(1, path);
    ReferenceFileState result;
    result.found = previous_.next();
    if (result.found && previous_.integer(1) == ReferenceOccurrenceVersion)
        result.hash = previous_.text(0).toLatin1();
    previous_.reset();
    return result;
}

bool ReferenceIndexWriter::replace(const ReferenceContent &content,
                                   const std::shared_ptr<std::atomic_bool> &cancel) {
    if (content.cancelled || cancelled(cancel) || content.unchanged ||
        (content.hash.isEmpty() && !content.previousFound))
        return false;
    IndexTransaction transaction(database_);
    const bool changed = updateInTransaction(content, cancel);
    if (cancelled(cancel))
        return false;
    transaction.commit();
    return changed;
}

bool ReferenceIndexWriter::updateInTransaction(const ReferenceContent &content,
                                               const std::shared_ptr<std::atomic_bool> &cancel) {
    if (content.cancelled || cancelled(cancel) || content.unchanged ||
        (content.hash.isEmpty() && !content.previousFound))
        return false;
    remove_.bind(1, content.path);
    remove_.next();
    remove_.reset();
    if (content.hash.isEmpty())
        return content.previousFound;
    file_.bind(1, content.path);
    file_.bind(2, QString::fromLatin1(content.hash));
    file_.bind(3, qint64{0});
    file_.bind(4, content.asciiOnly ? 1 : 0);
    file_.next();
    file_.reset();
    for (const auto &identifier : content.identifiers) {
        if (cancelled(cancel))
            return true;
        posting_.bind(1, content.path);
        posting_.bind(2, QString::fromLatin1(identifier));
        posting_.bind(3, QString::fromLatin1(identifier.toLower()));
        posting_.next();
        posting_.reset();
    }
    complete_.bind(1, ReferenceOccurrenceVersion);
    complete_.bind(2, content.path);
    complete_.next();
    complete_.reset();
    return true;
}

bool codecairn::detail::updateReferenceOccurrences(
    ProjectIndexStore &database, const QString &path, const QByteArray &bytes,
    const QByteArray &hash, const std::shared_ptr<std::atomic_bool> &cancel) {
    ReferenceIndexWriter writer(database);
    const auto content = prepareReferenceContent(path, bytes, hash, writer.previous(path), cancel);
    return writer.updateInTransaction(content, cancel);
}

ReferenceCandidateResult referenceCandidateFiles(const QString &root, const QStringList &paths,
                                                 const QString &identifier, bool caseSensitive,
                                                 const std::shared_ptr<std::atomic_bool> &cancel,
                                                 const QString &projectFile) {
    ReferenceCandidateResult result;
    result.files = paths;
    if (cancelled(cancel)) {
        result.cancelled = true;
        return result;
    }
    if (paths.isEmpty() || !acceleratedIdentifier(identifier))
        return result;
    if (!QFileInfo::exists(projectIndexPath(root, projectFile)))
        return result;
    try {
        ProjectIndexStore database(projectIndexPath(root, projectFile), false);
        database.exec("BEGIN;");
        {
            IndexStatement version(database, "PRAGMA user_version");
            version.next();
            const auto schemaVersion = version.integer(0);
            if (schemaVersion < 0 || schemaVersion > 2)
                throw std::runtime_error(
                    "Unsupported workspace database version; use a compatible CodeCairn version.");
            if (schemaVersion < 2) {
                qCDebug(indexLog) << "index.candidates_pending" << "root=" << root
                                  << "schemaVersion=" << schemaVersion;
                return result;
            }
        }
        IndexStatement metadata(
            database,
            "SELECT hash,version,ascii_only FROM reference_occurrence_files WHERE path=?");
        IndexStatement posting(
            database, caseSensitive
                          ? "SELECT 1 FROM reference_occurrences WHERE path=? AND identifier=?"
                          : "SELECT 1 FROM reference_occurrences WHERE path=? AND folded=?");
        const QString key = caseSensitive ? identifier : identifier.toLower();
        QStringList candidates;
        for (const auto &path : paths) {
            if (cancelled(cancel)) {
                result.cancelled = true;
                return result;
            }
            const auto identity = canonicalPath(path);
            metadata.bind(1, identity);
            const bool found = metadata.next();
            const auto hash = found ? metadata.text(0) : QString{};
            const bool complete = found && metadata.integer(1) == ReferenceOccurrenceVersion &&
                                  (caseSensitive || metadata.integer(2) == 1);
            metadata.reset();
            if (!complete || hash.isEmpty()) {
                candidates.append(path);
                continue;
            }
            posting.bind(1, identity);
            posting.bind(2, key);
            const bool present = posting.next();
            posting.reset();
            if (present) {
                candidates.append(path);
                continue;
            }
            // Size/mtime can be restored or have coarse resolution. Hash the actual bytes
            // before trusting a negative posting, including for unchanged-looking files.
            const QFileInfo before(path);
            const auto content = readSourceFile(path);
            const QFileInfo after(path);
            QByteArray sourceHash;
            if (content.error.isEmpty()) {
                sourceHash = sourceContentHash(content.bytes);
                result.sourceHashes.insert(canonicalPath(path), sourceHash);
            }
            if (!content.error.isEmpty() || before.size() != after.size() ||
                before.lastModified() != after.lastModified() ||
                QString::fromLatin1(sourceHash) != hash)
                candidates.append(path);
        }
        if (cancelled(cancel))
            result.cancelled = true;
        else
            result.files = candidates;
    } catch (const std::runtime_error &error) {
        result.error = QString::fromUtf8(error.what());
        if (!cancelled(cancel))
            qCWarning(indexLog) << "index.candidates_failed" << "root=" << root
                                << "files=" << paths.size() << "error=" << result.error;
    }
    return result;
}

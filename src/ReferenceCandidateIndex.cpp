#include "ReferenceCandidateIndex.h"
#include "Diagnostics.h"

#include "FileIO.h"
#include "SymbolIndexStore.h"
#include "WorkspacePaths.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFileInfo>
#include <QSet>
#include <stdexcept>

using namespace WorkspaceIndexDetail;

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

bool WorkspaceIndexDetail::updateReferenceOccurrences(
    SymbolIndexStore &database, const QString &path, const QByteArray &bytes,
    const QByteArray &hash, const std::shared_ptr<std::atomic_bool> &cancel) {
    IndexStatement previous(database,
                            "SELECT hash,version FROM reference_occurrence_files WHERE path=?");
    previous.bind(1, path);
    const bool found = previous.next();
    if (found && !hash.isEmpty() && previous.text(0) == QString::fromLatin1(hash) &&
        previous.integer(1) == ReferenceOccurrenceVersion)
        return false;
    previous.reset();
    IndexStatement remove(database, "DELETE FROM reference_occurrence_files WHERE path=?");
    remove.bind(1, path);
    remove.next();
    if (hash.isEmpty() || cancelled(cancel))
        return found;

    QSet<QByteArray> identifiers;
    bool asciiOnly = true;
    qsizetype start = 0;
    for (qsizetype i = 0; i <= bytes.size(); ++i) {
        if ((i & 4095) == 0 && cancelled(cancel))
            return true;
        const auto c = i < bytes.size() ? static_cast<unsigned char>(bytes.at(i)) : 0;
        if (c >= 128)
            asciiOnly = false;
        if (i < bytes.size() && identifierCharacter(c))
            continue;
        if (i > start)
            identifiers.insert(bytes.mid(start, i - start));
        start = i + 1;
    }
    IndexStatement file(database, "INSERT INTO reference_occurrence_files VALUES(?,?,?,?)");
    file.bind(1, path);
    file.bind(2, QString::fromLatin1(hash));
    file.bind(3, qint64{0});
    file.bind(4, asciiOnly ? 1 : 0);
    file.next();
    IndexStatement posting(database, "INSERT INTO reference_occurrences VALUES(?,?,?)");
    for (const auto &identifier : identifiers) {
        if (cancelled(cancel))
            return true;
        posting.bind(1, path);
        posting.bind(2, QString::fromLatin1(identifier));
        posting.bind(3, QString::fromLatin1(identifier.toLower()));
        posting.next();
        posting.reset();
    }
    IndexStatement complete(database,
                            "UPDATE reference_occurrence_files SET version=? WHERE path=?");
    complete.bind(1, ReferenceOccurrenceVersion);
    complete.bind(2, path);
    complete.next();
    return true;
}

ReferenceCandidateResult referenceCandidateFiles(const QString &root, const QStringList &paths,
                                                 const QString &identifier, bool caseSensitive,
                                                 const std::shared_ptr<std::atomic_bool> &cancel) {
    ReferenceCandidateResult result;
    result.files = paths;
    if (cancelled(cancel)) {
        result.cancelled = true;
        return result;
    }
    if (paths.isEmpty() || !acceleratedIdentifier(identifier))
        return result;
    if (!QFileInfo::exists(workspaceDatabase(root)))
        return result;
    try {
        SymbolIndexStore database(workspaceDatabase(root), false);
        database.exec("BEGIN;");
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
            metadata.bind(1, path);
            const bool found = metadata.next();
            const auto hash = found ? metadata.text(0) : QString{};
            const bool complete = found && metadata.integer(1) == ReferenceOccurrenceVersion &&
                                  (caseSensitive || metadata.integer(2) == 1);
            metadata.reset();
            if (!complete || hash.isEmpty()) {
                candidates.append(path);
                continue;
            }
            posting.bind(1, path);
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
                sourceHash =
                    QCryptographicHash::hash(content.bytes, QCryptographicHash::Sha256).toHex();
                result.sourceHashes.insert(workspacePath(path), sourceHash);
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

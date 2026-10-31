#pragma once

#include <QByteArray>
#include <QHash>
#include <QStringList>
#include <atomic>
#include <memory>

struct ReferenceCandidateResult {
    QStringList files;
    QString error;
    bool cancelled = false;
    QHash<QString, QByteArray> sourceHashes;
};

// Preserves input order and duplicates. Only complete postings whose content hash matches a
// fresh disk read can exclude a file. Errors conservatively return all paths. Callers must
// always scan dirty overlays separately, even when their disk paths are not returned.
// sourceHashes records normalized paths and hex SHA-256 hashes for successful fresh source reads,
// including changed sources retained as candidates. Positive postings do not require a disk read.
// Use only for exact whole-identifier searches, not substring or regular-expression searches.
// ASCII identifiers are accelerated; other queries bypass the index. Case-insensitive
// queries retain non-ASCII files to avoid differing Unicode case-fold/boundary semantics.
ReferenceCandidateResult
referenceCandidateFiles(const QString &root, const QStringList &paths, const QString &identifier,
                        bool caseSensitive, const std::shared_ptr<std::atomic_bool> &cancel = {});

namespace WorkspaceIndexDetail {
class SymbolIndexStore;

// Called inside the file-index transaction, after the parent files row exists.
// An empty hash invalidates previous postings instead of certifying unreadable content.
bool updateReferenceOccurrences(SymbolIndexStore &database, const QString &path,
                                const QByteArray &bytes, const QByteArray &hash,
                                const std::shared_ptr<std::atomic_bool> &cancel);
}

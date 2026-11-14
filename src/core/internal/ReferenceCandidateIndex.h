#pragma once

#include "ProjectIndexStore.h"
#include <QByteArray>
#include <QHash>
#include <QSet>
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
                        bool caseSensitive, const std::shared_ptr<std::atomic_bool> &cancel = {},
                        const QString &projectFile = {});

namespace codecairn::detail {

struct ReferenceFileState {
    QByteArray hash;
    bool found = false;
};

struct ReferenceContent {
    QString path;
    QByteArray hash;
    QSet<QByteArray> identifiers;
    bool asciiOnly = true;
    bool previousFound = false;
    bool unchanged = false;
    bool cancelled = false;
};

// CPU-only preparation; bytes are not retained after the bounded worker result is produced.
ReferenceContent prepareReferenceContent(const QString &path, const QByteArray &bytes,
                                         const QByteArray &hash, const ReferenceFileState &previous,
                                         const std::shared_ptr<std::atomic_bool> &cancel);

class ReferenceIndexWriter {
public:
    explicit ReferenceIndexWriter(ProjectIndexStore &database);
    ReferenceFileState previous(const QString &path);
    bool replace(const ReferenceContent &content, const std::shared_ptr<std::atomic_bool> &cancel);
    // Compatibility helper callers already own the file transaction.
    bool updateInTransaction(const ReferenceContent &content,
                             const std::shared_ptr<std::atomic_bool> &cancel);

private:
    ProjectIndexStore &database_;
    IndexStatement previous_;
    IndexStatement remove_;
    IndexStatement file_;
    IndexStatement posting_;
    IndexStatement complete_;
};

// Called inside the file-index transaction, after the parent files row exists.
// An empty hash invalidates previous postings instead of certifying unreadable content.
bool updateReferenceOccurrences(ProjectIndexStore &database, const QString &path,
                                const QByteArray &bytes, const QByteArray &hash,
                                const std::shared_ptr<std::atomic_bool> &cancel);
}

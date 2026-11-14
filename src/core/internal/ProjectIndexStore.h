#pragma once

#include <codecairn/SourceTypes.h>
#include <atomic>
#include <memory>

struct sqlite3;
struct sqlite3_stmt;

namespace codecairn::detail {

inline constexpr const char *ParserVersion = "codecairn-multilanguage-compiler-v17";
inline constexpr qint64 ReferenceOccurrenceVersion = 1;

// Closing the connection rolls back only the active file/batch transaction.
class ProjectIndexStore {
public:
    explicit ProjectIndexStore(const QString &path, bool write);
    ~ProjectIndexStore();
    ProjectIndexStore(const ProjectIndexStore &) = delete;
    ProjectIndexStore &operator=(const ProjectIndexStore &) = delete;

    void exec(const char *sql);
    void ensureSchema();
    void validateSchema();
    sqlite3 *handle() const { return database_; }

private:
    sqlite3 *database_ = nullptr;
};

class IndexStatement {
public:
    IndexStatement(ProjectIndexStore &database, const char *sql);
    ~IndexStatement();
    IndexStatement(const IndexStatement &) = delete;
    IndexStatement &operator=(const IndexStatement &) = delete;

    void bind(int index, const QString &value);
    void bind(int index, qint64 value);
    bool next();
    void reset();
    QString text(int column) const;
    qint64 integer(int column) const;

private:
    sqlite3 *database_;
    sqlite3_stmt *statement_ = nullptr;
};

class IndexTransaction {
public:
    explicit IndexTransaction(ProjectIndexStore &database);
    ~IndexTransaction();
    IndexTransaction(const IndexTransaction &) = delete;
    IndexTransaction &operator=(const IndexTransaction &) = delete;
    void commit();

private:
    ProjectIndexStore &database_;
    bool committed_ = false;
};

struct IndexedFileMetadata {
    QString hash;
    QString parser;
    QString state;
    qint64 size = 0;
    qint64 mtime = 0;
    bool found = false;
};

struct FileIndexRecord {
    QString path;
    QString hash;
    qint64 size = 0;
    qint64 mtime = 0;
    QString state;
    QString error;
};

// All statements and transactions stay on the coordinator's SQLite connection.
class ProjectIndexWriter {
public:
    explicit ProjectIndexWriter(ProjectIndexStore &database);
    IndexedFileMetadata metadata(const QString &path);
    bool reconcile(const QStringList &paths, const std::shared_ptr<std::atomic_bool> &cancel);
    void updateMetadata(const FileIndexRecord &file);
    bool replace(const FileIndexRecord &file, const QVector<codecairn::SourceSymbol> &symbols,
                 const std::shared_ptr<std::atomic_bool> &cancel);

private:
    ProjectIndexStore &database_;
    IndexStatement previous_;
    IndexStatement file_;
    IndexStatement remove_;
    IndexStatement metadata_;
    IndexStatement insert_;
};

}

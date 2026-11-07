#pragma once

#include <QString>

struct sqlite3;
struct sqlite3_stmt;

namespace WorkspaceIndexDetail {

inline constexpr const char *ParserVersion = "codecairn-multilanguage-compiler-v12";
inline constexpr qint64 ReferenceOccurrenceVersion = 1;

// Closing the connection rolls back an uncommitted build after its statements are destroyed.
class SymbolIndexStore {
public:
    explicit SymbolIndexStore(const QString &path, bool write);
    ~SymbolIndexStore();
    SymbolIndexStore(const SymbolIndexStore &) = delete;
    SymbolIndexStore &operator=(const SymbolIndexStore &) = delete;

    void exec(const char *sql);
    void ensureSchema();
    sqlite3 *handle() const { return database_; }

private:
    sqlite3 *database_ = nullptr;
};

class IndexStatement {
public:
    IndexStatement(SymbolIndexStore &database, const char *sql);
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

}

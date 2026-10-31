#include "SymbolIndexStore.h"

#include <sqlite3.h>
#include <stdexcept>

namespace WorkspaceIndexDetail {

SymbolIndexStore::SymbolIndexStore(const QString &path, bool write) {
    const auto bytes = path.toUtf8();
    if (sqlite3_open_v2(bytes.constData(), &database_,
                        write ? SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE : SQLITE_OPEN_READONLY,
                        nullptr) != SQLITE_OK) {
        const QString error =
            database_ ? QString::fromUtf8(sqlite3_errmsg(database_)) : "SQLite allocation failed";
        if (database_)
            sqlite3_close(database_);
        database_ = nullptr;
        throw std::runtime_error(error.toStdString());
    }
    sqlite3_busy_timeout(database_, 1500);
}

SymbolIndexStore::~SymbolIndexStore() {
    sqlite3_close(database_);
}

void SymbolIndexStore::exec(const char *sql) {
    char *message = nullptr;
    const int code = sqlite3_exec(database_, sql, nullptr, nullptr, &message);
    const QString error = message ? QString::fromUtf8(message) : QString{};
    sqlite3_free(message);
    if (code != SQLITE_OK)
        throw std::runtime_error(error.toStdString());
}

void SymbolIndexStore::ensureSchema() {
    qint64 value;
    {
        IndexStatement version(*this, "PRAGMA user_version");
        version.next();
        value = version.integer(0);
    }
    if (value < 0 || value > 2)
        throw std::runtime_error(
            "Unsupported workspace database version; use a compatible CodeCairn version.");
    exec("PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL;");
    exec("BEGIN IMMEDIATE; CREATE TABLE IF NOT EXISTS files("
         "path TEXT PRIMARY KEY, hash TEXT NOT NULL, size INTEGER, mtime INTEGER, state TEXT, "
         "error TEXT, parser TEXT);"
         "CREATE TABLE IF NOT EXISTS symbols("
         "path TEXT NOT NULL REFERENCES files(path) ON DELETE CASCADE, name TEXT, kind TEXT,"
         "qualified TEXT, signature TEXT, definition INTEGER, start INTEGER, end INTEGER,"
         "name_start INTEGER, name_end INTEGER, line INTEGER);"
         "CREATE INDEX IF NOT EXISTS symbol_name ON symbols(name COLLATE NOCASE);"
         "CREATE INDEX IF NOT EXISTS symbol_file ON symbols(path);");
    if (value < 2)
        exec("ALTER TABLE symbols ADD COLUMN category TEXT DEFAULT '';"
             "ALTER TABLE symbols ADD COLUMN generated INTEGER DEFAULT 0;"
             "ALTER TABLE symbols ADD COLUMN provider TEXT DEFAULT 'syntax';");
    exec(
        "CREATE TABLE IF NOT EXISTS reference_occurrence_files("
        "path TEXT PRIMARY KEY REFERENCES files(path) ON DELETE CASCADE,"
        "hash TEXT NOT NULL,version INTEGER NOT NULL,ascii_only INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS reference_occurrences("
        "path TEXT NOT NULL REFERENCES reference_occurrence_files(path) ON DELETE CASCADE,"
        "identifier TEXT NOT NULL,folded TEXT NOT NULL,PRIMARY KEY(path,identifier));"
        "CREATE INDEX IF NOT EXISTS reference_identifier ON reference_occurrences(identifier,path);"
        "CREATE INDEX IF NOT EXISTS reference_folded ON reference_occurrences(folded,path);"
        "PRAGMA user_version=2; COMMIT;");
}

IndexStatement::IndexStatement(SymbolIndexStore &database, const char *sql)
    : database_(database.handle()) {
    if (sqlite3_prepare_v2(database_, sql, -1, &statement_, nullptr) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(database_));
}

IndexStatement::~IndexStatement() {
    sqlite3_finalize(statement_);
}

void IndexStatement::bind(int index, const QString &value) {
    const auto utf8 = value.toUtf8();
    if (sqlite3_bind_text(statement_, index, utf8.constData(), static_cast<int>(utf8.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(database_));
}

void IndexStatement::bind(int index, qint64 value) {
    if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK)
        throw std::runtime_error(sqlite3_errmsg(database_));
}

bool IndexStatement::next() {
    const int result = sqlite3_step(statement_);
    if (result == SQLITE_ROW)
        return true;
    if (result != SQLITE_DONE)
        throw std::runtime_error(sqlite3_errmsg(database_));
    return false;
}

void IndexStatement::reset() {
    sqlite3_reset(statement_);
    sqlite3_clear_bindings(statement_);
}

QString IndexStatement::text(int column) const {
    const auto *text = sqlite3_column_text(statement_, column);
    return text ? QString::fromUtf8(reinterpret_cast<const char *>(text)) : QString{};
}

qint64 IndexStatement::integer(int column) const {
    return sqlite3_column_int64(statement_, column);
}

}

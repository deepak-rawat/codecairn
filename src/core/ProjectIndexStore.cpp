#include "ProjectIndexStore.h"
#include <codecairn/Diagnostics.h>

#include <QStringList>
#include <sqlite3.h>
#include <stdexcept>

namespace codecairn::detail {

ProjectIndexStore::ProjectIndexStore(const QString &path, bool write) {
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

ProjectIndexStore::~ProjectIndexStore() {
    sqlite3_close(database_);
}

void ProjectIndexStore::exec(const char *sql) {
    char *message = nullptr;
    const int code = sqlite3_exec(database_, sql, nullptr, nullptr, &message);
    const QString error = message ? QString::fromUtf8(message) : QString{};
    sqlite3_free(message);
    if (code != SQLITE_OK)
        throw std::runtime_error(error.toStdString());
}

void ProjectIndexStore::ensureSchema() {
    qint64 value;
    {
        IndexStatement version(*this, "PRAGMA user_version");
        version.next();
        value = version.integer(0);
    }
    if (value != 0 && value != 2)
        throw std::runtime_error(
            "Unsupported workspace database version; recreate the generated index.");
    if (value == 0) {
        IndexStatement tables(*this, "SELECT 1 FROM sqlite_master WHERE type='table' LIMIT 1");
        if (tables.next())
            throw std::runtime_error(
                "Unsupported workspace database version; recreate the generated index.");
    } else
        validateSchema();
    exec("PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL;");
    exec("BEGIN IMMEDIATE;");
    bool replaceCatalogIndex = false;
    {
        IndexStatement columns(*this, "PRAGMA index_info(symbol_catalog)");
        const QStringList prefix{"name", "path", "start"};
        int position = 0;
        while (position < prefix.size() && columns.next()) {
            replaceCatalogIndex = replaceCatalogIndex || columns.text(2) != prefix[position];
            ++position;
        }
    }
    if (replaceCatalogIndex)
        exec("DROP INDEX symbol_catalog;");
    bool replaceCategoryIndex = false;
    {
        IndexStatement columns(*this, "PRAGMA index_info(symbol_categories)");
        int count = 0;
        while (columns.next())
            ++count;
        replaceCategoryIndex = count > 0 && count < 8;
    }
    if (replaceCategoryIndex)
        exec("DROP INDEX symbol_categories;");
    exec("CREATE TABLE IF NOT EXISTS files("
         "path TEXT PRIMARY KEY, hash TEXT NOT NULL, size INTEGER, mtime INTEGER, state TEXT, "
         "error TEXT, parser TEXT);"
         "CREATE TABLE IF NOT EXISTS symbols("
         "path TEXT NOT NULL REFERENCES files(path) ON DELETE CASCADE, name TEXT, kind TEXT,"
         "qualified TEXT, signature TEXT, definition INTEGER, start INTEGER, end INTEGER,"
         "name_start INTEGER, name_end INTEGER, line INTEGER, category TEXT DEFAULT '',"
         "generated INTEGER DEFAULT 0, provider TEXT DEFAULT 'syntax');"
         "CREATE INDEX IF NOT EXISTS symbol_name ON symbols(name COLLATE NOCASE);"
         "CREATE INDEX IF NOT EXISTS symbol_order ON symbols(name,path,start);"
         "CREATE INDEX IF NOT EXISTS symbol_catalog ON symbols("
         "name,path,start,qualified,kind,category,definition,name_start,name_end,end);"
         "CREATE INDEX IF NOT EXISTS symbol_qualified ON symbols(qualified);"
         "CREATE INDEX IF NOT EXISTS symbol_categories ON symbols("
         "kind,category,definition,path,name,name_start,name_end,end) "
         "WHERE name IS NOT NULL AND name<>'' AND name_end>name_start AND end>start;"
         "CREATE INDEX IF NOT EXISTS symbol_file ON symbols(path);");
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

void ProjectIndexStore::validateSchema() {
    IndexStatement version(*this, "PRAGMA user_version");
    version.next();
    if (version.integer(0) != 2)
        throw std::runtime_error(
            "Unsupported workspace database version; recreate the generated index.");
    IndexStatement files(*this,
                         "SELECT path,hash,size,mtime,state,error,parser FROM files LIMIT 0");
    IndexStatement symbols(
        *this, "SELECT path,name,kind,qualified,signature,definition,start,end,name_start,name_end,"
               "line,category,generated,provider FROM symbols LIMIT 0");
    IndexStatement referenceFiles(
        *this, "SELECT path,hash,version,ascii_only FROM reference_occurrence_files LIMIT 0");
    IndexStatement occurrences(*this,
                               "SELECT path,identifier,folded FROM reference_occurrences LIMIT 0");
}

IndexStatement::IndexStatement(ProjectIndexStore &database, const char *sql)
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

IndexTransaction::IndexTransaction(ProjectIndexStore &database) : database_(database) {
    database_.exec("BEGIN IMMEDIATE;");
}

IndexTransaction::~IndexTransaction() {
    if (!committed_) {
        const int code = sqlite3_exec(database_.handle(), "ROLLBACK;", nullptr, nullptr, nullptr);
        if (code != SQLITE_OK)
            qCWarning(indexLog) << "index.rollback_failed" << "code=" << code
                                << "error=" << sqlite3_errmsg(database_.handle());
    }
}

void IndexTransaction::commit() {
    database_.exec("COMMIT;");
    committed_ = true;
}

ProjectIndexWriter::ProjectIndexWriter(ProjectIndexStore &database)
    : database_(database),
      previous_(database, "SELECT hash,parser,state,size,mtime FROM files WHERE path=?"),
      file_(database, "INSERT INTO files VALUES(?,?,?,?,?,?,?) ON CONFLICT(path) DO UPDATE SET "
                      "hash=excluded.hash,size=excluded.size,mtime=excluded.mtime,state=excluded."
                      "state,error=excluded.error,parser=excluded.parser"),
      remove_(database, "DELETE FROM symbols WHERE path=?"),
      metadata_(database, "UPDATE files SET size=?,mtime=? WHERE path=?"),
      insert_(database, "INSERT INTO symbols VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)") {
}

IndexedFileMetadata ProjectIndexWriter::metadata(const QString &path) {
    previous_.bind(1, path);
    IndexedFileMetadata result;
    result.found = previous_.next();
    if (result.found) {
        result.hash = previous_.text(0);
        result.parser = previous_.text(1);
        result.state = previous_.text(2);
        result.size = previous_.integer(3);
        result.mtime = previous_.integer(4);
    }
    previous_.reset();
    return result;
}

bool ProjectIndexWriter::reconcile(const QStringList &paths,
                                   const std::shared_ptr<std::atomic_bool> &cancel) {
    IndexTransaction transaction(database_);
    database_.exec("CREATE TEMP TABLE seen(path TEXT PRIMARY KEY);");
    IndexStatement seen(database_, "INSERT OR IGNORE INTO seen VALUES(?)");
    for (const auto &path : paths) {
        if (cancel && cancel->load())
            return false;
        seen.bind(1, path);
        seen.next();
        seen.reset();
    }
    if (cancel && cancel->load())
        return false;
    database_.exec("DELETE FROM files WHERE path NOT IN (SELECT path FROM seen);");
    const bool changed = sqlite3_changes(database_.handle()) > 0;
    database_.exec("DROP TABLE seen;");
    transaction.commit();
    return changed;
}

void ProjectIndexWriter::updateMetadata(const FileIndexRecord &file) {
    IndexTransaction transaction(database_);
    metadata_.bind(1, file.size);
    metadata_.bind(2, file.mtime);
    metadata_.bind(3, file.path);
    metadata_.next();
    metadata_.reset();
    transaction.commit();
}

bool ProjectIndexWriter::replace(const FileIndexRecord &file,
                                 const QVector<codecairn::SourceSymbol> &symbols,
                                 const std::shared_ptr<std::atomic_bool> &cancel) {
    if (cancel && cancel->load())
        return false;
    IndexTransaction transaction(database_);
    file_.bind(1, file.path);
    file_.bind(2, file.hash);
    file_.bind(3, file.size);
    file_.bind(4, file.mtime);
    file_.bind(5, file.state);
    file_.bind(6, file.error);
    file_.bind(7, ParserVersion);
    file_.next();
    file_.reset();
    remove_.bind(1, file.path);
    remove_.next();
    remove_.reset();
    for (const auto &symbol : symbols) {
        if (symbol.outlineOnly || symbol.local)
            continue;
        insert_.bind(1, file.path);
        insert_.bind(2, symbol.name);
        insert_.bind(3, symbol.kind);
        insert_.bind(4, symbol.qualifiedName);
        insert_.bind(5, symbol.signature);
        insert_.bind(6, symbol.definition ? 1 : 0);
        insert_.bind(7, symbol.start);
        insert_.bind(8, symbol.end);
        insert_.bind(9, symbol.nameStart);
        insert_.bind(10, symbol.nameEnd);
        insert_.bind(11, symbol.line);
        insert_.bind(12, symbol.category);
        insert_.bind(13, symbol.generated ? 1 : 0);
        insert_.bind(14, symbol.provider);
        insert_.next();
        insert_.reset();
    }
    transaction.commit();
    return true;
}

}

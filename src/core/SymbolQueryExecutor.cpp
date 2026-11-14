#include <codecairn/Indexing.h>
#include <codecairn/Diagnostics.h>
#include <codecairn/FileIO.h>
#include "ProjectIndexStore.h"
#include <codecairn/WorkspaceOptionsStore.h>

#include <QElapsedTimer>
#include <QFileInfo>
#include <QSet>
#include <re2/re2.h>
#include <sqlite3.h>
#include <algorithm>
#include <climits>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <tuple>

using namespace codecairn::detail;

namespace {
constexpr const char *ValidSymbol =
    "name IS NOT NULL AND name<>'' AND name_end>name_start AND end>start";

IndexedSymbol readSymbol(IndexStatement &rows) {
    IndexedSymbol entry;
    entry.path = rows.text(0);
    entry.hash = rows.text(1).toLatin1();
    auto &s = entry.symbol;
    s.name = rows.text(2);
    s.kind = rows.text(3);
    s.qualifiedName = rows.text(4);
    s.signature = rows.text(5);
    s.definition = rows.integer(6);
    s.start = static_cast<int>(rows.integer(7));
    s.end = static_cast<int>(rows.integer(8));
    s.nameStart = static_cast<int>(rows.integer(9));
    s.nameEnd = static_cast<int>(rows.integer(10));
    s.line = static_cast<int>(rows.integer(11));
    s.category = rows.text(12);
    s.generated = rows.integer(13);
    s.provider = rows.text(14);
    return entry;
}

bool ordered(const IndexedSymbol &a, const IndexedSymbol &b) {
    return std::tie(a.symbol.name, a.path, a.symbol.start) <
           std::tie(b.symbol.name, b.path, b.symbol.start);
}
}

SymbolPage queryWorkspaceSymbols(const QString &root, const SymbolQuery &query) {
    SymbolPage page;
    page.anchor = query.anchor;
    const auto cancelled = [&] {
        return query.cancel && query.cancel->load();
    };
    if (cancelled()) {
        page.cancelled = true;
        return page;
    }
    QElapsedTimer elapsed;
    elapsed.start();
    try {
        if (!root.isEmpty()) {
            const auto options = WorkspaceOptionsStore::loadOptions(root, query.projectFile);
            if (!options.error.isEmpty())
                throw std::runtime_error(options.error.toStdString());
        }
        QString text = query.text.trimmed();
        const bool functionsOnly = !query.exact && text.endsWith('(');
        if (functionsOnly)
            text.chop(1);
        std::unique_ptr<RE2> regex;
        if (!query.exact && text.startsWith('?')) {
            RE2::Options options;
            options.set_case_sensitive(false);
            options.set_log_errors(false);
            options.set_max_mem(8 * 1024 * 1024);
            regex = std::make_unique<RE2>(text.mid(1).toStdString(), options);
            if (!regex->ok())
                throw std::runtime_error("Invalid search expression: " + regex->error());
        }
        const auto fragments = text.split(' ', Qt::SkipEmptyParts);
        const bool asciiQuery = !regex && std::all_of(text.cbegin(), text.cend(),
                                                      [](QChar c) { return c.unicode() < 128; });
        const bool nativeFragments = asciiQuery && !query.exact && !fragments.isEmpty();
        auto nameMatches = [&](const QString &name, const QString &qualified) {
            const auto &display = qualified.isEmpty() ? name : qualified;
            if (query.exact)
                return name == text || display == text;
            if (regex)
                return RE2::PartialMatch(display.toStdString(), *regex);
            return std::all_of(fragments.cbegin(), fragments.cend(), [&](const auto &fragment) {
                return display.contains(fragment, Qt::CaseInsensitive);
            });
        };
        auto matches = [&](const SourceSymbol &symbol) {
            return (query.category.isEmpty() || query.category == symbolCategory(symbol)) &&
                   (!functionsOnly || symbol.kind == "Function") &&
                   nameMatches(symbol.name, symbol.qualifiedName);
        };
        auto sqlNameMatches = [&](sqlite3_value **values) {
            auto *value = sqlite3_value_bytes(values[1]) > 0 ? values[1] : values[0];
            const auto *bytes = sqlite3_value_text(value);
            const int size = sqlite3_value_bytes(value);
            if (nativeFragments && bytes &&
                std::all_of(bytes, bytes + size, [](unsigned char c) { return c < 128; }))
                return false;
            const auto display =
                bytes ? QString::fromUtf8(reinterpret_cast<const char *>(bytes), size) : QString{};
            return nameMatches(display, {});
        };
        QVector<IndexedSymbol> live;
        QSet<QString> overridden;
        for (const auto &overlay : query.overlays) {
            if (cancelled()) {
                page.cancelled = true;
                return page;
            }
            overridden.insert(workspacePath(overlay.path));
            const auto hash =
                overlay.hash.isEmpty() ? sourceContentHash(overlay.source) : overlay.hash;
            for (const auto &symbol : overlay.analysis.symbols)
                if (!symbol.outlineOnly && !symbol.local && !symbol.name.isEmpty() &&
                    symbol.nameEnd > symbol.nameStart && symbol.end > symbol.start)
                    live.append({overlay.path, hash, symbol, true});
        }
        std::sort(live.begin(), live.end(), ordered);
        const int offset = qMax(0, query.offset);
        const int limit = qMax(0, query.limit);
        const int categoryLimit =
            query.categoryLimit < 0 ? std::numeric_limits<int>::max() : query.categoryLimit;
        const auto database = workspaceDatabase(root, query.projectFile);
        std::unique_ptr<ProjectIndexStore> db;
        if (!root.isEmpty() && QFileInfo::exists(database)) {
            db = std::make_unique<ProjectIndexStore>(database, false);
            if (query.cancel)
                sqlite3_progress_handler(
                    db->handle(), 1000,
                    [](void *value) {
                        return static_cast<std::atomic_bool *>(value)->load() ? 1 : 0;
                    },
                    query.cancel.get());
            db->exec("BEGIN;");
            bool pending = false;
            if (query.allowPendingSchema) {
                IndexStatement version(*db, "PRAGMA user_version");
                version.next();
                if (version.integer(0) == 0) {
                    IndexStatement tables(*db, "SELECT 1 FROM sqlite_master LIMIT 1");
                    pending = !tables.next();
                }
            }
            if (pending)
                db.reset();
            else
                db->validateSchema();
        }
        QByteArray base = QByteArray(ValidSymbol);
        QByteArray match;
        if (db) {
            QByteArray catalog = "symbols";
            QByteArray categories = "symbols";
            {
                IndexStatement index(
                    *db,
                    "SELECT 1 FROM sqlite_master WHERE type='index' AND name='symbol_catalog'");
                if (index.next())
                    catalog += " INDEXED BY symbol_catalog";
            }
            {
                IndexStatement index(
                    *db,
                    "SELECT 1 FROM sqlite_master WHERE type='index' AND name='symbol_categories'");
                if (index.next())
                    categories += " INDEXED BY symbol_categories";
            }
            db->exec("CREATE TEMP TABLE overlay_paths(path TEXT PRIMARY KEY);");
            IndexStatement path(*db, "INSERT INTO overlay_paths VALUES(?)");
            for (const auto &value : overridden) {
                path.bind(1, value);
                path.next();
                path.reset();
            }
            if (!overridden.isEmpty())
                base += " AND path NOT IN (SELECT path FROM overlay_paths)";
            using Matcher = decltype(sqlNameMatches);
            const auto registered = sqlite3_create_function_v2(
                db->handle(), "cc_name_matches", 2, SQLITE_UTF8 | SQLITE_DETERMINISTIC,
                &sqlNameMatches,
                [](sqlite3_context *context, int, sqlite3_value **values) {
                    sqlite3_result_int(
                        context, (*static_cast<Matcher *>(sqlite3_user_data(context)))(values));
                },
                nullptr, nullptr, nullptr);
            if (registered != SQLITE_OK)
                throw std::runtime_error(sqlite3_errmsg(db->handle()));
            if (query.exact)
                match = " AND (name=? OR qualified=?)";
            else if (nativeFragments) {
                match = " AND ((";
                for (qsizetype i = 0; i < fragments.size(); ++i) {
                    if (i > 0)
                        match += " AND ";
                    match += "COALESCE(NULLIF(qualified,''),name) LIKE ? ESCAPE '\\'";
                }
                match += ") OR cc_name_matches(name,qualified))";
            } else if (!text.isEmpty())
                match = " AND cc_name_matches(name,qualified)";
            if (functionsOnly)
                match += " AND kind='Function'";
            if (!query.category.isEmpty()) {
                const auto categoryRegistered = sqlite3_create_function_v2(
                    db->handle(), "cc_category", 3, SQLITE_UTF8 | SQLITE_DETERMINISTIC, nullptr,
                    [](sqlite3_context *context, int, sqlite3_value **values) {
                        const auto value = [](sqlite3_value *v) {
                            const auto *bytes = sqlite3_value_text(v);
                            return bytes ? QString::fromUtf8(reinterpret_cast<const char *>(bytes))
                                         : QString{};
                        };
                        SourceSymbol symbol;
                        symbol.kind = value(values[0]);
                        symbol.category = value(values[1]);
                        symbol.definition = sqlite3_value_int(values[2]);
                        const auto bytes = symbolCategory(symbol).toUtf8();
                        sqlite3_result_text(context, bytes.constData(), bytes.size(),
                                            SQLITE_TRANSIENT);
                    },
                    nullptr, nullptr, nullptr);
                if (categoryRegistered != SQLITE_OK)
                    throw std::runtime_error(sqlite3_errmsg(db->handle()));
                match += " AND cc_category(kind,category,definition)=:category";
            }
            const auto bindMatch = [&](IndexStatement &statement) {
                int parameter = 1;
                if (query.exact) {
                    statement.bind(parameter++, text);
                    statement.bind(parameter++, text);
                } else if (nativeFragments)
                    for (auto fragment : fragments) {
                        fragment.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_");
                        statement.bind(parameter++, '%' + fragment + '%');
                    }
                if (!query.category.isEmpty())
                    statement.bind(parameter++, query.category);
                return parameter;
            };
            if (query.includeCategories) {
                IndexStatement counts(*db, ("SELECT kind,category,definition,count(*) FROM " +
                                            categories + " WHERE " + base +
                                            " GROUP BY kind,category,definition")
                                               .constData());
                while (counts.next()) {
                    SourceSymbol symbol;
                    symbol.kind = counts.text(0);
                    symbol.category = counts.text(1);
                    symbol.definition = counts.integer(2);
                    page.categories[symbolCategory(symbol)] += static_cast<int>(counts.integer(3));
                }
            }
            if (query.includeCounts) {
                if (match.isEmpty() && query.includeCategories) {
                    for (int count : page.categories)
                        page.total += count;
                } else {
                    IndexStatement count(*db, ("SELECT count(*) FROM " +
                                               (query.exact ? QByteArray("symbols") : catalog) +
                                               " WHERE " + base + match)
                                                  .constData());
                    bindMatch(count);
                    count.next();
                    page.total = static_cast<int>(count.integer(0));
                }
            }
            const QByteArray columns =
                "SELECT s.path,f.hash,s.name,s.kind,s.qualified,s.signature,s.definition,"
                "s.start,s.end,s.name_start,s.name_end,s.line,s.category,s.generated,s.provider "
                "FROM (SELECT rowid AS symbol_row FROM ";
            const auto fetch = [&](const QByteArray &filter, int maximum, int skip,
                                   QVector<IndexedSymbol> &output) {
                if (maximum == 0)
                    return;
                const auto table =
                    query.exact && !filter.isEmpty() ? QByteArray("symbols") : catalog;
                const auto sql = columns + table + " WHERE " + base + filter +
                                 " ORDER BY name,path,start LIMIT " + QByteArray::number(maximum) +
                                 " OFFSET " + QByteArray::number(skip) +
                                 ") selected JOIN symbols s ON s.rowid=selected.symbol_row "
                                 "JOIN files f ON f.path=s.path ORDER BY s.name,s.path,s.start";
                IndexStatement rows(*db, sql.constData());
                if (!filter.isEmpty())
                    bindMatch(rows);
                while (rows.next()) {
                    if (cancelled())
                        break;
                    output.append(readSymbol(rows));
                }
            };
            const qint64 needed = static_cast<qint64>(offset) + limit;
            const int maximum =
                live.isEmpty() ? limit : static_cast<int>(qMin<qint64>(needed, INT_MAX));
            const int diskMaximum =
                query.includeCounts
                    ? qMin(maximum, qMax(0, page.total - (live.isEmpty() ? offset : 0)))
                    : maximum;
            fetch(match, diskMaximum, live.isEmpty() ? offset : 0, page.symbols);
            if (query.includeCategorySymbols)
                fetch({}, categoryLimit, 0, page.categorySymbols);
            if (query.anchor) {
                const auto &anchor = *query.anchor;
                const auto same = [&](const IndexedSymbol &entry) {
                    return entry.path == anchor.path &&
                           entry.symbol.nameStart == anchor.symbol.nameStart &&
                           entry.symbol.name == anchor.symbol.name;
                };
                const auto liveAnchor = std::find_if(live.cbegin(), live.cend(), same);
                const bool isLive = liveAnchor != live.cend() && matches(liveAnchor->symbol);
                bool exists = isLive;
                if (!isLive) {
                    IndexStatement find(*db, ("SELECT 1 FROM symbols WHERE " + base + match +
                                              " AND path=? AND name_start=? AND name=? LIMIT 1")
                                                 .constData());
                    int parameter = bindMatch(find);
                    find.bind(parameter++, anchor.path);
                    find.bind(parameter++, anchor.symbol.nameStart);
                    find.bind(parameter, anchor.symbol.name);
                    exists = find.next();
                }
                if (exists) {
                    const QByteArray before =
                        isLive ? " AND name<?"
                               : " AND (name<? OR (name=? AND (path<? OR (path=? AND start<?))))";
                    IndexStatement rank(
                        *db, ("SELECT count(*) FROM symbols WHERE " + base + match + before)
                                 .constData());
                    int parameter = bindMatch(rank);
                    rank.bind(parameter++, anchor.symbol.name);
                    if (!isLive) {
                        rank.bind(parameter++, anchor.symbol.name);
                        rank.bind(parameter++, anchor.path);
                        rank.bind(parameter++, anchor.path);
                        rank.bind(parameter, anchor.symbol.start);
                    }
                    rank.next();
                    page.anchorRow = static_cast<int>(rank.integer(0));
                    for (auto it = live.cbegin(); it != live.cend(); ++it) {
                        if (isLive && it == liveAnchor)
                            break;
                        if (matches(it->symbol) && it->symbol.name <= anchor.symbol.name)
                            ++page.anchorRow;
                    }
                }
            }
        }
        QVector<IndexedSymbol> matchingLive;
        for (const auto &entry : live) {
            if (query.includeCategories)
                ++page.categories[symbolCategory(entry.symbol)];
            if (matches(entry.symbol)) {
                if (query.includeCounts)
                    ++page.total;
                matchingLive.append(entry);
            }
        }
        if (!db && query.anchor)
            for (qsizetype i = 0; i < matchingLive.size(); ++i) {
                const auto &entry = matchingLive[i];
                if (entry.path == query.anchor->path &&
                    entry.symbol.nameStart == query.anchor->symbol.nameStart &&
                    entry.symbol.name == query.anchor->symbol.name)
                    page.anchorRow = static_cast<int>(i);
            }
        const auto merge = [](QVector<IndexedSymbol> &disk,
                              const QVector<IndexedSymbol> &overlays) {
            QVector<IndexedSymbol> result;
            result.reserve(disk.size() + overlays.size());
            std::merge(disk.cbegin(), disk.cend(), overlays.cbegin(), overlays.cend(),
                       std::back_inserter(result), [](const auto &a, const auto &b) {
                           return a.symbol.name < b.symbol.name ||
                                  (a.symbol.name == b.symbol.name && a.live && !b.live);
                       });
            disk = std::move(result);
        };
        if (!live.isEmpty()) {
            merge(page.symbols, matchingLive);
            page.symbols = page.symbols.mid(offset, limit);
            if (query.includeCategorySymbols) {
                merge(page.categorySymbols, live);
                if (page.categorySymbols.size() > categoryLimit)
                    page.categorySymbols.resize(categoryLimit);
            }
        }
        if (cancelled()) {
            page = {};
            page.cancelled = true;
        }
    } catch (const std::runtime_error &error) {
        page = {};
        page.cancelled = cancelled();
        if (!page.cancelled) {
            page.error = QString::fromUtf8(error.what());
            qCWarning(indexLog) << "index.query_failed" << "root=" << root
                                << "offset=" << query.offset << "limit=" << query.limit
                                << "error=" << page.error;
        }
    }
    qCDebug(indexLog) << "index.query_completed" << "root=" << root
                      << "elapsedMs=" << elapsed.elapsed() << "total=" << page.total
                      << "returned=" << page.symbols.size() << "cancelled=" << page.cancelled;
    return page;
}

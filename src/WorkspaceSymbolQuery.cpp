#include "WorkspaceIndex.h"
#include "Diagnostics.h"
#include "SymbolIndexStore.h"
#include "WorkspaceOptionsStore.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QSet>
#include <re2/re2.h>
#include <sqlite3.h>
#include <algorithm>
#include <stdexcept>
#include <tuple>
#include <utility>

using namespace WorkspaceIndexDetail;

SymbolPage queryWorkspaceSymbols(const QString &root, const SymbolQuery &query) {
    SymbolPage page;
    const auto cancelled = [&] {
        return query.cancel && query.cancel->load();
    };
    if (cancelled()) {
        page.cancelled = true;
        return page;
    }
    try {
        if (!root.isEmpty()) {
            const auto options = WorkspaceOptionsStore::loadOptions(root);
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
        QVector<IndexedSymbol> live;
        QSet<QString> overridden;
        for (const auto &overlay : query.overlays) {
            if (cancelled()) {
                page.cancelled = true;
                return page;
            }
            overridden.insert(workspacePath(overlay.path));
            const auto hash =
                overlay.hash.isEmpty()
                    ? QCryptographicHash::hash(overlay.source, QCryptographicHash::Sha256).toHex()
                    : overlay.hash;
            for (const auto &symbol : overlay.analysis.symbols)
                if (!symbol.outlineOnly && !symbol.local)
                    live.append({overlay.path, hash, symbol, true});
        }
        auto consume = [&](IndexedSymbol symbol) {
            const auto category = symbolCategory(symbol.symbol);
            ++page.categories[category];
            if (query.includeCategorySymbols)
                page.categorySymbols.append(symbol);
            if (!query.category.isEmpty() && query.category != category)
                return;
            if (functionsOnly && symbol.symbol.kind != "Function")
                return;
            const auto &name = symbol.symbol.qualifiedName.isEmpty() ? symbol.symbol.name
                                                                     : symbol.symbol.qualifiedName;
            if (query.exact) {
                if (symbol.symbol.name != text && name != text)
                    return;
            } else if (regex) {
                if (!RE2::PartialMatch(name.toStdString(), *regex))
                    return;
            } else {
                for (const auto &fragment : fragments)
                    if (!name.contains(fragment, Qt::CaseInsensitive))
                        return;
            }
            const int position = page.total++;
            if (position >= query.offset && page.symbols.size() < query.limit)
                page.symbols.append(std::move(symbol));
        };
        std::sort(live.begin(), live.end(), [](const auto &a, const auto &b) {
            return std::tie(a.symbol.name, a.path, a.symbol.start) <
                   std::tie(b.symbol.name, b.path, b.symbol.start);
        });
        int liveIndex = 0;
        const auto database = workspaceDatabase(root);
        if (!root.isEmpty() && QFileInfo::exists(database)) {
            SymbolIndexStore db(database, false);
            if (query.cancel)
                sqlite3_progress_handler(
                    db.handle(), 1000,
                    [](void *value) {
                        return static_cast<std::atomic_bool *>(value)->load() ? 1 : 0;
                    },
                    query.cancel.get());
            IndexStatement version(db, "PRAGMA user_version");
            version.next();
            const bool rich = version.integer(0) >= 2;
            const QByteArray sql =
                QByteArray(
                    "SELECT s.path,f.hash,s.name,s.kind,s.qualified,s.signature,s.definition,"
                    "s.start,s.end,s.name_start,s.name_end,s.line,") +
                (rich ? "s.category,s.generated,s.provider" : "'',0,'syntax'") +
                " FROM symbols s JOIN files f ON f.path=s.path ORDER BY s.name,s.path,s.start";
            IndexStatement rows(db, sql.constData());
            while (rows.next()) {
                if (cancelled()) {
                    page = {};
                    page.cancelled = true;
                    return page;
                }
                if (overridden.contains(rows.text(0)))
                    continue;
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
                while (liveIndex < live.size() && live[liveIndex].symbol.name <= s.name)
                    consume(live[liveIndex++]);
                consume(entry);
            }
        }
        while (liveIndex < live.size())
            consume(live[liveIndex++]);
    } catch (const std::runtime_error &error) {
        page.cancelled = cancelled();
        if (!page.cancelled) {
            page.error = QString::fromUtf8(error.what());
            qCWarning(indexLog) << "index.query_failed" << "root=" << root
                                << "offset=" << query.offset << "limit=" << query.limit
                                << "error=" << page.error;
        }
        page.symbols.clear();
        page.categorySymbols.clear();
        page.categories.clear();
        page.total = 0;
    }
    return page;
}

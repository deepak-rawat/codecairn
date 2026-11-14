#pragma once

#include <codecairn/SourceAnalysis.h>
#include <codecairn/RuntimeLimits.h>
#include <QHash>
#include <QMap>
#include <atomic>
#include <memory>
#include <functional>
#include <optional>

struct IndexedSymbol {
    QString path;
    QByteArray hash;
    SourceSymbol symbol;
    bool live = false;
};

enum class IndexPhase { Discovery, Symbols, References };

struct IndexProgress {
    std::atomic_int discovered{0};
    std::atomic_int parsed{0};
    std::atomic_int committed{0};
    std::atomic_int activeParsers{0};
    std::atomic_int peakParsers{0};
    // Called on the coordinator thread, never a parser worker.
    std::function<void(const QStringList &)> catalogReady;
    std::function<void()> batchReady;
    std::atomic<IndexPhase> phase{IndexPhase::Discovery};
};

struct IndexResult {
    QString root;
    QStringList files;
    QStringList directories;
    QStringList warnings;
    QHash<QString, QByteArray> sourceHashes;
    QHash<QString, QByteArray> directorySnapshots;
    QString error;
    int parsed = 0;
    int reused = 0;
    bool cancelled = false;
    bool changed = false;
    bool automatic = false;
    bool skipped = false;
    qint64 discoveryMilliseconds = 0;
    qint64 symbolMilliseconds = 0;
    qint64 referenceMilliseconds = 0;
    int suppressedWarnings = 0;
};

struct SymbolOverlay {
    QString path;
    QByteArray source;
    SourceAnalysis analysis;
    QByteArray hash{};
};

struct SymbolQuery {
    QString text;
    QString category;
    QVector<SymbolOverlay> overlays;
    int offset = 0;
    int limit = RuntimeLimits::SymbolPageSize;
    bool exact = false;
    std::shared_ptr<std::atomic_bool> cancel;
    bool includeCategorySymbols = false;
    QString projectFile;
    bool allowPendingSchema = false;
    bool includeCounts = true;
    bool includeCategories = true;
    int categoryLimit = -1;
    std::optional<IndexedSymbol> anchor;
};

struct SymbolPage {
    QVector<IndexedSymbol> symbols;
    QMap<QString, int> categories;
    QString error;
    int total = 0;
    bool cancelled = false;
    QVector<IndexedSymbol> categorySymbols;
    int anchorRow = -1;
    std::optional<IndexedSymbol> anchor;
};

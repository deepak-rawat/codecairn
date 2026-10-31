#pragma once

#include "SourceAnalysis.h"
#include "RuntimeLimits.h"
#include <QHash>
#include <QMap>
#include <atomic>
#include <memory>

struct IndexedSymbol {
    QString path;
    QByteArray hash;
    SourceSymbol symbol;
    bool live = false;
};

struct IndexProgress {
    std::atomic_int discovered{0};
    std::atomic_int parsed{0};
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
};

struct SymbolPage {
    QVector<IndexedSymbol> symbols;
    QMap<QString, int> categories;
    QString error;
    int total = 0;
    bool cancelled = false;
    QVector<IndexedSymbol> categorySymbols;
};

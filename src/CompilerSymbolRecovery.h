#pragma once
#include "SourceTypes.h"
#include "IndexTypes.h"

void recoverCompilerSymbols(SourceAnalysis &analysis, const QByteArray &source,
                            const QString &path);

struct InactiveCodeRanges {
    QVector<QPair<int, int>> ranges;
    QString error;
};
InactiveCodeRanges compilerInactiveRanges(const QString &path, const QByteArray &source,
                                         const QVector<SymbolOverlay> &overlays = {});

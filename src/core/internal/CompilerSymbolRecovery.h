#pragma once
#include <codecairn/SourceTypes.h>
#include <codecairn/IndexTypes.h>

void recoverCompilerSymbols(codecairn::SourceAnalysis &analysis, const QByteArray &source,
                            const QString &path, const QString &compilationDirectory = {});

struct InactiveCodeRanges {
    QVector<QPair<int, int>> ranges;
    QString error;
};
InactiveCodeRanges compilerInactiveRanges(const QString &path, const QByteArray &source,
                                          const QVector<codecairn::SymbolOverlay> &overlays = {},
                                          const QString &compilationDirectory = {});

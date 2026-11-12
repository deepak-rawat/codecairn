#pragma once

#include <codecairn/SourceTypes.h>

enum class SourceAnalysisMode { Editor, Index, SyntaxOnly };

SourceAnalysis analyzeSource(const QByteArray &source, const QString &fileName,
                             SourceAnalysisMode mode = SourceAnalysisMode::Editor,
                             const QString &compilationDirectory = {});
QString symbolCategory(const SourceSymbol &symbol);
QStringList symbolCategoryOrder();

#pragma once

#include <codecairn/SourceTypes.h>

enum class SourceAnalysisMode { Editor, Index, SyntaxOnly };

codecairn::SourceAnalysis analyzeSource(const QByteArray &source, const QString &fileName,
                                        SourceAnalysisMode mode = SourceAnalysisMode::Editor,
                                        const QString &compilationDirectory = {});
QString symbolCategory(const codecairn::SourceSymbol &symbol);
QStringList symbolCategoryOrder();

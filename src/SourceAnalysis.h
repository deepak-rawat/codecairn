#pragma once

#include "SourceTypes.h"

SourceAnalysis analyzeSource(const QByteArray &source, const QString &fileName);
QString symbolCategory(const SourceSymbol &symbol);
QStringList symbolCategoryOrder();

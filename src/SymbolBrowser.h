#pragma once

#include "IndexTypes.h"
#include <Qt>

class QTreeWidget;

namespace SymbolItemRole {
constexpr int Index = Qt::UserRole;
constexpr int Path = Qt::UserRole + 1;
constexpr int Key = Qt::UserRole + 2;
constexpr int Kind = Qt::UserRole + 3;
constexpr int Line = Qt::UserRole + 4;
}

namespace SymbolBrowser {
void display(QTreeWidget *symbols, QTreeWidget *categories, const SymbolPage &page,
             const QString &workspaceRoot, const QString &selectedCategory);
}

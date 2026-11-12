#include "SymbolBrowser.h"
#include "SourceAnalysis.h"
#include "UiAppearance.h"
#include <QDir>
#include <QFileInfo>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QTreeWidget>

namespace SymbolBrowser {
void display(QTreeWidget *symbols, QTreeWidget *categoryTree, const SymbolPage &page,
             const QString &workspaceRoot, const QString &selectedCategory) {
    const QSignalBlocker symbolsBlock(symbols), categoriesBlock(categoryTree);
    QHash<QString, QIcon> icons;
    const auto iconForKind = [&icons](const QString &kind) {
        auto found = icons.constFind(kind);
        if (found == icons.cend())
            found = icons.insert(kind, UiAppearance::icon(kind));
        return *found;
    };
    QString selectedKey;
    QString selectedCategoryKey;
    QSet<QString> expandedCategories;
    const int symbolScroll = symbols->verticalScrollBar()->value();
    const int categoryScroll = categoryTree->verticalScrollBar()->value();
    if (const auto *selected = symbols->currentItem())
        selectedKey = selected->data(0, SymbolItemRole::Key).toString();
    if (const auto *selected = categoryTree->currentItem())
        selectedCategoryKey = selected->data(0, SymbolItemRole::Key).toString();
    for (int i = 0; i < categoryTree->topLevelItemCount(); ++i) {
        const auto *item = categoryTree->topLevelItem(i);
        if (item->isExpanded())
            expandedCategories.insert(item->data(0, SymbolItemRole::Index).toString());
    }
    symbols->clear();
    for (int i = 0; i < page.symbols.size(); ++i) {
        const auto &entry = page.symbols[i];
        const auto &symbol = entry.symbol;
        const auto path =
            workspaceRoot.isEmpty() ? entry.path : QDir(workspaceRoot).relativeFilePath(entry.path);
        auto *item = new QTreeWidgetItem(
            symbols, {symbol.qualifiedName.isEmpty() ? symbol.name : symbol.qualifiedName, path});
        item->setIcon(0, iconForKind(symbol.kind));
        item->setData(0, SymbolItemRole::Index, i);
        item->setData(0, SymbolItemRole::Path, entry.path);
        const auto key = entry.path + ':' + QString::number(symbol.nameStart) + ':' + symbol.name;
        item->setData(0, SymbolItemRole::Key, key);
        item->setData(0, SymbolItemRole::Kind, symbol.kind);
        item->setToolTip(
            0,
            QString("%1 - %2\n%3:%4\n%5%6")
                    .arg(symbol.kind, symbol.definition ? "definition" : "declaration", entry.path)
                    .arg(symbol.line + 1)
                    .arg(symbol.signature, entry.live ? "\nUnsaved/open buffer" : "") +
                (symbol.generated
                     ? "\nCompiler-expanded definition; location is the macro invocation."
                     : ""));
        if (key == selectedKey)
            symbols->setCurrentItem(item);
    }
    categoryTree->clear();
    auto categories = symbolCategoryOrder();
    for (auto it = page.categories.cbegin(); it != page.categories.cend(); ++it)
        if (!categories.contains(it.key()))
            categories.append(it.key());
    QHash<QString, QTreeWidgetItem *> categoryItems;
    for (const auto &category : categories) {
        const int count = page.categories.value(category);
        if (count == 0)
            continue;
        auto *item = new QTreeWidgetItem(categoryTree,
                                         {QString("%1 (%2)").arg(category).arg(count), QString{}});
        item->setData(0, SymbolItemRole::Index, category);
        item->setData(0, SymbolItemRole::Key, category);
        item->setIcon(0, iconForKind(category));
        item->setExpanded(expandedCategories.contains(category));
        categoryItems.insert(category, item);
        if (category == selectedCategoryKey ||
            (selectedCategoryKey.isEmpty() && category == selectedCategory))
            categoryTree->setCurrentItem(item);
    }
    for (int i = 0; i < page.categorySymbols.size(); ++i) {
        const auto &entry = page.categorySymbols[i];
        const auto &symbol = entry.symbol;
        const auto path =
            workspaceRoot.isEmpty() ? entry.path : QDir(workspaceRoot).relativeFilePath(entry.path);
        auto *item = new QTreeWidgetItem(
            categoryItems.value(symbolCategory(symbol)),
            {symbol.qualifiedName.isEmpty() ? symbol.name : symbol.qualifiedName,
             QFileInfo(entry.path).fileName()});
        const auto key = entry.path + ':' + QString::number(symbol.nameStart) + ':' + symbol.name;
        item->setData(0, SymbolItemRole::Index, i);
        item->setData(0, SymbolItemRole::Path, entry.path);
        item->setData(0, SymbolItemRole::Key, key);
        item->setIcon(0, iconForKind(symbolCategory(symbol)));
        item->setToolTip(0, QString("%1 - %2\n%3:%4\n%5")
                                .arg(symbolCategory(symbol),
                                     symbol.definition ? "definition" : "declaration", path)
                                .arg(symbol.line + 1)
                                .arg(symbol.signature));
        item->setToolTip(1, path);
        if (key == selectedCategoryKey)
            categoryTree->setCurrentItem(item);
    }
    symbols->doItemsLayout();
    categoryTree->doItemsLayout();
    symbols->verticalScrollBar()->setValue(symbolScroll);
    categoryTree->verticalScrollBar()->setValue(categoryScroll);
}
}

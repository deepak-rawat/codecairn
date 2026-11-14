#pragma once

#include <codecairn/IndexTypes.h>

namespace codecairn {

using Symbol = codecairn::SourceSymbol;

struct SymbolQuery {
    QString text;
    QString category;
    int offset = 0;
    int pageSize = RuntimeLimits::SymbolPageSize;
    bool exact = false;
    bool includeCategories = true;
    bool includeCategorySymbols = false;
    bool includeCounts = true;
    int categoryPageSize = -1;
    std::optional<codecairn::IndexedSymbol> anchor;
};

}

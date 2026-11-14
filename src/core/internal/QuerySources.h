#pragma once

#include <codecairn/IndexTypes.h>

namespace codecairn::detail {

struct QuerySources {
    QVector<codecairn::SymbolOverlay> overlays;
    QHash<QString, int> versions;
};

}

#pragma once

#include <codecairn/Document.h>
#include <codecairn/IndexTypes.h>
#include <codecairn/NavigationTarget.h>
#include <codecairn/Result.h>

namespace codecairn {

enum class DefinitionResolution { SyntaxOnly, SemanticOnly, SemanticPreferred };
enum class DefinitionOrigin { Include, ExcludeUnresolvedSelf };

struct DefinitionQuery {
    SourcePosition at;
    DefinitionResolution resolution = DefinitionResolution::SemanticPreferred;
    DefinitionOrigin origin = DefinitionOrigin::Include;
    int offset = 0;
};

struct Definition {
    SourceLocation location;
    std::optional<codecairn::IndexedSymbol> symbol;
};

struct DefinitionPage {
    QVector<Definition> definitions;
    int total = 0;
    bool complete = true;
    bool semantic = false;
    QString provider;
    QString warning;
    QString error;
};

struct SourceRead {
    SourceLocation location;
    FileContents file;
    ResolvedNavigation range;
};

}

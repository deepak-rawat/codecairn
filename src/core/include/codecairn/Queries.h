#pragma once

#include <codecairn/Definitions.h>
#include <codecairn/SearchOptions.h>
#include <codecairn/ReferenceSearch.h>

namespace codecairn {

enum class SearchScope { ProjectFiles, CurrentFile, SelectedFiles };

struct SearchFiles {
    SearchScope scope = SearchScope::ProjectFiles;
    QString currentFile;
    QStringList selectedFiles;
    QString patterns;
};

enum class ReferenceResolution { Semantic, IdentifierOccurrences };

struct ReferenceQuery {
    SourcePosition symbol;
    SearchFiles files;
    ReferenceResolution resolution = ReferenceResolution::Semantic;
    bool includeDeclaration = true;
    bool skipInactive = false;
    CommentSearch comments = CommentSearch::Exclude;
    bool strings = false;
    int offset = 0;
    int pageSize = 10000;
    SearchOutputOptions output;
};

enum class TextMatch { Literal, RegularExpression, KeywordExpression, WholeIdentifier };

struct TextQuery {
    QString text;
    SearchFiles files;
    TextMatch method = TextMatch::Literal;
    bool caseSensitive = true;
    bool wholeWords = false;
    CommentSearch comments = CommentSearch::Exclude;
    bool strings = false;
    bool skipInactive = false;
    int contextLines = 3;
    bool variations = false;
    bool fragments = false;
    int offset = 0;
    int pageSize = 10000;
    SearchOutputOptions output;
};

class Project;
namespace detail {
class SearchExecutor;
}
class SearchCursor {
public:
    explicit operator bool() const { return search_ != 0; }
    bool operator==(const SearchCursor &) const = default;

private:
    friend class detail::SearchExecutor;
    SearchCursor(quint64 search, int offset, bool references)
        : search_(search), offset_(offset), references_(references) {}
    quint64 search_ = 0;
    int offset_ = 0;
    bool references_ = false;

public:
    SearchCursor() = default;
};

struct SearchPage {
    QVector<ReferenceHit> hits;
    QStringList warnings;
    QString error;
    bool cancelled = false;
    int filesSearched = 0;
    SearchCursor next;
};

}

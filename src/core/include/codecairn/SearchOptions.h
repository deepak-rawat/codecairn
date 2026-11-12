#pragma once

#include <QString>

enum class SearchMethod { References, Occurrences, Literal, RegularExpression, KeywordExpression };
enum class CommentSearch { Exclude, Include, Only };

struct SearchOutputOptions {
    bool container = true;
    bool file = true;
    bool line = true;
    bool text = true;
    bool links = true;
    bool relativePaths = true;
};

struct SearchOptions {
    SearchMethod method = SearchMethod::References;
    bool caseSensitive = true;
    bool wholeWords = true;
    CommentSearch comments = CommentSearch::Exclude;
    bool strings = false;
    bool skipInactive = false;
    bool smartMatching = true;
    int contextLines = 3;
    bool variations = false;
    bool fragments = false;
    QString filePatterns;
    SearchOutputOptions output;
};

#include "ReferenceSearch.h"
#include "Diagnostics.h"
#include "ReferenceCandidateIndex.h"
#include "CompilerSymbolRecovery.h"
#include "FileIO.h"
#include "SourceAnalysis.h"
#include "WorkspacePaths.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <re2/re2.h>
#include <algorithm>
#include <memory>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
struct Match {
    int start;
    int end;
    int line;
};

bool word(char32_t c) {
    return c == '_' || c == '$' || QChar::isLetterOrNumber(c);
}

bool whole(const QByteArray &source, int start, int end) {
    if (start > 0) {
        int previous = start - 1;
        while (previous > 0 && (static_cast<unsigned char>(source[previous]) & 0xc0) == 0x80)
            --previous;
        const auto scalars = QString::fromUtf8(source.mid(previous, start - previous)).toUcs4();
        if (!scalars.isEmpty() && word(scalars.first()))
            return false;
    }
    if (end < source.size()) {
        const auto scalars = QString::fromUtf8(source.mid(end, 4)).toUcs4();
        if (!scalars.isEmpty() && word(scalars.first()))
            return false;
    }
    return true;
}

std::unique_ptr<RE2> expression(const QString &text, bool regex, bool sensitive,
                              bool variations = false) {
    std::string pattern;
    if (regex)
        pattern = "(?m:" + text.toStdString() + ')';
    else {
        QStringList forms{text};
        if (variations) {
            forms << text + 's' << text + "es" << text + "ed" << text + "ing";
            if (text.endsWith('e') && text.size() > 1)
                forms << text + 'd' << text.left(text.size() - 1) + "ing";
        }
        std::stable_sort(forms.begin(), forms.end(), [](const auto &a, const auto &b) {
            return a.size() > b.size();
        });
        for (const auto &form : forms) {
            if (!pattern.empty())
                pattern += '|';
            pattern += RE2::QuoteMeta(form.toStdString());
        }
        pattern = "(?:" + pattern + ')';
    }
    RE2::Options options;
    options.set_case_sensitive(sensitive);
    options.set_log_errors(false);
    options.set_max_mem(8 * 1024 * 1024);
    auto compiled = std::make_unique<RE2>(pattern, options);
    if (!compiled->ok())
        throw std::invalid_argument("Invalid search expression: " + compiled->error());
    return compiled;
}

struct Term {
    std::unique_ptr<RE2> regex;
    bool wholeWord;
};
struct Node {
    enum Kind { Leaf, And, Or, Not } kind = Leaf;
    int term = -1;
    std::unique_ptr<Node> left, right;
};

class Keywords {
public:
    std::vector<Term> terms;
    std::unique_ptr<Node> root;
    explicit Keywords(const QString &query, const SearchOptions &options)
        : query_(query), options_(options) {
        next();
        root = parseOr();
        if (token_ != End)
            fail("Unexpected keyword-expression token.");
    }

private:
    enum Token { End, Text, Left, Right, And, Or, Not, Sensitive, Regex };
    QString query_, text_;
    SearchOptions options_;
    int position_ = 0;
    Token token_ = End;
    int nodes_ = 0;
    [[noreturn]] void fail(const char *message) { throw std::invalid_argument(message); }
    void next() {
        while (position_ < query_.size() && query_[position_].isSpace())
            ++position_;
        if (position_ == query_.size()) {
            token_ = End;
            return;
        }
        const auto c = query_[position_++];
        if (c == '(') { token_ = Left; return; }
        if (c == ')') { token_ = Right; return; }
        if (c == '+') { token_ = And; return; }
        if (c == '-' || c == '!') { token_ = Not; return; }
        if (c == '=') { token_ = Sensitive; return; }
        if (c == '?') { token_ = Regex; return; }
        text_.clear();
        if (c == '"') {
            bool closed = false;
            while (position_ < query_.size()) {
                const auto ch = query_[position_++];
                if (ch == '"') { closed = true; break; }
                if (ch == '\\' && position_ < query_.size() && query_[position_] == '"')
                    text_ += query_[position_++];
                else
                    text_ += ch;
            }
            if (!closed)
                fail("Unterminated quoted keyword.");
            token_ = Text;
        } else {
            text_ += c;
            while (position_ < query_.size() && !query_[position_].isSpace() &&
                   !QString("()+!=\"").contains(query_[position_]))
                text_ += query_[position_++];
            const auto upper = text_.toUpper();
            token_ = upper == "AND" ? And : upper == "OR" ? Or : upper == "NOT" ? Not : Text;
        }
    }
    std::unique_ptr<Node> node(Node::Kind kind) {
        if (++nodes_ > 128)
            fail("Keyword expressions are limited to 128 terms/operators.");
        auto result = std::make_unique<Node>();
        result->kind = kind;
        return result;
    }
    std::unique_ptr<Node> parseOr() {
        auto left = parseAnd();
        while (token_ == Or) {
            next();
            auto parent = node(Node::Or);
            parent->left = std::move(left);
            parent->right = parseAnd();
            left = std::move(parent);
        }
        return left;
    }
    std::unique_ptr<Node> parseAnd() {
        auto left = parseUnary();
        while (token_ != End && token_ != Right && token_ != Or) {
            if (token_ == And)
                next();
            auto parent = node(Node::And);
            parent->left = std::move(left);
            parent->right = parseUnary();
            left = std::move(parent);
        }
        return left;
    }
    std::unique_ptr<Node> parseUnary() {
        if (token_ == Not) {
            next();
            auto result = node(Node::Not);
            result->left = parseUnary();
            return result;
        }
        if (token_ == Left) {
            auto guard = node(Node::And);
            next();
            auto result = parseOr();
            if (token_ != Right)
                fail("Missing closing parenthesis.");
            next();
            return result;
        }
        bool sensitive = options_.caseSensitive, regex = false;
        while (token_ == Sensitive || token_ == Regex) {
            sensitive = sensitive || token_ == Sensitive;
            regex = regex || token_ == Regex;
            next();
        }
        if (token_ != Text || text_.isEmpty())
            fail("Expected a keyword, quoted phrase, or parenthesized expression.");
        auto result = node(Node::Leaf);
        result->term = static_cast<int>(terms.size());
        terms.push_back({expression(text_, regex, sensitive, options_.variations && !regex),
                         !regex && !options_.fragments});
        next();
        return result;
    }
};

bool evaluate(const Node &node, const std::vector<QVector<Match>> &matches, int first, int last) {
    if (node.kind == Node::Not)
        return !evaluate(*node.left, matches, first, last);
    if (node.kind == Node::And)
        return evaluate(*node.left, matches, first, last) &&
               evaluate(*node.right, matches, first, last);
    if (node.kind == Node::Or)
        return evaluate(*node.left, matches, first, last) ||
               evaluate(*node.right, matches, first, last);
    const auto &hits = matches[node.term];
    const auto found = std::lower_bound(hits.cbegin(), hits.cend(), first,
                                       [](const Match &m, int line) { return m.line < line; });
    return found != hits.cend() && found->line <= last;
}

QString container(const SourceAnalysis &analysis, int start, int end) {
    const SourceSymbol *best = nullptr;
    for (const auto &symbol : analysis.symbols)
        if (!symbol.outlineOnly && symbol.definition && symbol.start <= start && symbol.end >= end &&
            (symbol.kind == "Function" || symbol.kind == "Type" || symbol.kind == "Enum" ||
             symbol.kind == "Namespace") &&
            (!best || symbol.end - symbol.start < best->end - best->start))
            best = &symbol;
    return best ? (best->qualifiedName.isEmpty() ? best->name : best->qualifiedName) : QString{};
}

bool allowed(const SourceAnalysis &analysis, const SearchOptions &options,
             const QVector<QPair<int, int>> &inactive, int start, int end) {
    // A multi-byte or multi-line match must satisfy filters over its entire range.
    for (int p = start; p < qMax(start + 1, end); ++p) {
        const auto style = p < analysis.styles.size()
                               ? static_cast<TextStyle>(analysis.styles[p]) : TextStyle::Plain;
        if ((options.comments == CommentSearch::Exclude && style == TextStyle::Comment) ||
            (options.comments == CommentSearch::Only && style != TextStyle::Comment) ||
            (!options.strings && style == TextStyle::String))
            return false;
    }
    for (const auto &range : inactive)
        if (start < range.second && end > range.first)
            return false;
    return true;
}

QVector<Match> matches(const RE2 &regex, const QByteArray &source, const SourceAnalysis &analysis,
                       const SearchOptions &options, bool words,
                       const QVector<QPair<int, int>> &inactive,
                       const std::shared_ptr<std::atomic_bool> &cancel,
                       int maximum, bool onePerLine = false) {
    QVector<Match> result;
    const re2::StringPiece input(source.constData(), source.size());
    re2::StringPiece found;
    size_t offset = 0;
    int line = 0, lineOffset = 0;
    while (!(cancel && cancel->load()) &&
           regex.Match(input, offset, input.size(), RE2::UNANCHORED, &found, 1)) {
        const int start = static_cast<int>(found.data() - source.constData());
        const int end = start + static_cast<int>(found.size());
        while (lineOffset < start)
            if (source[lineOffset++] == '\n')
                ++line;
        if ((!words || whole(source, start, end)) &&
            allowed(analysis, options, inactive, start, end)) {
            result.append({start, end, line});
            if (result.size() >= maximum)
                break;
            if (onePerLine) {
                const auto nextLine = source.indexOf('\n', start);
                if (nextLine < 0)
                    break;
                offset = static_cast<size_t>(qMax(end, static_cast<int>(nextLine) + 1));
                continue;
            }
        }
        offset = static_cast<size_t>(end);
        if (start == end) {
            if (offset == input.size())
                break;
            ++offset;
            while (offset < input.size() &&
                   (static_cast<unsigned char>(source[static_cast<qsizetype>(offset)]) & 0xc0) == 0x80)
                ++offset;
        }
    }
    return result;
}
}

QString validateReferenceSearch(const QString &query, const SearchOptions &options) {
    if (query.isEmpty())
        return "Enter a symbol name or search expression.";
    if (query.size() > 4096)
        return "Search expressions are limited to 4096 characters.";
    if (options.contextLines < 0 || options.contextLines > 1000)
        return "Lines of context must be between 0 and 1000.";
    try {
        if (options.method == SearchMethod::KeywordExpression)
            Keywords{query, options};
        else
            expression(query, options.method == SearchMethod::RegularExpression,
                       options.caseSensitive);
    } catch (const std::invalid_argument &error) {
        return QString::fromUtf8(error.what());
    }
    return {};
}

ReferenceResults searchReferences(const ReferenceSearchRequest &request,
                                 const std::shared_ptr<std::atomic_bool> &cancel) {
    ReferenceResults results;
    const auto fail = [&](const QString &error) {
        results.error = error;
        qCWarning(navigationLog) << "search.failed" << "root=" << request.workspaceRoot
                                 << "method=" << static_cast<int>(request.options.method)
                                 << "offset=" << request.offset << "error=" << error;
        return results;
    };
    results.error = validateReferenceSearch(request.query, request.options);
    if (!results.error.isEmpty())
        return fail(results.error);
    const auto &options = request.options;
    if (options.method == SearchMethod::References && options.smartMatching) {
        return fail("Smart reference matching requires a semantic provider and a selected symbol.");
    }
    if (request.offset < 0 || request.limit <= 0 || request.limit > RuntimeLimits::ReferenceResults) {
        return fail("Invalid search result page.");
    }
    const bool keywords = options.method == SearchMethod::KeywordExpression;
    std::unique_ptr<Keywords> parsed;
    std::unique_ptr<RE2> regex;
    if (keywords)
        parsed = std::make_unique<Keywords>(request.query, options);
    else
        regex = expression(request.query, options.method == SearchMethod::RegularExpression,
                           options.caseSensitive);
    QHash<QString, const SymbolOverlay *> overlays;
    for (const auto &overlay : request.overlays)
        if (!overlays.contains(workspacePath(overlay.path)))
            overlays.insert(workspacePath(overlay.path), &overlay);
    const auto patterns = options.filePatterns.split(QRegularExpression("[;\\s]+"), Qt::SkipEmptyParts);
    int consumed = 0;
    QSet<QString> visited;
    auto paths = request.paths;
    if (!keywords && options.method != SearchMethod::RegularExpression &&
        (options.wholeWords || options.method == SearchMethod::Occurrences ||
         options.method == SearchMethod::References) && !request.workspaceRoot.isEmpty()) {
        const auto candidates = referenceCandidateFiles(request.workspaceRoot, paths, request.query,
                                                        options.caseSensitive, cancel);
        if (!candidates.error.isEmpty()) {
            return fail(candidates.error);
        }
        if (candidates.cancelled) {
            results.cancelled = true;
            return results;
        }
        results.sourceHashes = candidates.sourceHashes;
        for (auto it = candidates.sourceHashes.cbegin(); it != candidates.sourceHashes.cend(); ++it)
            if (!overlays.contains(it.key()) && request.expectedHashes.contains(it.key()) &&
                request.expectedHashes.value(it.key()) != it.value()) {
                return fail("Source changed between result pages: " + it.key() +
                            ". Rerun the search.");
            }
        QSet<QString> allowedPaths;
        for (const auto &path : candidates.files)
            allowedPaths.insert(workspacePath(path));
        for (const auto &overlay : request.overlays)
            allowedPaths.insert(workspacePath(overlay.path));
        // Preserve original ordering so pagination stays deterministic.
        paths.removeIf([&](const auto &path) { return !allowedPaths.contains(workspacePath(path)); });
    }
    for (const auto &path : paths) {
        if (cancel && cancel->load()) { results.cancelled = true; break; }
        const auto identity = workspacePath(path);
        if (visited.contains(identity))
            continue;
        visited.insert(identity);
        if (!patterns.isEmpty() && !QDir::match(patterns, QFileInfo(path).fileName()))
            continue;
        FileContents file;
        SourceAnalysis analysis;
        if (const auto *overlay = overlays.value(identity, nullptr)) {
            file.bytes = overlay->source;
            analysis = overlay->analysis;
        } else
            file = readSourceFile(path);
        if (!file.error.isEmpty()) {
            if (request.expectedHashes.contains(identity)) {
                results.hits.clear();
                return fail("Cannot continue changed/unreadable source: " + path + ": " +
                            file.error);
            }
            results.warnings << path + ": " + file.error;
            results.sourceHashes.insert(identity, {});
            continue;
        }
        const auto hash = QCryptographicHash::hash(file.bytes, QCryptographicHash::Sha256).toHex();
        if (request.expectedHashes.contains(identity) && request.expectedHashes.value(identity) != hash) {
            results.hits.clear();
            return fail("Source changed between result pages: " + path + ". Rerun the search.");
        }
        results.sourceHashes.insert(identity, hash);
        ++results.filesSearched;
        if (!keywords && !RE2::PartialMatch(
                re2::StringPiece(file.bytes.constData(), file.bytes.size()), *regex))
            continue;
        if (analysis.styles.size() != file.bytes.size())
            analysis = analyzeSource(file.bytes, path);
        if (!analysis.error.isEmpty())
            results.warnings << path + ": " + analysis.error;
        if (!analysis.supported &&
            (options.comments != CommentSearch::Include || !options.strings))
            results.warnings << path + ": no syntax classifier; comment/string filters may be incomplete.";
        QVector<QPair<int, int>> inactive;
        if (options.skipInactive) {
            const auto regions = compilerInactiveRanges(path, file.bytes, request.overlays);
            inactive = regions.ranges;
            if (!regions.error.isEmpty())
                results.warnings << path + ": " + regions.error;
        }
        QVector<Match> found;
        const auto needed = static_cast<int>(std::clamp<qint64>(
            static_cast<qint64>(request.offset) + request.limit - consumed + 1,
            1, std::numeric_limits<int>::max()));
        if (keywords) {
            std::vector<QVector<Match>> termMatches;
            int remaining = 1000000;
            for (const auto &term : parsed->terms) {
                auto hits = matches(*term.regex, file.bytes, analysis, options,
                                    term.wholeWord, inactive, cancel, remaining + 1, true);
                remaining -= hits.size();
                if (remaining < 0) {
                    results.hits.clear();
                    return fail("Keyword expression exceeds one million term/line matches in a "
                                "file. Narrow the query.");
                }
                termMatches.push_back(std::move(hits));
            }
            QVector<int> lines{0};
            for (int i = 0; i < file.bytes.size(); ++i)
                if (file.bytes[i] == '\n' && i + 1 < file.bytes.size())
                    lines.append(i + 1);
            for (int line = 0; line < lines.size(); ++line) {
                if (cancel && cancel->load()) { results.cancelled = true; return results; }
                const int last = qMin(line + options.contextLines, static_cast<int>(lines.size()) - 1);
                if (!evaluate(*parsed->root, termMatches, line, last))
                    continue;
                int start = lines[line];
                int end = last + 1 < lines.size() ? lines[last + 1] - 1
                                                 : static_cast<int>(file.bytes.size());
                // Coalesce overlapping context blocks, rather than duplicating the same lines.
                if (!found.isEmpty() && start <= found.last().end)
                    found.last().end = end;
                else
                    found.append({start, end, line});
                if (found.size() >= needed)
                    break;
            }
        } else
            found = matches(*regex, file.bytes, analysis, options,
                            options.wholeWords || options.method == SearchMethod::Occurrences ||
                                options.method == SearchMethod::References, inactive, cancel, needed);
        for (const auto &match : found) {
            if (cancel && cancel->load()) { results.cancelled = true; return results; }
            if (consumed++ < request.offset)
                continue;
            if (results.hits.size() == request.limit) {
                results.truncated = true;
                return results;
            }
            const int lineStart = match.start == 0 ? 0 : file.bytes.lastIndexOf('\n', match.start - 1) + 1;
            auto lineEnd = file.bytes.indexOf('\n', match.end);
            if (lineEnd < 0)
                lineEnd = file.bytes.size();
            results.hits.append({{path, match.start, match.end, -1, hash},
                QString::fromUtf8(file.bytes.mid(lineStart, qMin<qsizetype>(4096, lineEnd - lineStart))).trimmed(),
                match.line, container(analysis, match.start, match.end), false, options.output.links});
        }
    }
    results.cancelled = results.cancelled || (cancel && cancel->load());
    return results;
}

bool adjustReferenceHit(ReferenceHit &hit, int position, int removed, int inserted,
                        const QByteArray &beforeHash, const QByteArray &source,
                        const QByteArray &afterHash) {
    if (hit.stale || !hit.linked)
        return false;
    if (hit.location.expectedHash != beforeHash) {
        hit.stale = true;
        return false;
    }
    if ((removed > 0 && position < hit.location.end && position + removed > hit.location.start) ||
        (inserted > 0 && position > hit.location.start && position < hit.location.end)) {
        hit.stale = true;
        return false;
    }
    if (position <= hit.location.start) {
        hit.location.start += inserted - removed;
        hit.location.end += inserted - removed;
    }
    hit.location.expectedHash = afterHash.isEmpty()
        ? QCryptographicHash::hash(source, QCryptographicHash::Sha256).toHex() : afterHash;
    hit.line = source.first(hit.location.start).count('\n');
    const int start = hit.location.start == 0 ? 0 : source.lastIndexOf('\n', hit.location.start - 1) + 1;
    auto end = source.indexOf('\n', hit.location.end);
    if (end < 0)
        end = source.size();
    hit.text = QString::fromUtf8(source.mid(start, qMin<qsizetype>(4096, end - start))).trimmed();
    return true;
}

QStringList touchReferenceFiles(const QVector<ReferenceHit> &hits) {
    QStringList errors;
    QSet<QString> visited;
    const auto now = QDateTime::currentDateTimeUtc();
    for (const auto &hit : hits) {
        const auto path = workspacePath(hit.location.path);
        if (visited.contains(path))
            continue;
        visited.insert(path);
        QFile file(path);
        const auto source = readSourceFile(path);
        if (!source.error.isEmpty()) {
            errors << path + ": " + source.error;
            continue;
        }
        if (QCryptographicHash::hash(source.bytes, QCryptographicHash::Sha256).toHex() !=
            hit.location.expectedHash) {
            errors << path + ": source changed or has unsaved matches; save and rerun before touching.";
            continue;
        }
        if (!file.open(QIODevice::ReadWrite | QIODevice::ExistingOnly) ||
            !file.setFileTime(now, QFileDevice::FileModificationTime))
            errors << path + ": " + file.errorString();
    }
    return errors;
}

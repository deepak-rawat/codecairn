#include "ReferenceSearch.h"
#include "Diagnostics.h"
#include "CompilerSymbolRecovery.h"
#include "FileIO.h"
#include "SourceAnalysis.h"
#include "WorkspacePaths.h"
#include <QCryptographicHash>
#include <QRegularExpression>

namespace {
QString containerAt(const SourceAnalysis &analysis, int start, int end) {
    const SourceSymbol *container = nullptr;
    for (const auto &symbol : analysis.symbols) {
        if (symbol.outlineOnly || !symbol.definition || symbol.start > start || symbol.end < end ||
            (symbol.kind != "Function" && symbol.kind != "Type" && symbol.kind != "Enum" &&
             symbol.kind != "Namespace"))
            continue;
        if (!container || symbol.end - symbol.start < container->end - container->start)
            container = &symbol;
    }
    return container
               ? (container->qualifiedName.isEmpty() ? container->name : container->qualifiedName)
               : QString{};
}
}

ReferenceResults findTextOccurrences(const QStringList &paths, const QString &identifier,
                                     const QVector<SymbolOverlay> &overlays, bool includeComments,
                                     bool includeStrings,
                                     const std::shared_ptr<std::atomic_bool> &cancel) {
    ReferenceResults results;
    const auto needle = identifier.toUtf8();
    if (needle.isEmpty()) {
        results.warnings << "Select an identifier before searching.";
        return results;
    }
    if (paths.isEmpty())
        return results;
    QHash<QString, const SymbolOverlay *> overlaysByPath;
    for (const auto &overlay : overlays) {
        if (cancel->load()) {
            results.cancelled = true;
            return results;
        }
        const auto path = workspacePath(overlay.path);
        if (!overlaysByPath.contains(path))
            overlaysByPath.insert(path, &overlay);
    }
    const auto word = [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u >= 128 || c == '_' || c == '$' || (c >= '0' && c <= '9') ||
               (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    };
    for (const auto &path : paths) {
        if (cancel->load()) {
            results.cancelled = true;
            return results;
        }
        FileContents file;
        SourceAnalysis analysis;
        const auto *overlay =
            overlaysByPath.isEmpty() ? nullptr : overlaysByPath.value(workspacePath(path), nullptr);
        if (overlay) {
            file.bytes = overlay->source;
            analysis = overlay->analysis;
        } else
            file = readSourceFile(path);
        if (!file.error.isEmpty()) {
            results.warnings << path + ": " + file.error;
            continue;
        }
        if (!file.bytes.contains(needle))
            continue;
        if (analysis.styles.size() != file.bytes.size())
            analysis = analyzeSource(file.bytes, path);
        if (!analysis.error.isEmpty())
            results.warnings << path + ": " + analysis.error;
        const auto hash = QCryptographicHash::hash(file.bytes, QCryptographicHash::Sha256).toHex();
        qsizetype start = 0;
        qsizetype lineStart = 0, lineEnd = file.bytes.indexOf('\n');
        if (lineEnd < 0)
            lineEnd = file.bytes.size();
        int line = 0;
        while ((start = file.bytes.indexOf(needle, start)) >= 0) {
            if (cancel->load()) {
                results.cancelled = true;
                return results;
            }
            const auto end = start + needle.size();
            const bool whole = (start == 0 || !word(file.bytes[start - 1])) &&
                               (end == file.bytes.size() || !word(file.bytes[end]));
            const auto style = start < analysis.styles.size()
                                   ? static_cast<TextStyle>(analysis.styles[start])
                                   : TextStyle::Plain;
            if (whole && (includeComments || style != TextStyle::Comment) &&
                (includeStrings || style != TextStyle::String)) {
                while (start > lineEnd) {
                    lineStart = lineEnd + 1;
                    lineEnd = file.bytes.indexOf('\n', lineStart);
                    if (lineEnd < 0)
                        lineEnd = file.bytes.size();
                    ++line;
                }
                const auto snippetStart = qMax(lineStart, start - 80);
                results.hits.append(
                    {{path, static_cast<int>(start), static_cast<int>(end), -1, hash},
                     QString::fromUtf8(
                         file.bytes.mid(snippetStart, qMin<qsizetype>(512, lineEnd - snippetStart)))
                         .trimmed(),
                     line,
                     containerAt(analysis, static_cast<int>(start), static_cast<int>(end))});
                if (results.hits.size() == RuntimeLimits::ReferenceResults) {
                    results.truncated = true;
                    return results;
                }
            }
            start = end;
        }
    }
    return results;
}

ReferenceResults resolveSemanticReferences(const QVector<LspLocation> &targets,
                                           const QStringList &paths, const QString &identifier,
                                           const QVector<SymbolOverlay> &overlays,
                                           const std::shared_ptr<std::atomic_bool> &cancel,
                                           int offset, int limit, bool skipInactive,
                                           const QHash<QString, QByteArray> &expectedHashes) {
    ReferenceResults results;
    const auto fail = [&](const QString &error) {
        results.error = error;
        qCWarning(navigationLog) << "search.semantic_failed" << "offset=" << offset
                                 << "targets=" << targets.size() << "error=" << error;
        return results;
    };
    if (offset < 0 || limit <= 0 || limit > RuntimeLimits::ReferenceResults) {
        return fail("Invalid semantic reference result page.");
    }
    int consumed = 0;
    QHash<QString, FileContents> sources;
    QHash<QString, SourceAnalysis> analyses;
    QHash<QString, QByteArray> hashes;
    QHash<QString, InactiveCodeRanges> inactive;
    for (const auto &overlay : overlays) {
        const auto path = workspacePath(overlay.path);
        sources.insert(path, {overlay.source, {}, false});
        analyses.insert(path, overlay.analysis);
    }
    QSet<QString> scope, seen;
    for (const auto &path : paths)
        scope.insert(workspacePath(path));
    for (const auto &target : targets) {
        if (cancel && cancel->load()) {
            results.cancelled = true;
            break;
        }
        const auto path = workspacePath(target.path);
        if (!scope.contains(path))
            continue;
        if (!sources.contains(path))
            sources.insert(path, readSourceFile(path));
        const auto &file = sources[path];
        if (!file.error.isEmpty()) {
            if (expectedHashes.contains(path)) {
                results.hits.clear();
                return fail("Cannot continue changed/unreadable source: " + path + ": " +
                            file.error);
            }
            results.warnings << path + ": " + file.error;
            continue;
        }
        if (!results.sourceHashes.contains(path)) {
            const auto hash = QCryptographicHash::hash(file.bytes, QCryptographicHash::Sha256).toHex();
            if (expectedHashes.contains(path) && expectedHashes.value(path) != hash) {
                results.hits.clear();
                return fail("Source changed between result pages: " + path + ". Rerun the search.");
            }
            results.sourceHashes.insert(path, hash);
        }
        const int start = sourceByteOffset(file.bytes, target.line, target.character);
        const int end = sourceByteOffset(file.bytes, target.endLine, target.endCharacter);
        static const QRegularExpression alternate(
            "^(?:~?[\\p{L}_$][\\p{L}\\p{N}_$]*|operator[^\\s]+)$");
        const auto spelling = start >= 0 && end > start
                                  ? QString::fromUtf8(file.bytes.mid(start, end - start)) : QString{};
        if (start < 0 || end <= start ||
            (spelling != identifier && !alternate.match(spelling).hasMatch())) {
            results.warnings << path + ": changed range or alternate spelling omitted.";
            continue;
        }
        if (skipInactive) {
            if (!inactive.contains(path)) {
                inactive.insert(path, compilerInactiveRanges(path, file.bytes, overlays));
                if (!inactive[path].error.isEmpty())
                    results.warnings << path + ": " + inactive[path].error;
            }
            bool skipped = false;
            for (const auto &range : inactive[path].ranges)
                skipped = skipped || (start < range.second && end > range.first);
            if (skipped)
                continue;
        }
        const auto key = path + ':' + QString::number(start) + ':' + QString::number(end);
        if (seen.contains(key))
            continue;
        seen.insert(key);
        if (consumed++ < offset)
            continue;
        if (results.hits.size() == limit) {
            results.truncated = true;
            break;
        }
        if (!hashes.contains(path)) {
            hashes.insert(path,
                          QCryptographicHash::hash(file.bytes, QCryptographicHash::Sha256).toHex());
            auto &analysis = analyses[path];
            if (analysis.styles.size() != file.bytes.size())
                analysis = analyzeSource(file.bytes, path);
            if (!analysis.error.isEmpty())
                results.warnings << path + ": " + analysis.error;
        }
        const auto lineStart = start == 0 ? 0 : file.bytes.lastIndexOf('\n', start - 1) + 1;
        auto lineEnd = file.bytes.indexOf('\n', end);
        if (lineEnd < 0)
            lineEnd = file.bytes.size();
        results.hits.append(
            {{path, start, end, -1, hashes[path]},
             QString::fromUtf8(file.bytes.mid(lineStart, qMin<qsizetype>(512, lineEnd - lineStart)))
                 .trimmed(),
             target.line,
             containerAt(analyses[path], start, end)});
    }
    return results;
}

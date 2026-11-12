#include "NavigationTarget.h"
#include "SourceCoordinates.h"

#include <QCryptographicHash>

ResolvedNavigation resolveNavigationTarget(const NavigationLocation &location,
                                           const QByteArray &source,
                                           const std::optional<IndexedSymbol> &indexedTarget) {
    ResolvedNavigation result;
    if (!location.expectedHash.isEmpty() &&
        QCryptographicHash::hash(source, QCryptographicHash::Sha256).toHex() !=
            location.expectedHash) {
        result.error = "Source changed since this target was indexed; refreshing instead of using "
                       "stale offsets.";
        result.stale = true;
        return result;
    }
    result.start = location.line >= 0 ? sourceByteOffset(source, location.line, location.character)
                                      : location.start;
    result.end = location.line >= 0
                     ? sourceByteOffset(source, location.endLine, location.endCharacter)
                     : location.end;
    if (result.start < 0 || result.end < result.start || result.end > source.size() ||
        (indexedTarget && (indexedTarget->symbol.start < 0 ||
                           indexedTarget->symbol.end < indexedTarget->symbol.start ||
                           indexedTarget->symbol.end > source.size())))
        result.error = "Definition range is no longer valid. Retry the lookup.";
    return result;
}

QString sourceIdentifier(const QByteArray &source, int position) {
    const auto word = [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u >= 128 || c == '_' || c == '$' || (c >= 'a' && c <= 'z') ||
               (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    };
    if (position < 0 || position >= source.size() || !word(source[position]))
        return {};
    int start = position, end = position;
    while (start > 0 && word(source[start - 1]))
        --start;
    while (end < source.size() && word(source[end]))
        ++end;
    return QString::fromUtf8(source.mid(start, end - start));
}

IdentifierSelection contextIdentifier(const QByteArray &source, int position) {
    const auto isWord = [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u >= 128 || c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9');
    };
    int start = qBound(0, position, static_cast<int>(source.size()));
    int end = start;
    while (start > 0 && isWord(source[start - 1]))
        --start;
    while (end < source.size() && isWord(source[end]))
        ++end;
    return {QString::fromUtf8(source.mid(start, end - start)), start, end};
}

#include "SourceCoordinates.h"

QJsonObject lspPosition(const QByteArray &source, int byteOffset) {
    byteOffset = qBound(0, byteOffset, static_cast<int>(source.size()));
    int line = 0, start = 0;
    for (int i = 0; i < byteOffset; ++i)
        if (source[i] == '\n') {
            ++line;
            start = i + 1;
        }
    return {{"line", line},
            {"character", QString::fromUtf8(source.mid(start, byteOffset - start)).size()}};
}

int sourceByteOffset(const QByteArray &source, int line, int utf16Character) {
    if (line < 0 || utf16Character < 0)
        return -1;
    qsizetype start = 0;
    for (int current = 0; current < line; ++current) {
        const auto newline = source.indexOf('\n', start);
        if (newline < 0)
            return -1;
        start = newline + 1;
    }
    const auto newline = source.indexOf('\n', start);
    auto bytes = source.mid(start, newline < 0 ? -1 : newline - start);
    if (bytes.endsWith('\r'))
        bytes.chop(1);
    const auto text = QString::fromUtf8(bytes);
    if (utf16Character > text.size() ||
        (utf16Character < text.size() && text[utf16Character].isLowSurrogate()))
        return -1;
    return static_cast<int>(start + text.left(utf16Character).toUtf8().size());
}

#pragma once

#include <QByteArray>
#include <QString>

inline constexpr qint64 MaximumDocumentBytes = 4 * 1024 * 1024;

struct FileContents {
    QByteArray bytes;
    QString error;
    bool utf8Bom = false;
};

enum class SourceReadContext { Document, Catalog };

FileContents readSourceFile(const QString &path,
                            SourceReadContext context = SourceReadContext::Document);
QString writeSourceFile(const QString &path, const QByteArray &bytes, bool utf8Bom);

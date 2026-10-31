#include "FileIO.h"
#include "Diagnostics.h"

#include <QFile>
#include <QSaveFile>
#include <QStringConverter>

FileContents readSourceFile(const QString &path, SourceReadContext context) {
    const auto fail = [&](const QString &error, bool unsupported = false) -> FileContents {
        if (unsupported && context == SourceReadContext::Catalog)
            qCDebug(fileLog) << "file.catalog_skipped" << "path=" << path << "reason=" << error;
        else
            qCWarning(fileLog) << "file.read_failed" << "path=" << path << "error=" << error;
        return {{}, error, false};
    };
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(file.errorString());
    if (file.size() > MaximumDocumentBytes)
        return fail("CodeCairn supports files up to 4 MiB.", true);
    QByteArray bytes = file.read(MaximumDocumentBytes + 1);
    if (file.error() != QFileDevice::NoError)
        return fail(file.errorString());
    if (bytes.size() > MaximumDocumentBytes)
        return fail("The file grew beyond the prototype's 4 MiB limit.", true);
    const bool bom = bytes.startsWith("\xef\xbb\xbf");
    if (bom)
        bytes.remove(0, 3);
    QStringDecoder decoder(QStringDecoder::Utf8, QStringConverter::Flag::Stateless);
    const QString decoded = decoder(bytes);
    Q_UNUSED(decoded);
    if (bytes.contains('\0') || decoder.hasError())
        return fail("Only UTF-8 text is supported. This file is binary or uses another encoding.",
                    true);
    return {bytes, {}, bom};
}

QString writeSourceFile(const QString &path, const QByteArray &bytes, bool utf8Bom) {
    const auto fail = [&](const QString &error) {
        qCWarning(fileLog) << "file.write_failed" << "path=" << path << "error=" << error;
        return error;
    };
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return fail(file.errorString());
    if ((utf8Bom && file.write("\xef\xbb\xbf", 3) != 3) || file.write(bytes) != bytes.size()) {
        const QString error = file.errorString();
        file.cancelWriting();
        return fail(error);
    }
    if (!file.commit())
        return fail(file.errorString());
    return {};
}

#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QMetaType>

struct LspLocation {
    QString path;
    int line = 0;
    int character = 0;
    int endLine = 0;
    int endCharacter = 0;
};
Q_DECLARE_METATYPE(LspLocation)

QJsonObject lspPosition(const QByteArray &source, int byteOffset);
int sourceByteOffset(const QByteArray &source, int line, int utf16Character);

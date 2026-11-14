#pragma once

#include <QByteArray>
#include <QString>

namespace codecairn {

struct SourceLocation {
    QString path;
    int start = 0;
    int end = 0;
    int firstVisibleLine = -1;
    QByteArray expectedHash{};
    int line = -1;
    int character = 0;
    int endLine = -1;
    int endCharacter = 0;
    bool wholeLine = false;
    bool operator==(const SourceLocation &) const = default;
};

}

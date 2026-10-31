#pragma once

#include <QString>

struct CompilationDatabaseResult {
    QString directory;
    bool configured = false;
    QString error;
};

CompilationDatabaseResult prepareCompilationDatabase(const QString &root, const QString &directory);

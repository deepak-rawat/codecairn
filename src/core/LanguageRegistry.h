#pragma once

#include <QByteArray>
#include <QStringList>

struct TSLanguage;
enum class LanguageId {
    Plain,
    C,
    Cpp,
    Python,
    Bash,
    JavaScript,
    TypeScript,
    Tsx,
    Go,
    Rust,
    Json,
    Yaml,
    CMake
};
struct Language {
    LanguageId id;
    QString name;
    QString lspId;
    const TSLanguage *grammar;
    QStringList server;
};

const Language &languageForFile(const QString &path, const QByteArray &source = {});
const Language &languageById(LanguageId id);
bool isCFamily(LanguageId id);

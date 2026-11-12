#pragma once

#include "LanguageRegistry.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>

enum class TextStyle : unsigned char {
    Plain,
    Comment,
    Keyword,
    String,
    Number,
    Type,
    Function,
    Field,
    Preprocessor,
    Operator,
    TypeReference,
    Primitive,
    Parameter,
    Variable,
    VariableDeclaration,
    Constant,
    FunctionReference,
    LibraryFunction,
    Punctuation,
    Count
};

struct SourceSymbol {
    QString name;
    QString kind;
    int start = 0;
    int end = 0;
    int nameStart = 0;
    int nameEnd = 0;
    int line = 0;
    int parent = -1;
    bool outlineOnly = false;
    QString qualifiedName;
    QString signature;
    bool definition = false;
    QString category;
    bool local = false;
    bool generated = false;
    QString provider = "syntax";
};

struct SourceAnalysis {
    QByteArray styles;
    QVector<SourceSymbol> symbols;
    QString error;
    bool syntaxErrors = false;
    bool supported = false;
    LanguageId language = LanguageId::Plain;
    QStringList diagnostics;
    bool compilerEnriched = false;
};

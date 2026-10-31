#include "CompilerSymbolRecovery.h"
#include "Diagnostics.h"
#include "LanguageRegistry.h"
#include "WorkspacePaths.h"
#include <clang-c/Index.h>
#include <clang-c/CXCompilationDatabase.h>
#include <QDir>
#include <QCoreApplication>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace {
QString string(CXString value) {
    const auto result = QString::fromUtf8(clang_getCString(value));
    clang_disposeString(value);
    return result;
}

QStringList arguments(const QString &path, LanguageId language, bool *configured = nullptr) {
    if (configured)
        *configured = false;
    QStringList result{"-x", language == LanguageId::Cpp ? "c++" : "c"};
    const auto version =
        QRegularExpression("(\\d+)\\.").match(string(clang_getClangVersion())).captured(1);
    QStringList resourceRoots;
#if CODECAIRN_BUNDLE_RUNTIME
    const QDir application(QCoreApplication::applicationDirPath());
    resourceRoots << application.filePath("lib/clang") << application.filePath("../lib/clang")
                  << application.filePath("../Resources/lib/clang");
#endif
    resourceRoots << QString::fromUtf8(CODECAIRN_CLANG_RESOURCE_ROOT);
    for (const auto &root : resourceRoots) {
        const auto resource = QDir(root).filePath(version);
        if (!version.isEmpty() && QFileInfo::exists(QDir(resource).filePath("include/stddef.h"))) {
            result << "-resource-dir=" + QDir::cleanPath(resource);
            break;
        }
    }
    QDir directory = QFileInfo(path).absoluteDir();
    while (true) {
        QString database;
        QStringList candidates{workspaceClangdDirectory(directory.absolutePath())};
        for (const auto &relative : {".", "build", "build/ucrt64", "build/desktop"})
            candidates.append(directory.filePath(relative));
        for (const auto &candidate : candidates) {
            if (QFileInfo::exists(QDir(candidate).filePath("compile_commands.json"))) {
                database = candidate;
                break;
            }
        }
        if (!database.isEmpty()) {
            CXCompilationDatabase_Error error;
            auto db =
                clang_CompilationDatabase_fromDirectory(database.toUtf8().constData(), &error);
            if (!db) {
                qCWarning(analysisLog) << "analysis.compilation_database_failed" << "path=" << path
                                       << "directory=" << database << "code=" << error;
                break;
            }
            auto commands = clang_CompilationDatabase_getCompileCommands(
                db, QFileInfo(path).absoluteFilePath().toUtf8().constData());
            const bool exactCommand = clang_CompileCommands_getSize(commands) > 0;
            if (clang_CompileCommands_getSize(commands) == 0) {
                clang_CompileCommands_dispose(commands);
                commands = clang_CompilationDatabase_getAllCompileCommands(db);
            }
            if (clang_CompileCommands_getSize(commands)) {
                if (configured)
                    *configured = exactCommand;
                const auto command = clang_CompileCommands_getCommand(commands, 0);
                result << "-working-directory=" +
                              string(clang_CompileCommand_getDirectory(command));
                // Only analysis flags are accepted: no plugins, response files, output actions or driver execution.
                const QSet<QString> paired{
                    "-I", "-isystem",  "-iquote",   "-idirafter", "-include", "-imacros",     "-D",
                    "-U", "--sysroot", "-isysroot", "-target",    "--target", "-resource-dir"};
                for (unsigned i = 1; i < clang_CompileCommand_getNumArgs(command); ++i) {
                    const auto arg = string(clang_CompileCommand_getArg(command, i));
                    if (paired.contains(arg) && i + 1 < clang_CompileCommand_getNumArgs(command)) {
                        result << arg << string(clang_CompileCommand_getArg(command, ++i));
                    } else if (arg.startsWith("-I") || arg.startsWith("-D") ||
                               arg.startsWith("-U") || arg.startsWith("-std=") ||
                               arg.startsWith("--sysroot=") || arg == "-fms-extensions" ||
                               arg == "-fdeclspec" || arg == "-pthread") {
                        result << arg;
                    }
                }
            }
            clang_CompileCommands_dispose(commands);
            clang_CompilationDatabase_dispose(db);
            break;
        }
        if (!directory.cdUp())
            break;
    }
    return result;
}

struct Visitor {
    QString path;
    QByteArray source;
    QVector<SourceSymbol> symbols;
    LanguageId language;
};

bool anonymousTag(CXCursor cursor, const Visitor &data) {
    const auto kind = clang_getCursorKind(cursor);
    if (kind != CXCursor_StructDecl && kind != CXCursor_ClassDecl && kind != CXCursor_UnionDecl &&
        kind != CXCursor_EnumDecl)
        return false;
    const auto name = string(clang_getCursorSpelling(cursor)).toUtf8();
    if (name.isEmpty() || clang_Cursor_isAnonymous(cursor))
        return true;
    CXFile spellingFile, expansionFile;
    unsigned spellingOffset, expansionOffset;
    const auto location = clang_getCursorLocation(cursor);
    clang_getSpellingLocation(location, &spellingFile, nullptr, nullptr, &spellingOffset);
    clang_getExpansionLocation(location, &expansionFile, nullptr, nullptr, &expansionOffset);
    // Clang can give an unnamed tag its typedef's name at the tag keyword's location.
    return spellingFile && expansionFile && clang_File_isEqual(spellingFile, expansionFile) &&
           spellingOffset == expansionOffset &&
           data.source.mid(expansionOffset, name.size()) != name;
}

CXChildVisitResult collect(CXCursor cursor, CXCursor, CXClientData user) {
    auto &data = *static_cast<Visitor *>(user);
    const auto kind = clang_getCursorKind(cursor);
    QString type;
    if (kind == CXCursor_FunctionDecl || kind == CXCursor_CXXMethod ||
        kind == CXCursor_Constructor || kind == CXCursor_Destructor ||
        kind == CXCursor_FunctionTemplate)
        type = "Function";
    else if (kind == CXCursor_VarDecl)
        type = "Variable";
    else if (kind == CXCursor_FieldDecl)
        type = "Field";
    else if (kind == CXCursor_StructDecl || kind == CXCursor_ClassDecl ||
             kind == CXCursor_UnionDecl || kind == CXCursor_TypedefDecl ||
             kind == CXCursor_TypeAliasDecl)
        type = "Type";
    else if (kind == CXCursor_EnumDecl)
        type = "Enum";
    else if (kind == CXCursor_EnumConstantDecl)
        type = "Constant";
    if (type.isEmpty())
        return CXChildVisit_Recurse;
    CXFile file;
    unsigned line, column, offset;
    clang_getExpansionLocation(clang_getCursorLocation(cursor), &file, &line, &column, &offset);
    if (!file || QFileInfo(string(clang_getFileName(file)))
                         .absoluteFilePath()
                         .compare(data.path,
#ifdef Q_OS_WIN
                                  Qt::CaseInsensitive
#else
                                  Qt::CaseSensitive
#endif
                                  ) != 0)
        return CXChildVisit_Continue;
    const auto name = string(clang_getCursorSpelling(cursor));
    if (name.isEmpty() || anonymousTag(cursor, data))
        return CXChildVisit_Recurse;
    SourceSymbol symbol;
    symbol.name = name;
    symbol.kind = type;
    symbol.category = kind == CXCursor_ClassDecl                                       ? "Class"
                      : kind == CXCursor_StructDecl                                    ? "Struct"
                      : kind == CXCursor_UnionDecl                                     ? "Union"
                      : kind == CXCursor_TypedefDecl || kind == CXCursor_TypeAliasDecl ? "Alias"
                      : kind == CXCursor_EnumConstantDecl ? "Enum Constant"
                                                          : type;
    symbol.definition = clang_isCursorDefinition(cursor);
    if (kind == CXCursor_VarDecl && data.language == LanguageId::C &&
        clang_Cursor_getStorageClass(cursor) != CX_SC_Extern)
        symbol.definition = true;
    if (symbol.category == "Alias")
        symbol.definition = true;
    symbol.nameStart = static_cast<int>(offset);
    symbol.line = static_cast<int>(line) - 1;
    const auto range = clang_getCursorExtent(cursor);
    unsigned first, last;
    clang_getExpansionLocation(clang_getRangeStart(range), nullptr, nullptr, nullptr, &first);
    clang_getExpansionLocation(clang_getRangeEnd(range), nullptr, nullptr, nullptr, &last);
    symbol.start = static_cast<int>(first);
    symbol.end = static_cast<int>(last);
    if (kind == CXCursor_TypedefDecl) {
        const auto underlying = clang_getCanonicalType(clang_getTypedefDeclUnderlyingType(cursor));
        const auto declaration = clang_getTypeDeclaration(underlying);
        if (underlying.kind == CXType_Enum && anonymousTag(declaration, data)) {
            unsigned enumStart, enumEnd;
            CXFile enumFile;
            const auto enumRange = clang_getCursorExtent(declaration);
            clang_getExpansionLocation(clang_getRangeStart(enumRange), &enumFile, nullptr, nullptr,
                                       &enumStart);
            clang_getExpansionLocation(clang_getRangeEnd(enumRange), nullptr, nullptr, nullptr,
                                       &enumEnd);
            if (enumFile && clang_File_isEqual(enumFile, file) && enumStart >= first &&
                enumEnd <= last)
                symbol.category = "Enum";
        }
    }
    symbol.generated = data.source.mid(symbol.nameStart, name.toUtf8().size()) != name.toUtf8();
    int length = static_cast<int>(name.toUtf8().size());
    if (symbol.generated) {
        length = 0;
        while (symbol.nameStart + length < data.source.size()) {
            const char c = data.source[symbol.nameStart + length];
            if (!(c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9')))
                break;
            ++length;
        }
    }
    symbol.nameEnd = qMin(static_cast<int>(data.source.size()), symbol.nameStart + length);
    symbol.end = qMax(symbol.end, symbol.nameEnd);
    if (symbol.start < 0 || symbol.end > data.source.size() || symbol.nameStart < symbol.start)
        return CXChildVisit_Continue;
    QStringList scopes;
    for (auto parent = clang_getCursorSemanticParent(cursor);
         !clang_Cursor_isNull(parent) && clang_getCursorKind(parent) != CXCursor_TranslationUnit;
         parent = clang_getCursorSemanticParent(parent)) {
        const auto scope = string(clang_getCursorSpelling(parent));
        if (!scope.isEmpty())
            scopes.prepend(scope);
        const auto parentKind = clang_getCursorKind(parent);
        if (parentKind == CXCursor_FunctionDecl || parentKind == CXCursor_CXXMethod)
            symbol.local = true;
    }
    scopes.append(name);
    symbol.qualifiedName = scopes.join("::");
    symbol.signature = type == "Function" ? string(clang_getCursorDisplayName(cursor)) : QString{};
    symbol.provider = "compiler";
    data.symbols.append(symbol);
    return CXChildVisit_Recurse;
}
}

void recoverCompilerSymbols(SourceAnalysis &analysis, const QByteArray &source,
                            const QString &path) {
    const auto language = analysis.language;
    if (!isCFamily(language))
        return;
    const auto absolute = QFileInfo(path).absoluteFilePath();
    const auto filename = absolute.toUtf8();
    const auto flags = arguments(path, language);
    QVector<QByteArray> encoded;
    for (const auto &arg : flags)
        encoded.append(arg.toUtf8());
    QVector<const char *> raw;
    for (const auto &arg : encoded)
        raw.append(arg.constData());
    CXUnsavedFile unsaved{filename.constData(), source.constData(),
                          static_cast<unsigned long>(source.size())};
    CXIndex index = clang_createIndex(1, 0);
    if (!index) {
        analysis.diagnostics.append("Compiler enrichment unavailable for this file.");
        qCWarning(analysisLog) << "analysis.compiler_failed" << "path=" << path << "stage=index"
                               << "error=Cannot create a libclang index.";
        return;
    }
    CXTranslationUnit unit = nullptr;
    const auto error = clang_parseTranslationUnit2(index, filename.constData(), raw.constData(),
                                                   static_cast<int>(raw.size()), &unsaved, 1,
                                                   CXTranslationUnit_KeepGoing, &unit);
    if (error != CXError_Success || !unit) {
        qCWarning(analysisLog) << "analysis.compiler_failed" << "path=" << path << "stage=parse"
                               << "code=" << error;
        analysis.diagnostics.append("Compiler enrichment unavailable for this file.");
        if (unit)
            clang_disposeTranslationUnit(unit);
        clang_disposeIndex(index);
        return;
    }
    Visitor data{absolute, source, {}, language};
    clang_visitChildren(clang_getTranslationUnitCursor(unit), collect, &data);
    for (const auto &symbol : data.symbols) {
        auto same =
            std::find_if(analysis.symbols.begin(), analysis.symbols.end(), [&](const auto &old) {
                return !old.outlineOnly && old.name == symbol.name && old.kind == symbol.kind &&
                       old.nameStart == symbol.nameStart;
            });
        if (same != analysis.symbols.end()) {
            same->definition = symbol.definition;
            same->category = symbol.category;
            continue;
        }
        if (symbol.generated && symbol.kind == "Function") {
            auto invocation = std::find_if(
                analysis.symbols.begin(), analysis.symbols.end(), [&](const auto &old) {
                    return !old.outlineOnly && old.provider != "compiler" &&
                           old.kind == "Function" && old.start == symbol.start;
                });
            if (invocation != analysis.symbols.end()) {
                *invocation = symbol;
                continue;
            }
        }
        if (symbol.kind == "Variable") {
            auto invalid = std::find_if(
                analysis.symbols.begin(), analysis.symbols.end(), [&](const auto &old) {
                    return !old.outlineOnly && old.provider != "compiler" &&
                           old.kind == "Variable" && old.line == symbol.line &&
                           std::none_of(data.symbols.cbegin(), data.symbols.cend(),
                                        [&](const auto &valid) {
                                            return valid.kind == old.kind &&
                                                   valid.name == old.name && valid.line == old.line;
                                        });
                });
            if (invalid != analysis.symbols.end()) {
                *invalid = symbol;
                continue;
            }
        }
        analysis.symbols.append(symbol);
    }
    // Rebuild parent indexes after merging; aliases are relationships, not lexical containers.
    for (auto &symbol : analysis.symbols) {
        if (symbol.provider != "compiler")
            continue;
        const auto separator = symbol.qualifiedName.lastIndexOf("::");
        if (separator < 0)
            continue;
        const auto parentName = symbol.qualifiedName.left(separator);
        for (int i = 0; i < analysis.symbols.size(); ++i)
            if (analysis.symbols[i].qualifiedName == parentName && analysis.symbols[i].definition) {
                symbol.parent = i;
                break;
            }
    }
    analysis.compilerEnriched = true;
    const unsigned diagnostics = clang_getNumDiagnostics(unit);
    int reported = 0;
    for (unsigned i = 0; i < diagnostics && analysis.diagnostics.size() < 12; ++i) {
        auto diagnostic = clang_getDiagnostic(unit, i);
        const auto severity = clang_getDiagnosticSeverity(diagnostic);
        if (severity >= CXDiagnostic_Warning && (reported < 12 || severity >= CXDiagnostic_Error)) {
            const auto message =
                string(clang_formatDiagnostic(diagnostic, clang_defaultDiagnosticDisplayOptions()));
            if (reported < 12) {
                qCWarning(analysisLog) << "analysis.compiler_diagnostic" << "path=" << path
                                       << "severity=" << severity << "message=" << message;
                ++reported;
            }
            if (severity >= CXDiagnostic_Error)
                analysis.diagnostics.append(message);
        }
        clang_disposeDiagnostic(diagnostic);
    }
    clang_disposeTranslationUnit(unit);
    clang_disposeIndex(index);
}

InactiveCodeRanges compilerInactiveRanges(const QString &path, const QByteArray &source,
                                         const QVector<SymbolOverlay> &overlays) {
    InactiveCodeRanges result;
    const auto fail = [&](const QString &error) {
        result.error = error;
        qCWarning(analysisLog) << "analysis.inactive_failed" << "path=" << path
                               << "error=" << error;
        return result;
    };
    const auto language = languageForFile(path, source).id;
    if (!isCFamily(language)) {
        return fail(
            "Inactive-code filtering is supported only for C/C++; other text was retained.");
    }
    const auto filename = QFileInfo(path).absoluteFilePath().toUtf8();
    bool configured = false;
    const auto flags = arguments(path, language, &configured);
    if (!configured) {
        static const QRegularExpression conditions(
            "(?m)^\\s*#\\s*(if|ifdef|ifndef|elif)\\b([^\\r\\n]*)");
        auto matches = conditions.globalMatch(QString::fromUtf8(source));
        while (matches.hasNext()) {
            const auto condition = matches.next();
            if ((condition.captured(1) != "if" && condition.captured(1) != "elif") ||
                (condition.captured(2).trimmed() != "0" && condition.captured(2).trimmed() != "1")) {
                return fail("No exact compilation command for conditional macros; uncertain text "
                            "was retained.");
            }
        }
    }
    QVector<QByteArray> encoded;
    for (const auto &arg : flags)
        encoded.append(arg.toUtf8());
    QVector<const char *> raw;
    for (const auto &arg : encoded)
        raw.append(arg.constData());
    QVector<QByteArray> overlayNames;
    QVector<const SymbolOverlay *> included;
    for (const auto &overlay : overlays)
        if (QDir::isAbsolutePath(overlay.path) && workspacePath(overlay.path) != workspacePath(path)) {
            overlayNames.append(QFileInfo(overlay.path).absoluteFilePath().toUtf8());
            included.append(&overlay);
        }
    QVector<CXUnsavedFile> unsaved{{filename.constData(), source.constData(),
                                  static_cast<unsigned long>(source.size())}};
    for (int i = 0; i < included.size(); ++i)
        unsaved.append({overlayNames[i].constData(), included[i]->source.constData(),
                        static_cast<unsigned long>(included[i]->source.size())});
    const auto index = clang_createIndex(1, 0);
    if (!index)
        return fail("Compiler could not determine inactive regions; all text was retained.");
    CXTranslationUnit unit = nullptr;
    const auto error = clang_parseTranslationUnit2(
        index, filename.constData(), raw.constData(), static_cast<int>(raw.size()),
        unsaved.data(), static_cast<unsigned>(unsaved.size()),
        CXTranslationUnit_DetailedPreprocessingRecord | CXTranslationUnit_KeepGoing,
        &unit);
    if (error != CXError_Success || !unit) {
        qCWarning(analysisLog) << "analysis.compiler_failed" << "path=" << path
                               << "stage=inactive_parse" << "code=" << error;
        if (unit)
            clang_disposeTranslationUnit(unit);
        result.error = "Compiler could not determine inactive regions; all text was retained.";
    } else {
        int reported = 0;
        for (unsigned i = 0; i < clang_getNumDiagnostics(unit); ++i) {
            const auto diagnostic = clang_getDiagnostic(unit, i);
            if (clang_getDiagnosticSeverity(diagnostic) >= CXDiagnostic_Error) {
                result.error = "Compiler diagnostics prevent reliable inactive-code filtering; all text was retained.";
                if (reported < 12) {
                    qCWarning(analysisLog)
                        << "analysis.compiler_diagnostic" << "path=" << path << "stage=inactive"
                        << "message="
                        << string(clang_formatDiagnostic(diagnostic,
                                                         clang_defaultDiagnosticDisplayOptions()));
                    ++reported;
                }
            }
            clang_disposeDiagnostic(diagnostic);
        }
        if (result.error.isEmpty()) {
            auto *ranges = clang_getSkippedRanges(unit, clang_getFile(unit, filename.constData()));
            if (ranges) {
                for (unsigned i = 0; i < ranges->count; ++i) {
                    unsigned start = 0, end = 0;
                    clang_getExpansionLocation(clang_getRangeStart(ranges->ranges[i]),
                                                nullptr, nullptr, nullptr, &start);
                    clang_getExpansionLocation(clang_getRangeEnd(ranges->ranges[i]),
                                                nullptr, nullptr, nullptr, &end);
                    result.ranges.append({static_cast<int>(start), static_cast<int>(end)});
                }
                clang_disposeSourceRangeList(ranges);
            }
        }
        clang_disposeTranslationUnit(unit);
    }
    clang_disposeIndex(index);
    if (!result.error.isEmpty())
        return fail(result.error);
    return result;
}

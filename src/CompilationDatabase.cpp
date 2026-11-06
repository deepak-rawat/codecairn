#include "CompilationDatabase.h"
#include "Diagnostics.h"
#include "WorkspaceOptionsStore.h"
#include "WorkspacePaths.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

CompilationDatabaseResult prepareCompilationDatabase(const QString &root,
                                                     const QString &directory) {
    CompilationDatabaseResult result;
    QString source;
    const auto fail = [&](const QString &error) {
        qCWarning(languageServiceLog)
            << "lsp.compilation_failed" << "root=" << root << "directory=" << directory
            << "source=" << source << "error=" << error;
        result.error = error;
        return result;
    };
    const auto storageError = WorkspaceOptionsStore::initialize(root);
    if (!storageError.isEmpty())
        return fail(storageError);
    const auto metadata = workspaceClangdDirectory(root);
    if (QFileInfo(metadata).isSymLink() ||
        (!QDir().mkpath(metadata) && !QFileInfo(metadata).isDir()))
        return fail("Cannot create clangd storage: " + metadata);
    result.directory = metadata;
    if (!directory.isEmpty())
        source = QDir(directory).filePath("compile_commands.json");
    else {
        for (const auto &candidate :
             {"compile_commands.json", "build/compile_commands.json",
              "build/ucrt64/compile_commands.json", "build/desktop/compile_commands.json"})
            if (QFileInfo::exists(QDir(root).filePath(candidate))) {
                source = QDir(root).filePath(candidate);
                break;
            }
    }
    const auto target = QDir(metadata).filePath("compile_commands.json");
    if (source.isEmpty()) {
        if (QFileInfo::exists(target) && !QFile::remove(target))
            return fail("Cannot remove stale generated compilation database: " + target);
        return result;
    }
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly))
        return fail("Cannot read compilation database: " + input.errorString());
    if (input.size() > 64 * 1024 * 1024)
        return fail("Compilation database exceeds the 64 MiB safety limit.");
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(input.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !document.isArray())
        return fail("Invalid compilation database JSON.");
    QJsonArray normalized;
    for (const auto &value : document.array()) {
        auto entry = value.toObject();
        auto working = entry.value("directory").toString();
        const auto file = entry.value("file").toString();
        if (working.isEmpty() || file.isEmpty())
            return fail("Compilation database entry has no directory/file.");
        if (QDir::isRelativePath(working))
            working = QFileInfo(source).dir().absoluteFilePath(working);
        entry["directory"] = workspacePath(working);
        entry["file"] =
            workspacePath(QDir::isRelativePath(file) ? QDir(working).absoluteFilePath(file) : file);
        normalized.append(entry);
    }
    QSaveFile output(target);
    const auto bytes = QJsonDocument(normalized).toJson(QJsonDocument::Compact);
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() ||
        !output.commit())
        return fail("Cannot save generated compilation database: " + target + ": " +
                    output.errorString());
    result.configured = true;
    return result;
}

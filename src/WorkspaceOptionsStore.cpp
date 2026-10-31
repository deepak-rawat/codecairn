#include "WorkspaceOptionsStore.h"
#include "Diagnostics.h"
#include "WorkspacePaths.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>

namespace WorkspaceOptionsStore {
WorkspaceOptions loadOptions(const QString &root) {
    WorkspaceOptions result;
    const auto path = workspaceOptionsPath(root);
    const auto fail = [&](const QString &message) {
        result.error = path + ": " + message;
        qCWarning(storageLog) << "storage.workspace_read_failed" << "root=" << root
                              << "path=" << path << "error=" << message;
        return result;
    };
    if (path.isEmpty())
        return fail("No workspace storage directory is available.");
    if (QFileInfo(path).isSymLink())
        return fail("Refusing a symlinked workspace configuration.");
    QFile file(path);
    if (!file.exists())
        return result;
    if (!file.open(QIODevice::ReadOnly))
        return fail(file.errorString());
    if (file.size() > 256 * 1024)
        return fail("Workspace configuration exceeds 256 KiB.");
    const auto bytes = file.readAll();
    if (file.error() != QFileDevice::NoError)
        return fail(file.errorString());
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return fail("Invalid workspace configuration JSON.");
    const auto object = document.object();
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!QStringList{"schemaVersion", "workspaceRoot", "compilationDirectory",
                         "excludedDirectories"}.contains(it.key()))
            return fail("Unknown workspace configuration field: " + it.key());
    if (object.value("schemaVersion").toDouble(-1) != 1)
        return fail("Unsupported or missing workspace schemaVersion; expected 1.");
    const auto storedRoot = object.value("workspaceRoot").toString();
    if (!QDir::isAbsolutePath(storedRoot) || workspacePath(storedRoot) != workspacePath(root))
        return fail("Workspace configuration root does not match the requested workspace.");
    if (object.contains("compilationDirectory") && !object.value("compilationDirectory").isString())
        return fail("compilationDirectory must be a string.");
    result.compilationDirectory = object.value("compilationDirectory").toString();
    if (object.contains("excludedDirectories")) {
        if (!object.value("excludedDirectories").isArray())
            return fail("excludedDirectories must be an array.");
        result.excludedDirectories.emplace();
        for (const auto &value : object.value("excludedDirectories").toArray()) {
            if (!value.isString() || value.toString().isEmpty())
                return fail("Each excluded directory must be a nonempty string.");
            result.excludedDirectories->append(value.toString());
        }
    }
    return result;
}

namespace {
QString writeOptions(const QString &root, const QString &compilationDirectory,
                     const QStringList &excludedDirectories, bool onlyIfMissing) {
    const auto directory = workspaceStorageDirectory(root);
    if (directory.isEmpty() || !QFileInfo(root).isDir())
        return "No valid workspace or application state directory is available.";
    for (const auto &name : excludedDirectories)
        if (name.isEmpty())
            return "Excluded directory names cannot be empty.";
    const auto parent = QFileInfo(directory).absolutePath();
    if (QFileInfo(parent).isSymLink() || QFileInfo(directory).isSymLink())
        return "Refusing a symlinked workspace storage directory: " + directory;
    if (!QDir().mkpath(directory))
        return "Cannot create workspace storage: " + directory;
    const auto path = workspaceOptionsPath(root);
    if (onlyIfMissing && QFileInfo::exists(path))
        return loadOptions(root).error;
    QLockFile lock(path + ".lock");
    if (!lock.tryLock(0))
        return path + ": cannot lock workspace configuration; another writer or permissions may "
                      "prevent saving.";
    const auto existing = loadOptions(root);
    if (!existing.error.isEmpty())
        return existing.error;
    if (onlyIfMissing && QFileInfo::exists(path))
        return {};
    const auto bytes =
        QJsonDocument(QJsonObject{{"schemaVersion", 1},
                                  {"workspaceRoot", workspacePath(root)},
                                  {"compilationDirectory", compilationDirectory},
                                  {"excludedDirectories",
                                   QJsonArray::fromStringList(excludedDirectories)}})
            .toJson();
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return path + ": could not save workspace configuration: " + file.errorString();
    return {};
}
}

QString initialize(const QString &root) {
    const auto error = writeOptions(root, {}, defaultWorkspaceExcludes(), true);
    if (!error.isEmpty())
        qCWarning(storageLog) << "storage.workspace_initialize_failed" << "root=" << root
                              << "error=" << error;
    return error;
}

QString saveOptions(const QString &root, const QString &compilationDirectory,
                    const QStringList &excludedDirectories) {
    const auto error = writeOptions(root, compilationDirectory, excludedDirectories, false);
    if (!error.isEmpty())
        qCWarning(storageLog) << "storage.workspace_write_failed" << "root=" << root
                              << "path=" << workspaceOptionsPath(root) << "error=" << error;
    return error;
}
}

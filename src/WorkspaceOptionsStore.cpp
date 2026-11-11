#include "WorkspaceOptionsStore.h"
#include "Diagnostics.h"
#include "WorkspacePaths.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>

namespace {
bool validRule(const QString &rule) {
    if (rule == ".")
        return true;
    if (rule.isEmpty() || QDir::isAbsolutePath(rule) || rule.contains('\\') || rule.contains(':') ||
        rule.contains(QChar::Null))
        return false;
    for (const auto &part : rule.split('/'))
        if (part.isEmpty() || part == "." || part == "..")
            return false;
    return true;
}

bool beneath(const QString &path, const QString &ancestor) {
    return ancestor == "." || path == ancestor || path.startsWith(ancestor + '/');
}

bool hasSymlinkComponent(const QString &path) {
    auto current = QFileInfo(path).absoluteFilePath();
    while (true) {
        const QFileInfo info(current);
        if (info.isSymLink())
            return true;
        const auto parent = info.absolutePath();
        if (parent == current)
            return false;
        current = parent;
    }
}

QString ruleError(const WorkspaceOptions &options) {
    for (const auto &rules : {options.includedPaths, options.excludedPaths})
        for (const auto &rule : rules)
            if (!validRule(rule))
                return "Invalid root-relative project path: " + rule;
    if (options.excludedDirectories)
        for (const auto &name : *options.excludedDirectories)
            if (!validRule(name) || name == "." || name.contains('/'))
                return "Invalid excluded directory name: " + name;
    return {};
}

QString relativeProjectPath(const QString &root, const QString &path) {
    if (root.isEmpty() || path.isEmpty() || !QDir::isAbsolutePath(root) ||
        !QFileInfo(root).isDir() || QFileInfo(root).isSymLink())
        return {};
    for (const auto &part : QDir::fromNativeSeparators(path).split('/'))
        if (part == "..")
            return {};
    const auto absolute = QDir::isAbsolutePath(path) ? path : QDir(root).filePath(path);
    const auto canonicalRoot = workspacePath(root);
    auto relative = QDir(QDir::cleanPath(root)).relativeFilePath(QDir::cleanPath(absolute));
    if (!validRule(relative))
        relative = QDir(canonicalRoot).relativeFilePath(QDir::cleanPath(absolute));
    if (!validRule(relative))
        return {};
    auto current = canonicalRoot;
    for (const auto &part : relative.split('/')) {
        current = QDir(current).filePath(part);
        if (QFileInfo(current).isSymLink())
            return {};
    }
    return relative;
}

bool excluded(const QString &relative, const QString &root, const WorkspaceOptions &options) {
    for (const auto &rule : options.excludedPaths)
        if (beneath(relative, rule))
            return true;
    const auto names = options.excludedDirectories.value_or(defaultWorkspaceExcludes());
    const auto parts = relative.split('/');
    QString current = root;
    for (qsizetype i = 0; i < parts.size(); ++i) {
        const auto &part = parts[i];
        current = QDir(current).filePath(part);
        if (names.contains(part) && (i + 1 < parts.size() || QFileInfo(current).isDir()))
            return true;
    }
    return false;
}
}

bool projectIncludesPath(const QString &root, const QString &path,
                         const WorkspaceOptions &options) {
    const auto relative = relativeProjectPath(root, path);
    if (relative.isEmpty() || !options.error.isEmpty() || !ruleError(options).isEmpty() ||
        excluded(relative, root, options))
        return false;
    if (!options.managed)
        return true;
    for (const auto &rule : options.includedPaths)
        if (beneath(relative, rule))
            return true;
    return false;
}

bool projectTraversesDirectory(const QString &root, const QString &path,
                               const WorkspaceOptions &options) {
    const auto relative = relativeProjectPath(root, path);
    if (relative.isEmpty() || !QFileInfo(QDir(root).filePath(relative)).isDir() ||
        !options.error.isEmpty() || !ruleError(options).isEmpty() ||
        excluded(relative, root, options))
        return false;
    if (!options.managed)
        return true;
    for (const auto &rule : options.includedPaths)
        if (!excluded(rule, root, options) && (beneath(relative, rule) || beneath(rule, relative)))
            return true;
    return false;
}

namespace WorkspaceOptionsStore {
WorkspaceOptions loadOptions(const QString &root, const QString &projectFile) {
    if (!projectFile.isEmpty()) {
        auto project = loadProject(projectFile);
        if (project.error.isEmpty() && workspacePath(project.root) != workspacePath(root))
            project.options.error = "Project root does not match the requested workspace.";
        return project.options;
    }
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
                         "excludedDirectories"}
                 .contains(it.key()))
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
    const auto bytes = QJsonDocument(QJsonObject{{"schemaVersion", 1},
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

QString initialize(const QString &root, const QString &projectFile) {
    if (!projectFile.isEmpty()) {
        const auto error = loadOptions(root, projectFile).error;
        if (!error.isEmpty())
            return error;
        const auto directory = workspaceStorageDirectory(root, {}, projectFile);
        if (!QFileInfo(root).isDir() || directory.isEmpty())
            return "No valid project root or application state directory is available.";
        if (hasSymlinkComponent(directory))
            return "Refusing a symlinked project storage directory: " + directory;
        if (!QDir().mkpath(directory) || hasSymlinkComponent(directory))
            return "Cannot create project storage: " + directory;
        return {};
    }
    const auto error = writeOptions(root, {}, defaultWorkspaceExcludes(), true);
    if (!error.isEmpty())
        qCWarning(storageLog) << "storage.workspace_initialize_failed" << "root=" << root
                              << "error=" << error;
    return error;
}

QString saveOptions(const QString &root, const QString &compilationDirectory,
                    const QStringList &excludedDirectories, const QString &projectFile) {
    if (!projectFile.isEmpty()) {
        auto options = loadOptions(root, projectFile);
        if (!options.error.isEmpty())
            return options.error;
        options.compilationDirectory = compilationDirectory;
        options.excludedDirectories = excludedDirectories;
        return saveProject(projectFile, root, options);
    }
    const auto error = writeOptions(root, compilationDirectory, excludedDirectories, false);
    if (!error.isEmpty())
        qCWarning(storageLog) << "storage.workspace_write_failed" << "root=" << root
                              << "path=" << workspaceOptionsPath(root) << "error=" << error;
    return error;
}

WorkspaceProject loadProject(const QString &projectFile) {
    WorkspaceProject result;
    result.options.managed = true;
    const auto fail = [&](const QString &message) {
        result.error = projectFile + ": " + message;
        result.options.error = result.error;
        return result;
    };
    if (projectFile.isEmpty() || QFileInfo(projectFile).isSymLink())
        return fail("No project descriptor or a symlinked descriptor path.");
    QFile file(projectFile);
    if (!file.open(QIODevice::ReadOnly))
        return fail(file.errorString());
    if (file.size() > 256 * 1024)
        return fail("Project descriptor exceeds 256 KiB.");
    const auto bytes = file.readAll();
    if (file.error() != QFileDevice::NoError)
        return fail(file.errorString());
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail("Invalid project JSON.");
    const auto object = document.object();
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!QStringList{"schemaVersion", "root", "name", "includedPaths", "excludedPaths",
                         "excludedDirectories", "compilationDirectory"}
                 .contains(it.key()))
            return fail("Unknown project field: " + it.key());
    if (object.value("schemaVersion").toDouble(-1) != 1)
        return fail("Unsupported or missing project schemaVersion; expected 1.");
    if (!object.value("root").isString() || !QDir::isAbsolutePath(object.value("root").toString()))
        return fail("Project root must be an absolute folder path.");
    result.root = object.value("root").toString();
    if (!object.value("name").isString() || object.value("name").toString().trimmed().isEmpty())
        return fail("Project name must be nonempty.");
    result.options.name = object.value("name").toString();
    for (const auto &key : {"includedPaths", "excludedPaths", "excludedDirectories"}) {
        if (QString(key) == "excludedDirectories" && !object.contains(key))
            continue;
        if (!object.value(key).isArray())
            return fail(QString(key) + " must be an array.");
        QStringList rules;
        for (const auto &value : object.value(key).toArray()) {
            if (!value.isString())
                return fail(QString(key) + " must contain strings.");
            rules.append(value.toString());
        }
        if (QString(key) == "includedPaths")
            result.options.includedPaths = rules;
        else if (QString(key) == "excludedPaths")
            result.options.excludedPaths = rules;
        else
            result.options.excludedDirectories = rules;
    }
    if (result.options.includedPaths.isEmpty())
        return fail("Select at least one project file or folder.");
    if (object.contains("compilationDirectory") && !object.value("compilationDirectory").isString())
        return fail("compilationDirectory must be a string.");
    result.options.compilationDirectory = object.value("compilationDirectory").toString();
    const auto validation = ruleError(result.options);
    if (!validation.isEmpty())
        return fail(validation);
    result.options.projectFile = workspacePath(projectFile);
    result.options.projectDigest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    return result;
}

QString saveProject(const QString &projectFile, const QString &root,
                    const WorkspaceOptions &options) {
    const auto fail = [&](const QString &message) {
        return projectFile + ": " + message;
    };
    if (projectFile.isEmpty() ||
        QFileInfo(projectFile).suffix().compare("cairn", Qt::CaseInsensitive))
        return fail("Choose a .cairn project descriptor.");
    if (!options.error.isEmpty())
        return options.error;
    if (!QDir::isAbsolutePath(root) || !QFileInfo(root).isDir() || QFileInfo(root).isSymLink())
        return fail("Choose an existing, nonsymlinked base folder.");
    if (options.name.trimmed().isEmpty() || options.includedPaths.isEmpty())
        return fail("A project needs a name and at least one selected file or folder.");
    const auto validation = ruleError(options);
    if (!validation.isEmpty())
        return fail(validation);
    if (QFileInfo(projectFile).isSymLink())
        return fail("Refusing a symlinked project descriptor path.");
    if (!QFileInfo(QFileInfo(projectFile).absolutePath()).isDir())
        return fail("The descriptor's parent folder does not exist.");
    QLockFile lock(projectFile + ".lock");
    if (!lock.tryLock(0))
        return fail(
            "Cannot lock project descriptor; another writer or permissions prevent saving.");
    const bool exists = QFileInfo::exists(projectFile);
    if (exists) {
        const auto existing = loadProject(projectFile);
        if (!existing.error.isEmpty())
            return existing.error;
        if (options.projectFile != workspacePath(projectFile) || options.projectDigest.isEmpty() ||
            options.projectDigest != existing.options.projectDigest)
            return fail("Project descriptor changed externally. Reopen it before saving.");
    } else if (options.projectFile == workspacePath(projectFile) &&
               !options.projectDigest.isEmpty()) {
        return fail(
            "Project descriptor was removed externally. Reopen it or save to a new location.");
    }
    QJsonObject object{{"schemaVersion", 1},
                       {"root", workspacePath(root)},
                       {"name", options.name.trimmed()},
                       {"includedPaths", QJsonArray::fromStringList(options.includedPaths)},
                       {"excludedPaths", QJsonArray::fromStringList(options.excludedPaths)},
                       {"compilationDirectory", options.compilationDirectory}};
    if (options.excludedDirectories)
        object.insert("excludedDirectories",
                      QJsonArray::fromStringList(*options.excludedDirectories));
    const auto bytes = QJsonDocument(object).toJson();
    if (bytes.size() > 256 * 1024)
        return fail("Project descriptor exceeds 256 KiB.");
    QSaveFile file(projectFile);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        return fail("Could not save project descriptor: " + file.errorString());
    if (exists) {
        const auto current = loadProject(projectFile);
        if (!current.error.isEmpty() || current.options.projectDigest != options.projectDigest)
            return fail("Project descriptor changed while saving. Reopen it before saving.");
    } else if (QFileInfo::exists(projectFile)) {
        return fail("Project descriptor appeared while saving. Reopen it before saving.");
    }
    if (!file.commit())
        return fail("Could not commit project descriptor: " + file.errorString());
    return {};
}
}

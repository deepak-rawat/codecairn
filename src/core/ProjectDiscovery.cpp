#include <codecairn/ProjectDiscovery.h>
#include <codecairn/Diagnostics.h>
#include <codecairn/WorkspacePaths.h>
#include "ProjectFileScanner.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QMutexLocker>
#include <QSet>
#include <algorithm>

namespace {
bool covers(const QString &ancestor, const QString &path) {
    return ancestor == "." || ancestor == path || path.startsWith(ancestor + '/');
}
}

QString includeProjectPaths(const QString &root, WorkspaceOptions &options,
                            const QStringList &paths) {
    auto next = options;
    for (const auto &path : paths) {
        const auto absolute = QDir(root).filePath(path);
        if (!pathInWorkspace(absolute, root) || QFileInfo(absolute).isSymLink())
            continue;
        for (const auto &excluded : next.excludedPaths)
            if (excluded != path && covers(excluded, path))
                return QCoreApplication::translate(
                           "ProjectDialog", "%1 is excluded by %2. Add that excluded folder first.")
                    .arg(path, excluded);
        next.includedPaths.removeIf([&](const auto &rule) { return covers(path, rule); });
        next.excludedPaths.removeIf([&](const auto &rule) { return covers(path, rule); });
        if (std::none_of(next.includedPaths.cbegin(), next.includedPaths.cend(),
                         [&](const auto &rule) { return covers(rule, path); }))
            next.includedPaths.append(path);
        if (!projectIncludesPath(root, absolute, next))
            return QCoreApplication::translate(
                       "ProjectDialog", "%1 is excluded by the project's directory exclusions.")
                .arg(path);
    }
    options = next;
    return {};
}

void excludeProjectPaths(WorkspaceOptions &options, const QStringList &paths) {
    for (const auto &path : paths) {
        options.includedPaths.removeAll(path);
        if (std::any_of(options.includedPaths.cbegin(), options.includedPaths.cend(),
                        [&](const auto &rule) { return covers(rule, path); }) &&
            !options.excludedPaths.contains(path))
            options.excludedPaths.append(path);
    }
}

ProjectScanResult discoverProjectFiles(const QString &root, const WorkspaceOptions &options,
                                       quint64 generation,
                                       const std::shared_ptr<std::atomic_bool> &cancel,
                                       const std::shared_ptr<ProjectScanProgress> &progress,
                                       ProjectFileScope scope) {
    ProjectScanResult result;
    result.generation = generation;
    QStringList directories;
    QSet<QString> visited;
    QSet<QString> seen;
    QStringList batch;
    const auto flush = [&] {
        QMutexLocker lock(&progress->mutex);
        progress->pending.append(batch);
        batch.clear();
    };
    const auto addFile = [&](const QString &path) {
        if (!projectIncludesPath(root, path, options))
            return;
        if (scope == ProjectFileScope::SourceFiles && !codecairn::detail::sourceLanguage(path))
            return;
        const auto relative = QDir(root).relativeFilePath(path);
        if (seen.contains(relative))
            return;
        seen.insert(relative);
        result.files.append(relative);
        batch.append(relative);
        ++progress->found;
        if (batch.size() >= 128)
            flush();
    };
    for (const auto &rule : options.includedPaths) {
        if (cancel->load())
            break;
        const auto path = QDir(root).filePath(rule);
        const QFileInfo info(path);
        if (info.isDir() && projectTraversesDirectory(root, path, options))
            directories.append(path);
        else if (info.isFile() && !info.isSymLink())
            addFile(path);
        else if (!info.exists() && result.errors.size() < 20)
            result.errors.append(
                QCoreApplication::translate("ProjectDialog", "Selected path is missing: %1")
                    .arg(path));
    }
    while (!directories.isEmpty() && !cancel->load()) {
        const auto directory = directories.takeLast();
        if (visited.contains(directory))
            continue;
        visited.insert(directory);
        if (!QFileInfo(directory).isReadable()) {
            if (result.errors.size() < 20)
                result.errors.append(
                    QCoreApplication::translate("ProjectDialog", "Unreadable folder: %1")
                        .arg(directory));
            continue;
        }
        QDirIterator entries(directory, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot |
                                            QDir::NoSymLinks);
        while (entries.hasNext() && !cancel->load()) {
            entries.next();
            const auto info = entries.fileInfo();
            const auto path = info.absoluteFilePath();
            if (info.isDir()) {
                if (projectTraversesDirectory(root, path, options))
                    directories.append(path);
            } else if (info.isFile() && projectIncludesPath(root, path, options)) {
                addFile(path);
            }
        }
    }
    flush();
    result.cancelled = cancel->load();
    result.files.sort();
    for (const auto &error : result.errors)
        qCWarning(storageLog) << "project.selection_scan_failed" << error;
    return result;
}

#include "WorkspaceScanner.h"
#include <codecairn/LanguageRegistry.h>
#include "SymbolIndexStore.h"
#include <codecairn/WorkspacePaths.h>

#include <QDataStream>
#include <QDateTime>
#include <QDir>
#include <QFile>

namespace WorkspaceIndexDetail {

bool sourceLanguage(const QString &path) {
    if (languageForFile(path).grammar)
        return true;
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && languageForFile(path, file.read(256)).grammar;
}

QFileInfoList directoryEntries(const QString &path) {
    return QDir(path).entryInfoList(
        QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
}

namespace {
bool excludedDirectory(const QFileInfo &entry, const QStringList &excludes) {
    return excludes.contains(entry.fileName());
}
}

QByteArray directorySnapshot(const QFileInfoList &entries, const QStringList &excludes) {
    QByteArray snapshot;
    QDataStream stream(&snapshot, QIODevice::WriteOnly);
    for (const auto &entry : entries) {
        if (entry.isDir()) {
            if (!excludedDirectory(entry, excludes))
                stream << entry.fileName() << true;
        } else if (entry.isFile()) {
            stream << entry.fileName() << false << entry.size()
                   << entry.lastModified().toMSecsSinceEpoch();
        }
    }
    return snapshot;
}

QStringList scanWorkspaceFiles(SymbolIndexStore &database, const QString &root, bool rescan,
                               const QStringList &excludes,
                               const std::shared_ptr<std::atomic_bool> &cancel,
                               const std::shared_ptr<IndexProgress> &progress, IndexResult &result,
                               const WorkspaceOptions &options) {
    auto membership = options;
    membership.excludedDirectories = excludes;
    QStringList paths;
    if (!rescan) {
        IndexStatement old(database, "SELECT path FROM files ORDER BY path");
        while (old.next()) {
            if (cancel->load())
                break;
            if (projectIncludesPath(root, old.text(0), membership))
                paths.append(old.text(0));
        }
    } else {
        QStringList directories{root};
        while (!directories.isEmpty() && !cancel->load()) {
            const auto directory = directories.takeLast();
            result.directories.append(directory);
            const QFileInfo info(directory);
            if (!info.isReadable()) {
                if (result.warnings.size() < 100)
                    result.warnings.append("Unreadable directory retained from previous catalog: " +
                                           directory);
                else
                    ++result.suppressedWarnings;
                IndexStatement old(database, "SELECT path FROM files");
                while (old.next())
                    if (pathInWorkspace(old.text(0), directory) &&
                        projectIncludesPath(root, old.text(0), membership))
                        paths.append(old.text(0));
                continue;
            }
            const auto entries = directoryEntries(directory);
            result.directorySnapshots.insert(directory, directorySnapshot(entries, excludes));
            for (const auto &entry : entries) {
                if (cancel->load())
                    break;
                if (entry.isDir()) {
                    if (!excludedDirectory(entry, excludes) &&
                        projectTraversesDirectory(root, entry.absoluteFilePath(), membership))
                        directories.append(entry.absoluteFilePath());
                } else if (entry.isFile() &&
                           projectIncludesPath(root, entry.absoluteFilePath(), membership)) {
                    paths.append(workspacePath(entry.absoluteFilePath()));
                    if (progress)
                        ++progress->discovered;
                }
            }
        }
    }
    paths.removeDuplicates();
    paths.sort();
    return paths;
}

}

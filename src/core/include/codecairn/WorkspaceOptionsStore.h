#pragma once

#include <QByteArray>
#include <QStringList>
#include <optional>

struct WorkspaceOptions {
    QString compilationDirectory;
    std::optional<QStringList> excludedDirectories;
    QString error;
    bool managed = false;
    QString name;
    QStringList includedPaths;
    QStringList excludedPaths;
    QString projectFile;
    QByteArray projectDigest;
};

struct WorkspaceProject {
    QString root;
    WorkspaceOptions options;
    QString error;
};

bool projectIncludesPath(const QString &root, const QString &path, const WorkspaceOptions &options);
bool projectTraversesDirectory(const QString &root, const QString &path,
                               const WorkspaceOptions &options);

namespace WorkspaceOptionsStore {
QString initialize(const QString &root, const QString &projectFile = {});
WorkspaceOptions loadOptions(const QString &root, const QString &projectFile = {});
QString saveOptions(const QString &root, const QString &compilationDirectory,
                    const QStringList &excludedDirectories, const QString &projectFile = {});
QString saveProject(const QString &projectFile, const QString &root,
                    const WorkspaceOptions &options);
WorkspaceProject loadProject(const QString &projectFile);
}

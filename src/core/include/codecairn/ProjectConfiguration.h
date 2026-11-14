#pragma once

#include <QByteArray>
#include <QStringList>
#include <optional>

namespace codecairn {

struct ProjectConfiguration {
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

struct ProjectDefinition {
    QString root;
    ProjectConfiguration options;
    QString error;
};

QString validateProjectConfiguration(const ProjectConfiguration &configuration);

namespace ProjectConfigurationStore {
QString initialize(const QString &root, const QString &projectFile = {});
ProjectConfiguration loadOptions(const QString &root, const QString &projectFile = {});
QString saveOptions(const QString &root, const QString &compilationDirectory,
                    const QStringList &excludedDirectories, const QString &projectFile = {});
QString saveProject(const QString &projectFile, const QString &root,
                    const ProjectConfiguration &options);
ProjectDefinition loadProject(const QString &projectFile);
}

}

bool projectIncludesPath(const QString &root, const QString &path,
                         const codecairn::ProjectConfiguration &options);
bool projectTraversesDirectory(const QString &root, const QString &path,
                               const codecairn::ProjectConfiguration &options);

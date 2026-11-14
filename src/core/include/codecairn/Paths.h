#pragma once

#include <QStringList>

QString defaultStateDirectory();
QString defaultSettingsPath();
QString configureStateDirectory(const QString &directory);
QString canonicalPath(const QString &path);
bool pathUnderRoot(const QString &path, const QString &root);
QString projectStateDirectory(const QString &root, const QString &stateDirectory = {},
                              const QString &projectFile = {});
QString projectConfigurationPath(const QString &root, const QString &projectFile = {});
QString projectCompilationDirectory(const QString &root, const QString &projectFile = {});
QString projectIndexPath(const QString &root, const QString &projectFile = {});
QStringList defaultExcludedDirectories();

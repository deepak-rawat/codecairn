#pragma once

#include <QStringList>

QString defaultStateDirectory();
QString workspacePath(const QString &path);
bool pathInWorkspace(const QString &path, const QString &root);
QString workspaceStorageDirectory(const QString &root, const QString &stateDirectory = {},
                                  const QString &projectFile = {});
QString workspaceOptionsPath(const QString &root, const QString &projectFile = {});
QString workspaceClangdDirectory(const QString &root, const QString &projectFile = {});
QString workspaceDatabase(const QString &root, const QString &projectFile = {});
QStringList defaultWorkspaceExcludes();

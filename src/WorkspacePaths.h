#pragma once

#include <QStringList>

QString defaultStateDirectory();
QString workspacePath(const QString &path);
bool pathInWorkspace(const QString &path, const QString &root);
QString workspaceStorageDirectory(const QString &root, const QString &stateDirectory = {});
QString workspaceOptionsPath(const QString &root);
QString workspaceClangdDirectory(const QString &root);
QString workspaceDatabase(const QString &root);
QStringList defaultWorkspaceExcludes();

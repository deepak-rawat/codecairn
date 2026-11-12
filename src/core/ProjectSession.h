#pragma once

#include "WorkspaceOptionsStore.h"

struct PreparedProject {
    QString root;
    QString projectFile;
    QString name;
    WorkspaceOptions options;
    QString error;
};

PreparedProject prepareProject(const QString &root, const QString &projectFile = {});

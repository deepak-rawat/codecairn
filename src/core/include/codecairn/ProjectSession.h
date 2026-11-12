#pragma once

#include <codecairn/WorkspaceOptionsStore.h>

struct PreparedProject {
    QString root;
    QString projectFile;
    QString name;
    WorkspaceOptions options;
    QString error;
};

PreparedProject prepareProject(const QString &root, const QString &projectFile = {});

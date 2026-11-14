#pragma once

#include <codecairn/ProjectConfiguration.h>

struct PreparedProject {
    QString root;
    QString projectFile;
    QString name;
    codecairn::ProjectConfiguration options;
    QString error;
};

PreparedProject prepareProject(const QString &root, const QString &projectFile = {});

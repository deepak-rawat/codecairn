#pragma once

#include <codecairn/IndexTypes.h>
#include <codecairn/ProjectConfiguration.h>
#include <QFileInfo>

namespace codecairn::detail {

class ProjectIndexStore;

bool sourceLanguage(const QString &path);
QFileInfoList directoryEntries(const QString &path);
QByteArray directorySnapshot(const QFileInfoList &entries, const QStringList &excludes);
QStringList scanProjectFiles(ProjectIndexStore &database, const QString &root, bool rescan,
                             const QStringList &excludes,
                             const std::shared_ptr<std::atomic_bool> &cancel,
                             const std::shared_ptr<codecairn::IndexProgress> &progress,
                             codecairn::IndexResult &result,
                             const codecairn::ProjectConfiguration &options);

}

#pragma once

#include <codecairn/IndexTypes.h>
#include <codecairn/WorkspaceOptionsStore.h>
#include <QFileInfo>

namespace WorkspaceIndexDetail {

class SymbolIndexStore;

bool sourceLanguage(const QString &path);
QFileInfoList directoryEntries(const QString &path);
QByteArray directorySnapshot(const QFileInfoList &entries, const QStringList &excludes);
QStringList scanWorkspaceFiles(SymbolIndexStore &database, const QString &root, bool rescan,
                               const QStringList &excludes,
                               const std::shared_ptr<std::atomic_bool> &cancel,
                               const std::shared_ptr<IndexProgress> &progress, IndexResult &result,
                               const WorkspaceOptions &options);

}

#pragma once

#include <codecairn/IndexTypes.h>
#include <codecairn/WorkspacePaths.h>

enum class IndexRefresh {
    DiscoverChanges,
    ReparseKnownFiles,
    RebuildFilesAndSymbols,
    CheckKnownFiles
};

IndexResult buildWorkspaceIndex(const QString &root, IndexRefresh mode, const QStringList &excludes,
                                const std::shared_ptr<std::atomic_bool> &cancel,
                                const std::shared_ptr<IndexProgress> &progress = {},
                                const QString &projectFile = {});
SymbolPage queryWorkspaceSymbols(const QString &root, const SymbolQuery &query);

namespace codecairn {
using IndexUpdate = ::IndexRefresh;
using IndexResult = ::IndexResult;
}

#pragma once

#include <codecairn/IndexTypes.h>
#include <codecairn/Paths.h>

namespace codecairn {
enum class IndexUpdate {
    DiscoverChanges,
    ReparseKnownFiles,
    RebuildFilesAndSymbols,
    CheckKnownFiles
};
}

codecairn::IndexResult
buildProjectIndex(const QString &root, codecairn::IndexUpdate mode, const QStringList &excludes,
                  const std::shared_ptr<std::atomic_bool> &cancel,
                  const std::shared_ptr<codecairn::IndexProgress> &progress = {},
                  const QString &projectFile = {});
codecairn::SymbolPage queryProjectSymbols(const QString &root,
                                          const codecairn::SymbolCatalogQuery &query);

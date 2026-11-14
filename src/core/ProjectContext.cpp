#include "ProjectContext.h"
#include <codecairn/Diagnostics.h>
#include <codecairn/Paths.h>
#include <QFileInfo>
#include <QDir>

namespace codecairn::detail {

Result<ContextChange> ProjectContext::open(const PreparedProject &prepared) {
    const auto validation = validateProjectConfiguration(prepared.options);
    if (!prepared.error.isEmpty() || !validation.isEmpty() || prepared.root.isEmpty() ||
        !QDir::isAbsolutePath(prepared.root) || !QFileInfo(prepared.root).isDir() ||
        !QFileInfo(prepared.root).isReadable()) {
        const auto error = !prepared.error.isEmpty() ? prepared.error
                           : !validation.isEmpty()
                               ? validation
                               : "Project folder is missing or unreadable: " + prepared.root;
        qCWarning(storageLog) << "project.open_failed" << error;
        return Result<ContextChange>::failure(ErrorCode::InvalidArgument, error);
    }
    ContextChange change;
    change.identity = info_.root != prepared.root || info_.projectFile != prepared.projectFile;
    change.analysis =
        change.identity ||
        info_.configuration.compilationDirectory != prepared.options.compilationDirectory ||
        info_.configuration.projectDigest != prepared.options.projectDigest;
    if (change.analysis)
        ++generation_;
    info_ = {prepared.root, prepared.projectFile, prepared.name, prepared.options};
    analysisDirectory_ = projectCompilationDirectory(info_.root, info_.projectFile);
    if (change.analysis)
        membership_.clear();
    return Result<ContextChange>::success(change);
}

Result<ContextChange> ProjectContext::reload() {
    const auto loaded =
        codecairn::ProjectConfigurationStore::loadOptions(info_.root, info_.projectFile);
    if (!loaded.error.isEmpty()) {
        qCWarning(storageLog) << "project.options_failed" << loaded.error;
        return Result<ContextChange>::failure(ErrorCode::Io, loaded.error);
    }
    const PreparedProject prepared{info_.root, info_.projectFile, info_.name, loaded, {}};
    auto changed = open(prepared);
    membership_.clear();
    return changed;
}

bool ProjectContext::includes(const QString &identity, bool cache) const {
    const auto found = membership_.constFind(identity);
    if (found != membership_.cend())
        return *found;
    const bool included = projectIncludesPath(info_.root, identity, info_.configuration);
    if (cache)
        membership_.insert(identity, included);
    return included;
}

void ProjectContext::clear() {
    ++generation_;
    info_ = {};
    analysisDirectory_.clear();
    membership_.clear();
}

Result<void> ProjectContext::setExcludedDirectories(const QStringList &names) {
    auto updated = info_.configuration;
    updated.excludedDirectories = names;
    const auto validation = validateProjectConfiguration(updated);
    if (!validation.isEmpty()) {
        qCWarning(storageLog) << "project.options_failed" << validation;
        return Result<void>::failure(ErrorCode::InvalidArgument, validation);
    }
    info_.configuration = updated;
    membership_.clear();
    return Result<void>::success();
}

}

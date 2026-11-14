#pragma once

#include <codecairn/ProjectConfiguration.h>

#include <QMutex>
#include <atomic>
#include <memory>

struct ProjectScanProgress {
    QMutex mutex;
    QStringList pending;
    std::atomic_int found{0};
};

struct ProjectScanResult {
    QStringList files;
    QStringList errors;
    quint64 generation = 0;
    bool cancelled = false;
};

enum class ProjectFileScope { AllFiles, SourceFiles };

ProjectScanResult discoverProjectFiles(const QString &root,
                                       const codecairn::ProjectConfiguration &options,
                                       quint64 generation,
                                       const std::shared_ptr<std::atomic_bool> &cancel,
                                       const std::shared_ptr<ProjectScanProgress> &progress,
                                       ProjectFileScope scope = ProjectFileScope::AllFiles);
QString includeProjectPaths(const QString &root, codecairn::ProjectConfiguration &options,
                            const QStringList &paths);
void excludeProjectPaths(codecairn::ProjectConfiguration &options, const QStringList &paths);

#pragma once

#include <QStringList>
#include <optional>

struct WorkspaceOptions {
    QString compilationDirectory;
    std::optional<QStringList> excludedDirectories;
    QString error;
};

namespace WorkspaceOptionsStore {
QString initialize(const QString &root);
WorkspaceOptions loadOptions(const QString &root);
QString saveOptions(const QString &root, const QString &compilationDirectory,
                    const QStringList &excludedDirectories);
}

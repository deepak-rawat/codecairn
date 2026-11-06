#include "WorkspacePaths.h"

#include <QDir>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QStandardPaths>

QString defaultStateDirectory() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    return QStandardPaths::writableLocation(QStandardPaths::StateLocation);
#else
    const auto data = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return data.isEmpty() ? QString{} : QDir(data).filePath("state");
#endif
}

QString workspacePath(const QString &path) {
    const QFileInfo info(path);
    const auto canonical = info.canonicalFilePath();
    if (!canonical.isEmpty())
        return QDir::cleanPath(canonical);
    auto ancestor = info.absoluteFilePath();
    QStringList missing;
    while (true) {
        const QFileInfo parent(ancestor);
        const auto resolved = parent.canonicalFilePath();
        if (!resolved.isEmpty())
            return QDir::cleanPath(QDir(resolved).filePath(missing.join('/')));
        const auto directory = parent.absolutePath();
        if (directory == ancestor)
            return QDir::cleanPath(info.absoluteFilePath());
        missing.prepend(parent.fileName());
        ancestor = directory;
    }
}

bool pathInWorkspace(const QString &path, const QString &root) {
    if (root.isEmpty() || path.startsWith(":/"))
        return false;
    const auto relative = QDir(workspacePath(root)).relativeFilePath(workspacePath(path));
    return relative != ".." && !relative.startsWith("../") && !QDir::isAbsolutePath(relative);
}

QString workspaceStorageDirectory(const QString &root, const QString &stateDirectory) {
    const auto state = stateDirectory.isEmpty() ? defaultStateDirectory() : stateDirectory;
    if (root.isEmpty() || !QDir::isAbsolutePath(root) || state.isEmpty())
        return {};
    const auto hash =
        QCryptographicHash::hash(workspacePath(root).toUtf8(), QCryptographicHash::Sha256).toHex();
    return QDir(state).filePath("workspaces/" + QString::fromLatin1(hash));
}

QString workspaceOptionsPath(const QString &root) {
    const auto directory = workspaceStorageDirectory(root);
    return directory.isEmpty() ? QString{} : QDir(directory).filePath("workspace.json");
}

QString workspaceClangdDirectory(const QString &root) {
    const auto directory = workspaceStorageDirectory(root);
    return directory.isEmpty() ? QString{} : QDir(directory).filePath("clangd");
}

QString workspaceDatabase(const QString &root) {
    const auto directory = workspaceStorageDirectory(root);
    return directory.isEmpty() ? QString{} : QDir(directory).filePath("index.sqlite3");
}

QStringList defaultWorkspaceExcludes() {
    return {".git", ".svn", "build", "node_modules", "target"};
}

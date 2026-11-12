#include <codecairn/ProjectSession.h>
#include <codecairn/CompilationDatabase.h>
#include <codecairn/WorkspacePaths.h>

#include <QFileInfo>

PreparedProject prepareProject(const QString &root, const QString &projectFile) {
    PreparedProject project;
    project.root = workspacePath(root);
    project.projectFile = projectFile;
    const QFileInfo folder(project.root);
    if (!folder.isDir() || !folder.isReadable()) {
        project.error = "Workspace folder is missing or unreadable: " + root;
        return project;
    }
    project.error = WorkspaceOptionsStore::initialize(project.root, projectFile);
    if (!project.error.isEmpty())
        return project;
    project.options = WorkspaceOptionsStore::loadOptions(project.root, projectFile);
    project.error = project.options.error;
    if (!project.error.isEmpty())
        return project;
    const auto database =
        prepareCompilationDatabase(project.root, project.options.compilationDirectory, projectFile);
    project.error = database.error;
    project.name = project.options.name.isEmpty() ? folder.fileName() : project.options.name;
    return project;
}

#include "WorkspaceServices.h"
#include "LanguageRegistry.h"
#include "Diagnostics.h"
#include <QFileInfo>
#include <utility>

WorkspaceServices::WorkspaceServices(QObject *parent)
    : QObject(parent), index_(new WorkspaceIndex(this)),
      cppService_(new LanguageServerClient(this)) {
}

QString WorkspaceServices::commitProject(const PreparedProject &project, bool indexing,
                                         bool semantic) {
    if (!project.error.isEmpty() || project.root.isEmpty() || !QFileInfo(project.root).isDir() ||
        !QFileInfo(project.root).isReadable()) {
        const auto error = project.error.isEmpty()
                               ? "Prepared workspace is missing or unreadable: " + project.root
                               : project.error;
        qCWarning(storageLog) << "project.commit_failed" << error;
        return error;
    }
    project_ = project;
    if (indexing)
        index_->open(project.root, project.projectFile);
    if (semantic)
        restartLanguageServices(project.root, project.options.compilationDirectory,
                                project.projectFile);
    return {};
}

QString WorkspaceServices::reloadProjectOptions() {
    const auto options = WorkspaceOptionsStore::loadOptions(project_.root, project_.projectFile);
    if (!options.error.isEmpty()) {
        qCWarning(storageLog) << "project.options_failed" << options.error;
        return options.error;
    }
    project_.options = options;
    if (options.excludedDirectories)
        index_->setExcludes(*options.excludedDirectories);
    return {};
}

void WorkspaceServices::closeProject() {
    index_->close();
    stopLanguageServices();
    project_ = {};
}

LanguageServerClient *WorkspaceServices::languageService(const QString &root, const QString &path,
                                                         const QByteArray &source) {
    if (path.startsWith(":/") || root.isEmpty())
        return nullptr;
    const auto &language = languageForFile(path, source);
    if (isCFamily(language.id))
        return cppService_;
    if (language.server.isEmpty())
        return nullptr;
    const int key = static_cast<int>(language.id);
    if (languageServices_.contains(key))
        return languageServices_.value(key);
    auto *service = new LanguageServerClient(this);
    languageServices_.insert(key, service);
    emit serviceCreated(service);
    service->startServer(root, language.server);
    return service;
}

void WorkspaceServices::restartLanguageServices(const QString &root,
                                                const QString &compilationDirectory,
                                                const QString &projectFile) {
    for (auto *service : std::as_const(languageServices_))
        delete service;
    languageServices_.clear();
    cppService_->start(root, compilationDirectory, {}, projectFile);
}

void WorkspaceServices::stopLanguageServices() {
    cppService_->stop();
    for (auto *service : std::as_const(languageServices_))
        service->stop();
}

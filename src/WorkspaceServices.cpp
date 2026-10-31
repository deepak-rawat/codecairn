#include "WorkspaceServices.h"
#include "LanguageRegistry.h"
#include <utility>

WorkspaceServices::WorkspaceServices(QObject *parent)
    : QObject(parent), index_(new WorkspaceIndex(this)),
      cppService_(new LanguageServerClient(this)) {
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
                                                const QString &compilationDirectory) {
    for (auto *service : std::as_const(languageServices_))
        delete service;
    languageServices_.clear();
    cppService_->start(root, compilationDirectory);
}

void WorkspaceServices::stopLanguageServices() {
    cppService_->stop();
    for (auto *service : std::as_const(languageServices_))
        service->stop();
}

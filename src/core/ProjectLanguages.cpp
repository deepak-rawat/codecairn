#include "ProjectLanguages.h"
#include <codecairn/LanguageRegistry.h>
#include <utility>

namespace codecairn::detail {

ProjectLanguages::ProjectLanguages(QObject *parent)
    : QObject(parent), cpp_(new LanguageServerClient(this)) {
}

LanguageServerClient *ProjectLanguages::serviceFor(const QString &path, const QByteArray &source) {
    if (path.startsWith(":/") || root_.isEmpty())
        return nullptr;
    const auto &language = languageForFile(path, source);
    if (isCFamily(language.id))
        return cpp_;
    if (language.server.isEmpty())
        return nullptr;
    const int key = static_cast<int>(language.id);
    if (auto *service = others_.value(key))
        return service;
    auto *service = new LanguageServerClient(this);
    others_.insert(key, service);
    emit serviceCreated(service);
    service->startServer(root_, language.server);
    return service;
}

void ProjectLanguages::start(const QString &root, const QString &compilationDirectory,
                             const QString &projectFile) {
    qDeleteAll(others_);
    others_.clear();
    root_ = root;
    cpp_->start(root, compilationDirectory, {}, projectFile);
}

void ProjectLanguages::stop() {
    root_.clear();
    cpp_->stop();
    for (auto *service : std::as_const(others_))
        service->stop();
}

void ProjectLanguages::closeDocument(const QString &path) {
    cpp_->closeDocument(path);
    for (auto *service : std::as_const(others_))
        service->closeDocument(path);
}

}

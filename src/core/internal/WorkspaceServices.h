#pragma once

#include "LanguageServerClient.h"
#include "WorkspaceIndex.h"
#include <codecairn/ProjectSession.h>
#include <QHash>
#include <QObject>

enum class IndexStartup { Automatic, Manual };
enum class LanguageStartup { Automatic, Manual };

struct ProjectOpenOptions {
    IndexStartup indexing = IndexStartup::Automatic;
    LanguageStartup languages = LanguageStartup::Automatic;
};

class WorkspaceServices final : public QObject {
    Q_OBJECT
public:
    explicit WorkspaceServices(QObject *parent = nullptr);
    const PreparedProject &project() const { return project_; }
    QString commitProject(const PreparedProject &project, const ProjectOpenOptions &options = {});
    QString reloadProjectOptions();
    void closeProject();
    WorkspaceIndex *index() const { return index_; }
    LanguageServerClient *cppService() const { return cppService_; }
    LanguageServerClient *languageService(const QString &root, const QString &path,
                                          const QByteArray &source);
    void restartLanguageServices(const QString &root, const QString &compilationDirectory,
                                 const QString &projectFile = {});
    void stopLanguageServices();
    void closeDocument(const QString &path);

signals:
    void projectAboutToChange();
    void serviceCreated(LanguageServerClient *service);

private:
    WorkspaceIndex *index_;
    LanguageServerClient *cppService_;
    QHash<int, LanguageServerClient *> languageServices_;
    PreparedProject project_;
};

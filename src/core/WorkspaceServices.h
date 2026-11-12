#pragma once

#include "LanguageServerClient.h"
#include "WorkspaceIndex.h"
#include "ProjectSession.h"
#include <QHash>
#include <QObject>

class WorkspaceServices final : public QObject {
    Q_OBJECT
public:
    explicit WorkspaceServices(QObject *parent = nullptr);
    const PreparedProject &project() const { return project_; }
    QString commitProject(const PreparedProject &project, bool indexing = true,
                          bool semantic = true);
    QString reloadProjectOptions();
    void closeProject();
    WorkspaceIndex *index() const { return index_; }
    LanguageServerClient *cppService() const { return cppService_; }
    LanguageServerClient *languageService(const QString &root, const QString &path,
                                          const QByteArray &source);
    void restartLanguageServices(const QString &root, const QString &compilationDirectory,
                                 const QString &projectFile = {});
    void stopLanguageServices();

signals:
    void serviceCreated(LanguageServerClient *service);

private:
    WorkspaceIndex *index_;
    LanguageServerClient *cppService_;
    QHash<int, LanguageServerClient *> languageServices_;
    PreparedProject project_;
};

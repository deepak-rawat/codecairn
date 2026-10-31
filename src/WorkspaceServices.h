#pragma once

#include "LanguageServerClient.h"
#include "WorkspaceIndex.h"
#include <QHash>
#include <QObject>

class WorkspaceServices final : public QObject {
    Q_OBJECT
public:
    explicit WorkspaceServices(QObject *parent = nullptr);
    WorkspaceIndex *index() const { return index_; }
    LanguageServerClient *cppService() const { return cppService_; }
    LanguageServerClient *languageService(const QString &root, const QString &path,
                                          const QByteArray &source);
    void restartLanguageServices(const QString &root, const QString &compilationDirectory);
    void stopLanguageServices();

signals:
    void serviceCreated(LanguageServerClient *service);

private:
    WorkspaceIndex *index_;
    LanguageServerClient *cppService_;
    QHash<int, LanguageServerClient *> languageServices_;
};

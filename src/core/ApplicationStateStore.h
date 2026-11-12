#pragma once

#include "NavigationHistory.h"
#include "ReferenceSearch.h"
#include "WorkspacePaths.h"
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <optional>

struct RecentProject {
    QString root;
    QString projectFile;
    QString name;
};

struct ApplicationState {
    QByteArray geometry;
    QByteArray mainSplitter;
    QByteArray rightSplitter;
    int browserTab = 0;
    QString lastWorkspace;
    QString lastProjectFile;
    QVector<RecentProject> recentProjects;
};

struct WorkspaceSession {
    QVector<NavigationLocation> files;
    QString activeFile;
    QVector<ReferenceGroup> references;
};

struct ApplicationStateResult {
    std::optional<ApplicationState> state;
    QString error;
};

struct WorkspaceSessionResult {
    std::optional<WorkspaceSession> session;
    QString error;
};

class ApplicationStateStore {
public:
    explicit ApplicationStateStore(QString directory);
    QString statePath() const;
    QString sessionPath(const QString &root, const QString &projectFile = {}) const;
    ApplicationStateResult loadState();
    QString saveState(const ApplicationState &state);
    WorkspaceSessionResult loadSession(const QString &root, const QString &projectFile = {});
    QString saveSession(const QString &root, const WorkspaceSession &session,
                        const QString &projectFile = {});
    void invalidateState();

private:
    struct FileState {
        QByteArray fingerprint;
        bool existed = false;
        bool loaded = false;
    };
    struct JsonResult {
        std::optional<QJsonObject> object;
        QString error;
    };
    JsonResult loadJson(const QString &path, const QJsonObject &defaults);
    QString saveJson(const QString &path, const QJsonObject &object);
    QString directory_;
    QHash<QString, FileState> files_;
};

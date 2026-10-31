#pragma once

#include "IndexTypes.h"
#include "WorkspacePaths.h"
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QMap>
#include <QObject>
#include <QTimer>
#include <QThreadPool>
#include <atomic>
#include <memory>

IndexResult buildWorkspaceIndex(const QString &root, bool force, bool rescan,
                                const QStringList &excludes,
                                const std::shared_ptr<std::atomic_bool> &cancel,
                                const std::shared_ptr<IndexProgress> &progress = {});
SymbolPage queryWorkspaceSymbols(const QString &root, const SymbolQuery &query);

class WorkspaceIndex final : public QObject {
    Q_OBJECT
public:
    explicit WorkspaceIndex(QObject *parent = nullptr);
    ~WorkspaceIndex() override;
    void open(const QString &root);
    void close();
    void refresh(bool force = false, bool rescan = true);
    void cancel();
    bool busy() const { return busy_; }
    QString root() const { return root_; }
    QStringList files() const { return files_; }
    QStringList excludes() const { return excludes_; }
    void setExcludes(const QStringList &names);
    void setRefreshIntervalMinutes(int minutes);
    int refreshIntervalMinutes() const { return pollTimer_.interval() / 60000; }
    void search(const SymbolQuery &query);
    void invalidateSearch() {
        ++queryGeneration_;
        queryPending_ = false;
        if (queryCancelled_)
            queryCancelled_->store(true);
    }
signals:
    void ready(const IndexResult &result);
    void status(const QString &message);
    void failed(const QString &message);
    void busyChanged(bool busy);
    void symbolsReady(const SymbolPage &page);

private:
    void startPending();
    void requestRefresh(bool force, bool rescan, bool automatic, bool verifyContents);
    QString root_;
    QStringList files_;
    QHash<QString, QByteArray> directorySnapshots_;
    QStringList excludes_ = defaultWorkspaceExcludes();
    QFutureWatcher<IndexResult> indexJob_;
    QThreadPool indexPool_;
    QFutureWatcher<SymbolPage> queryJob_;
    QFileSystemWatcher watcher_;
    QTimer refreshTimer_;
    QTimer pollTimer_;
    QTimer progressTimer_;
    std::shared_ptr<std::atomic_bool> cancelled_;
    std::shared_ptr<std::atomic_bool> queryCancelled_;
    std::shared_ptr<IndexProgress> progress_;
    quint64 generation_ = 0;
    quint64 runningGeneration_ = 0;
    quint64 queryGeneration_ = 0;
    quint64 runningQueryGeneration_ = 0;
    SymbolQuery pendingQuery_;
    bool queryPending_ = false;
    bool queryBusy_ = false;
    bool busy_ = false;
    bool pending_ = false;
    bool force_ = false;
    bool rescan_ = true;
    bool paused_ = false;
    bool automaticFileChanged_ = false;
    bool pendingAutomatic_ = false;
    bool verifyContents_ = false;
    bool runningAutomatic_ = false;
    void startQuery();
};

#pragma once

#include <codecairn/IndexTypes.h>
#include <QFutureWatcher>
#include <QObject>
#include <QThreadPool>

namespace codecairn::detail {

class ProjectSymbolQuery final : public QObject {
    Q_OBJECT
public:
    explicit ProjectSymbolQuery(QObject *parent = nullptr);
    ~ProjectSymbolQuery() override;
    void open(const QString &root, const QString &projectFile);
    void search(const ::SymbolQuery &query);
    void cancel();
    void waitForFinished();
    void setIndexing(bool indexing) { indexing_ = indexing; }

signals:
    void ready(const ::SymbolPage &page);

private:
    void startPending();
    QString root_;
    QString projectFile_;
    ::SymbolQuery pending_;
    QFutureWatcher<::SymbolPage> job_;
    QThreadPool pool_;
    std::shared_ptr<std::atomic_bool> cancelled_;
    quint64 generation_ = 0;
    quint64 runningGeneration_ = 0;
    bool busy_ = false;
    bool queued_ = false;
    bool indexing_ = false;
};

}

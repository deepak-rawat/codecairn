#pragma once

#include <codecairn/Queries.h>
#include <codecairn/Project.h>
#include "ProjectLanguages.h"
#include "QuerySources.h"
#include "PendingRequests.h"
#include <QObject>
#include <memory>

namespace codecairn::detail {

enum class SearchKind { Text, References };

class SearchExecutor final : public QObject {
    Q_OBJECT
public:
    using SnapshotProvider = std::function<std::optional<QByteArray>(const QString &)>;
    explicit SearchExecutor(ProjectLanguages *languages, SnapshotProvider snapshots,
                            QObject *parent = nullptr);
    ~SearchExecutor() override;
    void searchText(const ProjectInfo &project, const QStringList &catalog, QuerySources sources,
                    const TextQuery &query, RequestId request);
    void findReferences(const ProjectInfo &project, const QStringList &catalog,
                        QuerySources sources, const ReferenceQuery &query, RequestId request);
    void continueSearch(SearchCursor cursor, RequestId request);
    void cancel(RequestId request);
    void cancelAll();
    QStringList files(const QStringList &catalog, const QuerySources &sources,
                      const SearchFiles &scope) const;

signals:
    void ready(codecairn::RequestId request, codecairn::detail::SearchKind kind,
               const codecairn::SearchPage &page);

private:
    class Operation;
    friend class Operation;
    void accept(Operation *operation, const ReferenceResults &result);
    void deliver(RequestId request, SearchKind kind, SearchPage page);
    void retire(Operation *operation);
    void own(Operation *operation);
    ProjectLanguages *languages_;
    SnapshotProvider snapshots_;
    QHash<quint64, Operation *> operations_;
    QSet<Operation *> owned_;
    PendingRequests deliveries_;
    bool dispatching_ = false;
};

}

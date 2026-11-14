#pragma once

#include <codecairn/Engine.h>
#include <QObject>
#include <memory>

namespace codecairn::detail {

class AnalysisScheduler final : public QObject {
    Q_OBJECT
public:
    explicit AnalysisScheduler(QObject *parent = nullptr);
    ~AnalysisScheduler() override;
    void submit(RequestId request, Project *project, quint64 generation,
                const SourceSnapshot &source, AnalysisDetail detail,
                const QString &compilationDirectory);
    void cancel(RequestId request);
    void cancel(Project *owner, RequestId request);
    void cancelProject(Project *project);
    void shutdown();

signals:
    void completed(codecairn::RequestId request, codecairn::Project *project,
                   const codecairn::AnalysisResult &result);

private:
    static constexpr int MaximumQueuedJobs = 128;
    static constexpr int MaximumSubscribers = 512;
    void reject(RequestId request, Project *project, const SourceSnapshot &source);
    void deliverRejections();
    struct State;
    std::unique_ptr<State> state_;
};

}

#include "AnalysisScheduler.h"
#include "PendingRequests.h"
#include <codecairn/Project.h>
#include <codecairn/RuntimeLimits.h>
#include <QFutureWatcher>
#include <QHash>
#include <QPointer>
#include <QQueue>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrentRun>
#include <atomic>
#include <utility>

namespace codecairn::detail {

namespace {
struct AnalysisKey {
    Project *project;
    quint64 generation;
    SourceSnapshot source;
    AnalysisDetail detail;
    QString compilationDirectory;

    bool operator==(const AnalysisKey &other) const {
        return project == other.project && generation == other.generation &&
               source.path == other.source.path && source.revision == other.source.revision &&
               source.hash == other.source.hash && detail == other.detail &&
               compilationDirectory == other.compilationDirectory &&
               source.bytes == other.source.bytes;
    }
};

size_t qHash(const AnalysisKey &key, size_t seed = 0) {
    return qHashMulti(seed, key.project, key.generation, key.source.path, key.source.revision,
                      key.source.hash, static_cast<int>(key.detail), key.compilationDirectory);
}
}

struct AnalysisScheduler::State {
    struct Job {
        enum class Phase { Queued, Running, Finished, Cancelled };
        AnalysisKey key;
        QPointer<Project> project;
        QVector<RequestId> subscribers;
        std::atomic<Phase> phase{Phase::Queued};
    };
    struct Subscription {
        std::shared_ptr<Job> job;
        SourceSnapshot source;
    };
    struct Rejection {
        Project *context;
        QPointer<Project> project;
        SourceSnapshot source;
    };
    QThreadPool pool;
    QHash<AnalysisKey, std::shared_ptr<Job>> jobs;
    QHash<QFutureWatcher<AnalysisResult> *, std::shared_ptr<Job>> futures;
    QHash<quint64, Subscription> subscriptions;
    QHash<quint64, Rejection> rejections;
    QQueue<RequestId> rejected;
    PendingRequests requests;
    bool stopping = false;
    bool rejectionDeliveryQueued = false;
};

AnalysisScheduler::AnalysisScheduler(QObject *parent)
    : QObject(parent), state_(std::make_unique<State>()) {
    state_->pool.setMaxThreadCount(RuntimeLimits::WorkerThreads);
    state_->jobs.reserve(MaximumQueuedJobs + RuntimeLimits::WorkerThreads);
    state_->futures.reserve(MaximumQueuedJobs + RuntimeLimits::WorkerThreads);
    state_->subscriptions.reserve(MaximumSubscribers);
}

AnalysisScheduler::~AnalysisScheduler() {
    shutdown();
}

void AnalysisScheduler::submit(RequestId request, Project *project, quint64 generation,
                               const SourceSnapshot &source, AnalysisDetail detail,
                               const QString &compilationDirectory) {
    Q_ASSERT(QThread::currentThread() == thread());
    Q_ASSERT(!state_->stopping);
    Q_ASSERT(request && !state_->requests.contains(request));
    if (state_->subscriptions.size() >= MaximumSubscribers) {
        reject(request, project, source);
        return;
    }
    const AnalysisKey key{project, generation, source, detail, compilationDirectory};
    auto job = state_->jobs.value(key);
    const bool created = !job;
    if (!job) {
        if (state_->futures.size() >= MaximumQueuedJobs + RuntimeLimits::WorkerThreads) {
            reject(request, project, source);
            return;
        }
        job = std::make_shared<State::Job>();
        job->key = key;
        job->project = project;
        state_->jobs.insert(key, job);
    }
    job->subscribers.append(request);
    state_->subscriptions.insert(request.value, {job, source});
    state_->requests.add(request);
    if (!created)
        return;
    auto *watcher = new QFutureWatcher<AnalysisResult>(this);
    state_->futures.insert(watcher, job);
    connect(watcher, &QFutureWatcher<AnalysisResult>::finished, this, [this, watcher, job] {
        const auto result = watcher->result();
        state_->futures.remove(watcher);
        const auto indexed = state_->jobs.find(job->key);
        if (indexed != state_->jobs.end() && indexed.value() == job)
            state_->jobs.erase(indexed);
        watcher->deleteLater();
        if (job->subscribers.size() == 1) {
            const auto request = job->subscribers.first();
            if (!state_->requests.finish(request))
                return;
            const auto subscription = state_->subscriptions.find(request.value);
            Q_ASSERT(subscription != state_->subscriptions.end());
            if (job->key.project && !job->project) {
                state_->subscriptions.erase(subscription);
                return;
            }
            if (subscription->source.bytes.constData() == result.source.bytes.constData()) {
                state_->subscriptions.erase(subscription);
                emit completed(request, job->project.data(), result);
            } else {
                const AnalysisResult delivery{subscription->source, result.analysis};
                state_->subscriptions.erase(subscription);
                emit completed(request, job->project.data(), delivery);
            }
            return;
        }
        // Delivery can synchronously cancel another subscriber or destroy its project.
        const auto subscribers = job->subscribers;
        QPointer<AnalysisScheduler> alive(this);
        for (const auto request : subscribers) {
            if (!state_->requests.finish(request))
                continue;
            const auto subscription = state_->subscriptions.take(request.value);
            if (job->key.project && !job->project)
                continue;
            AnalysisResult delivery{subscription.source, result.analysis};
            emit completed(request, job->project.data(), delivery);
            if (!alive)
                return;
        }
    });
    const auto mode =
        detail == AnalysisDetail::Symbols ? SourceAnalysisMode::Index : SourceAnalysisMode::Editor;
    watcher->setFuture(QtConcurrent::run(&state_->pool, [job, source,
                                                         directory = compilationDirectory, mode] {
        auto expected = State::Job::Phase::Queued;
        if (!job->phase.compare_exchange_strong(expected, State::Job::Phase::Running,
                                                std::memory_order_relaxed))
            return AnalysisResult{source, {}};
        AnalysisResult result{source, analyzeSource(source.bytes, source.path, mode, directory)};
        job->phase.store(State::Job::Phase::Finished, std::memory_order_relaxed);
        return result;
    }));
}

void AnalysisScheduler::reject(RequestId request, Project *project, const SourceSnapshot &source) {
    state_->requests.add(request);
    state_->rejections.insert(request.value, {project, project, source});
    state_->rejected.enqueue(request);
    if (state_->rejectionDeliveryQueued)
        return;
    state_->rejectionDeliveryQueued = true;
    QTimer::singleShot(0, this, &AnalysisScheduler::deliverRejections);
}

void AnalysisScheduler::deliverRejections() {
    const auto count = state_->rejected.size();
    QPointer<AnalysisScheduler> alive(this);
    for (qsizetype i = 0; i < count && !state_->rejected.isEmpty(); ++i) {
        const auto request = state_->rejected.dequeue();
        const auto rejection = state_->rejections.take(request.value);
        if (!state_->requests.finish(request) || (rejection.context && !rejection.project))
            continue;
        AnalysisResult result;
        result.source = rejection.source;
        result.analysis.error = "Analysis queue is full; retry after pending work completes.";
        emit completed(request, rejection.project.data(), result);
        if (!alive)
            return;
    }
    if (state_->rejected.isEmpty())
        state_->rejectionDeliveryQueued = false;
    else
        QTimer::singleShot(0, this, &AnalysisScheduler::deliverRejections);
}

void AnalysisScheduler::cancel(RequestId request) {
    Q_ASSERT(QThread::currentThread() == thread());
    state_->requests.cancel(request);
    if (state_->rejections.remove(request.value))
        state_->rejected.removeAll(request);
    const auto found = state_->subscriptions.find(request.value);
    if (found == state_->subscriptions.end())
        return;
    const auto job = found->job;
    state_->subscriptions.erase(found);
    job->subscribers.removeAll(request);
    if (!job->subscribers.isEmpty())
        return;
    const auto indexed = state_->jobs.find(job->key);
    if (indexed != state_->jobs.end() && indexed.value() == job)
        state_->jobs.erase(indexed);
    auto queued = State::Job::Phase::Queued;
    job->phase.compare_exchange_strong(queued, State::Job::Phase::Cancelled,
                                       std::memory_order_relaxed);
}

void AnalysisScheduler::cancel(Project *owner, RequestId request) {
    Q_ASSERT(QThread::currentThread() == thread());
    const auto subscription = state_->subscriptions.constFind(request.value);
    if (subscription != state_->subscriptions.cend() && subscription->job->key.project == owner) {
        cancel(request);
        return;
    }
    const auto rejection = state_->rejections.constFind(request.value);
    if (rejection != state_->rejections.cend() && rejection->context == owner)
        cancel(request);
}

void AnalysisScheduler::cancelProject(Project *project) {
    Q_ASSERT(QThread::currentThread() == thread());
    QVector<RequestId> cancelled;
    for (auto it = state_->subscriptions.cbegin(); it != state_->subscriptions.cend(); ++it)
        if (it->job->key.project == project)
            cancelled.append({it.key()});
    for (auto it = state_->rejections.cbegin(); it != state_->rejections.cend(); ++it)
        if (it->context == project)
            cancelled.append({it.key()});
    for (const auto request : cancelled)
        cancel(request);
}

void AnalysisScheduler::shutdown() {
    Q_ASSERT(QThread::currentThread() == thread());
    if (state_->stopping)
        return;
    state_->stopping = true;
    state_->requests.clear();
    state_->subscriptions.clear();
    state_->rejections.clear();
    state_->rejected.clear();
    state_->jobs.clear();
    for (const auto &job : std::as_const(state_->futures)) {
        auto queued = State::Job::Phase::Queued;
        job->phase.compare_exchange_strong(queued, State::Job::Phase::Cancelled,
                                           std::memory_order_relaxed);
    }
    for (auto *watcher : state_->futures.keys()) {
        disconnect(watcher, nullptr, this, nullptr);
        watcher->waitForFinished();
    }
    state_->futures.clear();
    state_->pool.waitForDone();
}

}

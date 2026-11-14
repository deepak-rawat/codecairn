#pragma once

#include <codecairn/Document.h>
#include <codecairn/Engine.h>
#include <codecairn/FileIO.h>
#include <QFutureWatcher>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <limits>

class CodeEditor;
namespace codecairn {
class Project;
}

class SourceDocument final : public QObject {
    Q_OBJECT
public:
    SourceDocument(codecairn::Document *source, codecairn::Engine *engine, int analysisDebounceMs,
                   QObject *parent = nullptr);
    ~SourceDocument() override;

    codecairn::Document *source() const { return source_; }
    const QString &path() const { return source_->path(); }
    quint64 revision() const { return source_->revision(); }
    const QByteArray &savedBytes() const { return source_->savedBytes(); }
    bool hasUtf8Bom() const { return source_->hasUtf8Bom(); }
    const QByteArray &savedContentHash() const { return source_->savedContentHash(); }
    const QByteArray &snapshotBytes() const { return source_->contents(); }
    const QByteArray &snapshotHash() const { return source_->contentHash(); }
    void setSavedContents(const QByteArray &bytes, bool hasUtf8Bom);
    void scheduleAnalysis();
    void setAnalysisDebounceMs(int milliseconds);
    void setProject(codecairn::Project *project);
    void startAnalysis();
    bool isAnalysisRunning() const { return analysisBusy_; }
    void stopBackgroundWork();
    void reconcileDiskHash(const QByteArray &hash);

    CodeEditor *editor = nullptr;
    quint64 analyzedRevision = std::numeric_limits<quint64>::max();
    codecairn::SourceAnalysis analysis;
    QSet<QString> collapsedOutlineKeys;
    QString selectedOutlineKey;
    int outlineScroll = 0;
    bool outlineSelectionPending = false;
    bool isPreview = false;

signals:
    void analysisCompleted();
    void analysisRejected(const QString &error);
    void diskReloaded();
    void backgroundError(const QString &error);
    void statusMessage(const QString &message, int durationMs);

private:
    void acceptAnalysis(codecairn::RequestId request, const codecairn::AnalysisResult &result);
    void cancelAnalysis();

    QPointer<codecairn::Document> source_;
    QPointer<codecairn::Engine> engine_;
    QPointer<codecairn::Project> project_;
    QPointer<codecairn::Project> runningProject_;
    QMetaObject::Connection projectAnalysisConnection_;
    QMetaObject::Connection contextAnalysisConnection_;
    QMetaObject::Connection projectDestroyedConnection_;
    QTimer analysisTimer_;
    codecairn::RequestId analysisRequest_;
    bool analysisBusy_ = false;
    bool stopped_ = false;
    QFutureWatcher<FileContents> reloadWatcher_;
    bool reloading_ = false;
};

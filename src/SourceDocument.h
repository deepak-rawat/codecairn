#pragma once

#include "FileIO.h"
#include "SourceAnalysis.h"
#include <QFutureWatcher>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <limits>

class CodeEditor;

class SourceDocument final : public QObject {
    Q_OBJECT
public:
    SourceDocument(const QString &path, const QByteArray &bytes, bool hasUtf8Bom,
                   int analysisDebounceMs, QObject *parent = nullptr);
    ~SourceDocument() override;

    const QByteArray &snapshotBytes();
    const QByteArray &savedContentHash() const { return savedContentHash_; }
    void setSavedContents(const QByteArray &bytes, bool hasUtf8Bom);
    void scheduleAnalysis() { analysisTimer_.start(); }
    void setAnalysisDebounceMs(int milliseconds) { analysisTimer_.setInterval(milliseconds); }
    void startAnalysis();
    void stopBackgroundWork();
    void reconcileDiskHash(const QByteArray &hash);
    bool isAnalysisRunning() const { return analysisBusy_; }

    CodeEditor *editor = nullptr;
    QString path;
    QByteArray savedBytes;
    bool hasUtf8Bom = false;
    quint64 revision = 0;
    quint64 analyzedRevision = 0;
    SourceAnalysis analysis;
    QSet<QString> collapsedOutlineKeys;
    QString selectedOutlineKey;
    int outlineScroll = 0;
    bool outlineSelectionPending = false;
    QByteArray lastReportedExternalHash;
    bool isPreview = false;
    QByteArray snapshotHash;

signals:
    void analysisCompleted();
    void analysisRejected(const QString &error);
    void diskReloaded();
    void backgroundError(const QString &error);
    void statusMessage(const QString &message, int durationMs);

private:
    QTimer analysisTimer_;
    QFutureWatcher<SourceAnalysis> analysisWatcher_;
    QFutureWatcher<FileContents> reloadWatcher_;
    bool reloading_ = false;
    bool analysisBusy_ = false;
    quint64 runningRevision_ = 0;
    QByteArray snapshot_;
    QByteArray savedContentHash_;
    quint64 snapshotRevision_ = std::numeric_limits<quint64>::max();
};

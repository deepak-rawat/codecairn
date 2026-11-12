#pragma once

#include <codecairn/SourceAnalysis.h>
#include <QFutureWatcher>
#include <QObject>
#include <QTimer>
#include <limits>

class DocumentAnalysis : public QObject {
    Q_OBJECT
public:
    enum class ExternalChange { None, Removed, UnsavedBuffer, Reload };

    DocumentAnalysis(const QString &path, const QByteArray &bytes, bool hasUtf8Bom,
                     int analysisDebounceMs, QObject *parent = nullptr);
    ~DocumentAnalysis() override;

    void setBytes(const QByteArray &bytes);
    const QByteArray &snapshotBytes();
    const QByteArray &savedContentHash() const { return savedContentHash_; }
    void setSavedContents(const QByteArray &bytes, bool hasUtf8Bom);
    void scheduleAnalysis() { analysisTimer_.start(); }
    void setAnalysisDebounceMs(int milliseconds) { analysisTimer_.setInterval(milliseconds); }
    void startAnalysis();
    void setCompilationDirectory(const QString &directory);
    virtual void stopBackgroundWork();
    bool isAnalysisRunning() const { return analysisBusy_; }
    ExternalChange externalChange(const QByteArray &hash, bool fileExists, bool modified);

    QString path;
    QByteArray savedBytes;
    bool hasUtf8Bom = false;
    quint64 revision = 0;
    quint64 analyzedRevision = std::numeric_limits<quint64>::max();
    SourceAnalysis analysis;
    QByteArray lastReportedExternalHash;
    QByteArray snapshotHash;

signals:
    void analysisCompleted();
    void analysisRejected(const QString &error);

protected:
    // Called synchronously once per revision; an editor binding remains the live byte authority.
    virtual QByteArray readSnapshotBytes() const;

private:
    QTimer analysisTimer_;
    QFutureWatcher<SourceAnalysis> analysisWatcher_;
    bool analysisBusy_ = false;
    quint64 runningRevision_ = 0;
    QString compilationDirectory_;
    QString runningCompilationDirectory_;
    QByteArray snapshot_;
    QByteArray savedContentHash_;
    quint64 snapshotRevision_ = std::numeric_limits<quint64>::max();
};

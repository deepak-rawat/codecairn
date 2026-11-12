#pragma once

#include <codecairn/DocumentAnalysis.h>
#include <codecairn/FileIO.h>
#include <QSet>

class CodeEditor;

class SourceDocument final : public DocumentAnalysis {
    Q_OBJECT
public:
    SourceDocument(const QString &path, const QByteArray &bytes, bool hasUtf8Bom,
                   int analysisDebounceMs, QObject *parent = nullptr);
    ~SourceDocument() override;

    void stopBackgroundWork() override;
    void reconcileDiskHash(const QByteArray &hash);

    CodeEditor *editor = nullptr;
    QSet<QString> collapsedOutlineKeys;
    QString selectedOutlineKey;
    int outlineScroll = 0;
    bool outlineSelectionPending = false;
    bool isPreview = false;

signals:
    void diskReloaded();
    void backgroundError(const QString &error);
    void statusMessage(const QString &message, int durationMs);

private:
    using DocumentAnalysis::setBytes;
    QByteArray readSnapshotBytes() const override;

    QFutureWatcher<FileContents> reloadWatcher_;
    bool reloading_ = false;
};

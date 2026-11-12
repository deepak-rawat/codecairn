#pragma once

#include <codecairn/DocumentAnalysis.h>
#include <codecairn/NavigationService.h>
#include <codecairn/ReferenceSession.h>

class CoreSession final : public QObject {
    Q_OBJECT
public:
    explicit CoreSession(QObject *parent = nullptr);
    ~CoreSession() override;

    WorkspaceServices &projects() { return projects_; }
    WorkspaceIndex &index() { return *projects_.index(); }
    NavigationService &navigation() { return navigation_; }
    ReferenceSession &references() { return references_; }

    void setSnapshotProvider(std::function<std::optional<QByteArray>(const QString &)> provider);
    DocumentAnalysis *createDocument(const QString &path, const QByteArray &bytes,
                                     bool hasUtf8Bom = false, int analysisDebounceMs = 0);
    QString closeDocument(DocumentAnalysis *document);
    void closeProject();

private:
    WorkspaceServices projects_;
    NavigationService navigation_;
    ReferenceSession references_;
};

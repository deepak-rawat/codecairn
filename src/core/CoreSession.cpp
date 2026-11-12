#include <codecairn/CoreSession.h>
#include <codecairn/Diagnostics.h>
#include <utility>

CoreSession::CoreSession(QObject *parent)
    : QObject(parent), projects_(this), navigation_(&projects_, this),
      references_(&projects_, this) {
    connect(&projects_, &WorkspaceServices::projectAboutToChange, this, [this] {
        references_.cancel();
        navigation_.cancelLookups();
        navigation_.cancelRead();
    });
    setSnapshotProvider([this](const QString &path) -> std::optional<QByteArray> {
        for (auto *document :
             findChildren<DocumentAnalysis *>(QString{}, Qt::FindDirectChildrenOnly))
            if (document->path == path)
                return document->snapshotBytes();
        return {};
    });
}

CoreSession::~CoreSession() {
    references_.cancel();
    navigation_.cancelLookups();
    navigation_.cancelRead();
    for (auto *document : findChildren<DocumentAnalysis *>(QString{}, Qt::FindDirectChildrenOnly))
        delete document;
}

void CoreSession::setSnapshotProvider(
    std::function<std::optional<QByteArray>(const QString &)> provider) {
    navigation_.setSnapshotProvider(provider);
    references_.setSnapshotProvider(std::move(provider));
}

DocumentAnalysis *CoreSession::createDocument(const QString &path, const QByteArray &bytes,
                                              bool hasUtf8Bom, int analysisDebounceMs) {
    return new DocumentAnalysis(path, bytes, hasUtf8Bom, analysisDebounceMs, this);
}

QString CoreSession::closeDocument(DocumentAnalysis *document) {
    if (!document || document->parent() != this) {
        const QString error = "Document does not belong to this core session.";
        qCWarning(analysisLog) << "document.close_failed" << error;
        return error;
    }
    delete document;
    return {};
}

void CoreSession::closeProject() {
    projects_.closeProject();
}

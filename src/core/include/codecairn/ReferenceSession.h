#pragma once

#include <codecairn/ReferenceSearch.h>
#include <codecairn/WorkspaceServices.h>
#include <codecairn/WorkspaceOptionsStore.h>
#include <codecairn/DocumentAnalysis.h>

#include <QFutureWatcher>
#include <QPointer>
#include <functional>
#include <optional>

struct SemanticReferenceOrigin {
    QString path;
    QByteArray source;
    int version = 0;
    int position = 0;
    bool declarations = true;
    QHash<QString, int> versions;
};

struct OpenSource {
    DocumentAnalysis *document;
    bool modified;
};

enum class ReferenceScope { Project, CurrentFile, SelectedFiles };

struct ReferenceInputs {
    ReferenceScope scope = ReferenceScope::Project;
    QString currentFile;
    QStringList selectedFiles;
    QStringList catalog;
    QVector<SymbolOverlay> overlays;
    QString root;
    WorkspaceOptions options;
    QString filePatterns;
};

QVector<SymbolOverlay> captureDocumentOverlays(const QVector<OpenSource> &documents,
                                               const QString &root,
                                               const WorkspaceOptions &options);
QStringList resolveReferenceScope(const ReferenceInputs &inputs);

QStringList referenceSearchPaths(const QStringList &catalog, const QVector<SymbolOverlay> &overlays,
                                 const QString &root, const WorkspaceOptions &options,
                                 const QString &filePatterns);

class ReferenceSession final : public QObject {
    Q_OBJECT
public:
    explicit ReferenceSession(WorkspaceServices *services, QObject *parent = nullptr);
    ~ReferenceSession() override;
    void setSnapshotProvider(std::function<std::optional<QByteArray>(const QString &)> provider);
    void start(const ReferenceSearchRequest &request, quint64 token,
               const std::optional<SemanticReferenceOrigin> &origin = {});
    void nextPage(quint64 token);
    void cancel();

signals:
    void pageReady(quint64 token, const ReferenceResults &results, bool semantic);

private:
    void searchPage(quint64 token);
    void fail(quint64 token, const QString &error);
    std::optional<QByteArray> liveSource(const QString &path) const;
    WorkspaceServices *services_;
    QPointer<LanguageServerClient> service_;
    QMetaObject::Connection semanticConnection_;
    ReferenceSearchRequest request_;
    QHash<QString, QByteArray> overlaySources_;
    QVector<LspLocation> targets_;
    std::shared_ptr<std::atomic_bool> cancelled_;
    std::function<std::optional<QByteArray>(const QString &)> snapshotProvider_;
    quint64 generation_ = 0;
    bool semantic_ = false;
    bool busy_ = false;
};

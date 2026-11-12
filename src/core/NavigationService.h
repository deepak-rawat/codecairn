#pragma once

#include "DefinitionResolver.h"
#include "NavigationTarget.h"
#include "WorkspaceServices.h"

#include <QFutureWatcher>
#include <QPointer>
#include <functional>

enum class DefinitionPolicy { SyntaxOnly, SemanticOnly, PreferSemantic };

struct DefinitionRequest {
    QString root;
    QString projectFile;
    QString path;
    QByteArray source;
    int version = 0;
    int position = 0;
    QString identifier;
    int identifierStart = -1;
    int identifierEnd = -1;
    SourceAnalysis analysis;
    QVector<SymbolOverlay> overlays;
    QHash<QString, int> versions;
    int offset = 0;
    DefinitionPolicy policy = DefinitionPolicy::PreferSemantic;
    bool context = false;
};

struct DefinitionResult {
    QVector<LspLocation> locations;
    DefinitionCandidates candidates;
    bool semantic = false;
    QString provider;
    QString warning;
    QString error;
};

struct NavigationRead {
    NavigationLocation location;
    FileContents file;
    ResolvedNavigation range;
};

class NavigationService final : public QObject {
    Q_OBJECT
public:
    explicit NavigationService(WorkspaceServices *services, QObject *parent = nullptr);
    ~NavigationService() override;
    void setSnapshotProvider(std::function<std::optional<QByteArray>(const QString &)> provider);
    void lookup(const DefinitionRequest &request, quint64 token, const QString &channel);
    void cancelLookup(const QString &channel);
    void cancelLookups();
    void read(const NavigationLocation &location, quint64 token,
              const std::optional<IndexedSymbol> &indexedTarget = {});
    void cancelRead();

signals:
    void definitionReady(quint64 token, const QString &channel, const DefinitionResult &result);
    void readReady(quint64 token, const NavigationRead &result);

private:
    struct Lookup {
        DefinitionRequest request;
        quint64 token = 0;
        quint64 generation = 0;
        QPointer<LanguageServerClient> service;
        QMetaObject::Connection connection;
        QString warning;
    };
    struct ReadRequest {
        NavigationLocation location;
        quint64 token;
        quint64 generation;
        std::optional<IndexedSymbol> indexedTarget;
    };
    void semanticReady(const QString &channel, quint64 generation,
                       const QVector<LspLocation> &locations, const QString &error);
    void syntaxLookup(const QString &channel, quint64 generation, const QString &warning);
    void complete(const QString &channel, quint64 generation, DefinitionResult result);
    bool fresh(const DefinitionRequest &request) const;
    void startRead();
    WorkspaceServices *services_;
    QHash<QString, Lookup> lookups_;
    QHash<QString, DefinitionResolver *> resolvers_;
    quint64 generation_ = 0;
    std::function<std::optional<QByteArray>(const QString &)> snapshots_;
    QFutureWatcher<NavigationRead> reader_;
    std::optional<ReadRequest> pendingRead_;
    quint64 readGeneration_ = 0;
    bool reading_ = false;
    quint64 runningReadToken_ = 0;
    quint64 runningReadGeneration_ = 0;
};

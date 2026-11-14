#pragma once

#include <codecairn/Definitions.h>
#include "PendingRequests.h"
#include "ProjectLanguages.h"
#include <QPointer>
#include <functional>

class DefinitionResolver;

namespace codecairn::detail {

struct DefinitionInputs {
    DefinitionQuery query;
    QString root;
    QString projectFile;
    codecairn::SourceAnalysis originAnalysis;
    QVector<codecairn::SymbolOverlay> overlays;
    QHash<QString, int> versions;
};

class DefinitionQueryExecutor final : public QObject {
    Q_OBJECT
public:
    explicit DefinitionQueryExecutor(ProjectLanguages *languages, QObject *parent = nullptr);
    ~DefinitionQueryExecutor() override;
    void setSnapshotProvider(std::function<std::optional<QByteArray>(const QString &)> provider);
    void submit(const DefinitionInputs &inputs, RequestId request);
    void cancel(RequestId request);
    void cancelAll();

signals:
    void ready(codecairn::RequestId request, const codecairn::DefinitionPage &page);

private:
    struct Lookup {
        DefinitionInputs inputs;
        quint64 generation = 0;
        QPointer<LanguageServerClient> service;
        QMetaObject::Connection connection;
        QPointer<DefinitionResolver> resolver;
        QString channel;
        QString warning;
    };
    void semanticReady(RequestId request, quint64 generation, const QVector<LspLocation> &locations,
                       const QString &error);
    void syntaxLookup(RequestId request, quint64 generation, const QString &warning);
    void complete(RequestId request, quint64 generation, DefinitionPage page);
    bool fresh(const SourceSnapshot &snapshot) const;
    ProjectLanguages *languages_;
    QHash<quint64, Lookup> lookups_;
    PendingRequests pending_;
    quint64 generation_ = 0;
    std::function<std::optional<QByteArray>(const QString &)> snapshots_;
};

}

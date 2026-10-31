#pragma once

#include "IndexTypes.h"
#include <QFutureWatcher>
#include <QObject>
#include <optional>

struct DefinitionCandidates {
    QVector<IndexedSymbol> targets;
    int total = 0;
    bool complete = false;
    QString error;
};

DefinitionCandidates definitionCandidates(const SymbolPage &page);

class DefinitionResolver final : public QObject {
    Q_OBJECT
public:
    explicit DefinitionResolver(QObject *parent = nullptr);
    ~DefinitionResolver() override;
    void request(const QString &root, const SymbolQuery &query, quint64 token);

signals:
    void candidatesReady(quint64 token, const DefinitionCandidates &candidates);

private:
    struct Request {
        QString root;
        SymbolQuery query;
        quint64 token;
    };
    void startPending();
    QFutureWatcher<SymbolPage> watcher_;
    std::optional<Request> pending_;
    quint64 runningToken_ = 0;
    bool busy_ = false;
};

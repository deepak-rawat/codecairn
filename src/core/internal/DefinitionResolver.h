#pragma once

#include <codecairn/NavigationTarget.h>
#include <QFutureWatcher>
#include <QObject>
#include <optional>

DefinitionCandidates definitionCandidates(const SymbolPage &page, int offset = 0);

class DefinitionResolver final : public QObject {
    Q_OBJECT
public:
    explicit DefinitionResolver(QObject *parent = nullptr);
    ~DefinitionResolver() override;
    void request(const QString &root, const SymbolQuery &query, quint64 token);
    void discard();

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
    int runningOffset_ = 0;
    bool busy_ = false;
    bool discarded_ = false;
};

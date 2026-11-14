#pragma once
#include <codecairn/SourceLocation.h>
#include <codecairn/IndexTypes.h>
#include <codecairn/SourceCoordinates.h>
#include <codecairn/SearchOptions.h>
#include <QJsonArray>
#include <functional>

struct ReferenceHit {
    codecairn::SourceLocation location;
    QString text;
    int line = 0;
    QString container;
    bool stale = false;
    bool linked = true;
};
struct ReferenceResults {
    QVector<ReferenceHit> hits;
    QStringList warnings;
    bool cancelled = false;
    bool truncated = false;
    QString error;
    int filesSearched = 0;
    QHash<QString, QByteArray> sourceHashes;
};

struct ReferenceSearchRequest {
    QString query;
    QStringList paths;
    QVector<codecairn::SymbolOverlay> overlays;
    SearchOptions options;
    QString workspaceRoot;
    int offset = 0;
    int limit = RuntimeLimits::ReferenceResults;
    QHash<QString, QByteArray> expectedHashes;
    QString projectFile;
};

struct ReferenceGroup {
    QString title;
    SearchOutputOptions output;
    QVector<ReferenceHit> hits;
    QString query;
};

class ReferenceHistoryModel {
public:
    QString append(const ReferenceGroup &group, bool replace);
    bool commitReplacement(const std::function<bool()> &persist);
    QVector<ReferenceGroup> groups;
    QVector<ReferenceGroup> pending;
};

QJsonArray referenceHistoryJson(const QVector<ReferenceGroup> &groups);
QVector<ReferenceGroup> parseReferenceHistory(const QJsonArray &json, const QString &root,
                                              QString &error);
QString exportReferenceHistory(const QString &path, const QVector<ReferenceGroup> &groups,
                               const QString &root);

QString validateReferenceSearch(const QString &query, const SearchOptions &options);
ReferenceResults searchReferences(const ReferenceSearchRequest &request,
                                  const std::shared_ptr<std::atomic_bool> &cancel);
bool adjustReferenceHit(ReferenceHit &hit, int position, int removed, int inserted,
                        const QByteArray &beforeHash, const QByteArray &source,
                        const QByteArray &afterHash = {});
QStringList touchReferenceFiles(const QVector<ReferenceHit> &hits);

ReferenceResults findTextOccurrences(const QStringList &paths, const QString &identifier,
                                     const QVector<codecairn::SymbolOverlay> &overlays,
                                     bool includeComments, bool includeStrings,
                                     const std::shared_ptr<std::atomic_bool> &cancel);
ReferenceResults resolveSemanticReferences(const QVector<LspLocation> &targets,
                                           const QStringList &paths, const QString &identifier,
                                           const QVector<codecairn::SymbolOverlay> &overlays,
                                           const std::shared_ptr<std::atomic_bool> &cancel,
                                           int offset = 0,
                                           int limit = RuntimeLimits::ReferenceResults,
                                           bool skipInactive = false,
                                           const QHash<QString, QByteArray> &expectedHashes = {},
                                           const QString &compilationDirectory = {});

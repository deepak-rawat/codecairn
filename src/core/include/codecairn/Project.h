#pragma once

#include <codecairn/Engine.h>
#include <codecairn/Queries.h>
#include <codecairn/ReferenceSearch.h>
#include <codecairn/Indexing.h>
#include <codecairn/Symbols.h>

namespace codecairn {

using ProjectConfiguration = WorkspaceOptions;
struct ProjectInfo {
    QString root;
    QString projectFile;
    QString name;
    ProjectConfiguration configuration;
};

class Project final : public QObject {
    Q_OBJECT
public:
    ~Project() override;
    const ProjectInfo &info() const;
    Result<void> open(const PreparedProject &prepared, const ProjectOptions &options = {});
    Result<void> open(const ProjectSpec &spec, const ProjectOptions &options = {}) {
        return open(prepareProject(spec), options);
    }
    void close();
    Result<void> reloadOptions();
    bool contains(const QString &path) const;
    bool indexing() const;
    QStringList files() const;
    QStringList files(const SearchFiles &scope) const;
    QStringList excludedDirectories() const;
    void setExcludedDirectories(const QStringList &names);
    void setRefreshIntervalMinutes(int minutes);
    RequestId refreshIndex(IndexRefresh mode = IndexRefresh::DiscoverChanges);
    RequestId findSymbols(const SymbolQuery &query);
    RequestId findDefinitions(const DefinitionQuery &query);
    RequestId findReferences(const ReferenceQuery &query);
    RequestId searchText(const TextQuery &query);
    RequestId continueSearch(SearchCursor cursor);
    RequestId analyze(const SourceSnapshot &snapshot,
                      AnalysisDetail detail = AnalysisDetail::StylesAndSymbols);
    void cancel(RequestId request);
    void cancelIndexing();
    void cancelSymbolQuery();
    void restartLanguageServices();
    void synchronizeDocument(const SourceSnapshot &snapshot);
    Result<void> recreateIndex();

signals:
    void indexReady(codecairn::RequestId request, const IndexResult &result);
    void catalogReady(const QStringList &files);
    void indexBatchReady();
    void indexingChanged(bool busy);
    void symbolsReady(codecairn::RequestId request, const SymbolPage &page);
    void definitionsReady(codecairn::RequestId request, const codecairn::DefinitionPage &page);
    void referencesReady(codecairn::RequestId request, const codecairn::SearchPage &page);
    void textMatchesReady(codecairn::RequestId request, const codecairn::SearchPage &page);
    void analysisReady(codecairn::RequestId request, const codecairn::AnalysisResult &result);
    void failed(const QString &error);
    void status(const QString &message);
    void languageReady();
    void contextChanged();

private:
    friend class Engine;
    explicit Project(Engine *engine);
    RequestId startSearch(ReferenceSearchRequest request,
                          const std::optional<SourcePosition> &origin, bool references,
                          bool declarations);
    SourceAnalysis cachedAnalysis(const SourceSnapshot &snapshot) const;
    struct State;
    std::unique_ptr<State> state_;
};

}

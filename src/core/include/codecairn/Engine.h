#pragma once

#include <codecairn/Document.h>
#include <codecairn/Definitions.h>
#include <codecairn/ProjectSession.h>
#include <codecairn/Result.h>
#include <QObject>
#include <memory>

namespace codecairn {

class Project;
struct ProjectSpec {
    enum class Kind { Folder, Managed };
    Kind kind;
    QString path;
    static ProjectSpec folder(QString path) { return {Kind::Folder, std::move(path)}; }
    static ProjectSpec managed(QString path) { return {Kind::Managed, std::move(path)}; }
};

PreparedProject prepareProject(const ProjectSpec &spec);

enum class IndexStartup { Automatic, Manual };
enum class LanguageStartup { Automatic, Manual };
struct ProjectOptions {
    IndexStartup indexing = IndexStartup::Automatic;
    LanguageStartup languages = LanguageStartup::Automatic;
};
enum class AnalysisDetail { Symbols, StylesAndSymbols };
struct AnalysisResult {
    SourceSnapshot source;
    codecairn::SourceAnalysis analysis;
};

class Engine final : public QObject {
    Q_OBJECT
public:
    explicit Engine(QObject *parent = nullptr);
    ~Engine() override;
    Result<Project *> openProject(const PreparedProject &project,
                                  const ProjectOptions &options = {});
    Result<Project *> openProject(const ProjectSpec &project, const ProjectOptions &options = {});
    Project *createProject();
    Result<Document *> openDocument(const QString &path);
    Result<Document *> createDocument(const QString &path, const QByteArray &contents,
                                      bool hasUtf8Bom = false);
    Document *document(const QString &path) const;
    Result<void> renameDocument(Document *document, const QString &path);
    Result<void> closeDocument(Document *document);
    Result<void> closeProject(Project *project);
    RequestId analyze(const SourceSnapshot &snapshot,
                      AnalysisDetail detail = AnalysisDetail::StylesAndSymbols);
    RequestId readSource(const SourceLocation &location,
                         const std::optional<codecairn::IndexedSymbol> &symbol = {});
    void cancel(RequestId request);

signals:
    void analysisReady(codecairn::RequestId request, const codecairn::AnalysisResult &result);
    void sourceReady(codecairn::RequestId request, const codecairn::SourceRead &result);
    void documentClosed(const QString &path);

private:
    friend class Project;
    RequestId nextRequest();
    QVector<codecairn::SymbolOverlay> overlays(const Project *project) const;
    RequestId analyzeFor(Project *project, const SourceSnapshot &snapshot, AnalysisDetail detail);
    void cancelFor(Project *project);
    void cancelFor(Project *project, RequestId request);
    void startRead();
    struct State;
    std::unique_ptr<State> state_;
};

}

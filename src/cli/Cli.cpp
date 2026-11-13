#include <codecairn/Engine.h>
#include <codecairn/FileIO.h>
#include <codecairn/Project.h>
#include <codecairn/ProjectSession.h>
#include <codecairn/ProjectDiscovery.h>
#include <codecairn/Indexing.h>

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFutureWatcher>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrentRun>
#include <csignal>
#include <functional>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace {
#ifdef Q_OS_WIN
volatile LONG interrupted = 0;

BOOL WINAPI interrupt(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT)
        return FALSE;
    InterlockedExchange(&interrupted, 1);
    return TRUE;
}

bool wasInterrupted() {
    return InterlockedCompareExchange(&interrupted, 0, 0) != 0;
}
#else
volatile std::sig_atomic_t interrupted = 0;

void interrupt(int) {
    interrupted = 1;
}

bool wasInterrupted() {
    return interrupted != 0;
}
#endif

class InterruptHandler {
public:
    InterruptHandler() {
#ifdef Q_OS_WIN
        installed_ = SetConsoleCtrlHandler(interrupt, TRUE);
#else
        previous_ = std::signal(SIGINT, interrupt);
        installed_ = previous_ != SIG_ERR;
#endif
    }

    ~InterruptHandler() {
        if (!installed_)
            return;
#ifdef Q_OS_WIN
        SetConsoleCtrlHandler(interrupt, FALSE);
#else
        std::signal(SIGINT, previous_);
#endif
    }

    bool installed() const { return installed_; }

private:
    bool installed_ = false;
#ifndef Q_OS_WIN
    using Handler = void (*)(int);
    Handler previous_ = SIG_DFL;
#endif
};

QJsonObject symbolJson(const SourceSymbol &symbol) {
    return {{"name", symbol.name},
            {"qualifiedName", symbol.qualifiedName},
            {"kind", symbol.kind},
            {"category", symbol.category},
            {"start", symbol.start},
            {"end", symbol.end},
            {"nameStart", symbol.nameStart},
            {"nameEnd", symbol.nameEnd},
            {"line", symbol.line},
            {"parent", symbol.parent},
            {"signature", symbol.signature},
            {"definition", symbol.definition},
            {"local", symbol.local},
            {"generated", symbol.generated},
            {"outlineOnly", symbol.outlineOnly},
            {"provider", symbol.provider}};
}

QJsonObject indexedJson(const IndexedSymbol &entry) {
    auto result = symbolJson(entry.symbol);
    result.insert("path", entry.path);
    result.insert("hash", QString::fromLatin1(entry.hash));
    result.insert("live", entry.live);
    return result;
}

QJsonObject locationJson(const codecairn::SourceLocation &location) {
    return {{"path", location.path},
            {"start", location.start},
            {"end", location.end},
            {"line", location.line},
            {"character", location.character},
            {"endLine", location.endLine},
            {"endCharacter", location.endCharacter},
            {"hash", QString::fromLatin1(location.expectedHash)}};
}

class Operation {
public:
    explicit Operation(int timeoutMs) : timeoutMs_(timeoutMs) { elapsed_.start(); }

    int stopCode() const {
        return wasInterrupted() ? 130 : elapsed_.elapsed() >= timeoutMs_ ? 124 : 0;
    }

    void wait(QEventLoop &loop, const std::function<void()> &cancel) {
        QTimer poll;
        poll.setInterval(25);
        QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
            if (stopCode()) {
                cancel();
                loop.quit();
            }
        });
        poll.start();
        if (stopCode())
            cancel();
        else
            loop.exec();
    }

    template <typename T>
    T result(const QFuture<T> &future, const std::shared_ptr<std::atomic_bool> &cancel) {
        QEventLoop loop;
        QFutureWatcher<T> watcher;
        QObject::connect(&watcher, &QFutureWatcher<T>::finished, &loop, &QEventLoop::quit);
        watcher.setFuture(future);
        wait(loop, [&] { cancel->store(true); });
        watcher.waitForFinished();
        return watcher.result();
    }

private:
    int timeoutMs_;
    QElapsedTimer elapsed_;
};

int output(QJsonObject result, const QString &command, bool json, int code) {
    result.insert("schemaVersion", 1);
    result.insert("command", command);
    if (!result.contains("status"))
        result.insert("status", code ? "error" : "ok");
    QTextStream out(stdout);
    QTextStream err(stderr);
    if (json)
        out << QJsonDocument(result).toJson(QJsonDocument::Compact) << '\n';
    else if (!result.value("error").toString().isEmpty())
        err << result.value("error").toString() << '\n';
    else {
        if (command == "index") {
            const auto files = result.value("files").toInt();
            out << "Indexed " << result.value("root").toString() << ": " << files
                << (files == 1 ? " file, " : " files, ") << result.value("parsed").toInt()
                << " parsed, " << result.value("reused").toInt() << " reused.\n";
        } else if (command == "analyze") {
            const auto count = result.value("symbols").toArray().size();
            out << result.value("path").toString() << ": " << count
                << (count == 1 ? " symbol" : " symbols");
            if (!result.value("supported").toBool())
                out << " (unsupported language)";
            if (result.value("syntaxErrors").toBool())
                out << " (syntax errors)";
            out << '\n';
        } else if (result.contains("hits")) {
            const auto count = result.value("hits").toArray().size();
            const auto files = result.value("filesSearched").toInt();
            out << count << (count == 1 ? " match; " : " matches; ") << files
                << (files == 1 ? " file searched.\n" : " files searched.\n");
        } else if (result.contains("locations")) {
            const auto count = result.value("locations").toArray().size();
            out << count << (count == 1 ? " definition.\n" : " definitions.\n");
        } else {
            out << result.value("symbols").toArray().size() << " of "
                << result.value("total").toInt() << " symbols (offset "
                << result.value("offset").toInt() << ").\n";
        }
        for (const auto &value : result.value("symbols").toArray()) {
            const auto entry = value.toObject();
            const auto qualified = entry.value("qualifiedName").toString();
            out << entry.value("path").toString(result.value("path").toString()) << " [bytes "
                << entry.value("start").toInt() << '-' << entry.value("end").toInt() << "] "
                << entry.value("kind").toString() << ' '
                << (qualified.isEmpty() ? entry.value("name").toString() : qualified) << '\n';
        }
        const auto locations =
            result.value(result.contains("hits") ? "hits" : "locations").toArray();
        for (const auto &value : locations) {
            const auto entry = value.toObject();
            out << entry.value("path").toString() << " [bytes " << entry.value("start").toInt()
                << '-' << entry.value("end").toInt() << ']';
            if (entry.contains("text"))
                out << ' ' << entry.value("text").toString().trimmed();
            out << '\n';
        }
        if (result.value("more").toBool()) {
            const int next = result.value("nextOffset")
                                 .toInt(result.value("offset").toInt() +
                                        result.value("symbols").toArray().size());
            out << "More results; use --offset " << next << ".\n";
        }
        if (result.contains("provenance"))
            out << "Provenance: " << result.value("provenance").toString() << '\n';
    }
    for (const auto &warning : result.value("warnings").toArray())
        err << warning.toString() << '\n';
    return code;
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("CodeCairnCLI");
    QCoreApplication::setApplicationVersion(CODECAIRN_VERSION);
    QThreadPool::globalInstance()->setMaxThreadCount(RuntimeLimits::WorkerThreads);
    const InterruptHandler interruptHandler;
    QCommandLineParser parser;
    parser.setApplicationDescription("Headless CodeCairn analysis, indexing, and search.");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("command", "index, symbols, analyze, definition, or references");
    parser.addPositionalArgument("path", "Source path for analyze.", "[path]");
    parser.addOptions({
        {"root", "Workspace folder.", "directory"},
        {"project", "Managed .cairn project.", "file"},
        {"state-dir", "Isolated absolute core state directory.", "directory"},
        {"file", "Source file for navigation or current-file search.", "file"},
        {"overlay", "Use UTF-8 FILE as unsaved contents of PATH (repeatable).", "PATH=FILE"},
        {"query", "Symbol filter or reference query.", "text"},
        {"method", "Search method: literal, regex, keywords, occurrences, references.", "method",
         "literal"},
        {"semantic", "Require language-server references or definitions; no text fallback."},
        {"byte-offset", "Zero-based UTF-8 byte offset for a semantic source selection.", "offset"},
        {"offset", "Result page offset.", "offset", "0"},
        {"limit", "Result page size (at most 10000).", "count", "300"},
        {"timeout", "Operation timeout in milliseconds.", "milliseconds", "120000"},
        {"file-patterns", "Search filename patterns, separated by semicolons.", "patterns"},
        {"include-comments", "Include comments in reference searches."},
        {"include-strings", "Include strings in reference searches."},
        {"skip-inactive", "Skip compiler-proven inactive C/C++ regions."},
        {"force", "Reparse files when indexing."},
        {"json", "Write versioned compact JSON to stdout."},
    });
    const bool parsed = parser.parse(app.arguments());
    const bool json = parser.isSet("json");
    const auto positional = parser.positionalArguments();
    const auto command = positional.value(0);
    const auto fail = [&](const QString &error, int code = 2) {
        return output({{"error", error}}, command, json, code);
    };
    if (!parsed)
        return fail(parser.errorText());
    if (parser.isSet("help"))
        parser.showHelp();
    if (parser.isSet("version"))
        parser.showVersion();
    if (!interruptHandler.installed())
        return fail("Unable to install the console interrupt handler.", 1);
    if (!QStringList{"index", "symbols", "analyze", "definition", "references"}.contains(command))
        return fail("Expected command: index, symbols, analyze, definition, or references.");
    if (positional.size() > (command == "analyze" ? 2 : 1))
        return fail("Unexpected positional arguments.");
    QStringList allowedOptions{"state-dir", "timeout", "json"};
    if (command == "analyze")
        allowedOptions.append({"file", "overlay"});
    else {
        allowedOptions.append({"root", "project", "file"});
        if (command == "index")
            allowedOptions.append("force");
        else if (command == "symbols")
            allowedOptions.append({"query", "offset", "limit", "overlay"});
        else if (command == "definition")
            allowedOptions.append({"byte-offset", "offset", "overlay", "semantic"});
        else
            allowedOptions.append({"query", "method", "semantic", "byte-offset", "offset", "limit",
                                   "file-patterns", "include-comments", "include-strings",
                                   "skip-inactive", "overlay"});
    }
    for (const auto &option : parser.optionNames())
        if (parser.isSet(option) && !allowedOptions.contains(option))
            return fail("--" + option + " is not available for " + command + ".");
    if ((command == "index" || command == "symbols") && parser.isSet("file") &&
        (parser.isSet("root") || parser.isSet("project")))
        return fail("--file only supplies an inferred workspace root for " + command +
                    "; do not combine it with --root or --project.");
    if (command == "definition" && parser.isSet("semantic") && parser.isSet("offset"))
        return fail("--offset is only available for syntax definitions, not --semantic.");
    if (command == "references") {
        if (parser.isSet("byte-offset") && !parser.isSet("semantic"))
            return fail("--byte-offset requires --semantic for references.");
        if (!QStringList{"literal", "regex", "keywords", "occurrences", "references"}.contains(
                parser.value("method")))
            return fail("Unknown --method.");
        if (parser.isSet("semantic") && parser.isSet("method") &&
            parser.value("method") != "references")
            return fail("--semantic only accepts an explicit --method references.");
    }
    bool validOffset = false, validLimit = false, validTimeout = false;
    const int offset = parser.value("offset").toInt(&validOffset);
    const int limit = parser.value("limit").toInt(&validLimit);
    const int timeout = parser.value("timeout").toInt(&validTimeout);
    if (!validOffset || offset < 0 || !validLimit || limit < 1 ||
        limit > RuntimeLimits::ReferenceResults || !validTimeout || timeout < 1)
        return fail("Expected nonnegative --offset, --limit 1..10000, and positive --timeout.");
    if (parser.isSet("root") && parser.isSet("project"))
        return fail("Choose either --root or --project.");
    if (parser.isSet("state-dir")) {
        const auto error = configureStateDirectory(parser.value("state-dir"));
        if (!error.isEmpty())
            return fail(error);
    }
    Operation operation(timeout);
    const auto cancel = std::make_shared<std::atomic_bool>(false);
    QJsonObject result;
    const auto stopped = [&]() {
        const int code = operation.stopCode();
        return output({{"status", code == 130 ? "cancelled" : "error"},
                       {"error", code == 130 ? "Operation cancelled." : "Operation timed out."}},
                      command, json, code);
    };
    auto filePath = parser.value("file");
    if (command == "analyze" && positional.size() == 2) {
        if (!filePath.isEmpty())
            return fail("Specify either an analyze path or --file, not both.");
        filePath = positional[1];
    }
    if (!filePath.isEmpty())
        filePath = workspacePath(filePath);
    QHash<QString, FileContents> overlaySources;
    for (const auto &specification : parser.values("overlay")) {
        const auto separator = specification.indexOf('=');
        if (separator <= 0 || separator == specification.size() - 1)
            return fail("Expected --overlay PATH=FILE with nonempty paths.");
        const auto path = workspacePath(specification.left(separator));
        if (overlaySources.contains(path))
            return fail("Duplicate overlay source identity: " + path);
        const auto source = readSourceFile(specification.mid(separator + 1));
        if (!source.error.isEmpty())
            return fail(source.error, 1);
        overlaySources.insert(path, source);
        if (operation.stopCode())
            return stopped();
    }
    const auto sourceForPath = [&](const QString &path) {
        const auto found = overlaySources.constFind(workspacePath(path));
        return found == overlaySources.cend() ? readSourceFile(path) : *found;
    };
    codecairn::Engine engine;
    const auto documentForPath = [&](const QString &path, const FileContents &source) {
        auto *document = engine.document(path);
        if (!document) {
            const auto created = engine.createDocument(path, {}, source.utf8Bom);
            if (!created)
                return created;
            document = created.value();
        }
        document->updateContents(source.bytes);
        return codecairn::Result<codecairn::Document *>::success(document);
    };
    if (command == "analyze") {
        if (filePath.isEmpty())
            return fail("analyze requires a source file.");
        if (overlaySources.size() > 1 ||
            (!overlaySources.isEmpty() && !overlaySources.contains(filePath)))
            return fail("analyze accepts only an overlay for its source file.");
        const auto source = sourceForPath(filePath);
        if (!source.error.isEmpty())
            return fail(source.error, 1);
        const auto created = documentForPath(filePath, source);
        if (!created)
            return fail(created.error().message, 1);
        const auto snapshot = created.value()->snapshot();
        QEventLoop loop;
        codecairn::RequestId request;
        codecairn::AnalysisResult analyzed;
        bool finished = false;
        QObject::connect(&engine, &codecairn::Engine::analysisReady, &loop,
                         [&](codecairn::RequestId id, const codecairn::AnalysisResult &reply) {
                             if (id == request) {
                                 analyzed = reply;
                                 finished = true;
                                 loop.quit();
                             }
                         });
        request = engine.analyze(snapshot);
        if (!finished)
            operation.wait(loop, [&] { engine.cancel(request); });
        if (operation.stopCode())
            return stopped();
        if (!analyzed.analysis.error.isEmpty())
            return fail(analyzed.analysis.error, 1);
        QJsonArray symbols;
        for (const auto &symbol : analyzed.analysis.symbols)
            symbols.append(symbolJson(symbol));
        result = {{"path", filePath},
                  {"hash", QString::fromLatin1(analyzed.source.hash)},
                  {"supported", analyzed.analysis.supported},
                  {"syntaxErrors", analyzed.analysis.syntaxErrors},
                  {"compilerEnriched", analyzed.analysis.compilerEnriched},
                  {"symbols", symbols},
                  {"warnings", QJsonArray::fromStringList(analyzed.analysis.diagnostics)}};
        return output(result, command, json, 0);
    }
    QString root = parser.value("root");
    QString projectFile = parser.value("project");
    if (!projectFile.isEmpty()) {
        projectFile = workspacePath(projectFile);
        const auto loaded = WorkspaceOptionsStore::loadProject(projectFile);
        if (!loaded.error.isEmpty())
            return fail(loaded.error, 1);
        root = loaded.root;
    }
    if (root.isEmpty() && !filePath.isEmpty())
        root = QFileInfo(filePath).absolutePath();
    if (root.isEmpty())
        return fail("This command requires --root, --project, or --file.");
    const auto prepared = prepareProject(root, projectFile);
    if (!prepared.error.isEmpty())
        return fail(prepared.error, 1);
    root = prepared.root;
    for (auto source = overlaySources.cbegin(); source != overlaySources.cend(); ++source) {
        const auto created = documentForPath(source.key(), *source);
        if (!created)
            return fail(created.error().message, 1);
        if (operation.stopCode())
            return stopped();
    }
    const bool semantic = parser.isSet("semantic");
    const codecairn::ProjectOptions startup{codecairn::IndexStartup::Manual,
                                            semantic ? codecairn::LanguageStartup::Automatic
                                                     : codecairn::LanguageStartup::Manual};
    const auto opened = engine.openProject(prepared, startup);
    if (!opened)
        return fail(opened.error().message, 1);
    auto &project = *opened.value();
    const auto analyzeDocument = [&](codecairn::Document *document) {
        QEventLoop loop;
        codecairn::RequestId request;
        bool finished = false;
        QObject::connect(&project, &codecairn::Project::analysisReady, &loop,
                         [&](codecairn::RequestId id, const codecairn::AnalysisResult &) {
                             if (id == request) {
                                 finished = true;
                                 loop.quit();
                             }
                         });
        request = project.analyze(document->snapshot(), codecairn::AnalysisDetail::Symbols);
        if (!finished)
            operation.wait(loop, [&] { project.cancel(request); });
    };
    for (auto source = overlaySources.cbegin(); source != overlaySources.cend(); ++source) {
        analyzeDocument(engine.document(source.key()));
        if (operation.stopCode())
            return stopped();
    }
    if (command == "index") {
        const auto excludes =
            project.info().configuration.excludedDirectories.value_or(defaultWorkspaceExcludes());
        const auto indexed = operation.result(
            QtConcurrent::run([&] {
                const auto mode = parser.isSet("force") ? IndexRefresh::RebuildFilesAndSymbols
                                                        : IndexRefresh::DiscoverChanges;
                return buildWorkspaceIndex(root, mode, excludes, cancel, {}, projectFile);
            }),
            cancel);
        if (operation.stopCode())
            return stopped();
        if (!indexed.error.isEmpty())
            return fail(indexed.error, 1);
        result = {{"root", root},
                  {"files", indexed.files.size()},
                  {"parsed", indexed.parsed},
                  {"reused", indexed.reused},
                  {"cancelled", indexed.cancelled},
                  {"warnings", QJsonArray::fromStringList(indexed.warnings)}};
        return output(result, command, json, indexed.cancelled ? 130 : 0);
    }
    if (command == "symbols") {
        codecairn::SymbolQuery query;
        query.text = parser.value("query");
        query.offset = offset;
        query.pageSize = limit;
        query.includeCategories = false;
        QEventLoop loop;
        codecairn::RequestId request;
        SymbolPage page;
        bool finished = false;
        QObject::connect(&project, &codecairn::Project::symbolsReady, &loop,
                         [&](codecairn::RequestId id, const SymbolPage &reply) {
                             if (id == request) {
                                 page = reply;
                                 finished = true;
                                 loop.quit();
                             }
                         });
        request = project.findSymbols(query);
        if (!finished)
            operation.wait(loop, [&] { project.cancel(request); });
        if (operation.stopCode())
            return stopped();
        if (!page.error.isEmpty())
            return fail(page.error, 1);
        QJsonArray symbols;
        for (const auto &entry : page.symbols)
            symbols.append(indexedJson(entry));
        result = {{"symbols", symbols},
                  {"total", page.total},
                  {"offset", offset},
                  {"more", offset + page.symbols.size() < page.total},
                  {"provenance", "syntax-index candidates, not compiler verified"}};
        return output(result, command, json, 0);
    }
    std::optional<codecairn::SourcePosition> origin;
    if (semantic || command == "definition") {
        if (filePath.isEmpty() || !parser.isSet("byte-offset"))
            return fail("Definition and semantic queries require --file and --byte-offset.");
        bool valid = false;
        const int position = parser.value("byte-offset").toInt(&valid);
        const auto source = sourceForPath(filePath);
        if (!source.error.isEmpty())
            return fail(source.error, 1);
        if (!valid || position < 0 || position >= source.bytes.size())
            return fail("Invalid --byte-offset for this source.");
        const auto created = documentForPath(filePath, source);
        if (!created)
            return fail(created.error().message, 1);
        if (command == "definition" && !overlaySources.contains(filePath)) {
            analyzeDocument(created.value());
            if (operation.stopCode())
                return stopped();
        }
        origin = codecairn::SourcePosition{created.value()->snapshot(), position};
    }
    if (command == "definition") {
        const auto identifier = sourceIdentifier(origin->snapshot.bytes, origin->byteOffset);
        if (identifier.isEmpty())
            return fail("The byte offset must select an identifier.");
        codecairn::DefinitionQuery definition;
        definition.at = *origin;
        definition.offset = offset;
        definition.resolution = semantic ? codecairn::DefinitionResolution::SemanticOnly
                                         : codecairn::DefinitionResolution::SyntaxOnly;
        QEventLoop loop;
        codecairn::RequestId request;
        codecairn::DefinitionPage definitions;
        bool finished = false;
        QObject::connect(&project, &codecairn::Project::definitionsReady, &loop,
                         [&](codecairn::RequestId id, const codecairn::DefinitionPage &reply) {
                             if (id == request) {
                                 definitions = reply;
                                 finished = true;
                                 loop.quit();
                             }
                         });
        request = project.findDefinitions(definition);
        if (!finished)
            operation.wait(loop, [&] { project.cancel(request); });
        if (operation.stopCode())
            return stopped();
        if (!definitions.error.isEmpty())
            return fail(definitions.error, 1);
        const auto readTarget = [&](const codecairn::SourceLocation &location,
                                    const std::optional<IndexedSymbol> &indexed = {}) {
            QEventLoop readLoop;
            codecairn::RequestId readRequest;
            codecairn::SourceRead read;
            bool readFinished = false;
            const auto connection =
                QObject::connect(&engine, &codecairn::Engine::sourceReady, &readLoop,
                                 [&](codecairn::RequestId id, const codecairn::SourceRead &reply) {
                                     if (id == readRequest) {
                                         read = reply;
                                         readFinished = true;
                                         readLoop.quit();
                                     }
                                 });
            readRequest = engine.readSource(location, indexed);
            if (!readFinished)
                operation.wait(readLoop, [&] { engine.cancel(readRequest); });
            QObject::disconnect(connection);
            return read;
        };
        if (!semantic) {
            QJsonArray symbols;
            for (const auto &target : definitions.definitions) {
                const auto read = readTarget(target.location, target.symbol);
                if (operation.stopCode())
                    return stopped();
                if (!read.file.error.isEmpty())
                    return fail(read.file.error, 1);
                if (!read.range.error.isEmpty())
                    return fail(read.range.error, 1);
                if (target.symbol)
                    symbols.append(indexedJson(*target.symbol));
            }
            return output({{"symbols", symbols},
                           {"total", definitions.total},
                           {"offset", offset},
                           {"more", !definitions.complete},
                           {"provenance", "syntax-index candidates, not compiler verified"}},
                          command, json, 0);
        }
        QJsonArray locations;
        for (const auto &target : definitions.definitions) {
            auto location = target.location;
            const auto read = readTarget(location, target.symbol);
            if (operation.stopCode())
                return stopped();
            if (!read.file.error.isEmpty())
                return fail(read.file.error, 1);
            if (!read.range.error.isEmpty())
                return fail(read.range.error, 1);
            location.start = read.range.start;
            location.end = read.range.end;
            location.expectedHash = sourceContentHash(read.file.bytes);
            locations.append(locationJson(location));
        }
        return output({{"locations", locations}, {"provenance", "semantic"}}, command, json, 0);
    }
    auto queryText = parser.value("query");
    const auto method = parser.value("method");
    if (origin && queryText.isEmpty())
        queryText = sourceIdentifier(origin->snapshot.bytes, origin->byteOffset);
    if (origin && queryText != sourceIdentifier(origin->snapshot.bytes, origin->byteOffset))
        return fail("Semantic query must match the selected source identifier.");
    codecairn::SearchFiles files;
    files.patterns = parser.value("file-patterns");
    if (!filePath.isEmpty()) {
        files.scope = codecairn::SearchScope::CurrentFile;
        files.currentFile = filePath;
    } else {
        files.scope = codecairn::SearchScope::SelectedFiles;
        auto options = project.info().configuration;
        if (!options.managed)
            options.includedPaths = {"."};
        const auto progress = std::make_shared<ProjectScanProgress>();
        const auto scan =
            operation.result(QtConcurrent::run([&] {
                                 return discoverProjectFiles(root, options, 0, cancel, progress,
                                                             ProjectFileScope::SourceFiles);
                             }),
                             cancel);
        if (operation.stopCode())
            return stopped();
        if (!scan.errors.isEmpty())
            return fail(scan.errors.join('\n'), 1);
        for (const auto &relative : scan.files)
            files.selectedFiles.append(QDir(root).filePath(relative));
        for (auto source = overlaySources.cbegin(); source != overlaySources.cend(); ++source)
            if (project.contains(source.key()) && !files.selectedFiles.contains(source.key()))
                files.selectedFiles.append(source.key());
    }
    QEventLoop loop;
    codecairn::RequestId request;
    codecairn::SearchPage matches;
    bool finished = false;
    const auto receive = [&](codecairn::RequestId id, const codecairn::SearchPage &page) {
        if (id == request) {
            matches = page;
            finished = true;
            loop.quit();
        }
    };
    const auto comments =
        parser.isSet("include-comments") ? CommentSearch::Include : CommentSearch::Exclude;
    if (semantic) {
        QObject::connect(&project, &codecairn::Project::referencesReady, &loop, receive);
        codecairn::ReferenceQuery query;
        query.symbol = *origin;
        query.files = files;
        query.resolution = codecairn::ReferenceResolution::Semantic;
        query.comments = comments;
        query.strings = parser.isSet("include-strings");
        query.skipInactive = parser.isSet("skip-inactive");
        query.offset = offset;
        query.pageSize = limit;
        request = project.findReferences(query);
    } else {
        QObject::connect(&project, &codecairn::Project::textMatchesReady, &loop, receive);
        codecairn::TextQuery query;
        query.text = queryText;
        query.files = files;
        query.method = method == "literal"    ? codecairn::TextMatch::Literal
                       : method == "regex"    ? codecairn::TextMatch::RegularExpression
                       : method == "keywords" ? codecairn::TextMatch::KeywordExpression
                                              : codecairn::TextMatch::WholeIdentifier;
        query.wholeWords = method != "literal";
        query.comments = comments;
        query.strings = parser.isSet("include-strings");
        query.skipInactive = parser.isSet("skip-inactive");
        query.offset = offset;
        query.pageSize = limit;
        request = project.searchText(query);
    }
    if (!finished)
        operation.wait(loop, [&] { project.cancel(request); });
    if (operation.stopCode())
        return stopped();
    if (!matches.error.isEmpty())
        return fail(matches.error, 1);
    QJsonArray hits;
    for (const auto &hit : matches.hits) {
        auto entry = locationJson(hit.location);
        entry.insert("line", hit.line);
        entry.insert("text", hit.text);
        entry.insert("container", hit.container);
        hits.append(entry);
    }
    result = {
        {"hits", hits},
        {"offset", offset},
        {"nextOffset", offset + matches.hits.size()},
        {"more", static_cast<bool>(matches.next)},
        {"cancelled", matches.cancelled},
        {"filesSearched", matches.filesSearched},
        {"warnings", QJsonArray::fromStringList(matches.warnings)},
        {"provenance", semantic ? "semantic; provider coverage applies" : "text, not semantic"}};
    if (matches.next)
        result.insert("status", "partial");
    return output(result, command, json, matches.cancelled ? 130 : 0);
}

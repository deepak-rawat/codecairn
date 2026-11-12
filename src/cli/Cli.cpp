#include <codecairn/DocumentAnalysis.h>
#include <codecairn/CoreSession.h>
#include <codecairn/FileIO.h>
#include <codecairn/NavigationService.h>
#include <codecairn/ProjectSession.h>
#include <codecairn/ProjectDiscovery.h>
#include <codecairn/ReferenceSession.h>
#include <codecairn/WorkspaceIndex.h>

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

QJsonObject locationJson(const NavigationLocation &location) {
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
    if (command == "analyze") {
        if (filePath.isEmpty())
            return fail("analyze requires a source file.");
        if (overlaySources.size() > 1 ||
            (!overlaySources.isEmpty() && !overlaySources.contains(filePath)))
            return fail("analyze accepts only an overlay for its source file.");
        const auto source = sourceForPath(filePath);
        if (!source.error.isEmpty())
            return fail(source.error, 1);
        CoreSession core;
        auto &document = *core.createDocument(filePath, source.bytes, source.utf8Bom);
        QEventLoop loop;
        QObject::connect(&document, &DocumentAnalysis::analysisCompleted, &loop, &QEventLoop::quit);
        QObject::connect(&document, &DocumentAnalysis::analysisRejected, &loop, &QEventLoop::quit);
        document.startAnalysis();
        if (document.isAnalysisRunning())
            operation.wait(loop, [&] { document.stopBackgroundWork(); });
        if (operation.stopCode())
            return stopped();
        if (!document.analysis.error.isEmpty())
            return fail(document.analysis.error, 1);
        QJsonArray symbols;
        for (const auto &symbol : document.analysis.symbols)
            symbols.append(symbolJson(symbol));
        result = {{"path", filePath},
                  {"hash", QString::fromLatin1(document.snapshotHash)},
                  {"supported", document.analysis.supported},
                  {"syntaxErrors", document.analysis.syntaxErrors},
                  {"compilerEnriched", document.analysis.compilerEnriched},
                  {"symbols", symbols},
                  {"warnings", QJsonArray::fromStringList(document.analysis.diagnostics)}};
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
    const auto project = prepareProject(root, projectFile);
    if (!project.error.isEmpty())
        return fail(project.error, 1);
    root = project.root;
    QVector<SymbolOverlay> overlays;
    for (auto source = overlaySources.cbegin(); source != overlaySources.cend(); ++source) {
        overlays.append({source.key(), source->bytes,
                         analyzeSource(source->bytes, source.key(), SourceAnalysisMode::Index),
                         sourceContentHash(source->bytes)});
        if (operation.stopCode())
            return stopped();
    }
    CoreSession core;
    auto &services = core.projects();
    core.setSnapshotProvider([&](const QString &path) -> std::optional<QByteArray> {
        const auto source = overlaySources.constFind(workspacePath(path));
        if (source != overlaySources.cend())
            return source->bytes;
        return {};
    });
    const bool semantic = parser.isSet("semantic");
    const ProjectOpenOptions startup{IndexStartup::Manual, semantic ? LanguageStartup::Automatic
                                                                    : LanguageStartup::Manual};
    if (const auto error = services.commitProject(project, startup); !error.isEmpty())
        return fail(error, 1);
    if (command == "index") {
        const auto excludes =
            project.options.excludedDirectories.value_or(defaultWorkspaceExcludes());
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
        SymbolQuery query;
        query.projectFile = projectFile;
        query.text = parser.value("query");
        query.offset = offset;
        query.limit = limit;
        query.cancel = cancel;
        query.includeCategories = false;
        query.overlays = overlays;
        const auto page = operation.result(
            QtConcurrent::run([&] { return queryWorkspaceSymbols(root, query); }), cancel);
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
    std::optional<SemanticReferenceOrigin> origin;
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
        origin = SemanticReferenceOrigin{filePath, source.bytes, 0, position, true, {}};
    }
    if (command == "definition") {
        const auto identifier = sourceIdentifier(origin->source, origin->position);
        if (identifier.isEmpty())
            return fail("The byte offset must select an identifier.");
        DefinitionRequest definition;
        definition.root = root;
        definition.projectFile = projectFile;
        definition.path = filePath;
        definition.source = origin->source;
        definition.position = origin->position;
        definition.offset = offset;
        definition.identifier = identifier;
        definition.analysis = analyzeSource(origin->source, filePath, SourceAnalysisMode::Index);
        definition.overlays = overlays;
        if (!overlaySources.contains(filePath))
            definition.overlays.append({filePath, definition.source, definition.analysis,
                                        sourceContentHash(definition.source)});
        definition.policy =
            semantic ? DefinitionPolicy::SemanticOnly : DefinitionPolicy::SyntaxOnly;
        auto &navigation = core.navigation();
        QEventLoop loop;
        DefinitionResult definitions;
        bool finished = false;
        QObject::connect(&navigation, &NavigationService::definitionReady, &loop,
                         [&](quint64 token, const QString &channel, const DefinitionResult &reply) {
                             if (token == 1 && channel == "navigation") {
                                 definitions = reply;
                                 finished = true;
                                 loop.quit();
                             }
                         });
        navigation.lookup(definition, 1, "navigation");
        if (!finished)
            operation.wait(loop, [&] { navigation.cancelLookup("navigation"); });
        if (operation.stopCode())
            return stopped();
        if (!definitions.error.isEmpty())
            return fail(definitions.error, 1);
        const auto readTarget = [&](const NavigationLocation &location,
                                    const std::optional<IndexedSymbol> &indexed = {}) {
            QEventLoop readLoop;
            NavigationRead read;
            bool readFinished = false;
            const auto connection =
                QObject::connect(&navigation, &NavigationService::readReady, &readLoop,
                                 [&](quint64 token, const NavigationRead &reply) {
                                     if (token == 2) {
                                         read = reply;
                                         readFinished = true;
                                         readLoop.quit();
                                     }
                                 });
            navigation.read(location, 2, indexed);
            if (!readFinished)
                operation.wait(readLoop, [&] { navigation.cancelRead(); });
            QObject::disconnect(connection);
            return read;
        };
        if (!semantic) {
            QJsonArray symbols;
            for (const auto &target : definitions.candidates.targets) {
                NavigationLocation location;
                location.path = target.path;
                location.start = target.symbol.nameStart;
                location.end = target.symbol.nameEnd;
                location.expectedHash = target.hash;
                const auto read = readTarget(location, target);
                if (operation.stopCode())
                    return stopped();
                if (!read.file.error.isEmpty())
                    return fail(read.file.error, 1);
                if (!read.range.error.isEmpty())
                    return fail(read.range.error, 1);
                symbols.append(indexedJson(target));
            }
            return output({{"symbols", symbols},
                           {"total", definitions.candidates.total},
                           {"offset", offset},
                           {"more", !definitions.candidates.complete},
                           {"provenance", "syntax-index candidates, not compiler verified"}},
                          command, json, 0);
        }
        QJsonArray locations;
        for (const auto &target : definitions.locations) {
            NavigationLocation location;
            location.path = target.path;
            location.line = target.line;
            location.character = target.character;
            location.endLine = target.endLine;
            location.endCharacter = target.endCharacter;
            const auto read = readTarget(location);
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
    ReferenceSearchRequest request;
    request.workspaceRoot = root;
    request.projectFile = projectFile;
    request.overlays = overlays;
    request.query = parser.value("query");
    request.offset = offset;
    request.limit = limit;
    request.options.smartMatching = semantic;
    request.options.comments =
        parser.isSet("include-comments") ? CommentSearch::Include : CommentSearch::Exclude;
    request.options.strings = parser.isSet("include-strings");
    request.options.skipInactive = parser.isSet("skip-inactive");
    const auto method = parser.value("method");
    if (semantic || method == "references")
        request.options.method = SearchMethod::References;
    else if (method == "literal") {
        request.options.method = SearchMethod::Literal;
        request.options.wholeWords = false;
    } else if (method == "regex")
        request.options.method = SearchMethod::RegularExpression;
    else if (method == "keywords")
        request.options.method = SearchMethod::KeywordExpression;
    else if (method == "occurrences")
        request.options.method = SearchMethod::Occurrences;
    else
        return fail("Unknown --method.");
    if (origin && request.query.isEmpty())
        request.query = sourceIdentifier(origin->source, origin->position);
    if (origin && request.query != sourceIdentifier(origin->source, origin->position))
        return fail("Semantic query must match the selected source identifier.");
    QStringList paths;
    if (!filePath.isEmpty())
        paths.append(filePath);
    else {
        auto options = project.options;
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
            paths.append(QDir(root).filePath(relative));
    }
    ReferenceInputs inputs;
    inputs.scope = filePath.isEmpty() ? ReferenceScope::Project : ReferenceScope::CurrentFile;
    inputs.currentFile = filePath;
    inputs.catalog = paths;
    inputs.overlays = overlays;
    inputs.root = root;
    inputs.options = project.options;
    inputs.filePatterns = parser.value("file-patterns");
    request.paths = resolveReferenceScope(inputs);
    auto &session = core.references();
    QEventLoop loop;
    ReferenceResults matches;
    bool finished = false;
    QObject::connect(&session, &ReferenceSession::pageReady, &loop,
                     [&](quint64, const ReferenceResults &page, bool) {
                         matches = page;
                         finished = true;
                         loop.quit();
                     });
    session.start(request, 1, origin);
    if (!finished)
        operation.wait(loop, [&] { session.cancel(); });
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
        {"more", matches.truncated},
        {"cancelled", matches.cancelled},
        {"filesSearched", matches.filesSearched},
        {"warnings", QJsonArray::fromStringList(matches.warnings)},
        {"provenance", semantic ? "semantic; provider coverage applies" : "text, not semantic"}};
    if (matches.truncated)
        result.insert("status", "partial");
    return output(result, command, json, matches.cancelled ? 130 : 0);
}

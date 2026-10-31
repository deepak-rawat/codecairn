#include "LanguageServerClient.h"
#include "LanguageRegistry.h"
#include "WorkspacePaths.h"
#include "CompilationDatabase.h"
#include "Diagnostics.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QUrl>
#include <utility>

namespace {
QString uri(const QString &path) {
    return QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded);
}
}

LanguageServerClient::LanguageServerClient(QObject *parent) : QObject(parent) {
    connect(this, &LanguageServerClient::status, this, [this](const QString &message) {
        qCInfo(languageServiceLog)
            << "lsp.status" << "service=" << serviceName_ << "root=" << root_ << message;
    });
    timeout_.setInterval(500);
    connect(&timeout_, &QTimer::timeout, this, [this] {
        const auto now = QDateTime::currentMSecsSinceEpoch();
        const auto keys = pending_.keys();
        for (int id : keys) {
            if (pending_.contains(id) && pending_[id].deadline < now) {
                auto pending = pending_.take(id);
                notify("$/cancelRequest", {{"id", id}});
                qCWarning(languageServiceLog)
                    << "lsp.timeout" << "service=" << serviceName_ << "root=" << root_
                    << "request=" << id << "method=" << pending.method << "path=" << pending.path;
                pending.reply({}, "clangd request timed out.");
            }
        }
    });
    connect(&process_, &QProcess::readyReadStandardOutput, this, &LanguageServerClient::receive);
    connect(&process_, &QProcess::readyReadStandardError, this, [this] {
        const auto message = QString::fromUtf8(process_.readAllStandardError()).trimmed();
        if (!message.isEmpty()) {
            qCWarning(languageServiceLog) << "lsp.stderr" << "service=" << serviceName_
                                          << "root=" << root_ << message.right(4096);
            emit status("clangd diagnostics: " + message.right(400));
        }
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (!stopping_) {
            qCWarning(languageServiceLog) << "lsp.process_error" << "service=" << serviceName_
                                          << "root=" << root_ << process_.errorString();
            emit status(serviceName_ + " process error: " + process_.errorString());
            failQueuedLookups(process_.errorString());
            if (error == QProcess::FailedToStart && haveSelection_) {
                haveSelection_ = false;
                emit definitionReady(selection_.token, {}, process_.errorString());
            }
        }
    });
    connect(&process_, &QProcess::finished, this,
            [this](int code, QProcess::ExitStatus exitStatus) {
                initialized_ = false;
                timeout_.stop();
                auto pending = std::exchange(pending_, {});
                if (!stopping_) {
                    qCWarning(languageServiceLog)
                        << "lsp.unexpected_exit" << "service=" << serviceName_ << "root=" << root_
                        << "exitCode=" << code << "crashed=" << (exitStatus == QProcess::CrashExit);
                    emit status(QString("%1 stopped (%2). Use Project > Restart Language Service.")
                                    .arg(serviceName_)
                                    .arg(code));
                    failQueuedLookups(serviceName_ + " stopped.");
                    for (const auto &entry : pending)
                        entry.reply({}, serviceName_ + " stopped.");
                }
            });
    connect(&process_, &QProcess::started, this, [this] {
        timeout_.start();
        request(
            "initialize",
            {{"processId", static_cast<qint64>(QCoreApplication::applicationPid())},
             {"rootUri", uri(root_)},
             {"workspaceFolders",
              QJsonArray{QJsonObject{{"uri", uri(root_)}, {"name", QFileInfo(root_).fileName()}}}},
             {"capabilities",
              QJsonObject{
                  {"workspace", QJsonObject{{"configuration", true}, {"workspaceFolders", true}}},
                  {"general", QJsonObject{{"positionEncodings", QJsonArray{"utf-16"}}}},
                  {"textDocument",
                   QJsonObject{{"definition", QJsonObject{{"linkSupport", true}}}}}}},
             {"initializationOptions", QJsonObject{{"clangdFileStatus", true}}}},
            [this](const QJsonValue &value, const QString &error) {
                if (!error.isEmpty()) {
                    emit status(error);
                    failQueuedLookups(error);
                    if (haveSelection_) {
                        haveSelection_ = false;
                        emit definitionReady(selection_.token, {}, error);
                    }
                    return;
                }
                const auto encoding = value.toObject()
                                          .value("capabilities")
                                          .toObject()
                                          .value("positionEncoding")
                                          .toString("utf-16");
                if (encoding != "utf-16") {
                    const auto error = "Unsupported language-server position encoding: " + encoding;
                    qCWarning(languageServiceLog)
                        << "lsp.initialize_failed" << "service=" << serviceName_ << "root=" << root_
                        << "error=" << error;
                    emit status(error);
                    failQueuedLookups(error);
                    if (haveSelection_)
                        emit definitionReady(selection_.token, {}, error);
                    stop();
                    return;
                }
                initialized_ = true;
                notify("initialized", {});
                for (const auto &path : documents_.keys())
                    flushDocument(path);
                emit status(serviceName_ != "clangd" ? serviceName_ + " ready."
                            : configured_ ? "clangd ready (compilation database)."
                                          : "clangd ready with fallback flags; configure "
                                            "compile_commands.json for reliable C/C++ resolution.");
                emit ready();
                requestSelection();
                const auto queued = std::exchange(queuedLookups_, {});
                for (auto it = queued.cbegin(); it != queued.cend(); ++it) {
                    const auto &s = it->selection;
                    locations(s.path, s.bytes, s.version, s.position, s.token, it.key(),
                              it->references, it->includeDeclaration);
                }
            });
    });
}

LanguageServerClient::~LanguageServerClient() {
    stop();
}

void LanguageServerClient::failQueuedLookups(const QString &error) {
    const auto queued = std::exchange(queuedLookups_, {});
    for (auto it = queued.cbegin(); it != queued.cend(); ++it)
        emit locationsReady(it->selection.token, it.key(), {}, error);
}

void LanguageServerClient::start(const QString &root, const QString &compilationDirectory,
                                 const QString &executable) {
    stop();
    stopping_ = false;
    serviceName_ = "clangd";
    root_ = workspacePath(root);
    configured_ = false;
    const auto database = prepareCompilationDatabase(root_, compilationDirectory);
    if (!database.error.isEmpty()) {
        emit status(database.error);
        return;
    }
    commandDirectory_ = database.directory;
    configured_ = database.configured;
    qCInfo(languageServiceLog) << "lsp.starting" << "root=" << root_
                               << "compilationDatabase=" << commandDirectory_
                               << "configured=" << configured_;
    QString program = executable;
    if (program.isEmpty()) {
        qCWarning(languageServiceLog)
            << "lsp.unavailable" << "service=" << serviceName_ << "root=" << root_;
#if CODECAIRN_BUNDLE_RUNTIME
        const auto adjacent = QDir(QCoreApplication::applicationDirPath())
                                  .filePath(
#ifdef Q_OS_WIN
                                      "clangd.exe"
#else
                                      "clangd"
#endif
                                  );
        if (QFileInfo(adjacent).isExecutable())
            program = adjacent;
#endif
        if (program.isEmpty())
            program = QStandardPaths::findExecutable("clangd");
#ifdef CODECAIRN_CONFIGURED_CLANGD
        if (program.isEmpty() &&
            QFileInfo(QString::fromUtf8(CODECAIRN_CONFIGURED_CLANGD)).isExecutable())
            program = QString::fromUtf8(CODECAIRN_CONFIGURED_CLANGD);
#endif
    }
    if (program.isEmpty()) {
        emit status("clangd is not installed. Syntax-index candidates remain available.");
        return;
    }
    process_.setWorkingDirectory(root_);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("XDG_CACHE_HOME", QDir(commandDirectory_).filePath("cache"));
    process_.setProcessEnvironment(environment);
    process_.start(program,
                   {"--background-index", "--log=error", "--pch-storage=memory", "-j=2",
                    "--enable-config=false", "--compile-commands-dir=" + commandDirectory_});
}

void LanguageServerClient::startServer(const QString &root, const QStringList &command) {
    stop();
    stopping_ = false;
    root_ = workspacePath(root);
    configured_ = false;
    serviceName_ = command.value(0);
    const auto executable = QStandardPaths::findExecutable(serviceName_);
    if (executable.isEmpty()) {
        qCWarning(languageServiceLog)
            << "lsp.unavailable" << "service=" << serviceName_ << "root=" << root_;
        emit status(serviceName_ +
                    " is not installed; syntax candidates and text occurrences remain available.");
        return;
    }
    process_.setWorkingDirectory(root_);
    process_.setProcessEnvironment(QProcessEnvironment::systemEnvironment());
    process_.start(executable, command.mid(1));
}

void LanguageServerClient::stop() {
    stopping_ = true;
    initialized_ = false;
    haveSelection_ = false;
    queuedLookups_.clear();
    lookupIds_.clear();
    ++selectionGeneration_;
    timeout_.stop();
    pending_.clear();
    documents_.clear();
    input_.clear();
    if (process_.state() != QProcess::NotRunning) {
        notify("exit", {});
        process_.closeWriteChannel();
        if (!process_.waitForFinished(200)) {
            process_.kill();
            process_.waitForFinished(1000);
        }
    }
}

void LanguageServerClient::send(const QJsonObject &message) {
    if (process_.state() != QProcess::Running)
        return;
    const auto body = QJsonDocument(message).toJson(QJsonDocument::Compact);
    const auto frame = "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
    if (process_.write(frame) != frame.size()) {
        qCWarning(languageServiceLog)
            << "lsp.write_failed" << "service=" << serviceName_ << "root=" << root_
            << "method=" << message.value("method").toString()
            << "error=" << process_.errorString();
        emit status("Failed to write a clangd protocol message.");
    }
}

int LanguageServerClient::request(const QString &method, const QJsonObject &parameters,
                                  Reply reply) {
    const int id = ++nextId_;
    pending_.insert(
        id,
        {std::move(reply), QDateTime::currentMSecsSinceEpoch() + 20000, method,
         QUrl(parameters.value("textDocument").toObject().value("uri").toString()).toLocalFile()});
    send({{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", parameters}});
    return id;
}

void LanguageServerClient::notify(const QString &method, const QJsonObject &parameters) {
    send({{"jsonrpc", "2.0"}, {"method", method}, {"params", parameters}});
}

void LanguageServerClient::receive() {
    input_.append(process_.readAllStandardOutput());
    while (true) {
        const auto end = input_.indexOf("\r\n\r\n");
        if (end < 0) {
            if (input_.size() > 8192) {
                qCWarning(languageServiceLog) << "lsp.protocol_error" << "service=" << serviceName_
                                              << "root=" << root_ << "reason=Oversized header.";
                emit status("Invalid clangd protocol header.");
                stop();
            }
            return;
        }
        qint64 length = -1;
        for (const auto &line : input_.left(end).split('\n')) {
            if (line.toLower().startsWith("content-length:")) {
                bool ok = false;
                length = line.mid(15).trimmed().toLongLong(&ok);
                if (!ok)
                    length = -1;
            }
        }
        if (length < 0 || length > 32 * 1024 * 1024) {
            qCWarning(languageServiceLog)
                << "lsp.protocol_error" << "service=" << serviceName_ << "root=" << root_
                << "length=" << length << "reason=Invalid length.";
            emit status("Invalid clangd message length.");
            stop();
            return;
        }
        if (input_.size() < end + 4 + length)
            return;
        const auto body = input_.mid(end + 4, length);
        input_.remove(0, end + 4 + length);
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(body, &error);
        if (error.error != QJsonParseError::NoError || !doc.isObject()) {
            qCWarning(languageServiceLog)
                << "lsp.protocol_error" << "service=" << serviceName_ << "root=" << root_
                << "offset=" << error.offset << "error=" << error.errorString()
                << "object=" << doc.isObject();
            emit status("Invalid clangd JSON message.");
            stop();
            return;
        }
        processMessage(doc.object());
    }
}

void LanguageServerClient::processMessage(const QJsonObject &message) {
    if (message.contains("method")) {
        const auto method = message.value("method").toString();
        if (!message.contains("id") &&
            (method == "window/logMessage" || method == "window/showMessage")) {
            const auto parameters = message.value("params").toObject();
            const auto type = parameters.value("type").toInt();
            if (type == 1 || type == 2)
                qCWarning(languageServiceLog)
                    << "lsp.message" << "service=" << serviceName_ << "root=" << root_
                    << "type=" << type << "message=" << parameters.value("message").toString();
        }
        if (!message.contains("id") && method == "textDocument/publishDiagnostics") {
            const auto parameters = message.value("params").toObject();
            const auto path = QUrl(parameters.value("uri").toString()).toLocalFile();
            int reported = 0;
            for (const auto &value : parameters.value("diagnostics").toArray()) {
                const auto diagnostic = value.toObject();
                const auto severity = diagnostic.value("severity").toInt(1);
                if (severity > 2)
                    continue;
                if (reported == 12) {
                    qCWarning(languageServiceLog)
                        << "lsp.diagnostic_limit" << "service=" << serviceName_ << "root=" << root_
                        << "path=" << path << "limit=" << reported;
                    break;
                }
                const auto start = diagnostic.value("range").toObject().value("start").toObject();
                qCWarning(languageServiceLog)
                    << "lsp.diagnostic" << "service=" << serviceName_ << "root=" << root_
                    << "path=" << path << "severity=" << severity
                    << "line=" << start.value("line").toInt(-1) + 1
                    << "utf16Column=" << start.value("character").toInt(-1) + 1
                    << "message=" << diagnostic.value("message").toString();
                ++reported;
            }
        }
        if (message.contains("id")) {
            const auto method = message.value("method").toString();
            if (method == "workspace/configuration") {
                QJsonArray values;
                for (const auto &item :
                     message.value("params").toObject().value("items").toArray()) {
                    Q_UNUSED(item);
                    values.append(QJsonValue::Null);
                }
                send({{"jsonrpc", "2.0"}, {"id", message.value("id")}, {"result", values}});
            } else if (method == "workspace/workspaceFolders") {
                send({{"jsonrpc", "2.0"},
                      {"id", message.value("id")},
                      {"result", QJsonArray{QJsonObject{{"uri", uri(root_)},
                                                        {"name", QFileInfo(root_).fileName()}}}}});
            } else if (method == "window/workDoneProgress/create")
                send({{"jsonrpc", "2.0"},
                      {"id", message.value("id")},
                      {"result", QJsonValue::Null}});
            else
                send({{"jsonrpc", "2.0"},
                      {"id", message.value("id")},
                      {"error",
                       QJsonObject{{"code", -32601}, {"message", "Client method not supported"}}}});
        }
        return;
    }
    const int id = message.value("id").toInt(-1);
    if (!pending_.contains(id))
        return;
    auto pending = pending_.take(id);
    const auto error = message.value("error").toObject();
    if (!error.isEmpty()) {
        const auto code = error.value("code").toInt();
        if (code == -32800 || code == -32801 || code == -32802)
            qCDebug(languageServiceLog)
                << "lsp.request_cancelled" << "service=" << serviceName_ << "request=" << id
                << "method=" << pending.method << "code=" << code;
        else
            qCWarning(languageServiceLog)
                << "lsp.request_failed" << "service=" << serviceName_ << "root=" << root_
                << "request=" << id << "method=" << pending.method << "path=" << pending.path
                << "code=" << code << "error=" << error.value("message").toString();
    }
    pending.reply(message.value("result"),
                  error.isEmpty() ? QString{}
                                  : error.value("message").toString("clangd request failed"));
}

void LanguageServerClient::syncDocument(const QString &path, const QByteArray &source, int version,
                                        const QString &languageId) {
    if (path.startsWith(":/"))
        return;
    const auto key = workspacePath(path);
    auto &doc = documents_[key];
    if (!languageId.isEmpty() && languageId != doc.language) {
        if (doc.sent)
            notify("textDocument/didClose", {{"textDocument", QJsonObject{{"uri", uri(key)}}}});
        doc.sent = false;
        doc.language = languageId;
    }
    if (doc.language.isEmpty())
        doc.language = languageForFile(path, source).lspId;
    if (doc.sent && doc.bytes == source)
        return;
    doc.bytes = source;
    doc.version = version;
    if (initialized_)
        flushDocument(key);
}

void LanguageServerClient::flushDocument(const QString &path) {
    auto &doc = documents_[path];
    if (!doc.sent) {
        notify("textDocument/didOpen",
               {{"textDocument", QJsonObject{{"uri", uri(path)},
                                             {"languageId", doc.language},
                                             {"version", doc.version},
                                             {"text", QString::fromUtf8(doc.bytes)}}}});
        doc.sent = true;
    } else {
        notify(
            "textDocument/didChange",
            {{"textDocument", QJsonObject{{"uri", uri(path)}, {"version", doc.version}}},
             {"contentChanges", QJsonArray{QJsonObject{{"text", QString::fromUtf8(doc.bytes)}}}}});
    }
}

void LanguageServerClient::locations(const QString &path, const QByteArray &source, int version,
                                     int position, quint64 token, const QString &channel,
                                     bool references, bool includeDeclaration) {
    if (!initialized_) {
        if (process_.state() == QProcess::NotRunning) {
            qCWarning(languageServiceLog)
                << "lsp.lookup_unavailable" << "service=" << serviceName_ << "root=" << root_
                << "path=" << path << "channel=" << channel;
            emit locationsReady(token, channel, {}, serviceName_ + " is unavailable.");
        } else
            queuedLookups_.insert(
                channel,
                {{path, source, version, position, token}, references, includeDeclaration});
        return;
    }
    syncDocument(path, source, version);
    cancelLocations(channel);
    QJsonObject parameters{{"textDocument", QJsonObject{{"uri", uri(path)}}},
                           {"position", lspPosition(source, position)}};
    if (references)
        parameters["context"] = QJsonObject{{"includeDeclaration", includeDeclaration}};
    lookupIds_[channel] = request(
        references ? "textDocument/references" : "textDocument/definition", parameters,
        [this, token, channel](const QJsonValue &value, const QString &error) {
            QVector<LspLocation> targets;
            const auto entries = value.isArray()    ? value.toArray()
                                 : value.isObject() ? QJsonArray{value}
                                                    : QJsonArray{};
            for (const auto &entry : entries) {
                const auto object = entry.toObject();
                const QUrl url(object.value("uri").toString(object.value("targetUri").toString()));
                if (!url.isLocalFile())
                    continue;
                const auto range =
                    object
                        .value(object.contains("targetSelectionRange") ? "targetSelectionRange"
                                                                       : "range")
                        .toObject();
                const auto start = range.value("start").toObject(),
                           end = range.value("end").toObject();
                if (start.value("line").toInt(-1) < 0 || start.value("character").toInt(-1) < 0)
                    continue;
                targets.append({url.toLocalFile(), start.value("line").toInt(),
                                start.value("character").toInt(), end.value("line").toInt(-1),
                                end.value("character").toInt(-1)});
            }
            emit locationsReady(token, channel, targets, error);
        });
}

void LanguageServerClient::cancelLocations(const QString &channel) {
    queuedLookups_.remove(channel);
    const int previous = lookupIds_.take(channel);
    if (pending_.contains(previous)) {
        notify("$/cancelRequest", {{"id", previous}});
        pending_.remove(previous);
    }
}

void LanguageServerClient::closeDocument(const QString &path) {
    const auto key = workspacePath(path);
    if (documents_.contains(key) && documents_[key].sent)
        notify("textDocument/didClose", {{"textDocument", QJsonObject{{"uri", uri(key)}}}});
    documents_.remove(key);
}

void LanguageServerClient::definition(const QString &path, const QByteArray &source, int version,
                                      int position, quint64 token) {
    syncDocument(path, source, version);
    selection_ = {workspacePath(path), source, version, position, token};
    haveSelection_ = true;
    ++selectionGeneration_;
    if (initialized_)
        requestSelection();
    else if (process_.state() == QProcess::NotRunning) {
        qCWarning(languageServiceLog) << "lsp.lookup_unavailable" << "service=" << serviceName_
                                      << "root=" << root_ << "path=" << path << "token=" << token;
        emit definitionReady(token, {}, "Language service unavailable.");
    }
}

void LanguageServerClient::requestSelection() {
    if (!haveSelection_ || !initialized_)
        return;
    haveSelection_ = false;
    if (pending_.contains(definitionId_)) {
        notify("$/cancelRequest", {{"id", definitionId_}});
        pending_.remove(definitionId_);
    }
    const auto token = selection_.token;
    const auto generation = selectionGeneration_;
    definitionId_ = request(
        "textDocument/definition",
        {{"textDocument", QJsonObject{{"uri", uri(selection_.path)}}},
         {"position", lspPosition(selection_.bytes, selection_.position)}},
        [this, token, generation](const QJsonValue &value, const QString &error) {
            if (generation != selectionGeneration_)
                return;
            QVector<LspLocation> targets;
            const auto entries = value.isArray()    ? value.toArray()
                                 : value.isObject() ? QJsonArray{value}
                                                    : QJsonArray{};
            for (const auto &entry : entries) {
                const auto object = entry.toObject();
                const QUrl targetUri(
                    object.value("uri").toString(object.value("targetUri").toString()));
                if (!targetUri.isLocalFile())
                    continue;
                const auto range =
                    object
                        .value(object.contains("targetSelectionRange") ? "targetSelectionRange"
                                                                       : "range")
                        .toObject();
                const auto start = range.value("start").toObject(),
                           end = range.value("end").toObject();
                targets.append({workspacePath(targetUri.toLocalFile()),
                                start.value("line").toInt(-1), start.value("character").toInt(-1),
                                end.value("line").toInt(-1), end.value("character").toInt(-1)});
            }
            emit definitionReady(token, targets, error);
        });
}

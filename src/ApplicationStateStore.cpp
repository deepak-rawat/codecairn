#include "ApplicationStateStore.h"
#include "Diagnostics.h"
#include "WorkspacePaths.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
constexpr qint64 MaximumStateBytes = 4 * 1024 * 1024;
constexpr qint64 MaximumSessionBytes = 32 * 1024 * 1024;

[[noreturn]] void invalid(const QString &message) {
    throw std::invalid_argument(message.toStdString());
}

void keys(const QJsonObject &object, const QStringList &allowed) {
    for (auto it = object.begin(); it != object.end(); ++it)
        if (!allowed.contains(it.key()))
            invalid("Unknown state field: " + it.key());
}

QString string(const QJsonObject &object, const QString &key) {
    if (!object.contains(key))
        return {};
    if (!object.value(key).isString())
        invalid(key + " must be a string.");
    return object.value(key).toString();
}

int integer(const QJsonObject &object, const QString &key, int maximum) {
    if (!object.contains(key))
        return 0;
    const auto value = object.value(key);
    const double number = value.toDouble(-1);
    if (!value.isDouble() || !std::isfinite(number) || number < 0 || number > maximum ||
        std::floor(number) != number)
        invalid(key + " must be a nonnegative integer no greater than " + QString::number(maximum) +
                '.');
    return static_cast<int>(number);
}

void schema(const QJsonObject &object) {
    const auto version = object.value("schemaVersion");
    if (!version.isDouble() || version.toDouble() != 1)
        invalid("Unsupported or missing state schemaVersion; expected 1.");
}

QByteArray binary(const QJsonObject &object, const QString &key) {
    const auto encoded = string(object, key).toUtf8();
    const auto decoded =
        QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded || decoded.decoded.toBase64() != encoded)
        invalid(key + " must be canonical Base64.");
    return decoded.decoded;
}

void absolutePath(const QString &value, const QString &key, bool allowEmpty = true) {
    if ((!allowEmpty || !value.isEmpty()) &&
        (value.isEmpty() || !QDir::isAbsolutePath(value) || value.contains(QChar::Null)))
        invalid(key + " must be an absolute filesystem path.");
}

ApplicationState parseState(const QJsonObject &object) {
    schema(object);
    keys(object, {"schemaVersion", "window", "lastWorkspace"});
    ApplicationState state;
    state.lastWorkspace = string(object, "lastWorkspace");
    absolutePath(state.lastWorkspace, "lastWorkspace");
    if (object.contains("window") && !object.value("window").isObject())
        invalid("window must be an object.");
    const auto window = object.value("window").toObject();
    keys(window, {"geometry", "mainSplitter", "rightSplitter", "browserTab"});
    state.geometry = binary(window, "geometry");
    state.mainSplitter = binary(window, "mainSplitter");
    state.rightSplitter = binary(window, "rightSplitter");
    state.browserTab = integer(window, "browserTab", 3);
    return state;
}

QJsonObject stateJson(const ApplicationState &state) {
    return {{"schemaVersion", 1},
            {"lastWorkspace", state.lastWorkspace},
            {"window",
             QJsonObject{{"geometry", QString::fromLatin1(state.geometry.toBase64())},
                         {"mainSplitter", QString::fromLatin1(state.mainSplitter.toBase64())},
                         {"rightSplitter", QString::fromLatin1(state.rightSplitter.toBase64())},
                         {"browserTab", state.browserTab}}}};
}

WorkspaceSession parseSession(const QJsonObject &object, const QString &root) {
    schema(object);
    keys(object, {"schemaVersion", "workspaceRoot", "files", "activeFile", "references"});
    const auto storedRoot = string(object, "workspaceRoot");
    absolutePath(storedRoot, "workspaceRoot", false);
    if (workspacePath(storedRoot) != workspacePath(root))
        invalid("Session workspaceRoot does not match the requested workspace.");
    if (object.contains("files") && !object.value("files").isArray())
        invalid("files must be an array.");
    WorkspaceSession session;
    QSet<QString> seen;
    for (const auto &value : object.value("files").toArray()) {
        if (!value.isObject())
            invalid("Each session file must be an object.");
        const auto entry = value.toObject();
        keys(entry, {"path", "start", "end", "firstLine"});
        NavigationLocation location;
        location.path = string(entry, "path");
        absolutePath(location.path, "path", false);
        if (!pathInWorkspace(location.path, root))
            invalid("Session file is outside its workspace: " + location.path);
        const auto identity = workspacePath(location.path);
        if (seen.contains(identity))
            invalid("Duplicate session file: " + location.path);
        seen.insert(identity);
        location.start = integer(entry, "start", std::numeric_limits<int>::max());
        location.end = integer(entry, "end", std::numeric_limits<int>::max());
        location.firstVisibleLine = integer(entry, "firstLine", std::numeric_limits<int>::max());
        if (location.end < location.start)
            invalid("Session selection end precedes its start.");
        session.files.append(location);
    }
    session.activeFile = string(object, "activeFile");
    absolutePath(session.activeFile, "activeFile");
    if (!session.activeFile.isEmpty() && !seen.contains(workspacePath(session.activeFile)))
        invalid("activeFile is not an open session file.");
    if (object.contains("references") && !object.value("references").isArray())
        invalid("references must be an array.");
    QString referenceError;
    session.references = parseReferenceHistory(object.value("references").toArray(), root, referenceError);
    if (!referenceError.isEmpty())
        invalid(referenceError);
    return session;
}

QJsonObject sessionJson(const QString &root, const WorkspaceSession &session) {
    QJsonArray files;
    for (const auto &location : session.files)
        files.append(QJsonObject{{"path", location.path},
                                 {"start", location.start},
                                 {"end", location.end},
                                 {"firstLine", location.firstVisibleLine}});
    return {{"schemaVersion", 1},
            {"workspaceRoot", workspacePath(root)},
            {"files", files},
            {"activeFile", session.activeFile},
            {"references", referenceHistoryJson(session.references)}};
}

QByteArray fingerprint(const QByteArray &bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}
}

ApplicationStateStore::ApplicationStateStore(QString directory)
    : directory_(directory.isEmpty() ? QString{} : QFileInfo(directory).absoluteFilePath()) {
}

QString ApplicationStateStore::statePath() const {
    return directory_.isEmpty() ? QString{} : QDir(directory_).filePath("state.json");
}

QString ApplicationStateStore::sessionPath(const QString &root) const {
    if (directory_.isEmpty() || root.isEmpty())
        return {};
    const auto directory = workspaceStorageDirectory(root, directory_);
    return directory.isEmpty() ? QString{} : QDir(directory).filePath("session.json");
}

ApplicationStateStore::JsonResult ApplicationStateStore::loadJson(const QString &path,
                                                                  const QJsonObject &defaults) {
    if (path.isEmpty())
        return {{}, "No application state directory is available."};
    auto &state = files_[path];
    state = {};
    state.existed = QFileInfo::exists(path);
    if (!state.existed) {
        state.loaded = true;
        return {defaults, {}};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {{}, path + ": " + file.errorString()};
    const auto maximum = path == statePath() ? MaximumStateBytes : MaximumSessionBytes;
    const auto sizeError = path + QString(": state exceeds the %1 MiB limit.").arg(maximum / (1024 * 1024));
    if (file.size() > maximum)
        return {{}, sizeError};
    const auto bytes = file.read(maximum + 1);
    if (file.error() != QFileDevice::NoError)
        return {{}, path + ": " + file.errorString()};
    if (bytes.size() > maximum)
        return {{}, sizeError};
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError)
        return {{}, path + ": invalid state JSON: " + error.errorString()};
    if (!document.isObject())
        return {{}, path + ": state must be a JSON object."};
    state.fingerprint = fingerprint(bytes);
    state.loaded = true;
    return {document.object(), {}};
}

QString ApplicationStateStore::saveJson(const QString &path, const QJsonObject &object) {
    if (path.isEmpty())
        return "No application state directory is available.";
    auto &state = files_[path];
    if (!state.loaded)
        return path + ": reload and fix invalid state before saving; it will not be overwritten.";
    const auto bytes = QJsonDocument(object).toJson();
    const auto maximum = path == statePath() ? MaximumStateBytes : MaximumSessionBytes;
    if (bytes.size() > maximum)
        return path + QString(": state exceeds the %1 MiB limit.").arg(maximum / (1024 * 1024));
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return path + ": cannot create the state directory.";
    QLockFile lock(path + ".lock");
    if (!lock.tryLock(0))
        return path + (lock.error() == QLockFile::LockFailedError
                           ? ": another process is saving this state; retry after it finishes."
                           : ": cannot lock state; check directory permissions.");
    const auto changed =
        path + ": state changed externally. Restart CodeCairn before saving again.";
    QFile current(path);
    if (current.exists() != state.existed)
        return changed;
    if (state.existed) {
        if (!current.open(QIODevice::ReadOnly))
            return path + ": " + current.errorString();
        if (current.size() > maximum)
            return changed;
        const auto existing = current.read(maximum + 1);
        if (current.error() != QFileDevice::NoError)
            return path + ": " + current.errorString();
        if (existing.size() > maximum || fingerprint(existing) != state.fingerprint)
            return changed;
        current.close();
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return path + ": " + file.errorString();
    if (file.write(bytes) != bytes.size()) {
        const auto error = file.errorString();
        file.cancelWriting();
        return path + ": " + error;
    }
    if (!file.commit())
        return path + ": " + file.errorString();
    state.existed = true;
    state.fingerprint = fingerprint(bytes);
    return {};
}

ApplicationStateResult ApplicationStateStore::loadState() {
    const auto path = statePath();
    const auto result = loadJson(path, stateJson({}));
    if (!result.object) {
        qCWarning(storageLog) << "storage.state_read_failed" << "path=" << path
                              << "error=" << result.error;
        return {{}, result.error};
    }
    try {
        return {parseState(*result.object), {}};
    } catch (const std::invalid_argument &error) {
        invalidateState();
        qCWarning(storageLog) << "storage.state_read_failed" << "path=" << path
                              << "error=" << error.what();
        return {{}, path + ": " + QString::fromUtf8(error.what())};
    }
}

QString ApplicationStateStore::saveState(const ApplicationState &state) {
    const auto object = stateJson(state);
    try {
        parseState(object);
    } catch (const std::invalid_argument &error) {
        qCWarning(storageLog) << "storage.state_write_failed" << "path=" << statePath()
                              << "error=" << error.what();
        return statePath() + ": " + QString::fromUtf8(error.what());
    }
    const auto error = saveJson(statePath(), object);
    if (!error.isEmpty())
        qCWarning(storageLog) << "storage.state_write_failed" << "path=" << statePath()
                              << "error=" << error;
    return error;
}

WorkspaceSessionResult ApplicationStateStore::loadSession(const QString &root) {
    const auto path = sessionPath(root);
    try {
        absolutePath(root, "workspaceRoot", false);
        const auto result = loadJson(path, sessionJson(root, {}));
        if (!result.object) {
            qCWarning(storageLog) << "storage.session_read_failed" << "path=" << path
                                  << "error=" << result.error;
            return {{}, result.error};
        }
        return {parseSession(*result.object, root), {}};
    } catch (const std::invalid_argument &error) {
        files_[path].loaded = false;
        qCWarning(storageLog) << "storage.session_read_failed" << "path=" << path
                              << "error=" << error.what();
        return {{}, path + ": " + QString::fromUtf8(error.what())};
    }
}

QString ApplicationStateStore::saveSession(const QString &root, const WorkspaceSession &session) {
    try {
        absolutePath(root, "workspaceRoot", false);
        const auto object = sessionJson(root, session);
        parseSession(object, root);
        const auto error = saveJson(sessionPath(root), object);
        if (!error.isEmpty())
            qCWarning(storageLog) << "storage.session_write_failed" << "path=" << sessionPath(root)
                                  << "error=" << error;
        return error;
    } catch (const std::invalid_argument &error) {
        qCWarning(storageLog) << "storage.session_write_failed" << "path=" << sessionPath(root)
                              << "error=" << error.what();
        return sessionPath(root) + ": " + QString::fromUtf8(error.what());
    }
}

void ApplicationStateStore::invalidateState() {
    files_[statePath()].loaded = false;
}

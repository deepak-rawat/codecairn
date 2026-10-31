#include "Diagnostics.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMutex>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QSysInfo>
#include <QThread>
#include <QUuid>
#include <cstdio>
#include <utility>

Q_LOGGING_CATEGORY(analysisLog, "codecairn.analysis", QtInfoMsg)
Q_LOGGING_CATEGORY(indexLog, "codecairn.index", QtInfoMsg)
Q_LOGGING_CATEGORY(languageServiceLog, "codecairn.lsp", QtInfoMsg)
Q_LOGGING_CATEGORY(navigationLog, "codecairn.navigation", QtInfoMsg)
Q_LOGGING_CATEGORY(fileLog, "codecairn.files", QtInfoMsg)
Q_LOGGING_CATEGORY(storageLog, "codecairn.storage", QtInfoMsg)

namespace {
QRecursiveMutex handlerMutex;
Diagnostics *active = nullptr;
QString pendingError;
QString requestedDirectory;
constexpr qint64 MaximumMetadataBytes = 64 * 1024;

QString now() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

QString severity(QtMsgType type) {
    switch (type) {
    case QtDebugMsg:
        return "debug";
    case QtInfoMsg:
        return "info";
    case QtWarningMsg:
        return "warning";
    case QtCriticalMsg:
        return "critical";
    case QtFatalMsg:
        return "fatal";
    }
    return "unknown";
}

void stderrMessage(const QString &text) {
    const auto bytes = text.toLocal8Bit();
    std::fprintf(stderr, "%s\n", bytes.constData());
    std::fflush(stderr);
}

QString saveMetadata(const QString &path, const QJsonObject &object) {
    if (QFileInfo(path).isSymLink())
        return path + ": refusing a symbolic-link session marker.";
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return path + ": " + file.errorString();
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return path + ": cannot restrict marker permissions: " + file.errorString();
    const auto bytes = QJsonDocument(object).toJson();
    if (file.write(bytes) != bytes.size() || !file.commit())
        return path + ": " + file.errorString();
    return {};
}
}

QString Diagnostics::defaultDirectory() {
#ifdef Q_OS_WIN
    const auto base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return base.isEmpty() ? QString{} : QDir(base).filePath("Logs");
#elif defined(Q_OS_MACOS)
    return QDir::home().filePath("Library/Logs/" + QCoreApplication::applicationName());
#else
    auto base = qEnvironmentVariable("XDG_STATE_HOME");
    if (!QDir::isAbsolutePath(base))
        base = QDir::home().filePath(".local/state");
    return QDir(base).filePath(QCoreApplication::applicationName() + "/logs");
#endif
}

Diagnostics::Diagnostics() = default;

QString Diagnostics::directory() {
    const QMutexLocker guard(&handlerMutex);
    return requestedDirectory.isEmpty() ? defaultDirectory() : requestedDirectory;
}

QString Diagnostics::currentLogPath() {
    const QMutexLocker guard(&handlerMutex);
    return active ? active->logPath() : QString{};
}

QString Diagnostics::takePendingError() {
    const QMutexLocker guard(&handlerMutex);
    return std::exchange(pendingError, {});
}

QString Diagnostics::logPath() const {
    return sessionDirectory_.isEmpty() ? QString{}
                                       : QDir(sessionDirectory_).filePath("codecairn.jsonl");
}

Diagnostics::~Diagnostics() {
    const QMutexLocker guard(&handlerMutex);
    if (active == this) {
        qInstallMessageHandler(previousHandler_);
        active = nullptr;
    }
    file_.close();
    if (sessionLock_)
        sessionLock_->unlock();
}

QString Diagnostics::start(const DiagnosticOptions &options) {
    const QMutexLocker guard(&handlerMutex);
    if (started_ || active)
        return "A diagnostics session is already active.";
    if (options.maxFileBytes < 4096 || options.maxFileBytes > 1024LL * 1024 * 1024 ||
        options.maxFiles < 1 || options.maxFiles > 20 || options.maxSessions < 1 ||
        options.maxSessions > 1000)
        return "Invalid diagnostics rotation/retention limits.";
    options_ = options;
    if (options_.directory.isEmpty())
        options_.directory = defaultDirectory();
    requestedDirectory = options_.directory;
    if (options_.directory.isEmpty() || !QDir::isAbsolutePath(options_.directory))
        return "No absolute diagnostics directory is available.";
    options_.directory = QFileInfo(options_.directory).absoluteFilePath();
    requestedDirectory = options_.directory;
    if (QFileInfo(options_.directory).isSymLink() || !QDir().mkpath(options_.directory))
        return options_.directory + ": cannot create the diagnostics directory.";
    const auto id = "session-" + QDateTime::currentDateTimeUtc().toString("yyyyMMddTHHmmsszzzZ") +
                    '-' + QString::number(QCoreApplication::applicationPid()) + '-' +
                    QUuid::createUuid().toString(QUuid::Id128);
    sessionDirectory_ = QDir(options_.directory).filePath(id);
    if (!QDir().mkdir(sessionDirectory_))
        return sessionDirectory_ + ": cannot create the diagnostics session.";
    sessionLock_ = std::make_unique<QLockFile>(QDir(sessionDirectory_).filePath("session.lock"));
    sessionLock_->setStaleLockTime(0);
    if (!sessionLock_->tryLock(0)) {
        QDir().rmdir(sessionDirectory_);
        return sessionDirectory_ + ": cannot lock the diagnostics session.";
    }
    file_.setFileName(logPath());
    if (!file_.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        sessionLock_->unlock();
        QDir().rmdir(sessionDirectory_);
        return logPath() + ": " + file_.errorString();
    }
    if (!file_.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        const auto error = logPath() + ": cannot restrict log permissions: " + file_.errorString();
        file_.close();
        file_.remove();
        sessionLock_->unlock();
        QDir().rmdir(sessionDirectory_);
        return error;
    }
    startedAt_ = now();
    environment_ = {{"version", QCoreApplication::applicationVersion()},
                    {"qt", qVersion()},
                    {"os", QSysInfo::prettyProductName()},
                    {"architecture", QSysInfo::currentCpuArchitecture()},
                    {"executable", QCoreApplication::applicationFilePath()},
#ifdef CODECAIRN_BUILD_CONFIGURATION
                    {"build", QString::fromUtf8(CODECAIRN_BUILD_CONFIGURATION)},
                    {"compiler", QString::fromUtf8(CODECAIRN_COMPILER_ID)},
                    {"compilerVersion", QString::fromUtf8(CODECAIRN_COMPILER_VERSION)}
#elif defined(NDEBUG)
                    {"build", "release"}
#else
                    {"build", "debug"}
#endif
    };
    const auto error = writeSession("running");
    if (!error.isEmpty()) {
        file_.close();
        file_.remove();
        sessionLock_->unlock();
        QDir().rmdir(sessionDirectory_);
        return error;
    }
    previousHandler_ = qInstallMessageHandler(messageHandler);
    active = this;
    started_ = true;
    pendingError.clear();
    maintainSessions();
    const QMessageLogContext context(__FILE__, __LINE__, Q_FUNC_INFO, "codecairn.session");
    auto startup = environment_;
    startup["event"] = "session.started";
    startup["level"] = options_.debug ? "debug" : "info";
    const auto record = [&](QtMsgType type, const QString &message) {
        const auto error = append(type, context, message);
        if (!error.isEmpty()) {
            pendingError = error;
            stderrMessage(error);
        }
    };
    record(QtInfoMsg, "session.started " +
                          QString::fromUtf8(QJsonDocument(startup).toJson(QJsonDocument::Compact)));
    for (const auto &path : uncleanSessions_)
        record(QtWarningMsg, "session.unclean_detected path=" + path);
    for (const auto &warning : startupWarnings_)
        record(QtWarningMsg, "session.maintenance_warning " + warning);
    return {};
}

QString Diagnostics::finish(int exitCode) {
    const QMutexLocker guard(&handlerMutex);
    if (!started_ || finished_)
        return {};
    const QMessageLogContext context(__FILE__, __LINE__, Q_FUNC_INFO, "codecairn.session");
    auto error =
        append(QtInfoMsg, context, "session.finished exitCode=" + QString::number(exitCode));
    if (!error.isEmpty())
        return error;
    error = writeSession("completed", exitCode);
    if (error.isEmpty())
        finished_ = true;
    return error;
}

QString Diagnostics::writeSession(const QString &state, int exitCode) {
    QJsonObject object{{"schemaVersion", 1},
                       {"application", "CodeCairn"},
                       {"session", QFileInfo(sessionDirectory_).fileName()},
                       {"pid", QCoreApplication::applicationPid()},
                       {"startedAt", startedAt_},
                       {"state", state},
                       {"logFile", "codecairn.jsonl"},
                       {"environment", environment_}};
    if (state == "completed") {
        object["loggingComplete"] = !failed_;
        object["finishedAt"] = now();
        object["exitCode"] = exitCode;
    }
    return saveMetadata(QDir(sessionDirectory_).filePath("session.json"), object);
}

void Diagnostics::maintainSessions() {
    if (QFileInfo(QDir(options_.directory).filePath("maintenance.lock")).isSymLink()) {
        startupWarnings_
            << "Symbolic-link diagnostics maintenance lock; recovery and retention were skipped.";
        return;
    }
    QLockFile maintenance(QDir(options_.directory).filePath("maintenance.lock"));
    maintenance.setStaleLockTime(0);
    if (!maintenance.tryLock(1000)) {
        startupWarnings_ << "Diagnostics maintenance is busy; recovery and retention were skipped.";
        return;
    }
    const QRegularExpression ownSession("^session-[0-9]{8}T[0-9]{9}Z-[0-9]+-[0-9a-f]{32}$");
    const QRegularExpression ownFile("^(session\\.json|codecairn(?:\\.[0-9]+)?\\.jsonl)$");
    const auto sessions =
        QDir(options_.directory)
            .entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
    int retained = 0;
    for (const auto &entry : sessions) {
        if (entry.isSymLink() || !ownSession.match(entry.fileName()).hasMatch())
            continue;
        if (entry.absoluteFilePath() == sessionDirectory_) {
            ++retained;
            continue;
        }
        QDir directory(entry.absoluteFilePath());
        const auto marker = directory.filePath("session.json");
        QFile file(marker);
        if (QFileInfo(marker).isSymLink() || !file.open(QIODevice::ReadOnly) ||
            file.size() > MaximumMetadataBytes) {
            startupWarnings_ << marker + ": unreadable or invalid session marker retained.";
            continue;
        }
        QJsonParseError parseError;
        const auto bytes = file.read(MaximumMetadataBytes + 1);
        const bool readError =
            file.error() != QFileDevice::NoError || bytes.size() > MaximumMetadataBytes;
        const auto document = QJsonDocument::fromJson(bytes, &parseError);
        file.close();
        auto object = document.object();
        const auto state = object.value("state").toString();
        if (readError || parseError.error != QJsonParseError::NoError || !document.isObject() ||
            object.value("schemaVersion").toInt() != 1 ||
            object.value("application").toString() != "CodeCairn" ||
            object.value("session").toString() != entry.fileName() ||
            !QStringList{"running", "completed", "unclean"}.contains(state)) {
            startupWarnings_ << marker + ": invalid session marker retained.";
            continue;
        }
        ++retained;
        if (QFileInfo(directory.filePath("session.lock")).isSymLink()) {
            startupWarnings_ << directory.path() + ": symbolic-link session lock retained.";
            continue;
        }
        QLockFile lock(directory.filePath("session.lock"));
        lock.setStaleLockTime(0);
        if (!lock.tryLock(0))
            continue;
        bool recovered = false;
        if (state == "running") {
            object["state"] = "unclean";
            object["detectedAt"] = now();
            object["reason"] =
                "No normal shutdown marker; possible crash, forced termination, or power loss.";
            const auto error = saveMetadata(marker, object);
            if (!error.isEmpty()) {
                startupWarnings_ << error;
                continue;
            }
            uncleanSessions_ << entry.absoluteFilePath();
            recovered = true;
        }
        if (retained <= options_.maxSessions || recovered)
            continue;
        const auto contents = directory.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot |
                                                      QDir::Hidden | QDir::System);
        bool safe = true;
        for (const auto &child : contents)
            if (child.fileName() != "session.lock" && (child.isSymLink() || !child.isFile() ||
                                                       !ownFile.match(child.fileName()).hasMatch()))
                safe = false;
        if (!safe) {
            startupWarnings_ << directory.path() +
                                    ": unexpected files retained during log cleanup.";
            continue;
        }
        for (const auto &child : contents)
            if (child.fileName() != "session.lock" && !QFile::remove(child.absoluteFilePath()))
                startupWarnings_ << child.absoluteFilePath() +
                                        ": could not remove old diagnostic file.";
        lock.unlock();
        if (!QDir().rmdir(directory.path()))
            startupWarnings_ << directory.path() + ": could not remove old diagnostic session.";
    }
}

QString Diagnostics::rotate() {
    file_.close();
    const auto path = [this](int number) {
        return QDir(sessionDirectory_)
            .filePath(number ? QString("codecairn.%1.jsonl").arg(number)
                             : QString("codecairn.jsonl"));
    };
    for (int i = options_.maxFiles - 1; i >= 0; --i) {
        const auto old = path(i);
        if (!QFileInfo::exists(old))
            continue;
        if (QFileInfo(old).isSymLink())
            return old + ": refusing a symbolic-link log.";
        const bool ok =
            i == options_.maxFiles - 1 ? QFile::remove(old) : QFile::rename(old, path(i + 1));
        if (!ok)
            return old + ": cannot rotate the diagnostic log.";
    }
    if (!file_.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return logPath() + ": " + file_.errorString();
    if (!file_.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner))
        return logPath() + ": cannot restrict log permissions: " + file_.errorString();
    return {};
}

QString Diagnostics::append(QtMsgType type, const QMessageLogContext &context,
                            const QString &message) {
    if (failed_)
        return {};
    if (type == QtDebugMsg && !options_.debug)
        return {};
    auto text = message.left(8192);
    bool truncated = text.size() != message.size();
    const auto category = QString::fromUtf8(context.category ? context.category : "default");
    truncated = truncated || category.size() > 128;
    QJsonObject record{
        {"time", now()},
        {"sequence", ++sequence_},
        {"level", severity(type)},
        {"pid", QCoreApplication::applicationPid()},
        {"thread", QString::number(reinterpret_cast<quintptr>(QThread::currentThreadId()), 16)},
        {"category", category.left(128)}};
    const auto event = message.section(' ', 0, 0);
    static const QRegularExpression eventName(
        "^(analysis|index|lsp|navigation|workspace|session|file|storage|search)\\.[a-z_]+$");
    if (eventName.match(event).hasMatch())
        record["event"] = event;
    if (context.file) {
        const auto path = QString::fromUtf8(context.file);
        truncated = truncated || path.size() > 512;
        record["file"] = path.left(512);
    }
    if (context.function) {
        const auto function = QString::fromUtf8(context.function);
        truncated = truncated || function.size() > 256;
        record["function"] = function.left(256);
    }
    if (context.line)
        record["line"] = context.line;
    QByteArray bytes;
    while (true) {
        if (!text.isEmpty() && text.back().isHighSurrogate()) {
            text.chop(1);
            truncated = true;
        }
        record["message"] = text;
        record["truncated"] = truncated;
        bytes = QJsonDocument(record).toJson(QJsonDocument::Compact) + '\n';
        if (bytes.size() <= options_.maxFileBytes)
            break;
        if (text.isEmpty()) {
            failed_ = true;
            return logPath() + ": diagnostic metadata exceeds the log record limit.";
        }
        text.truncate(text.size() / 2);
        truncated = true;
    }
    QString error;
    if (file_.size() + bytes.size() > options_.maxFileBytes)
        error = rotate();
    if (error.isEmpty() && (file_.write(bytes) != bytes.size() || !file_.flush()))
        error = logPath() + ": " + file_.errorString();
    if (!error.isEmpty())
        failed_ = true;
    return error;
}

void Diagnostics::messageHandler(QtMsgType type, const QMessageLogContext &context,
                                 const QString &message) {
    thread_local bool handling = false;
    if (handling) {
        stderrMessage(qFormatLogMessage(type, context, message));
        return;
    }
    handling = true;
    const auto reset = qScopeGuard([&] { handling = false; });
    QtMessageHandler previous = nullptr;
    QString error;
    {
        const QMutexLocker guard(&handlerMutex);
        if (active) {
            previous = active->previousHandler_;
            error = active->append(type, context, message);
            if (!error.isEmpty())
                pendingError = error;
        }
    }
    if (!error.isEmpty())
        stderrMessage("CodeCairn file logging failed: " + error);
    if (previous)
        previous(type, context, message);
    else
        stderrMessage(qFormatLogMessage(type, context, message));
}

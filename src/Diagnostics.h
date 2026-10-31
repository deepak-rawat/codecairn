#pragma once

#include <QFile>
#include <QLoggingCategory>
#include <QJsonObject>
#include <QStringList>
#include <memory>

class QLockFile;

Q_DECLARE_LOGGING_CATEGORY(analysisLog)
Q_DECLARE_LOGGING_CATEGORY(indexLog)
Q_DECLARE_LOGGING_CATEGORY(languageServiceLog)
Q_DECLARE_LOGGING_CATEGORY(navigationLog)
Q_DECLARE_LOGGING_CATEGORY(fileLog)
Q_DECLARE_LOGGING_CATEGORY(storageLog)

struct DiagnosticOptions {
    QString directory;
    qint64 maxFileBytes = 5 * 1024 * 1024;
    int maxFiles = 5;
    int maxSessions = 10;
    bool debug = false;
};

class Diagnostics {
public:
    Diagnostics();
    ~Diagnostics();
    Diagnostics(const Diagnostics &) = delete;
    Diagnostics &operator=(const Diagnostics &) = delete;

    QString start(const DiagnosticOptions &options = {});
    QString finish(int exitCode);
    QString sessionDirectory() const { return sessionDirectory_; }
    QString logPath() const;
    QStringList uncleanSessions() const { return uncleanSessions_; }
    QStringList startupWarnings() const { return startupWarnings_; }

    static QString defaultDirectory();
    static QString directory();
    static QString currentLogPath();
    static QString takePendingError();

private:
    static void messageHandler(QtMsgType type, const QMessageLogContext &context,
                               const QString &message);
    QString append(QtMsgType type, const QMessageLogContext &context, const QString &message);
    QString rotate();
    QString writeSession(const QString &state, int exitCode = 0);
    void maintainSessions();
    DiagnosticOptions options_;
    QString sessionDirectory_;
    QString startedAt_;
    QStringList uncleanSessions_;
    QStringList startupWarnings_;
    QJsonObject environment_;
    QFile file_;
    std::unique_ptr<QLockFile> sessionLock_;
    QtMessageHandler previousHandler_ = nullptr;
    qint64 sequence_ = 0;
    bool started_ = false;
    bool finished_ = false;
    bool failed_ = false;
};

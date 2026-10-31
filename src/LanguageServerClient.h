#pragma once
#include "SourceCoordinates.h"

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <functional>

class LanguageServerClient final : public QObject {
    Q_OBJECT
public:
    explicit LanguageServerClient(QObject *parent = nullptr);
    ~LanguageServerClient() override;
    void start(const QString &root, const QString &compilationDirectory = {},
               const QString &executable = {});
    void startServer(const QString &root, const QStringList &command);
    void stop();
    void syncDocument(const QString &path, const QByteArray &source, int version,
                      const QString &languageId = {});
    void closeDocument(const QString &path);
    void definition(const QString &path, const QByteArray &source, int version, int position,
                    quint64 token);
    void locations(const QString &path, const QByteArray &source, int version, int position,
                   quint64 token, const QString &channel, bool references = false,
                   bool includeDeclaration = true);
    void cancelLocations(const QString &channel);
    bool available() const { return initialized_; }
    bool hasCompilationDatabase() const { return configured_; }
    QString name() const { return serviceName_; }
signals:
    void ready();
    void status(const QString &message);
    void definitionReady(quint64 token, const QVector<LspLocation> &targets, const QString &error);
    void locationsReady(quint64 token, const QString &channel, const QVector<LspLocation> &targets,
                        const QString &error);

private:
    using Reply = std::function<void(const QJsonValue &, const QString &)>;
    struct Pending {
        Reply reply;
        qint64 deadline;
        QString method;
        QString path;
    };
    struct Document {
        QByteArray bytes;
        int version = 0;
        bool sent = false;
        QString language;
    };
    struct Selection {
        QString path;
        QByteArray bytes;
        int version = 0;
        int position = 0;
        quint64 token = 0;
    };
    void send(const QJsonObject &message);
    int request(const QString &method, const QJsonObject &parameters, Reply reply);
    void notify(const QString &method, const QJsonObject &parameters);
    void receive();
    void processMessage(const QJsonObject &message);
    void requestSelection();
    void flushDocument(const QString &path);
    QProcess process_;
    QTimer timeout_;
    QByteArray input_;
    QHash<int, Pending> pending_;
    QHash<QString, Document> documents_;
    QString root_;
    QString commandDirectory_;
    Selection selection_;
    bool haveSelection_ = false;
    bool initialized_ = false;
    bool configured_ = false;
    bool stopping_ = false;
    int nextId_ = 0;
    int definitionId_ = 0;
    quint64 selectionGeneration_ = 0;
    struct Lookup {
        Selection selection;
        bool references;
        bool includeDeclaration;
    };
    QHash<QString, Lookup> queuedLookups_;
    QHash<QString, int> lookupIds_;
    QString serviceName_ = "clangd";
    void failQueuedLookups(const QString &error);
};

#pragma once

#include "LanguageServerClient.h"
#include <QHash>
#include <QObject>

namespace codecairn::detail {

class ProjectLanguages final : public QObject {
    Q_OBJECT
public:
    explicit ProjectLanguages(QObject *parent = nullptr);
    LanguageServerClient *cpp() const { return cpp_; }
    LanguageServerClient *serviceFor(const QString &path, const QByteArray &source);
    void start(const QString &root, const QString &compilationDirectory,
               const QString &projectFile);
    void stop();
    void closeDocument(const QString &path);

signals:
    void serviceCreated(LanguageServerClient *service);

private:
    QString root_;
    LanguageServerClient *cpp_;
    QHash<int, LanguageServerClient *> others_;
};

}

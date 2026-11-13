#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <functional>

namespace codecairn {

struct SourceSnapshot {
    QString path;
    QByteArray bytes;
    QByteArray hash;
    quint64 revision = 0;
};

struct SourcePosition {
    SourceSnapshot snapshot;
    int byteOffset = 0;
};

class Document final : public QObject {
    Q_OBJECT
public:
    enum class ExternalChange { None, Removed, UnsavedBuffer, Reload };

    Document(const QString &path, const QByteArray &bytes, bool hasUtf8Bom = false,
             QObject *parent = nullptr);

    const QString &path() const { return path_; }
    quint64 revision() const { return revision_; }
    const QByteArray &savedBytes() const { return savedBytes_; }
    bool hasUtf8Bom() const { return hasUtf8Bom_; }
    const QByteArray &savedContentHash() const { return savedContentHash_; }
    bool modified() const;

    const QByteArray &contents() const;
    const QByteArray &contentHash() const;
    SourceSnapshot snapshot() const;
    SourcePosition position(int byteOffset) const;
    void updateContents(const QByteArray &bytes);
    void contentChanged();
    // Providers run synchronously on the owning thread, once per requested revision.
    // Unbinding retains the current contents and releases the provider.
    void bindContents(std::function<QByteArray()> provider);
    void setSavedContents(const QByteArray &bytes, bool hasUtf8Bom);
    ExternalChange externalChange(const QByteArray &hash, bool fileExists, bool modified);

signals:
    void contentsChanged();

private:
    friend class Engine;
    void setPath(QString path);
    void assertOwnerThread() const;

    QString path_;
    quint64 revision_ = 0;
    QByteArray savedBytes_;
    QByteArray savedContentHash_;
    bool hasUtf8Bom_ = false;
    QByteArray lastReportedExternalHash_;
    std::function<QByteArray()> provider_;
    mutable QByteArray bytes_;
    mutable QByteArray hash_;
    mutable bool bytesCurrent_ = true;
    mutable bool hashCurrent_ = false;
};

}

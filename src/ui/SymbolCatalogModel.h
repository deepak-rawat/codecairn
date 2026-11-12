#pragma once

#include <codecairn/IndexTypes.h>
#include <QAbstractItemModel>
#include <QCache>
#include <QFutureWatcher>
#include <QQueue>
#include <QThreadPool>
#include <optional>

class SymbolCatalogModel final : public QAbstractItemModel {
    Q_OBJECT
public:
    explicit SymbolCatalogModel(bool categories, QObject *parent = nullptr);
    ~SymbolCatalogModel() override;
    void setCatalog(const QString &root, const SymbolQuery &query, const SymbolPage &summary);
    void clear();
    void invalidatePages();
    QModelIndex index(int row, int column, const QModelIndex &parent = {}) const override;
    QModelIndex parent(const QModelIndex &index) const override;
    int rowCount(const QModelIndex &parent = {}) const override;
    int columnCount(const QModelIndex & = {}) const override { return 2; }
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    std::optional<IndexedSymbol> entry(const QModelIndex &index) const;
    QString category(const QModelIndex &index) const;
    int cachedRows() const;
signals:
    void rowsReady();
    void failed(const QString &error);

private:
    struct Request {
        QString category;
        int offset = 0;
        quint64 generation = 0;
        QString key() const;
    };
    void request(const QModelIndex &index);
    void startNext();
    QString root_;
    SymbolQuery query_;
    QMap<QString, int> counts_;
    QStringList categories_;
    int total_ = 0;
    bool grouped_ = false;
    quint64 generation_ = 0;
    QCache<QString, QVector<IndexedSymbol>> pages_{16};
    QQueue<Request> pending_;
    QSet<QString> queued_;
    QHash<QString, QString> failedPages_;
    Request running_;
    QFutureWatcher<SymbolPage> job_;
    QThreadPool pool_;
    std::shared_ptr<std::atomic_bool> cancelled_;
    bool busy_ = false;
};
